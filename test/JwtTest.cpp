// The account side of the API: JWT access tokens, vaults, account event streams, internal/notify and CORS,
// against the shared test vectors of the accounts spec (test/vectors/test-vectors.json, a copy of
// the accounts contract's docs/plans/accounts/test-vectors.json). Built twice: SYNC_AUTH_MODE=both, and with
// SYNC_TEST_JWT_ONLY for SYNC_AUTH_MODE=jwt. Uses Postgres and S3 when SYNC_DATABASE_URL and SYNC_S3_* are set.
#define DROGON_TEST_MAIN
#include <drogon/drogon.h>
#include <drogon/drogon_test.h>

#include "Config.h"
#include "Jwt.h"
#include "Migrate.h"
#include "Storage.h"
#include "SyncApi.h"
#include "Util.h"

#ifdef SYNC_SERVER_WITH_CLOUD
#include "BlobStore.h"
#include "Postgres.h"
#endif

#include <openssl/evp.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace drogon;
namespace fs = std::filesystem;

namespace
{
    const std::string KDBX_HEADER("\x03\xd9\xa2\x9a\x67\xfb\x4b\xb5", 8);
    const std::string ISSUER = "https://accounts.openvireo.example";
    const std::string INTERNAL_TOKEN = "internal-shared-secret";
    const std::string WEB_ORIGIN = "https://app.openvireo.example";
    // The vaults of the valid vector tokens: the user has manage on the first and read on the second,
    // the service account write on the first
    const std::string VAULT_A = "3f1c2a9e-7b4d-4e8a-9c21-5d6e7f809a1b";
    const std::string VAULT_B = "6c7d8e9f-0a1b-4c2d-8e3f-4a5b6c7d8e9f";

    syncserver::Config g_config;
    syncserver::Storage g_storage;
    std::string g_baseUrl;
    std::string g_deviceToken;
    Json::Value g_vectors;
    int64_t g_vectorNow = 0;

    // The server's clock and JWKS, both under the tests' control
    std::atomic<int64_t> g_now{0};
    std::mutex g_jwksMutex;
    std::string g_jwks;
    std::atomic<int> g_jwksFetches{0};

    std::string fakeDatabase(const std::string& contents)
    {
        return KDBX_HEADER + contents;
    }

    std::string base64Url(std::string_view data)
    {
        return utils::base64EncodeUnpadded(reinterpret_cast<const unsigned char*>(data.data()), data.size(), true);
    }

    std::string compact(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, value);
    }

    std::string unhex(const std::string& hex)
    {
        std::string out;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
        }
        return out;
    }

    // Ed25519 signing as the auth server does it, for tokens with other claims than the vectors'
    std::string
    sign(const Json::Value& claims, const std::string& kid = "test-2026-10", const std::string& seedHex = {})
    {
        auto seed = unhex(seedHex.empty() ? g_vectors["jwt"]["signing_seed_hex"].asString() : seedHex);
        EVP_PKEY* key = EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(seed.data()), seed.size());
        Json::Value header;
        header["alg"] = "EdDSA";
        header["typ"] = "JWT";
        header["kid"] = kid;
        auto input = base64Url(compact(header)) + "." + base64Url(compact(claims));
        unsigned char signature[64];
        size_t length = sizeof(signature);
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
        EVP_DigestSign(ctx, signature, &length, reinterpret_cast<const unsigned char*>(input.data()), input.size());
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        return input + "." + base64Url(std::string_view(reinterpret_cast<char*>(signature), length));
    }

    // A token like the vectors' user token, valid at g_now, with these vaults ({id, role} pairs)
    std::string accountToken(const std::vector<std::pair<std::string, std::string>>& vaults,
                             const std::string& subject = "user:5a0b7c1d-2e3f-4a5b-8c6d-7e8f9a0b1c2d",
                             int64_t lifetime = 600)
    {
        Json::Value claims;
        claims["iss"] = ISSUER;
        claims["aud"].append("ov-api");
        claims["aud"].append("ov-sync");
        claims["sub"] = subject;
        claims["vlt"] = Json::arrayValue;
        for (const auto& [id, role] : vaults) {
            Json::Value grant;
            grant["id"] = id;
            grant["r"] = role;
            claims["vlt"].append(grant);
        }
        const auto now = static_cast<Json::Int64>(g_now.load());
        claims["iat"] = now;
        claims["nbf"] = now;
        claims["exp"] = now + lifetime;
        claims["jti"] = utils::getUuid();
        return sign(claims);
    }

    // A lowercase hyphenated UUID
    std::string newVaultId()
    {
        std::string hex;
        for (unsigned char c : utils::getUuid()) {
            if (std::isxdigit(c)) {
                hex += static_cast<char>(std::tolower(c));
            }
        }
        return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-"
               + hex.substr(20, 12);
    }

    std::string userToken()
    {
        return g_vectors["jwt"]["valid"][0]["token"].asString();
    }

    std::string serviceAccountToken()
    {
        return g_vectors["jwt"]["valid"][1]["token"].asString();
    }

    struct Request
    {
        std::string token;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
    };

    HttpResponsePtr request(HttpMethod method, const std::string& path, Request options = {})
    {
        auto client = HttpClient::newHttpClient(g_baseUrl);
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath("/api/v1" + path);
        if (!options.token.empty()) {
            req->addHeader("Authorization", "Bearer " + options.token);
        }
        for (const auto& [name, value] : options.headers) {
            req->addHeader(name, value);
        }
        if (!options.body.empty()) {
            req->setContentTypeCode(CT_APPLICATION_OCTET_STREAM);
            req->setBody(options.body);
        }
        auto [result, resp] = client->sendRequest(req, 10);
        if (result != ReqResult::Ok) {
            throw std::runtime_error("request failed: " + to_string(result));
        }
        return resp;
    }

    HttpResponsePtr postJson(const std::string& path, const Json::Value& body, const std::string& token = {})
    {
        auto client = HttpClient::newHttpClient(g_baseUrl);
        auto req = HttpRequest::newHttpJsonRequest(body);
        req->setMethod(Post);
        req->setPath("/api/v1" + path);
        if (!token.empty()) {
            req->addHeader("Authorization", "Bearer " + token);
        }
        auto [result, resp] = client->sendRequest(req, 10);
        if (result != ReqResult::Ok) {
            throw std::runtime_error("request failed: " + to_string(result));
        }
        return resp;
    }

    Json::Value json(const HttpResponsePtr& resp)
    {
        if (resp->contentType() != CT_APPLICATION_JSON) {
            return {};
        }
        auto body = resp->getJsonObject();
        return body ? *body : Json::Value();
    }

    std::string errorCode(const HttpResponsePtr& resp)
    {
        return json(resp)["code"].asString();
    }

    // Creates a vault with a manage token and returns its ETag
    std::string createVault(const std::string& id, const std::string& contents = "base")
    {
        auto resp = request(
            Put, "/vaults/" + id, {accountToken({{id, "manage"}}), {{"If-None-Match", "*"}}, fakeDatabase(contents)});
        if (resp->statusCode() != k201Created) {
            throw std::runtime_error("cannot create vault " + id);
        }
        return resp->getHeader("ETag");
    }

    // Sets the server's clock for one test and puts it back afterwards
    struct ClockAt
    {
        explicit ClockAt(int64_t time)
            : previous(g_now.exchange(time))
        {
        }
        ~ClockAt()
        {
            g_now = previous;
        }
        int64_t previous;
    };

    // An open GET /events, read with a plain socket because the HTTP client waits for whole responses
    class EventStream
    {
    public:
        explicit EventStream(const std::string& token)
        {
            auto port = std::stoi(g_baseUrl.substr(g_baseUrl.rfind(':') + 1));
            m_socket = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<uint16_t>(port));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                throw std::runtime_error("cannot connect");
            }
            timeval timeout{0, 200 * 1000};
            ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            auto request = "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token
                           + "\r\nAccept: text/event-stream\r\n\r\n";
            ::send(m_socket, request.data(), request.size(), 0);
        }

        ~EventStream()
        {
            ::close(m_socket);
        }

        // Reads until `text` arrives or `seconds` pass
        bool waitFor(const std::string& text, double seconds = 5)
        {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (m_received.find(text) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
                char buffer[4096];
                auto n = ::recv(m_socket, buffer, sizeof(buffer), 0);
                if (n > 0) {
                    m_received.append(buffer, static_cast<size_t>(n));
                } else if (n == 0) {
                    m_closed = true;
                    break;
                }
            }
            return m_received.find(text) != std::string::npos;
        }

        // The server ended the chunked response (or closed the connection)
        bool waitForEnd(double seconds = 5)
        {
            return waitFor("\r\n0\r\n\r\n", seconds) || m_closed;
        }

        const std::string& received() const
        {
            return m_received;
        }

    private:
        int m_socket = -1;
        bool m_closed = false;
        std::string m_received;
    };

    syncserver::JwtVerifier verifierAt(int64_t* now, int* fetches, std::string* jwks)
    {
        return syncserver::JwtVerifier(
            ISSUER,
            "ov-sync",
            [fetches, jwks]() -> std::optional<std::string> {
                ++*fetches;
                return *jwks;
            },
            [now] { return *now; });
    }
} // namespace

// ---------------------------------------------------------------- JwtVerifier on its own

DROGON_TEST(VerifierAcceptsTheValidVectors)
{
    int64_t now = g_vectorNow;
    int fetches = 0;
    auto jwks = compact(g_vectors["jwt"]["jwks"]);
    auto verifier = verifierAt(&now, &fetches, &jwks);

    auto user = verifier.verify(userToken());
    REQUIRE(user.has_value());
    CHECK(user->subject == "user:5a0b7c1d-2e3f-4a5b-8c6d-7e8f9a0b1c2d");
    CHECK(user->expiresAt == 1792224600);
    REQUIRE(user->vaults.size() == 2);
    CHECK(user->vaults.at(VAULT_A) == syncserver::VaultRole::Manage);
    CHECK(user->vaults.at(VAULT_B) == syncserver::VaultRole::Read);

    auto sa = verifier.verify(serviceAccountToken());
    REQUIRE(sa.has_value());
    CHECK(sa->subject == "sa:9b2e4f61-0c3a-4d7e-8f15-a2b3c4d5e6f7");
    REQUIRE(sa->vaults.size() == 1);
    CHECK(sa->vaults.at(VAULT_A) == syncserver::VaultRole::Write);
    // Fetched once and cached
    CHECK(fetches == 1);
}

DROGON_TEST(VerifierRejectsTheInvalidVectors)
{
    // At the vectors' now every case fails, the "expired" one included (61 s past exp, see the next test)
    int64_t now = g_vectorNow;
    int fetches = 0;
    auto jwks = compact(g_vectors["jwt"]["jwks"]);
    auto verifier = verifierAt(&now, &fetches, &jwks);
    for (const auto& vector : g_vectors["jwt"]["invalid"]) {
        CHECK(!verifier.verify(vector["token"].asString()).has_value());
    }
    // The valid ones still pass at that time
    CHECK(verifier.verify(userToken()).has_value());
}

DROGON_TEST(VerifierAllowsSixtySecondsOfClockSkew)
{
    int64_t now = g_vectorNow;
    int fetches = 0;
    auto jwks = compact(g_vectors["jwt"]["jwks"]);
    auto verifier = verifierAt(&now, &fetches, &jwks);
    const auto expired = g_vectors["jwt"]["invalid"][0];
    REQUIRE(expired["case"].asString().rfind("expired", 0) == 0);
    // exp = 1792223999 = now - 61: past the skew at the vectors' time
    CHECK(!verifier.verify(expired["token"].asString()).has_value());
    now = 1792223999 + 59;
    CHECK(verifier.verify(expired["token"].asString()).has_value());
    now = 1792223999 + 60;
    CHECK(!verifier.verify(expired["token"].asString()).has_value());

    // nbf = iat: 60 s early still works, 61 s doesn't
    now = 1792224000 - 60;
    CHECK(verifier.verify(userToken()).has_value());
    now = 1792224000 - 61;
    CHECK(!verifier.verify(userToken()).has_value());
    // exp = 1792224600: up to 59 s later
    now = 1792224600 + 59;
    CHECK(verifier.verify(userToken()).has_value());
    now = 1792224600 + 60;
    CHECK(!verifier.verify(userToken()).has_value());
}

DROGON_TEST(VerifierRefetchesTheJwksForUnknownKidsAtMostEvery30Seconds)
{
    int64_t now = g_vectorNow;
    int fetches = 0;
    auto jwks = compact(g_vectors["jwt"]["jwks"]);
    auto verifier = verifierAt(&now, &fetches, &jwks);
    std::string unknownKid;
    for (const auto& vector : g_vectors["jwt"]["invalid"]) {
        if (vector["case"].asString().rfind("unknown kid", 0) == 0) {
            unknownKid = vector["token"].asString();
        }
    }
    REQUIRE(!unknownKid.empty());

    CHECK(verifier.verify(userToken()).has_value());
    CHECK(fetches == 1);
    now += 30;
    // Refetches once, then rejects
    CHECK(!verifier.verify(unknownKid).has_value());
    CHECK(fetches == 2);
    now += 29;
    CHECK(!verifier.verify(unknownKid).has_value());
    CHECK(!verifier.verify(unknownKid).has_value());
    CHECK(fetches == 2);
    // Known keys never wait for a fetch
    CHECK(verifier.verify(userToken()).has_value());
    CHECK(fetches == 2);

    // Key rollover: a token signed with a new key works once the JWKS has it
    const std::string newSeed = "1111111111111111111111111111111111111111111111111111111111111111";
    unsigned char x[32];
    size_t length = sizeof(x);
    auto seed = unhex(newSeed);
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(seed.data()), seed.size());
    EVP_PKEY_get_raw_public_key(key, x, &length);
    EVP_PKEY_free(key);
    auto rolled = g_vectors["jwt"]["jwks"];
    Json::Value jwk = rolled["keys"][0];
    jwk["kid"] = "test-2026-11";
    jwk["x"] = base64Url(std::string_view(reinterpret_cast<char*>(x), length));
    rolled["keys"].append(jwk);
    jwks = compact(rolled);
    auto claims = g_vectors["jwt"]["valid"][0]["claims"];
    auto token = sign(claims, "test-2026-11", newSeed);
    now += 1;
    CHECK(verifier.verify(token).has_value());
    CHECK(fetches == 3);
    // The same key under the old kid is still a bad signature
    CHECK(!verifier.verify(sign(claims, "test-2026-10", newSeed)).has_value());
}

DROGON_TEST(VerifierRejectsMalformedTokens)
{
    int64_t now = g_vectorNow;
    int fetches = 0;
    auto jwks = compact(g_vectors["jwt"]["jwks"]);
    auto verifier = verifierAt(&now, &fetches, &jwks);
    auto claims = g_vectors["jwt"]["valid"][0]["claims"];
    CHECK(!verifier.verify("").has_value());
    CHECK(!verifier.verify("a.b").has_value());
    CHECK(!verifier.verify("a.b.c.d").has_value());
    CHECK(!verifier.verify(userToken() + "x").has_value());
    // The audience may also be a plain string
    auto single = claims;
    single["aud"] = "ov-sync";
    CHECK(verifier.verify(sign(single)).has_value());
    auto noExp = claims;
    noExp.removeMember("exp");
    CHECK(!verifier.verify(sign(noExp)).has_value());
    auto badSubject = claims;
    badSubject["sub"] = "admin";
    CHECK(!verifier.verify(sign(badSubject)).has_value());
    // Grants it can't read grant nothing
    auto oddGrants = claims;
    oddGrants["vlt"][0]["r"] = "owner";
    oddGrants["vlt"][1]["id"] = "not-a-uuid";
    auto parsed = verifier.verify(sign(oddGrants));
    REQUIRE(parsed.has_value());
    CHECK(parsed->vaults.empty());
}

// ---------------------------------------------------------------- the API

DROGON_TEST(EveryInvalidVectorGets401)
{
    ClockAt clock(g_vectorNow);
    for (const auto& vector : g_vectors["jwt"]["invalid"]) {
        auto resp = request(Get, "/vaults", {vector["token"].asString()});
        CHECK(resp->statusCode() == vector["expect"].asInt());
        CHECK(errorCode(resp) == "unauthorized");
        CHECK(resp->getHeader("WWW-Authenticate") == "Bearer realm=\"openvireo-sync\"");
        CHECK(request(Get, "/vaults/" + VAULT_A, {vector["token"].asString()})->statusCode() == k401Unauthorized);
    }
    CHECK(request(Get, "/vaults")->statusCode() == k401Unauthorized);
    CHECK(request(Get, "/vaults", {"not-a-token"})->statusCode() == k401Unauthorized);
}

DROGON_TEST(VaultLifecycleWithTheVectorTokens)
{
    // The service account has write on vault A: it may not create it
    auto saCreate =
        request(Put, "/vaults/" + VAULT_A, {serviceAccountToken(), {{"If-None-Match", "*"}}, fakeDatabase("sa")});
    CHECK(saCreate->statusCode() == k403Forbidden);
    CHECK(errorCode(saCreate) == "forbidden");

    // The user has manage on it
    auto created =
        request(Put, "/vaults/" + VAULT_A, {userToken(), {{"If-None-Match", "*"}}, fakeDatabase("version 1")});
    REQUIRE(created->statusCode() == k201Created);
    auto etag1 = created->getHeader("ETag");
    CHECK(created->getHeader("X-Database-Revision") == "1");
    CHECK(json(created)["id"].asString() == VAULT_A);
    auto again = request(Put, "/vaults/" + VAULT_A, {userToken(), {{"If-None-Match", "*"}}, fakeDatabase("x")});
    CHECK(again->statusCode() == k412PreconditionFailed);
    CHECK(errorCode(again) == "precondition_failed");

    // Both can read it
    for (const auto& token : {userToken(), serviceAccountToken()}) {
        auto read = request(Get, "/vaults/" + VAULT_A, {token});
        REQUIRE(read->statusCode() == k200OK);
        CHECK(read->body() == fakeDatabase("version 1"));
        CHECK(read->getHeader("ETag") == etag1);
        CHECK(!read->getHeader("Last-Modified").empty());
    }
    auto head = request(Head, "/vaults/" + VAULT_A, {userToken()});
    CHECK(head->statusCode() == k200OK);
    CHECK(head->getHeader("ETag") == etag1);
    CHECK(request(Get, "/vaults/" + VAULT_A, {userToken(), {{"If-None-Match", etag1}}})->statusCode()
          == k304NotModified);

    // The service account updates it with If-Match
    auto updated =
        request(Put, "/vaults/" + VAULT_A, {serviceAccountToken(), {{"If-Match", etag1}}, fakeDatabase("version 2")});
    REQUIRE(updated->statusCode() == k200OK);
    auto etag2 = updated->getHeader("ETag");
    CHECK(updated->getHeader("X-Database-Revision") == "2");
    // A stale ETag gets 412, no precondition 428
    auto stale =
        request(Put, "/vaults/" + VAULT_A, {serviceAccountToken(), {{"If-Match", etag1}}, fakeDatabase("lost")});
    CHECK(stale->statusCode() == k412PreconditionFailed);
    auto blind = request(Put, "/vaults/" + VAULT_A, {serviceAccountToken(), {}, fakeDatabase("blind")});
    CHECK(blind->statusCode() == k428PreconditionRequired);
    CHECK(errorCode(blind) == "precondition_required");

    // History, with read
    auto history = json(request(Get, "/vaults/" + VAULT_A + "/history", {serviceAccountToken()}));
    REQUIRE(history["versions"].size() == 2);
    CHECK(history["versions"][0]["revision"].asInt() == 2);
    CHECK(request(Get, "/vaults/" + VAULT_A + "/history/1", {userToken()})->body() == fakeDatabase("version 1"));

    // Listed for both tokens; vault B doesn't exist yet
    auto list = json(request(Get, "/vaults", {userToken()}));
    REQUIRE(list["vaults"].size() == 1);
    CHECK(list["vaults"][0]["id"].asString() == VAULT_A);
    CHECK(list["vaults"][0]["etag"].asString() == etag2);
    CHECK(list["vaults"][0]["revision"].asInt() == 2);
    CHECK(list["vaults"][0]["updatedAt"].isString());

    // Write can't delete, manage can
    CHECK(request(Delete, "/vaults/" + VAULT_A, {serviceAccountToken()})->statusCode() == k403Forbidden);
    CHECK(request(Delete, "/vaults/" + VAULT_A, {userToken()})->statusCode() == k204NoContent);
    auto gone = request(Get, "/vaults/" + VAULT_A, {userToken()});
    CHECK(gone->statusCode() == k404NotFound);
    CHECK(errorCode(gone) == "not_found");
    CHECK(json(request(Get, "/vaults", {userToken()}))["vaults"].empty());
}

DROGON_TEST(RolesLimitWhatATokenMayDo)
{
    auto id = newVaultId();
    auto etag = createVault(id);

    // A vault that isn't in the token at all
    auto stranger = request(Get, "/vaults/" + id, {userToken()});
    CHECK(stranger->statusCode() == k403Forbidden);
    CHECK(errorCode(stranger) == "forbidden");
    CHECK(request(Get, "/vaults/" + id + "/history", {userToken()})->statusCode() == k403Forbidden);
    // ... even when it doesn't exist
    CHECK(request(Get, "/vaults/" + newVaultId(), {userToken()})->statusCode() == k403Forbidden);

    // Read can't write, write can't create or delete
    auto reader = accountToken({{id, "read"}});
    CHECK(request(Get, "/vaults/" + id, {reader})->statusCode() == k200OK);
    auto readPut = request(Put, "/vaults/" + id, {reader, {{"If-Match", etag}}, fakeDatabase("no")});
    CHECK(readPut->statusCode() == k403Forbidden);
    CHECK(errorCode(readPut) == "forbidden");
    CHECK(request(Delete, "/vaults/" + id, {reader})->statusCode() == k403Forbidden);

    auto writer = accountToken({{id, "write"}});
    auto other = newVaultId();
    auto writerToo = accountToken({{id, "write"}, {other, "write"}});
    CHECK(request(Put, "/vaults/" + other, {writerToo, {{"If-None-Match", "*"}}, fakeDatabase("new")})->statusCode()
          == k403Forbidden);
    // If-Match on a vault that doesn't exist: write can't create one that way either
    CHECK(request(Put, "/vaults/" + other, {writerToo, {{"If-Match", "*"}}, fakeDatabase("new")})->statusCode()
          == k412PreconditionFailed);
    CHECK(request(Put, "/vaults/" + id, {writer, {{"If-Match", etag}}, fakeDatabase("ok")})->statusCode() == k200OK);
    CHECK(request(Delete, "/vaults/" + id, {writer})->statusCode() == k403Forbidden);

    // Manage creates with If-None-Match: * only
    auto manager = accountToken({{other, "manage"}});
    CHECK(request(Put, "/vaults/" + other, {manager, {}, fakeDatabase("new")})->statusCode()
          == k428PreconditionRequired);
    CHECK(request(Put, "/vaults/" + other, {manager, {{"If-None-Match", "*"}}, fakeDatabase("new")})->statusCode()
          == k201Created);

    // Bodies and ids are checked as for databases
    auto notKdbx = request(Put, "/vaults/" + other, {manager, {{"If-Match", "*"}}, "plain text"});
    CHECK(notKdbx->statusCode() == k415UnsupportedMediaType);
    CHECK(errorCode(notKdbx) == "invalid_request");
    auto badId = request(Get, "/vaults/NOT-A-UUID", {manager});
    CHECK(badId->statusCode() == k400BadRequest);
    CHECK(errorCode(badId) == "invalid_request");
    CHECK(request(Get, "/vaults/" + other + "/history/99", {manager})->statusCode() == k404NotFound);
}

DROGON_TEST(VaultsNeverShareStorageWithDatabases)
{
    auto id = newVaultId();
    createVault(id, "vault contents");
    // A legacy database of the same name is another object entirely
    auto legacy = g_storage.databases->meta("alice", id);
    CHECK(!legacy.has_value());
    g_storage.databases->write("alice", id, fakeDatabase("legacy contents"), [](const auto&) {});
    CHECK(request(Get, "/vaults/" + id, {accountToken({{id, "read"}})})->body() == fakeDatabase("vault contents"));
    CHECK(g_storage.databases->read("alice", id)->data == fakeDatabase("legacy contents"));
    CHECK(g_storage.vaults->read(syncserver::VaultScope, id)->data == fakeDatabase("vault contents"));
    if (!g_config.usesPostgres()) {
        CHECK(fs::exists(g_config.dataDir / "vaults" / (id + ".kdbx")));
        CHECK(fs::exists(g_config.dataDir / "databases" / "alice" / (id + ".kdbx")));
    }
    g_storage.databases->remove("alice", id, [](const auto&) {});
}

#ifndef SYNC_TEST_JWT_ONLY
DROGON_TEST(BothModeAcceptsDeviceTokensAndJwts)
{
    // A device token on the legacy routes, a JWT on the vault routes and the events of either
    auto created = request(
        Put, "/databases/both-mode", {g_deviceToken, {{"If-None-Match", "*"}}, fakeDatabase("device database")});
    CHECK(created->statusCode() == k201Created);
    CHECK(request(Get, "/databases/both-mode", {g_deviceToken})->body() == fakeDatabase("device database"));
    CHECK(request(Get, "/vaults", {userToken()})->statusCode() == k200OK);
    // Legacy error bodies stay as they were
    CHECK(!json(request(Get, "/databases/nope", {g_deviceToken})).isMember("code"));

    // ... but not the other way round
    auto deviceOnVaults = request(Get, "/vaults", {g_deviceToken});
    CHECK(deviceOnVaults->statusCode() == k403Forbidden);
    CHECK(errorCode(deviceOnVaults) == "forbidden");
    CHECK(request(Get, "/vaults/" + VAULT_A, {g_deviceToken})->statusCode() == k403Forbidden);
    CHECK(request(Get, "/databases", {userToken()})->statusCode() == k403Forbidden);
    CHECK(request(Get, "/databases/both-mode", {userToken()})->statusCode() == k403Forbidden);
    // A token that looks like a JWT is never tried as a device token
    CHECK(request(Get, "/databases", {"a.b.c"})->statusCode() == k401Unauthorized);

    EventStream device(g_deviceToken);
    EventStream account(userToken());
    REQUIRE(device.waitFor(": connected"));
    REQUIRE(account.waitFor(": connected"));
    request(Put,
            "/databases/both-mode",
            {g_deviceToken, {{"If-Match", created->getHeader("ETag")}}, fakeDatabase("device database 2")});
    CHECK(device.waitFor("event: database"));
    // Account streams never get legacy events
    CHECK(!account.waitFor("event: database", 0.5));
}
#else
DROGON_TEST(JwtModeHasNoLegacyRoutes)
{
    auto databases = request(Get, "/databases", {userToken()});
    CHECK(databases->statusCode() == k404NotFound);
    CHECK(request(Get, "/databases/x", {userToken()})->statusCode() == k404NotFound);
    CHECK(request(Post, "/devices/pairing", {userToken()})->statusCode() == k404NotFound);
    // Device tokens aren't accepted anywhere
    CHECK(request(Get, "/vaults", {g_deviceToken})->statusCode() == k401Unauthorized);
    EventStream device(g_deviceToken);
    CHECK(device.waitFor("401"));
    CHECK(request(Get, "/vaults", {userToken()})->statusCode() == k200OK);
}
#endif

DROGON_TEST(StreamsOnlyTheTokensVaults)
{
    auto mine = newVaultId();
    auto theirs = newVaultId();
    auto mineEtag = createVault(mine);
    createVault(theirs);

    EventStream stream(accountToken({{mine, "read"}}));
    REQUIRE(stream.waitFor(": connected"));
    REQUIRE(stream.received().find("text/event-stream") != std::string::npos);

    request(Put, "/vaults/" + theirs, {accountToken({{theirs, "manage"}}), {{"If-Match", "*"}}, fakeDatabase("2")});
    auto updated =
        request(Put, "/vaults/" + mine, {accountToken({{mine, "write"}}), {{"If-Match", mineEtag}}, fakeDatabase("2")});
    auto etag = updated->getHeader("ETag");
    CHECK(stream.waitFor("event: vault"));
    CHECK(stream.waitFor("\"revision\":2"));
    CHECK(stream.received().find("\"id\":\"" + mine + "\"") != std::string::npos);
    CHECK(stream.received().find(etag.substr(1, etag.size() - 2)) != std::string::npos);
    // Nothing about the other vault
    CHECK(stream.received().find(theirs) == std::string::npos);

    request(Delete, "/vaults/" + mine, {accountToken({{mine, "manage"}})});
    CHECK(stream.waitFor("\"deleted\":true"));
    CHECK(stream.received().find(theirs) == std::string::npos);

    EventStream invalid(g_vectors["jwt"]["invalid"][4]["token"].asString());
    CHECK(invalid.waitFor("401"));
}

DROGON_TEST(DeliversAccessEventsFromTheAuthServer)
{
    const std::string alice = "user:11111111-1111-4111-8111-111111111111";
    const std::string robot = "sa:22222222-2222-4222-8222-222222222222";
    EventStream aliceStream(accountToken({}, alice));
    EventStream robotStream(accountToken({}, robot));
    EventStream otherStream(accountToken({}, "user:33333333-3333-4333-8333-333333333333"));
    REQUIRE(aliceStream.waitFor(": connected"));
    REQUIRE(robotStream.waitFor(": connected"));
    REQUIRE(otherStream.waitFor(": connected"));

    Json::Value body;
    body["subjects"].append(alice);
    body["subjects"].append(robot);
    body["event"] = "access";
    body["data"]["vault"] = VAULT_B;
    body["data"]["change"] = "granted";

    // Only with the shared secret, never with an account token
    auto missing = postJson("/internal/notify", body);
    CHECK(missing->statusCode() == k401Unauthorized);
    CHECK(errorCode(missing) == "unauthorized");
    CHECK(postJson("/internal/notify", body, "wrong")->statusCode() == k401Unauthorized);
    CHECK(postJson("/internal/notify", body, userToken())->statusCode() == k401Unauthorized);

    auto badBody = body;
    badBody["data"]["change"] = "promoted";
    auto bad = postJson("/internal/notify", badBody, INTERNAL_TOKEN);
    CHECK(bad->statusCode() == k400BadRequest);
    CHECK(errorCode(bad) == "invalid_request");
    badBody = body;
    badBody["event"] = "vault";
    CHECK(postJson("/internal/notify", badBody, INTERNAL_TOKEN)->statusCode() == k400BadRequest);
    CHECK(!aliceStream.waitFor("event: access", 0.3));

    CHECK(postJson("/internal/notify", body, INTERNAL_TOKEN)->statusCode() == k204NoContent);
    CHECK(aliceStream.waitFor("event: access"));
    CHECK(aliceStream.waitFor("\"change\":\"granted\""));
    CHECK(aliceStream.received().find("\"vault\":\"" + VAULT_B + "\"") != std::string::npos);
    CHECK(robotStream.waitFor("event: access"));
    CHECK(!otherStream.waitFor("event: access", 0.5));

    // vault may be null, for org-wide changes
    Json::Value org;
    org["subjects"].append(robot);
    org["event"] = "access";
    org["data"]["vault"] = Json::nullValue;
    org["data"]["change"] = "org";
    CHECK(postJson("/internal/notify", org, INTERNAL_TOKEN)->statusCode() == k204NoContent);
    CHECK(robotStream.waitFor("{\"change\":\"org\",\"vault\":null}"));
}

DROGON_TEST(ClosesAccountStreamsWhenTheirTokenExpires)
{
    ClockAt clock(g_vectorNow);
    auto shortLived = accountToken({{VAULT_A, "read"}}, "user:44444444-4444-4444-8444-444444444444", 120);
    EventStream stream(shortLived);
    EventStream longLived(accountToken({{VAULT_A, "read"}}, "user:44444444-4444-4444-8444-444444444444", 600));
    REQUIRE(stream.waitFor(": connected"));
    REQUIRE(longLived.waitFor(": connected"));

    g_now = g_vectorNow + 119;
    CHECK(!stream.waitForEnd(1.5));
    // exp has passed: the server ends the stream within a second
    g_now = g_vectorNow + 120;
    CHECK(stream.waitForEnd(3));
    CHECK(!longLived.waitForEnd(0.5));
    g_now = g_vectorNow + 600;
    CHECK(longLived.waitForEnd(3));
}

DROGON_TEST(MovesALegacyDatabaseToAVault)
{
    for (int i = 1; i <= 3; ++i) {
        g_storage.databases->write("alice", "to-move", fakeDatabase("notes " + std::to_string(i)), [](const auto&) {});
    }
    auto source = *g_storage.databases->meta("alice", "to-move");
    auto vault = newVaultId();
    std::ostringstream out;
    syncserver::migrateToVault(g_storage, "alice", "to-move", vault, out);
    CHECK(out.str().find("revision 3") != std::string::npos);

    auto token = accountToken({{vault, "read"}});
    auto read = request(Get, "/vaults/" + vault, {token});
    REQUIRE(read->statusCode() == k200OK);
    CHECK(read->getHeader("ETag") == source.etag);
    CHECK(read->getHeader("X-Database-Revision") == "3");
    CHECK(read->body() == fakeDatabase("notes 3"));
    auto history = json(request(Get, "/vaults/" + vault + "/history", {token}))["versions"];
    // The test server keeps two older versions
    REQUIRE(history.size() == 3);
    CHECK(history[2]["id"].asString() == vault);
    CHECK(request(Get, "/vaults/" + vault + "/history/2", {token})->body() == fakeDatabase("notes 2"));
    CHECK(!g_storage.databases->meta("alice", "to-move").has_value());

    // Nothing is overwritten or lost when it can't move
    g_storage.databases->write("alice", "to-move", fakeDatabase("again"), [](const auto&) {});
    CHECK_THROWS_AS(syncserver::migrateToVault(g_storage, "alice", "to-move", vault, out), std::invalid_argument);
    CHECK(g_storage.databases->meta("alice", "to-move").has_value());
    CHECK_THROWS_AS(syncserver::migrateToVault(g_storage, "alice", "missing", newVaultId(), out),
                    std::invalid_argument);
    CHECK_THROWS_AS(syncserver::migrateToVault(g_storage, "alice", "to-move", "Not-A-Uuid", out),
                    std::invalid_argument);
    g_storage.databases->remove("alice", "to-move", [](const auto&) {});
}

DROGON_TEST(AnswersCorsForAllowedOrigins)
{
    auto client = HttpClient::newHttpClient(g_baseUrl);
    auto preflight = [&](const std::string& origin) {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Options);
        req->setPath("/api/v1/vaults/" + VAULT_A);
        req->addHeader("Origin", origin);
        req->addHeader("Access-Control-Request-Method", "PUT");
        req->addHeader("Access-Control-Request-Headers", "authorization, if-match, content-type");
        auto [result, resp] = client->sendRequest(req, 10);
        if (result != ReqResult::Ok) {
            std::cerr << "preflight failed: " << to_string(result) << std::endl;
        }
        return resp;
    };
    auto allowed = preflight(WEB_ORIGIN);
    REQUIRE(allowed);
    CHECK(allowed->statusCode() == k204NoContent);
    CHECK(allowed->getHeader("Access-Control-Allow-Origin") == WEB_ORIGIN);
    CHECK(allowed->getHeader("Access-Control-Allow-Headers") == "Authorization, Content-Type, If-Match, If-None-Match");
    CHECK(allowed->getHeader("Access-Control-Allow-Methods").find("PUT") != std::string::npos);
    CHECK(allowed->getHeader("Access-Control-Allow-Methods").find("DELETE") != std::string::npos);
    CHECK(allowed->getHeader("Access-Control-Allow-Credentials").empty());

    auto denied = preflight("https://evil.example");
    REQUIRE(denied);
    CHECK(denied->getHeader("Access-Control-Allow-Origin").empty());

    auto id = newVaultId();
    createVault(id);
    auto req = HttpRequest::newHttpRequest();
    req->setPath("/api/v1/vaults/" + id);
    req->addHeader("Origin", WEB_ORIGIN);
    req->addHeader("Authorization", "Bearer " + accountToken({{id, "read"}}));
    auto read = client->sendRequest(req, 10).second;
    REQUIRE(read);
    CHECK(read->statusCode() == k200OK);
    CHECK(read->getHeader("Access-Control-Allow-Origin") == WEB_ORIGIN);
    CHECK(read->getHeader("Access-Control-Expose-Headers") == "ETag, X-Database-Revision, Last-Modified");
    CHECK(read->getHeader("Vary").find("Origin") != std::string::npos);

    // Errors carry the headers too, so the web app can read them
    auto unauthorized = HttpRequest::newHttpRequest();
    unauthorized->setPath("/api/v1/vaults");
    unauthorized->addHeader("Origin", WEB_ORIGIN);
    auto error = client->sendRequest(unauthorized, 10).second;
    REQUIRE(error);
    CHECK(error->statusCode() == k401Unauthorized);
    CHECK(error->getHeader("Access-Control-Allow-Origin") == WEB_ORIGIN);
}

int main(int argc, char** argv)
{
    {
        std::ifstream file(SYNC_TEST_VECTORS);
        Json::CharReaderBuilder builder;
        std::string errors;
        if (!Json::parseFromStream(builder, file, &g_vectors, &errors)) {
            std::cerr << "Cannot read " << SYNC_TEST_VECTORS << ": " << errors << "\n";
            return 1;
        }
    }
    g_vectorNow = g_vectors["jwt"]["now"].asInt64();
    g_now = g_vectorNow;
    g_jwks = compact(g_vectors["jwt"]["jwks"]);

    auto dataDir = fs::temp_directory_path() / ("openvireo-sync-jwt-test-" + utils::getUuid());
    fs::create_directories(dataDir);

    auto& config = g_config;
    try {
        auto environment = syncserver::Config::fromEnvironment();
        config.databaseUrl = environment.databaseUrl;
        config.s3 = environment.s3;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    config.dataDir = dataDir;
    config.maxDatabaseBytes = 1024;
    config.historySize = 2;
    config.threads = 4;
#ifdef SYNC_TEST_JWT_ONLY
    config.authMode = syncserver::AuthMode::Jwt;
#else
    config.authMode = syncserver::AuthMode::Both;
#endif
    config.authIssuer = ISSUER;
    config.authJwksUrl = "http://127.0.0.1:9/never-fetched/jwks.json";
    config.internalToken = INTERNAL_TOKEN;
    config.allowedOrigins = {WEB_ORIGIN};
    if (config.usesPostgres()) {
        std::string run;
        for (unsigned char c : utils::getUuid()) {
            if (std::isalnum(c) && run.size() < 12) {
                run += static_cast<char>(std::tolower(c));
            }
        }
        config.databaseSchema = "test_" + run;
        config.s3.prefix += "test-" + run + "/";
        std::cout << "Testing against Postgres and S3 (schema " << config.databaseSchema << ")" << std::endl;
    }

    g_storage = syncserver::openStorage(config);
    g_deviceToken = g_storage.users->addUser("alice", "laptop");
    syncserver::SyncApiOptions options;
    options.clock = [] { return g_now.load(); };
    options.jwksFetcher = []() -> std::optional<std::string> {
        ++g_jwksFetches;
        std::lock_guard lock(g_jwksMutex);
        return g_jwks;
    };
    syncserver::registerSyncApi(config, syncserver::openStorage(config), options);

    std::promise<void> started;
    std::thread server([&] {
        app().setLogLevel(trantor::Logger::kWarn).setThreadNum(config.threads).addListener("127.0.0.1", 0);
        app().getLoop()->queueInLoop([&] { started.set_value(); });
        app().run();
    });
    started.get_future().wait();
    g_baseUrl = "http://127.0.0.1:" + std::to_string(app().getListeners().front().toPort());

    int status = test::run(argc, argv);

    app().getLoop()->queueInLoop([] { app().quit(); });
    server.join();
    fs::remove_all(dataDir);
#ifdef SYNC_SERVER_WITH_CLOUD
    if (config.usesPostgres()) {
        syncserver::Postgres postgres(config);
        auto blobs = syncserver::BlobStore(config.s3);
        for (const auto& object : blobs.list()) {
            blobs.remove(object.key);
        }
        postgres.transaction([&](drogon::orm::Transaction& trans) {
            trans.execSqlSync("SET LOCAL client_min_messages = warning");
            trans.execSqlSync("DROP SCHEMA " + config.databaseSchema + " CASCADE");
        });
    }
#endif
    g_storage = {};
    return status;
}
