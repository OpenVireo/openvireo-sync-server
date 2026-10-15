#define DROGON_TEST_MAIN
#include <drogon/drogon.h>
#include <drogon/drogon_test.h>

#include "Config.h"
#include "SyncApi.h"
#include "UserStore.h"
#include "Util.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
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

    fs::path g_dataDir;
    std::string g_baseUrl;
    std::string g_aliceToken;
    std::string g_bobToken;

    std::string fakeDatabase(const std::string& contents)
    {
        return KDBX_HEADER + contents;
    }

    struct Request
    {
        std::string token = g_aliceToken;
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

    Json::Value json(const HttpResponsePtr& resp);

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

    Json::Value claim(const std::string& code, const std::string& deviceName = "phone")
    {
        Json::Value body;
        body["code"] = code;
        body["deviceName"] = deviceName;
        auto resp = postJson("/devices/pairing/claim", body);
        auto result = json(resp);
        result["status"] = static_cast<int>(resp->statusCode());
        return result;
    }

    Json::Value json(const HttpResponsePtr& resp)
    {
        if (resp->contentType() != CT_APPLICATION_JSON) {
            return {};
        }
        auto body = resp->getJsonObject();
        return body ? *body : Json::Value();
    }

    // Creates a database and returns its ETag
    std::string create(const std::string& id, const std::string& contents = "base")
    {
        return request(Put, "/databases/" + id, {g_aliceToken, {{"If-None-Match", "*"}}, fakeDatabase(contents)})
            ->getHeader("ETag");
    }
} // namespace

DROGON_TEST(HealthCheckNeedsNoAuth)
{
    auto resp = request(Get, "/health", {""});
    REQUIRE(resp->statusCode() == k200OK);
    CHECK(json(resp)["status"].asString() == "ok");
    CHECK(resp->getHeader("Cache-Control") == "no-store");
}

DROGON_TEST(RejectsMissingAndInvalidTokens)
{
    auto missing = request(Get, "/databases", {""});
    CHECK(missing->statusCode() == k401Unauthorized);
    CHECK(missing->getHeader("WWW-Authenticate") == "Bearer realm=\"keepassxc-sync\"");
    CHECK(request(Get, "/databases", {"nope"})->statusCode() == k401Unauthorized);
}

DROGON_TEST(RejectsPathTraversalInDatabaseIds)
{
    CHECK(request(Get, "/databases/..%2F..%2Fusers.json")->statusCode() == k400BadRequest);
    CHECK(request(Get, "/databases/.hidden")->statusCode() == k400BadRequest);
}

DROGON_TEST(UnknownRoutesReturnJson404)
{
    auto resp = request(Get, "/nope");
    REQUIRE(resp->statusCode() == k404NotFound);
    CHECK(json(resp)["error"].asString() == "Not found.");
}

DROGON_TEST(CreateReadUpdateWithOptimisticConcurrency)
{
    CHECK(request(Get, "/databases/main")->statusCode() == k404NotFound);

    auto v1 = fakeDatabase("version 1");
    auto created = request(Put, "/databases/main", {g_aliceToken, {{"If-None-Match", "*"}}, v1});
    REQUIRE(created->statusCode() == k201Created);
    auto etag1 = created->getHeader("ETag");
    CHECK(etag1.size() == 66);
    CHECK(etag1.front() == '"');
    CHECK(etag1.find_first_not_of("0123456789abcdef", 1) == 65);

    // Creating again must fail
    auto again = request(Put, "/databases/main", {g_aliceToken, {{"If-None-Match", "*"}}, v1});
    CHECK(again->statusCode() == k412PreconditionFailed);

    auto read = request(Get, "/databases/main");
    REQUIRE(read->statusCode() == k200OK);
    CHECK(read->getHeader("ETag") == etag1);
    CHECK(read->getHeader("X-Database-Revision") == "1");
    CHECK(!read->getHeader("Last-Modified").empty());
    CHECK(read->body() == v1);

    auto head = request(Head, "/databases/main");
    CHECK(head->statusCode() == k200OK);
    CHECK(head->getHeader("ETag") == etag1);

    auto notModified = request(Get, "/databases/main", {g_aliceToken, {{"If-None-Match", etag1}}});
    CHECK(notModified->statusCode() == k304NotModified);

    // Blind overwrite is refused
    auto blind = request(Put, "/databases/main", {g_aliceToken, {}, fakeDatabase("x")});
    CHECK(blind->statusCode() == k428PreconditionRequired);

    auto updated = request(Put, "/databases/main", {g_aliceToken, {{"If-Match", etag1}}, fakeDatabase("version 2")});
    REQUIRE(updated->statusCode() == k200OK);
    auto etag2 = updated->getHeader("ETag");
    CHECK(etag2 != etag1);
    CHECK(json(updated)["revision"].asInt() == 2);

    // A second device still holding etag1 loses the race
    auto stale = request(Put, "/databases/main", {g_aliceToken, {{"If-Match", etag1}}, fakeDatabase("v2b")});
    CHECK(stale->statusCode() == k412PreconditionFailed);

    auto list = request(Get, "/databases");
    REQUIRE(list->statusCode() == k200OK);
    bool found = false;
    auto databases = json(list)["databases"];
    for (const auto& db : databases) {
        if (db["id"].asString() == "main") {
            found = true;
            CHECK(db["revision"].asInt() == 2);
            CHECK(db["etag"].asString() == etag2);
        }
    }
    CHECK(found);
}

DROGON_TEST(KeepsBoundedHistory)
{
    auto etag = create("history", "version 1");
    for (int i = 2; i <= 6; ++i) {
        auto resp = request(
            Put, "/databases/history", {g_aliceToken, {{"If-Match", etag}}, fakeDatabase("version " + std::to_string(i))});
        REQUIRE(resp->statusCode() == k200OK);
        etag = resp->getHeader("ETag");
    }
    std::vector<std::string> history;
    for (const auto& item : fs::directory_iterator(g_dataDir / "databases" / "alice" / ".history" / "history")) {
        history.push_back(item.path().filename().string());
    }
    std::sort(history.begin(), history.end());
    // Each kept version has its metadata next to it
    CHECK((history == std::vector<std::string>{"4.json", "4.kdbx", "5.json", "5.kdbx"}));
}

DROGON_TEST(UsersCannotSeeEachOthersDatabases)
{
    create("private");
    CHECK(request(Get, "/databases/private", {g_bobToken})->statusCode() == k404NotFound);
    auto list = request(Get, "/databases", {g_bobToken});
    REQUIRE(list->statusCode() == k200OK);
    CHECK(json(list)["databases"].size() == 0);
}

DROGON_TEST(RejectsBodiesThatAreNotKdbxOrTooLarge)
{
    auto notKdbx = request(Put, "/databases/other", {g_aliceToken, {}, "hello world, not a db"});
    CHECK(notKdbx->statusCode() == k415UnsupportedMediaType);

    auto tooShort = request(Put, "/databases/other", {g_aliceToken, {}, "abc"});
    CHECK(tooShort->statusCode() == k400BadRequest);

    auto tooBig = request(Put, "/databases/other", {g_aliceToken, {}, fakeDatabase(std::string(2048, 'x'))});
    CHECK(tooBig->statusCode() == k413RequestEntityTooLarge);
}

DROGON_TEST(ConcurrentConditionalWritesExactlyOneWins)
{
    auto etag = create("race");
    std::vector<std::future<HttpResponsePtr>> writers;
    for (int i = 1; i <= 5; ++i) {
        writers.push_back(std::async(std::launch::async, [etag, i] {
            return request(
                Put, "/databases/race", {g_aliceToken, {{"If-Match", etag}}, fakeDatabase("writer " + std::to_string(i))});
        }));
    }
    int ok = 0;
    int conflicts = 0;
    for (auto& writer : writers) {
        auto status = writer.get()->statusCode();
        ok += status == k200OK;
        conflicts += status == k412PreconditionFailed;
    }
    CHECK(ok == 1);
    CHECK(conflicts == 4);
}

DROGON_TEST(DeleteHonoursIfMatch)
{
    auto etag = create("doomed");
    CHECK(request(Delete, "/databases/doomed", {g_aliceToken, {{"If-Match", "\"0000\""}}})->statusCode()
          == k412PreconditionFailed);
    CHECK(request(Delete, "/databases/doomed", {g_aliceToken, {{"If-Match", etag}}})->statusCode() == k204NoContent);
    CHECK(request(Get, "/databases/doomed")->statusCode() == k404NotFound);
    CHECK(request(Delete, "/databases/doomed")->statusCode() == k404NotFound);
}

DROGON_TEST(EachDeviceHasItsOwnRevocableToken)
{
    syncserver::UserStore users(g_dataDir);
    auto [phoneId, phoneToken] = users.addDevice("alice", "phone");
    CHECK(phoneToken != g_aliceToken);

    // Both devices see the same databases
    create("shared-by-devices");
    CHECK(request(Get, "/databases/shared-by-devices", {phoneToken})->statusCode() == k200OK);
    CHECK(request(Get, "/databases/shared-by-devices", {g_aliceToken})->statusCode() == k200OK);

    // Revoking the phone shuts it out at once and leaves the laptop alone
    CHECK(users.revokeDevice("alice", phoneId));
    CHECK(request(Get, "/databases/shared-by-devices", {phoneToken})->statusCode() == k401Unauthorized);
    CHECK(request(Get, "/databases/shared-by-devices", {g_aliceToken})->statusCode() == k200OK);

    CHECK(!users.revokeDevice("alice", phoneId));
    CHECK(!users.revokeDevice("nobody", phoneId));
    // A failed revoke for an unknown user doesn't create it
    auto names = users.listUsers();
    CHECK(std::find(names.begin(), names.end(), "nobody") == names.end());
}

DROGON_TEST(ListsDevicesAndWhenTheyWereLastSeen)
{
    auto dir = fs::temp_directory_path() / ("kpxc-sync-users-" + utils::getUuid());
    syncserver::UserStore users(dir);
    auto token = users.addUser("carol", "desktop");
    auto [tabletId, tabletToken] = users.addDevice("carol", "tablet");

    // Devices added in the same millisecond have no order, so look them up by name
    auto device = [&](const std::string& name) {
        auto devices = users.listDevices("carol");
        auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.name == name; });
        return it != devices.end() ? *it : syncserver::Device{};
    };
    REQUIRE(users.listDevices("carol").size() == 2);
    CHECK(device("tablet").id == tabletId);
    CHECK(device("desktop").lastSeenAt.empty());

    auto identity = users.authenticate(token);
    REQUIRE(identity.has_value());
    CHECK(identity->user == "carol");
    CHECK(identity->deviceId == device("desktop").id);
    CHECK(!device("desktop").lastSeenAt.empty());
    CHECK(device("tablet").lastSeenAt.empty());

    CHECK_THROWS(users.addUser("carol", "again"));
    CHECK_THROWS(users.addDevice("carol", ""));
    CHECK_THROWS(users.addDevice("nobody", "phone"));
    fs::remove_all(dir);
}

DROGON_TEST(KeepsTokensFromBeforeDevicesWorking)
{
    auto dir = fs::temp_directory_path() / ("kpxc-sync-users-" + utils::getUuid());
    fs::create_directories(dir);
    const std::string oldToken = "token-from-an-older-server";
    Json::Value data;
    data["users"]["dave"]["tokenHash"] = syncserver::sha256Hex(oldToken);
    data["users"]["dave"]["createdAt"] = "2026-01-01T00:00:00.000Z";
    syncserver::atomicWriteFile(dir / "users.json", data.toStyledString());

    syncserver::UserStore users(dir);
    auto identity = users.authenticate(oldToken);
    REQUIRE(identity.has_value());
    CHECK(identity->user == "dave");
    CHECK(identity->deviceId == "legacy");

    // It can be revoked like any other device once the others have their own tokens
    users.addDevice("dave", "laptop");
    CHECK(users.listDevices("dave").size() == 2);
    CHECK(users.revokeDevice("dave", "legacy"));
    CHECK(!users.authenticate(oldToken).has_value());
    fs::remove_all(dir);
}

DROGON_TEST(ListsAndReturnsKeptVersions)
{
    auto etag = create("versions", "version 1");
    for (int i = 2; i <= 3; ++i) {
        auto resp = request(
            Put, "/databases/versions", {g_aliceToken, {{"If-Match", etag}}, fakeDatabase("version " + std::to_string(i))});
        REQUIRE(resp->statusCode() == k200OK);
        etag = resp->getHeader("ETag");
    }

    auto list = request(Get, "/databases/versions/history");
    REQUIRE(list->statusCode() == k200OK);
    auto versions = json(list)["versions"];
    REQUIRE(versions.size() == 3);
    CHECK(versions[0]["revision"].asInt64() == 3);
    CHECK(versions[0]["etag"].asString() == etag);
    CHECK(versions[2]["revision"].asInt64() == 1);
    CHECK(!versions[2]["updatedAt"].asString().empty());

    auto first = request(Get, "/databases/versions/history/1");
    REQUIRE(first->statusCode() == k200OK);
    CHECK(first->body() == fakeDatabase("version 1"));
    CHECK(first->getHeader("X-Database-Revision") == "1");
    CHECK(first->getHeader("ETag") == versions[2]["etag"].asString());
    CHECK(request(Get, "/databases/versions/history/3")->body() == fakeDatabase("version 3"));

    CHECK(request(Get, "/databases/versions/history/9")->statusCode() == k404NotFound);
    CHECK(request(Get, "/databases/versions/history/abc")->statusCode() == k404NotFound);
    CHECK(request(Get, "/databases/missing/history")->statusCode() == k404NotFound);
    CHECK(request(Put, "/databases/versions/history")->statusCode() == k405MethodNotAllowed);
    // Other users can't read them
    CHECK(request(Get, "/databases/versions/history", {g_bobToken})->statusCode() == k404NotFound);
}

namespace
{
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
                    break;
                }
            }
            return m_received.find(text) != std::string::npos;
        }

        const std::string& received() const
        {
            return m_received;
        }

    private:
        int m_socket = -1;
        std::string m_received;
    };
} // namespace

DROGON_TEST(StreamsChangesToTheOwnersDevices)
{
    EventStream alice(g_aliceToken);
    EventStream bob(g_bobToken);
    REQUIRE(alice.waitFor(": connected"));
    REQUIRE(alice.received().find("text/event-stream") != std::string::npos);
    REQUIRE(bob.waitFor(": connected"));

    auto etag = create("streamed");
    CHECK(alice.waitFor("event: database"));
    CHECK(alice.waitFor("\"id\":\"streamed\""));
    CHECK(alice.received().find(etag.substr(1, etag.size() - 2)) != std::string::npos);
    CHECK(alice.received().find("\"revision\":1") != std::string::npos);

    request(Delete, "/databases/streamed", {g_aliceToken, {{"If-Match", etag}}});
    CHECK(alice.waitFor("\"deleted\":true"));

    // Bob only hears about his own databases
    CHECK(!bob.waitFor("event: database", 0.5));

    EventStream stranger("not-a-token");
    CHECK(stranger.waitFor("401"));
}

DROGON_TEST(PairsADeviceWithAOneTimeCode)
{
    // Only a signed-in device can ask for a code
    CHECK(postJson("/devices/pairing", Json::objectValue)->statusCode() == k401Unauthorized);

    auto created = postJson("/devices/pairing", Json::objectValue, g_aliceToken);
    REQUIRE(created->statusCode() == k201Created);
    auto code = json(created)["code"].asString();
    CHECK(code.size() == 9);
    CHECK(code[4] == '-');
    CHECK(!json(created)["expiresAt"].asString().empty());

    // Typed by hand: lower case and without the dash still match
    std::string typed = code.substr(0, 4) + " " + code.substr(5);
    std::transform(typed.begin(), typed.end(), typed.begin(), [](unsigned char c) { return std::tolower(c); });
    auto paired = claim(typed, "  work phone ");
    REQUIRE(paired["status"].asInt() == k201Created);
    CHECK(paired["user"].asString() == "alice");
    auto token = paired["token"].asString();
    CHECK(token.size() >= 40);
    CHECK(request(Get, "/databases", {token})->statusCode() == k200OK);

    syncserver::UserStore users(g_dataDir);
    auto devices = users.listDevices("alice");
    auto added = std::find_if(
        devices.begin(), devices.end(), [&](const auto& d) { return d.id == paired["deviceId"].asString(); });
    REQUIRE(added != devices.end());
    CHECK(added->name == "work phone");

    // A code adds one device only
    CHECK(claim(code)["status"].asInt() == k400BadRequest);
    CHECK(claim("AAAA-AAAA")["error"].asString() == "Unknown or expired pairing code.");

    auto another = json(postJson("/devices/pairing", Json::objectValue, g_aliceToken))["code"].asString();
    CHECK(claim(another, "")["status"].asInt() == k400BadRequest);
    // A bad device name doesn't use up the code
    CHECK(claim(another, "tablet")["status"].asInt() == k201Created);

    Json::Value missing;
    missing["code"] = another;
    CHECK(postJson("/devices/pairing/claim", missing)->statusCode() == k400BadRequest);
}

DROGON_TEST(PairingCodesExpireAndStayFew)
{
    // Its own folder: tests run in parallel, and the server would write its copy of users.json over this one's
    auto dataDir = fs::temp_directory_path() / ("kpxc-sync-pairing-" + utils::getUuid());
    fs::create_directories(dataDir);
    syncserver::UserStore users(dataDir);
    users.addUser("bob", "laptop");
    users.setPairingLifetime(std::chrono::seconds(-1));
    auto expired = users.createPairingCode("bob");
    CHECK(!users.claimPairingCode(expired.code, "late"));

    users.setPairingLifetime(std::chrono::minutes(10));
    std::vector<std::string> codes;
    for (std::size_t i = 0; i < syncserver::UserStore::MaxPairingsPerUser + 1; ++i) {
        codes.push_back(users.createPairingCode("bob").code);
    }

    // The codes themselves never reach the file
    auto file = syncserver::readFile(dataDir / "users.json");
    const auto newest = syncserver::UserStore::normalizePairingCode(codes.back());
    CHECK(file.find(newest) == std::string::npos);
    CHECK(file.find(syncserver::sha256Hex(newest)) != std::string::npos);

    // One that expires first made room for the newest (codes made in the same millisecond tie)
    CHECK(users.claimPairingCode(codes.back(), "last"));
    std::size_t claimed = 1;
    for (std::size_t i = 0; i + 1 < codes.size(); ++i) {
        if (users.claimPairingCode(codes[i], "device " + std::to_string(i))) {
            ++claimed;
        }
    }
    CHECK(claimed == syncserver::UserStore::MaxPairingsPerUser);
    CHECK_THROWS(users.createPairingCode("nobody"));
    fs::remove_all(dataDir);
}

DROGON_TEST(LimitsFailuresPerClientAndInTotal)
{
    using namespace std::chrono_literals;
    syncserver::FailureLimiter limiter(2, 3, 60s);
    auto t0 = syncserver::FailureLimiter::Clock::time_point();
    CHECK(limiter.retryAfter("a", t0) == 0);
    limiter.recordFailure("a", t0);
    limiter.recordFailure("a", t0 + 10s);
    CHECK(limiter.retryAfter("a", t0 + 10s) == 50);
    CHECK(limiter.retryAfter("b", t0 + 10s) == 0);
    limiter.recordFailure("b", t0 + 20s);
    // Three in all blocks everyone until the first one is a minute old
    CHECK(limiter.retryAfter("c", t0 + 20s) == 40);
    CHECK(limiter.retryAfter("c", t0 + 60s) == 0);
    CHECK(limiter.retryAfter("a", t0 + 60s) == 0);
    limiter.recordFailure("a", t0 + 60s);
    CHECK(limiter.retryAfter("a", t0 + 60s) == 10);
    CHECK(limiter.retryAfter("a", t0 + 70s) == 0);
}

int main(int argc, char** argv)
{
    g_dataDir = fs::temp_directory_path() / ("kpxc-sync-test-" + utils::getUuid());
    fs::create_directories(g_dataDir);

    syncserver::UserStore users(g_dataDir);
    g_aliceToken = users.addUser("alice", "laptop");
    g_bobToken = users.addUser("bob", "laptop");

    syncserver::Config config;
    config.dataDir = g_dataDir;
    config.maxDatabaseBytes = 1024;
    config.historySize = 2;
    config.threads = 4;
    syncserver::registerSyncApi(config);

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
    fs::remove_all(g_dataDir);
    return status;
}
