#include "SyncApi.h"

#include "Jwt.h"
#include "Util.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

using namespace drogon;

namespace syncserver
{
    namespace
    {
        // KDBX 2/3/4 files start with these two little-endian uint32 signatures
        constexpr std::string_view KDBX_SIGNATURE("\x03\xd9\xa2\x9a\x67\xfb\x4b\xb5", 8);

        using Callback = std::function<void(const HttpResponsePtr&)>;

        struct HttpError : std::runtime_error
        {
            // code overrides errorCodeFor(status); extra adds members to the JSON body (account routes only)
            HttpError(HttpStatusCode status,
                      const std::string& message,
                      std::string code = {},
                      Json::Value extra = Json::Value())
                : std::runtime_error(message)
                , status(status)
                , code(std::move(code))
                , extra(std::move(extra))
            {
            }
            HttpStatusCode status;
            std::string code;
            Json::Value extra;
        };

        HttpResponsePtr jsonResponse(HttpStatusCode status, const Json::Value& body)
        {
            auto resp = HttpResponse::newHttpJsonResponse(body);
            resp->setStatusCode(status);
            return resp;
        }

        // Machine-readable error codes of openapi.yaml, sent on the account routes only
        std::string errorCodeFor(HttpStatusCode status)
        {
            switch (status) {
            case k401Unauthorized:
                return "unauthorized";
            case k403Forbidden:
                return "forbidden";
            case k404NotFound:
                return "not_found";
            case k409Conflict:
                return "conflict";
            case k412PreconditionFailed:
                return "precondition_failed";
            case k428PreconditionRequired:
                return "precondition_required";
            case k429TooManyRequests:
                return "rate_limited";
            default:
                return status >= 500 ? "internal_error" : "invalid_request";
            }
        }

        HttpResponsePtr errorResponse(HttpStatusCode status,
                                      const std::string& message,
                                      bool withCode = false,
                                      const std::string& code = {},
                                      const Json::Value& extra = Json::Value())
        {
            Json::Value body;
            body["error"] = message;
            if (withCode) {
                body["code"] = code.empty() ? errorCodeFor(status) : code;
                if (extra.isObject()) {
                    for (const auto& name : extra.getMemberNames()) {
                        body[name] = extra[name];
                    }
                }
            }
            auto resp = jsonResponse(status, body);
            if (status == k401Unauthorized) {
                resp->addHeader("WWW-Authenticate", "Bearer realm=\"openvireo-sync\"");
            }
            resp->addHeader("Cache-Control", "no-store");
            return resp;
        }

        // Runs a handler body, turning HttpError and unexpected exceptions into JSON error responses;
        // withCode adds the "code" of the account routes
        template <typename Fn> void respond(const Callback& callback, Fn&& fn, bool withCode = false)
        {
            HttpResponsePtr resp;
            try {
                resp = fn();
            } catch (const HttpError& e) {
                resp = errorResponse(e.status, e.what(), withCode, e.code, e.extra);
            } catch (const std::exception& e) {
                LOG_ERROR << e.what();
                resp = errorResponse(k500InternalServerError, "Internal server error.", withCode);
            }
            callback(resp);
        }

        std::string trim(std::string_view s)
        {
            auto begin = s.find_first_not_of(" \t");
            if (begin == std::string_view::npos) {
                return {};
            }
            auto end = s.find_last_not_of(" \t");
            return std::string(s.substr(begin, end - begin + 1));
        }

        // Parses an If-Match / If-None-Match list, treating weak tags as strong
        std::optional<std::vector<std::string>> parseEtagList(const std::string& header)
        {
            if (header.empty()) {
                return std::nullopt;
            }
            std::vector<std::string> tags;
            size_t start = 0;
            while (start <= header.size()) {
                auto end = header.find(',', start);
                if (end == std::string::npos) {
                    end = header.size();
                }
                auto tag = trim(std::string_view(header).substr(start, end - start));
                if (tag.rfind("W/", 0) == 0) {
                    tag.erase(0, 2);
                }
                if (!tag.empty()) {
                    tags.push_back(std::move(tag));
                }
                start = end + 1;
            }
            return tags;
        }

        bool etagMatches(const std::vector<std::string>& tags, const std::optional<DatabaseMeta>& meta)
        {
            if (!meta) {
                return false;
            }
            return std::find(tags.begin(), tags.end(), "*") != tags.end()
                   || std::find(tags.begin(), tags.end(), meta->etag) != tags.end();
        }

        /**
         * Optimistic concurrency for writes, following RFC 9110:
         *   If-None-Match: *     -> only create, fail if it already exists
         *   If-Match: "<etag>"   -> only overwrite the version the client last saw
         * Overwriting an existing database without either header is refused (428) so a
         * client can never blindly clobber changes made from another device.
         */
        void checkWritePreconditions(const HttpRequestPtr& req, const std::optional<DatabaseMeta>& meta)
        {
            auto ifMatch = parseEtagList(req->getHeader("If-Match"));
            auto ifNoneMatch = parseEtagList(req->getHeader("If-None-Match"));

            if (ifMatch && !etagMatches(*ifMatch, meta)) {
                throw HttpError(k412PreconditionFailed,
                                "The database was changed on the server. Download, merge and try again.");
            }
            if (ifNoneMatch) {
                bool wildcard = std::find(ifNoneMatch->begin(), ifNoneMatch->end(), "*") != ifNoneMatch->end();
                if (wildcard ? meta.has_value() : etagMatches(*ifNoneMatch, meta)) {
                    throw HttpError(k412PreconditionFailed, "The database already exists on the server.");
                }
            }
            if (meta && !ifMatch && !ifNoneMatch) {
                throw HttpError(k428PreconditionRequired,
                                "Updating an existing database requires an If-Match header.");
            }
        }

        void setMetaHeaders(const HttpResponsePtr& resp, const DatabaseMeta& meta)
        {
            resp->addHeader("ETag", meta.etag);
            resp->addHeader("Last-Modified", httpDateFromIso(meta.updatedAt));
            resp->addHeader("X-Database-Revision", std::to_string(meta.revision));
        }

        /**
         * Server-sent event streams (GET /api/v1/events), per user. After every change to one of
         * a user's databases each of their open streams gets
         *   event: database
         *   data: {"id": "...", "etag": "...", "revision": N}          (or "deleted": true)
         * so devices can sync right away instead of waiting for their next poll.
         *
         * Streams opened with an account access token (SPEC.md §11) instead get
         *   event: vault     for changes to the vaults in the token's vlt claim, with the same data
         *   event: access    {"vault": uuid|null, "change": ...} from the auth server (POST /internal/notify)
         * and are closed once the token's exp has passed, so the client reconnects with a fresh token.
         */
        class EventHub
        {
        public:
            void subscribe(const std::string& user, ResponseStreamPtr stream)
            {
                std::shared_ptr<ResponseStream> shared(std::move(stream));
                std::lock_guard lock(m_mutex);
                // Tell the client how long to wait before reconnecting, and that it is connected
                if (shared->send("retry: 5000\n: connected\n\n")) {
                    m_streams[user].push_back(std::move(shared));
                }
            }

            void subscribeAccount(const Identity& identity, ResponseStreamPtr stream)
            {
                AccountStream account;
                account.subject = identity.subject;
                for (const auto& [vault, role] : identity.vaults) {
                    account.vaults.insert(vault);
                }
                account.expiresAt = identity.expiresAt;
                account.stream = std::shared_ptr<ResponseStream>(std::move(stream));
                std::lock_guard lock(m_mutex);
                if (account.stream->send("retry: 5000\n: connected\n\n")) {
                    m_accountStreams.push_back(std::move(account));
                }
            }

            void publish(const std::string& user, const Json::Value& data)
            {
                sendTo(user, "event: database\ndata: " + compact(data) + "\n\n");
            }

            // A change to a vault, for the account streams whose token lists it
            void publishVault(const std::string& vault, const Json::Value& data)
            {
                const auto message = "event: vault\ndata: " + compact(data) + "\n\n";
                std::lock_guard lock(m_mutex);
                dropFailedAccounts([&](const AccountStream& account) -> const std::string* {
                    return account.vaults.count(vault) ? &message : nullptr;
                });
            }

            // An access change from the auth server, for every account stream of these subjects
            void publishAccess(const std::set<std::string>& subjects, const Json::Value& data)
            {
                const auto message = "event: access\ndata: " + compact(data) + "\n\n";
                std::lock_guard lock(m_mutex);
                dropFailedAccounts([&](const AccountStream& account) -> const std::string* {
                    return subjects.count(account.subject) ? &message : nullptr;
                });
            }

            // Ends the account streams whose token has expired by `now` (seconds since the epoch)
            void expire(int64_t now)
            {
                std::lock_guard lock(m_mutex);
                m_accountStreams.erase(std::remove_if(m_accountStreams.begin(),
                                                      m_accountStreams.end(),
                                                      [now](const AccountStream& account) {
                                                          if (now < account.expiresAt) {
                                                              return false;
                                                          }
                                                          account.stream->close();
                                                          return true;
                                                      }),
                                       m_accountStreams.end());
            }

            // Comments keep idle connections (and proxies) open and find streams whose client went away
            void ping()
            {
                const std::string message = ": ping\n\n";
                std::lock_guard lock(m_mutex);
                for (auto& [user, streams] : m_streams) {
                    dropFailed(streams, message);
                }
                dropFailedAccounts([&](const AccountStream&) { return &message; });
            }

        private:
            struct AccountStream
            {
                std::string subject;
                std::set<std::string> vaults;
                int64_t expiresAt = 0;
                std::shared_ptr<ResponseStream> stream;
            };

            static std::string compact(const Json::Value& data)
            {
                Json::StreamWriterBuilder builder;
                builder["indentation"] = "";
                return Json::writeString(builder, data);
            }

            void sendTo(const std::string& user, const std::string& message)
            {
                std::lock_guard lock(m_mutex);
                auto it = m_streams.find(user);
                if (it != m_streams.end()) {
                    dropFailed(it->second, message);
                }
            }

            static void dropFailed(std::vector<std::shared_ptr<ResponseStream>>& streams, const std::string& message)
            {
                streams.erase(std::remove_if(streams.begin(),
                                             streams.end(),
                                             [&message](const auto& stream) {
                                                 if (stream->send(message)) {
                                                     return false;
                                                 }
                                                 stream->close();
                                                 return true;
                                             }),
                              streams.end());
            }

            // Sends each account stream the message `pick` returns for it (none for nullptr), dropping failed ones
            template <typename Pick> void dropFailedAccounts(Pick&& pick)
            {
                m_accountStreams.erase(std::remove_if(m_accountStreams.begin(),
                                                      m_accountStreams.end(),
                                                      [&pick](const AccountStream& account) {
                                                          const std::string* message = pick(account);
                                                          if (!message || account.stream->send(*message)) {
                                                              return false;
                                                          }
                                                          account.stream->close();
                                                          return true;
                                                      }),
                                       m_accountStreams.end());
            }

            std::mutex m_mutex;
            std::map<std::string, std::vector<std::shared_ptr<ResponseStream>>> m_streams;
            std::vector<AccountStream> m_accountStreams;
        };

        class SyncApi
        {
        public:
            SyncApi(Storage storage, const Config& config, const SyncApiOptions& options)
                : m_storage(std::move(storage))
                , m_users(*m_storage.users)
                , m_store(*m_storage.databases)
                , m_vaults(*m_storage.vaults)
                , m_events(std::make_shared<EventHub>())
                , m_acceptsDeviceTokens(config.acceptsDeviceTokens())
                , m_internalToken(config.internalToken)
                , m_clock(options.clock ? options.clock : JwtVerifier::systemClock())
            {
                m_store.setChangeListener([events = m_events](const std::string& user, const Json::Value& event) {
                    events->publish(user, event);
                });
                // Nothing of the account side runs unless SYNC_AUTH_MODE asks for it
                if (config.acceptsJwt()) {
                    auto fetcher =
                        options.jwksFetcher ? options.jwksFetcher : JwtVerifier::httpFetcher(config.authJwksUrl);
                    m_jwt = std::make_unique<JwtVerifier>(config.authIssuer, config.authAudience, fetcher, m_clock);
                    m_vaults.setBroadcastListener([events = m_events](const Json::Value& message) {
                        std::set<std::string> subjects;
                        for (const auto& subject : message["subjects"]) {
                            subjects.insert(subject.asString());
                        }
                        events->publishAccess(subjects, message["data"]);
                    });
                    m_vaults.setChangeListener([events = m_events](const std::string&, const Json::Value& event) {
                        events->publishVault(event["id"].asString(), event);
                    });
                }
            }

            std::shared_ptr<EventHub> events() const
            {
                return m_events;
            }

            int64_t now() const
            {
                return m_clock();
            }

            // The user of a device token, for the legacy routes
            std::string authenticate(const HttpRequestPtr& req)
            {
                auto identity = identify(req);
                if (identity.isAccount()) {
                    throw HttpError(k403Forbidden, "This route needs a device token; account tokens use /vaults.");
                }
                return identity.user;
            }

            // The subject and vaults of an account access token, for the vault routes
            Identity authenticateAccount(const HttpRequestPtr& req)
            {
                auto identity = identify(req);
                if (!identity.isAccount()) {
                    throw HttpError(k403Forbidden, "This route needs an account access token.");
                }
                return identity;
            }

            static std::optional<std::string> bearerToken(const HttpRequestPtr& req)
            {
                // Authorization: Bearer <token>
                const auto& header = req->getHeader("Authorization");
                auto space = header.find_first_of(" \t");
                if (space == std::string::npos) {
                    return std::nullopt;
                }
                auto scheme = header.substr(0, space);
                std::transform(
                    scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
                auto token = trim(std::string_view(header).substr(space));
                if (scheme != "bearer" || token.empty() || token.find_first_of(" \t") != std::string::npos) {
                    return std::nullopt;
                }
                return token;
            }

            Identity identify(const HttpRequestPtr& req)
            {
                std::optional<Identity> identity;
                if (auto token = bearerToken(req)) {
                    // A JWT has exactly two "."; device tokens are base64url and have none
                    if (m_jwt && std::count(token->begin(), token->end(), '.') == 2) {
                        if (auto claims = m_jwt->verify(*token)) {
                            identity.emplace();
                            identity->subject = std::move(claims->subject);
                            identity->vaults = std::move(claims->vaults);
                            identity->quotas = std::move(claims->quotas);
                            identity->expiresAt = claims->expiresAt;
                        }
                    } else if (m_acceptsDeviceTokens) {
                        identity = m_users.authenticate(*token);
                    }
                }
                if (!identity) {
                    throw HttpError(k401Unauthorized, "Missing or invalid access token.");
                }
                return *identity;
            }

            // POST /api/v1/devices/pairing: a code another device of the same user can claim
            HttpResponsePtr createPairing(const HttpRequestPtr& req)
            {
                auto identity = identify(req);
                auto pairing = m_users.createPairingCode(identity.user, identity.deviceId);
                Json::Value body;
                body["code"] = pairing.code;
                body["expiresAt"] = pairing.expiresAt;
                return jsonResponse(k201Created, body);
            }

            // POST /api/v1/devices/pairing/claim {"code": "...", "deviceName": "..."}: no token
            // needed, the code is the credential; answers with the new device's token
            HttpResponsePtr claimPairing(const HttpRequestPtr& req)
            {
                const auto client = req->peerAddr().toIp();
                if (auto wait = m_pairingFailures.retryAfter(client)) {
                    auto resp = errorResponse(k429TooManyRequests, "Too many wrong pairing codes. Try again later.");
                    resp->addHeader("Retry-After", std::to_string(wait));
                    return resp;
                }

                auto json = req->getJsonObject();
                if (!json || !(*json)["code"].isString() || !(*json)["deviceName"].isString()) {
                    throw HttpError(k400BadRequest, "Expected {\"code\": \"...\", \"deviceName\": \"...\"}.");
                }
                std::optional<PairedDevice> device;
                try {
                    device =
                        m_users.claimPairingCode((*json)["code"].asString(), trim((*json)["deviceName"].asString()));
                } catch (const std::invalid_argument& e) {
                    throw HttpError(k400BadRequest, std::string(e.what()) + ".");
                }
                if (!device) {
                    m_pairingFailures.recordFailure(client);
                    throw HttpError(k400BadRequest, "Unknown or expired pairing code.");
                }
                Json::Value body;
                body["user"] = device->user;
                body["deviceId"] = device->deviceId;
                body["token"] = device->token;
                return jsonResponse(k201Created, body);
            }

            static void validateId(const std::string& id)
            {
                if (!isValidName(id)) {
                    throw HttpError(
                        k400BadRequest,
                        "Invalid database id (allowed: letters, digits, \".\", \"_\", \"-\"; max 64 chars).");
                }
            }

            HttpResponsePtr list(const HttpRequestPtr& req)
            {
                auto user = authenticate(req);
                Json::Value body;
                body["databases"] = Json::arrayValue;
                for (const auto& meta : m_store.list(user)) {
                    body["databases"].append(meta.toJson());
                }
                return jsonResponse(k200OK, body);
            }

            HttpResponsePtr download(const HttpRequestPtr& req,
                                     DatabaseRepository& store,
                                     const std::string& user,
                                     const std::string& id,
                                     const std::string& noun = "Database")
            {
                auto meta = store.meta(user, id);
                if (!meta) {
                    throw HttpError(k404NotFound, noun + " not found.");
                }

                auto ifNoneMatch = parseEtagList(req->getHeader("If-None-Match"));
                if (ifNoneMatch && etagMatches(*ifNoneMatch, meta)) {
                    auto resp = HttpResponse::newHttpResponse(k304NotModified, CT_NONE);
                    setMetaHeaders(resp, *meta);
                    return resp;
                }

                // drogon routes HEAD to this handler and drops the body while keeping its Content-Length
                auto entry = store.read(user, id);
                if (!entry) {
                    throw HttpError(k404NotFound, noun + " not found.");
                }
                auto resp = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_OCTET_STREAM);
                resp->setBody(std::move(entry->data));
                setMetaHeaders(resp, entry->meta);
                return resp;
            }

            HttpResponsePtr
            upload(const HttpRequestPtr& req, DatabaseRepository& store, const std::string& user, const std::string& id)
            {
                // Bodies over SYNC_MAX_DB_BYTES never get here, drogon answers them with a bare 413
                auto data = req->body();
                if (data.size() < KDBX_SIGNATURE.size()) {
                    throw HttpError(k400BadRequest, "Request body must be a KeePass (.kdbx) database.");
                }
                if (data.substr(0, KDBX_SIGNATURE.size()) != KDBX_SIGNATURE) {
                    throw HttpError(k415UnsupportedMediaType, "Request body is not a KDBX database.");
                }

                auto result =
                    store.write(user, id, data, [&](const auto& current) { checkWritePreconditions(req, current); });
                auto resp = jsonResponse(result.created ? k201Created : k200OK, result.meta.toJson());
                setMetaHeaders(resp, result.meta);
                return resp;
            }

            HttpResponsePtr remove(const HttpRequestPtr& req,
                                   DatabaseRepository& store,
                                   const std::string& user,
                                   const std::string& id,
                                   const std::string& noun = "Database")
            {
                auto removed = store.remove(user, id, [&](const auto& current) {
                    auto ifMatch = parseEtagList(req->getHeader("If-Match"));
                    if (current && ifMatch && !etagMatches(*ifMatch, current)) {
                        throw HttpError(k412PreconditionFailed, "The database was changed on the server.");
                    }
                });
                if (!removed) {
                    throw HttpError(k404NotFound, noun + " not found.");
                }
                return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
            }

            // GET <id>/history lists the kept versions, GET <id>/history/<revision> returns one
            HttpResponsePtr history(const HttpRequestPtr& req, const std::string& user, const std::string& path)
            {
                auto slash = path.find('/');
                auto id = path.substr(0, slash);
                validateId(id);
                return history(req, m_store, user, id, path.substr(slash + 1));
            }

            HttpResponsePtr history(const HttpRequestPtr& req,
                                    DatabaseRepository& store,
                                    const std::string& user,
                                    const std::string& id,
                                    const std::string& rest,
                                    const std::string& noun = "Database")
            {
                if (req->method() != Get) {
                    throw HttpError(k405MethodNotAllowed, "Method not allowed.");
                }

                if (rest == "history") {
                    auto versions = store.history(user, id);
                    if (!versions) {
                        throw HttpError(k404NotFound, noun + " not found.");
                    }
                    Json::Value body;
                    body["versions"] = Json::arrayValue;
                    for (const auto& meta : *versions) {
                        body["versions"].append(meta.toJson());
                    }
                    return jsonResponse(k200OK, body);
                }

                const std::string prefix = "history/";
                auto revisionText = rest.substr(prefix.size());
                if (rest.rfind(prefix, 0) != 0 || revisionText.empty() || revisionText.size() > 18
                    || !std::all_of(revisionText.begin(), revisionText.end(), [](unsigned char c) {
                           return std::isdigit(c);
                       })) {
                    throw HttpError(k404NotFound, "Not found.");
                }
                auto entry = store.readRevision(user, id, std::stoll(revisionText));
                if (!entry) {
                    throw HttpError(k404NotFound, "Version not found.");
                }
                auto resp = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_OCTET_STREAM);
                resp->setBody(std::move(entry->data));
                setMetaHeaders(resp, entry->meta);
                return resp;
            }

            HttpResponsePtr database(const HttpRequestPtr& req, const std::string& id)
            {
                auto user = authenticate(req);
                if (id.find('/') != std::string::npos) {
                    return history(req, user, id);
                }
                validateId(id);
                switch (req->method()) {
                case Get:
                    return download(req, m_store, user, id);
                case Put:
                    return upload(req, m_store, user, id);
                case Delete:
                    return remove(req, m_store, user, id);
                default:
                    throw HttpError(k405MethodNotAllowed, "Method not allowed.");
                }
            }

            // GET /api/v1/vaults: the vaults of the token that exist, with any role
            HttpResponsePtr listVaults(const HttpRequestPtr& req)
            {
                auto identity = authenticateAccount(req);
                Json::Value body;
                body["vaults"] = Json::arrayValue;
                for (const auto& [id, role] : identity.vaults) {
                    if (auto meta = m_vaults.meta(VaultScope, id)) {
                        body["vaults"].append(meta->toJson());
                    }
                }
                return jsonResponse(k200OK, body);
            }

            static void requireRole(const Identity& identity, const std::string& vault, VaultRole needed)
            {
                auto it = identity.vaults.find(vault);
                if (it == identity.vaults.end()) {
                    throw HttpError(k403Forbidden, "The access token doesn't grant this vault.");
                }
                if (it->second < needed) {
                    throw HttpError(k403Forbidden,
                                    needed == VaultRole::Manage
                                        ? "Creating or deleting this vault needs the manage role."
                                        : "Changing this vault needs the write role.");
                }
            }

            // The plan's limit on the vault's size, from the token (SPEC.md §19)
            static void requireQuota(const Identity& identity, const std::string& vault, size_t bytes)
            {
                auto it = identity.quotas.find(vault);
                if (it == identity.quotas.end() || bytes <= static_cast<uint64_t>(it->second)) {
                    return;
                }
                Json::Value extra;
                extra["max"] = static_cast<Json::Int64>(it->second);
                throw HttpError(k413RequestEntityTooLarge,
                                "The vault is larger than its plan allows (" + std::to_string(it->second) + " bytes).",
                                "quota_exceeded",
                                extra);
            }

            /**
             * /api/v1/vaults/{uuid}[/history[/{revision}]] (sync-openapi.yaml). Roles from the token's vlt claim:
             * GET and HEAD need read; PUT with If-Match needs write; PUT with If-None-Match (create) and DELETE
             * need manage. Unlike a legacy database, a PUT without either precondition is always refused (428).
             */
            HttpResponsePtr vault(const HttpRequestPtr& req, const std::string& path)
            {
                auto identity = authenticateAccount(req);
                auto slash = path.find('/');
                auto id = path.substr(0, slash);
                if (!isUuid(id)) {
                    throw HttpError(k400BadRequest, "Invalid vault id (expected a lowercase UUID).");
                }
                if (slash != std::string::npos) {
                    requireRole(identity, id, VaultRole::Read);
                    return history(req, m_vaults, VaultScope, id, path.substr(slash + 1), "Vault");
                }
                switch (req->method()) {
                case Get:
                case Head:
                    requireRole(identity, id, VaultRole::Read);
                    return download(req, m_vaults, VaultScope, id, "Vault");
                case Put: {
                    const bool creating = !req->getHeader("If-None-Match").empty();
                    requireRole(identity, id, creating ? VaultRole::Manage : VaultRole::Write);
                    if (!creating && req->getHeader("If-Match").empty()) {
                        throw HttpError(k428PreconditionRequired,
                                        "Uploading a vault requires If-Match (update) or If-None-Match: * (create).");
                    }
                    requireQuota(identity, id, req->body().size());
                    return upload(req, m_vaults, VaultScope, id);
                }
                case Delete:
                    requireRole(identity, id, VaultRole::Manage);
                    return remove(req, m_vaults, VaultScope, id, "Vault");
                default:
                    throw HttpError(k405MethodNotAllowed, "Method not allowed.");
                }
            }

            /**
             * POST /api/v1/internal/notify from the auth server (SPEC.md §11), authenticated with
             * SYNC_INTERNAL_TOKEN: {"subjects": [...], "event": "access", "data": {"vault", "change"}}.
             * The event goes to every open account stream of those subjects, on every instance.
             */
            HttpResponsePtr notify(const HttpRequestPtr& req)
            {
                authenticateInternal(req);
                auto json = req->getJsonObject();
                if (!json || !json->isObject() || !(*json)["subjects"].isArray() || (*json)["event"] != "access"
                    || !(*json)["data"].isObject()) {
                    throw HttpError(k400BadRequest,
                                    "Expected {\"subjects\": [...], \"event\": \"access\", \"data\": {...}}.");
                }
                const auto& data = (*json)["data"];
                static const std::set<std::string> changes{"granted", "revoked", "rotating", "rotated", "org"};
                if (!data["change"].isString() || !changes.count(data["change"].asString())
                    || !(data["vault"].isNull() || (data["vault"].isString() && isUuid(data["vault"].asString())))) {
                    throw HttpError(k400BadRequest,
                                    "data needs \"change\" (granted, revoked, rotating, rotated or org) and "
                                    "\"vault\" (a vault id or null).");
                }
                std::vector<std::string> subjects;
                for (const auto& subject : (*json)["subjects"]) {
                    if (!subject.isString()) {
                        throw HttpError(k400BadRequest, "subjects must be principals such as \"user:<uuid>\".");
                    }
                    subjects.push_back(subject.asString());
                }
                Json::Value event;
                event["vault"] = data["vault"];
                event["change"] = data["change"];
                // In batches, so every message fits a Postgres notification
                constexpr std::size_t batchSize = 50;
                for (std::size_t start = 0; start < subjects.size(); start += batchSize) {
                    Json::Value message;
                    message["subjects"] = Json::arrayValue;
                    for (std::size_t i = start; i < std::min(subjects.size(), start + batchSize); ++i) {
                        message["subjects"].append(subjects[i]);
                    }
                    message["data"] = event;
                    m_vaults.broadcast(message);
                }
                return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
            }

            /**
             * DELETE /api/v1/internal/vaults/{uuid} from the auth server (SPEC.md §18), authenticated with
             * SYNC_INTERNAL_TOKEN: deletes a vault blob and its history when no client does it, e.g. an expired
             * recovery archive. Like a JWT DELETE /vaults/{uuid}, the vault's streams get "deleted": true.
             */
            HttpResponsePtr deleteVault(const HttpRequestPtr& req, const std::string& id)
            {
                authenticateInternal(req);
                if (!isUuid(id)) {
                    throw HttpError(k400BadRequest, "Invalid vault id (expected a lowercase UUID).");
                }
                return remove(req, m_vaults, VaultScope, id, "Vault");
            }

        private:
            // The internal routes take only the auth server's SYNC_INTERNAL_TOKEN, never an account or device token
            void authenticateInternal(const HttpRequestPtr& req) const
            {
                auto token = bearerToken(req);
                if (!token || !constantTimeEquals(sha256Hex(*token), sha256Hex(m_internalToken))) {
                    throw HttpError(k401Unauthorized, "Missing or invalid internal token.");
                }
            }


            // Codes have 40 bits; ten wrong guesses per address and a hundred in all per ten
            // minutes leave no time to find one before it expires
            FailureLimiter m_pairingFailures{10, 100, std::chrono::minutes(10)};
            Storage m_storage;
            UserRepository& m_users;
            DatabaseRepository& m_store;
            DatabaseRepository& m_vaults;
            std::shared_ptr<EventHub> m_events;
            bool m_acceptsDeviceTokens;
            std::string m_internalToken;
            std::function<int64_t()> m_clock;
            // Only when SYNC_AUTH_MODE is jwt or both
            std::unique_ptr<JwtVerifier> m_jwt;
        };

        std::string errorMessageFor(HttpStatusCode status)
        {
            switch (status) {
            case k404NotFound:
                return "Not found.";
            case k405MethodNotAllowed:
                return "Method not allowed.";
            case k400BadRequest:
                return "Bad request.";
            default:
                return status >= 500 ? "Internal server error." : "Request failed.";
            }
        }

        bool originAllowed(const std::vector<std::string>& allowed, const std::string& origin)
        {
            return !origin.empty()
                   && std::any_of(allowed.begin(), allowed.end(), [&origin](const std::string& candidate) {
                          return candidate == "*" || candidate == origin;
                      });
        }

        /**
         * CORS for web apps on SYNC_ALLOWED_ORIGINS (e.g. the hosted web app reaching a self-hosted server).
         * Tokens travel in the Authorization header, never in cookies, so no credentials mode. Nothing changes
         * when the list is empty.
         */
        void registerCors(const std::vector<std::string>& allowed)
        {
            if (allowed.empty()) {
                return;
            }
            auto& app = drogon::app();
            // Preflight requests are answered before routing, for every route
            app.registerPreRoutingAdvice(
                [allowed](const HttpRequestPtr& req, AdviceCallback&& callback, AdviceChainCallback&& chain) {
                    const auto& origin = req->getHeader("Origin");
                    if (req->method() != Options || req->getHeader("Access-Control-Request-Method").empty()
                        || !originAllowed(allowed, origin)) {
                        chain();
                        return;
                    }
                    auto resp = HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
                    resp->addHeader("Access-Control-Allow-Origin", origin);
                    resp->addHeader("Access-Control-Allow-Methods", "GET, HEAD, PUT, POST, DELETE");
                    resp->addHeader("Access-Control-Allow-Headers",
                                    "Authorization, Content-Type, If-Match, If-None-Match");
                    resp->addHeader("Access-Control-Max-Age", "600");
                    // Chrome's Private Network Access: a public web app reaching a server on localhost or the LAN
                    if (req->getHeader("Access-Control-Request-Private-Network") == "true") {
                        resp->addHeader("Access-Control-Allow-Private-Network", "true");
                    }
                    resp->addHeader("Vary", "Origin, Access-Control-Request-Method, Access-Control-Request-Headers");
                    callback(resp);
                });
            app.registerPostHandlingAdvice([allowed](const HttpRequestPtr& req, const HttpResponsePtr& resp) {
                const auto& origin = req->getHeader("Origin");
                resp->addHeader("Vary", "Origin");
                if (originAllowed(allowed, origin)) {
                    resp->addHeader("Access-Control-Allow-Origin", origin);
                    resp->addHeader("Access-Control-Expose-Headers", "ETag, X-Database-Revision, Last-Modified");
                }
            });
        }
    } // namespace

    void registerSyncApi(const Config& config, Storage storage, SyncApiOptions options)
    {
        auto api = std::make_shared<SyncApi>(std::move(storage), config, options);
        auto& app = drogon::app();

        app.enableServerHeader(false)
            // Keep uploads in memory: they are hashed and written in one go
            .setClientMaxBodySize(config.maxDatabaseBytes)
            .setClientMaxMemoryBodySize(config.maxDatabaseBytes)
            .registerPostHandlingAdvice([](const HttpRequestPtr&, const HttpResponsePtr& resp) {
                resp->addHeader("Cache-Control", "no-store");
            })
            .setCustomErrorHandler(
                [](HttpStatusCode status, const HttpRequestPtr&) { return errorResponse(status, errorMessageFor(status)); })
            .setCustom404Page(errorResponse(k404NotFound, "Not found."))
            .setExceptionHandler([](const std::exception& e, const HttpRequestPtr&, Callback&& callback) {
                LOG_ERROR << e.what();
                callback(errorResponse(k500InternalServerError, "Internal server error."));
            });

        registerCors(config.allowedOrigins);

        app.registerHandler(
            "/api/v1/health",
            [](const HttpRequestPtr&, Callback&& callback) {
                Json::Value body;
                body["status"] = "ok";
                callback(jsonResponse(k200OK, body));
            },
            {Get});

        // Device tokens: the self-hosted setup (SYNC_AUTH_MODE standalone or both)
        if (config.acceptsDeviceTokens()) {
            app.registerHandler("/api/v1/databases",
                                [api](const HttpRequestPtr& req, Callback&& callback) {
                                    respond(callback, [&] { return api->list(req); });
                                },
                                {Get});

            app.registerHandler("/api/v1/devices/pairing",
                                [api](const HttpRequestPtr& req, Callback&& callback) {
                                    respond(callback, [&] { return api->createPairing(req); });
                                },
                                {Post});

            app.registerHandler("/api/v1/devices/pairing/claim",
                                [api](const HttpRequestPtr& req, Callback&& callback) {
                                    respond(callback, [&] { return api->claimPairing(req); });
                                },
                                {Post});

            // A regex route so ids that are not path-safe (e.g. "..%2Fusers.json") reach validateId() and get a 400
            app.registerHandlerViaRegex("/api/v1/databases/(.+)",
                                        [api](const HttpRequestPtr& req, Callback&& callback, const std::string& id) {
                                            respond(callback, [&] { return api->database(req, id); });
                                        },
                                        {Get, Put, Delete});
        }

        // Account access tokens (SYNC_AUTH_MODE jwt or both); in standalone mode these routes don't exist (404)
        if (config.acceptsJwt()) {
            app.registerHandler("/api/v1/vaults",
                                [api](const HttpRequestPtr& req, Callback&& callback) {
                                    respond(callback, [&] { return api->listVaults(req); }, true);
                                },
                                {Get});

            app.registerHandlerViaRegex("/api/v1/vaults/(.+)",
                                        [api](const HttpRequestPtr& req, Callback&& callback, const std::string& path) {
                                            respond(callback, [&] { return api->vault(req, path); }, true);
                                        },
                                        {Get, Put, Delete});

            if (!config.internalToken.empty()) {
                app.registerHandler("/api/v1/internal/notify",
                                    [api](const HttpRequestPtr& req, Callback&& callback) {
                                        respond(callback, [&] { return api->notify(req); }, true);
                                    },
                                    {Post});

                app.registerHandlerViaRegex(
                    "/api/v1/internal/vaults/(.+)",
                    [api](const HttpRequestPtr& req, Callback&& callback, const std::string& id) {
                        respond(callback, [&] { return api->deleteVault(req, id); }, true);
                    },
                    {Delete});
            }

            // Account streams end when their token expires
            app.getLoop()->runEvery(1.0, [api] { api->events()->expire(api->now()); });
        }

        app.registerHandler("/api/v1/events",
                            [api, withCode = config.acceptsJwt()](const HttpRequestPtr& req, Callback&& callback) {
                                Identity identity;
                                try {
                                    identity = api->identify(req);
                                } catch (const HttpError& e) {
                                    callback(errorResponse(e.status, e.what(), withCode, e.code, e.extra));
                                    return;
                                }
                                auto events = api->events();
                                auto resp = HttpResponse::newAsyncStreamResponse(
                                    [events, identity](ResponseStreamPtr stream) {
                                        if (identity.isAccount()) {
                                            events->subscribeAccount(identity, std::move(stream));
                                        } else {
                                            events->subscribe(identity.user, std::move(stream));
                                        }
                                    },
                                    true);
                                resp->setContentTypeString("text/event-stream");
                                // Reverse proxies such as nginx would otherwise hold events back
                                resp->addHeader("X-Accel-Buffering", "no");
                                callback(resp);
                            },
                            {Get});
        app.getLoop()->runEvery(25.0, [events = api->events()] { events->ping(); });
    }
} // namespace syncserver
