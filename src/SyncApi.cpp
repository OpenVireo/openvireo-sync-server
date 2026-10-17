#include "SyncApi.h"

#include "Util.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
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
            HttpError(HttpStatusCode status, const std::string& message)
                : std::runtime_error(message)
                , status(status)
            {
            }
            HttpStatusCode status;
        };

        HttpResponsePtr jsonResponse(HttpStatusCode status, const Json::Value& body)
        {
            auto resp = HttpResponse::newHttpJsonResponse(body);
            resp->setStatusCode(status);
            return resp;
        }

        HttpResponsePtr errorResponse(HttpStatusCode status, const std::string& message)
        {
            Json::Value body;
            body["error"] = message;
            auto resp = jsonResponse(status, body);
            if (status == k401Unauthorized) {
                resp->addHeader("WWW-Authenticate", "Bearer realm=\"keepassxc-sync\"");
            }
            resp->addHeader("Cache-Control", "no-store");
            return resp;
        }

        // Runs a handler body, turning HttpError and unexpected exceptions into JSON error responses
        template <typename Fn> void respond(const Callback& callback, Fn&& fn)
        {
            HttpResponsePtr resp;
            try {
                resp = fn();
            } catch (const HttpError& e) {
                resp = errorResponse(e.status, e.what());
            } catch (const std::exception& e) {
                LOG_ERROR << e.what();
                resp = errorResponse(k500InternalServerError, "Internal server error.");
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

            void publish(const std::string& user, const Json::Value& data)
            {
                Json::StreamWriterBuilder builder;
                builder["indentation"] = "";
                sendTo(user, "event: database\ndata: " + Json::writeString(builder, data) + "\n\n");
            }

            // Comments keep idle connections (and proxies) open and find streams whose client went away
            void ping()
            {
                std::lock_guard lock(m_mutex);
                for (auto& [user, streams] : m_streams) {
                    dropFailed(streams, ": ping\n\n");
                }
            }

        private:
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

            std::mutex m_mutex;
            std::map<std::string, std::vector<std::shared_ptr<ResponseStream>>> m_streams;
        };

        class SyncApi
        {
        public:
            explicit SyncApi(Storage storage)
                : m_storage(std::move(storage))
                , m_users(*m_storage.users)
                , m_store(*m_storage.databases)
                , m_events(std::make_shared<EventHub>())
            {
                m_store.setChangeListener([events = m_events](const std::string& user, const Json::Value& event) {
                    events->publish(user, event);
                });
            }

            std::shared_ptr<EventHub> events() const
            {
                return m_events;
            }

            std::string authenticate(const HttpRequestPtr& req)
            {
                return identify(req).user;
            }

            Identity identify(const HttpRequestPtr& req)
            {
                // Authorization: Bearer <token>
                const auto& header = req->getHeader("Authorization");
                std::optional<Identity> identity;
                auto space = header.find_first_of(" \t");
                if (space != std::string::npos) {
                    auto scheme = header.substr(0, space);
                    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) {
                        return std::tolower(c);
                    });
                    auto token = trim(std::string_view(header).substr(space));
                    if (scheme == "bearer" && token.find_first_of(" \t") == std::string::npos) {
                        identity = m_users.authenticate(token);
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

            HttpResponsePtr download(const HttpRequestPtr& req, const std::string& user, const std::string& id)
            {
                auto meta = m_store.meta(user, id);
                if (!meta) {
                    throw HttpError(k404NotFound, "Database not found.");
                }

                auto ifNoneMatch = parseEtagList(req->getHeader("If-None-Match"));
                if (ifNoneMatch && etagMatches(*ifNoneMatch, meta)) {
                    auto resp = HttpResponse::newHttpResponse(k304NotModified, CT_NONE);
                    setMetaHeaders(resp, *meta);
                    return resp;
                }

                // drogon routes HEAD to this handler and drops the body while keeping its Content-Length
                auto entry = m_store.read(user, id);
                if (!entry) {
                    throw HttpError(k404NotFound, "Database not found.");
                }
                auto resp = HttpResponse::newHttpResponse(k200OK, CT_APPLICATION_OCTET_STREAM);
                resp->setBody(std::move(entry->data));
                setMetaHeaders(resp, entry->meta);
                return resp;
            }

            HttpResponsePtr upload(const HttpRequestPtr& req, const std::string& user, const std::string& id)
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
                    m_store.write(user, id, data, [&](const auto& current) { checkWritePreconditions(req, current); });
                auto resp = jsonResponse(result.created ? k201Created : k200OK, result.meta.toJson());
                setMetaHeaders(resp, result.meta);
                return resp;
            }

            HttpResponsePtr remove(const HttpRequestPtr& req, const std::string& user, const std::string& id)
            {
                auto removed = m_store.remove(user, id, [&](const auto& current) {
                    auto ifMatch = parseEtagList(req->getHeader("If-Match"));
                    if (current && ifMatch && !etagMatches(*ifMatch, current)) {
                        throw HttpError(k412PreconditionFailed, "The database was changed on the server.");
                    }
                });
                if (!removed) {
                    throw HttpError(k404NotFound, "Database not found.");
                }
                return HttpResponse::newHttpResponse(k204NoContent, CT_NONE);
            }

            // GET <id>/history lists the kept versions, GET <id>/history/<revision> returns one
            HttpResponsePtr history(const HttpRequestPtr& req, const std::string& user, const std::string& path)
            {
                auto slash = path.find('/');
                auto id = path.substr(0, slash);
                auto rest = path.substr(slash + 1);
                validateId(id);
                if (req->method() != Get) {
                    throw HttpError(k405MethodNotAllowed, "Method not allowed.");
                }

                if (rest == "history") {
                    auto versions = m_store.history(user, id);
                    if (!versions) {
                        throw HttpError(k404NotFound, "Database not found.");
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
                auto entry = m_store.readRevision(user, id, std::stoll(revisionText));
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
                    return download(req, user, id);
                case Put:
                    return upload(req, user, id);
                case Delete:
                    return remove(req, user, id);
                default:
                    throw HttpError(k405MethodNotAllowed, "Method not allowed.");
                }
            }

        private:
            // Codes have 40 bits; ten wrong guesses per address and a hundred in all per ten
            // minutes leave no time to find one before it expires
            FailureLimiter m_pairingFailures{10, 100, std::chrono::minutes(10)};
            Storage m_storage;
            UserRepository& m_users;
            DatabaseRepository& m_store;
            std::shared_ptr<EventHub> m_events;
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
    } // namespace

    void registerSyncApi(const Config& config, Storage storage)
    {
        auto api = std::make_shared<SyncApi>(std::move(storage));
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

        app.registerHandler(
            "/api/v1/health",
            [](const HttpRequestPtr&, Callback&& callback) {
                Json::Value body;
                body["status"] = "ok";
                callback(jsonResponse(k200OK, body));
            },
            {Get});

        app.registerHandler(
            "/api/v1/databases",
            [api](const HttpRequestPtr& req, Callback&& callback) {
                respond(callback, [&] { return api->list(req); });
            },
            {Get});

        app.registerHandler(
            "/api/v1/devices/pairing",
            [api](const HttpRequestPtr& req, Callback&& callback) {
                respond(callback, [&] { return api->createPairing(req); });
            },
            {Post});

        app.registerHandler(
            "/api/v1/devices/pairing/claim",
            [api](const HttpRequestPtr& req, Callback&& callback) {
                respond(callback, [&] { return api->claimPairing(req); });
            },
            {Post});

        app.registerHandler(
            "/api/v1/events",
            [api](const HttpRequestPtr& req, Callback&& callback) {
                std::string user;
                try {
                    user = api->authenticate(req);
                } catch (const HttpError& e) {
                    callback(errorResponse(e.status, e.what()));
                    return;
                }
                auto events = api->events();
                auto resp = HttpResponse::newAsyncStreamResponse(
                    [events, user](ResponseStreamPtr stream) { events->subscribe(user, std::move(stream)); }, true);
                resp->setContentTypeString("text/event-stream");
                // Reverse proxies such as nginx would otherwise hold events back
                resp->addHeader("X-Accel-Buffering", "no");
                callback(resp);
            },
            {Get});
        app.getLoop()->runEvery(25.0, [events = api->events()] { events->ping(); });

        // A regex route so ids that are not path-safe (e.g. "..%2Fusers.json") reach validateId() and get a 400
        app.registerHandlerViaRegex(
            "/api/v1/databases/(.+)",
            [api](const HttpRequestPtr& req, Callback&& callback, const std::string& id) {
                respond(callback, [&] { return api->database(req, id); });
            },
            {Get, Put, Delete});
    }
} // namespace syncserver
