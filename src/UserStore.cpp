#include "UserStore.h"

#include "Util.h"

#include <json/json.h>
#include <sqlite3.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace syncserver
{
    namespace
    {
        // Each migration is a list of statements. Append new ones; never change one that has shipped.
        const std::vector<std::vector<const char*>> MIGRATIONS = {
            {
                R"(CREATE TABLE users (
                       name        TEXT PRIMARY KEY,
                       created_at  TEXT NOT NULL
                   ))",
                R"(CREATE TABLE devices (
                       user_name     TEXT NOT NULL REFERENCES users(name) ON DELETE CASCADE,
                       id            TEXT NOT NULL,
                       name          TEXT NOT NULL,
                       token_hash    TEXT NOT NULL UNIQUE,
                       created_at    TEXT NOT NULL,
                       last_seen_at  TEXT,
                       PRIMARY KEY (user_name, id)
                   ))",
                R"(CREATE TABLE pairing_codes (
                       code_hash   TEXT PRIMARY KEY,
                       user_name   TEXT NOT NULL REFERENCES users(name) ON DELETE CASCADE,
                       created_by  TEXT,
                       created_at  TEXT NOT NULL,
                       expires_at  TEXT NOT NULL
                   ))",
                "CREATE INDEX pairing_codes_user_name ON pairing_codes (user_name)",
            },
        };

        // How long to wait for another process (the server or a command) to finish writing
        constexpr int BUSY_TIMEOUT_MS = 10000;

        [[noreturn]] void fail(sqlite3* db, const std::string& what)
        {
            throw std::runtime_error(what + ": " + sqlite3_errmsg(db));
        }

        void exec(sqlite3* db, const char* sql)
        {
            if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
                fail(db, sql);
            }
        }

        class Statement
        {
        public:
            template <typename... Args> Statement(sqlite3* db, const char* sql, const Args&... args)
                : m_db(db)
            {
                if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK) {
                    fail(db, "Cannot prepare a query");
                }
                int index = 0;
                (bind(++index, args), ...);
            }

            ~Statement()
            {
                sqlite3_finalize(m_stmt);
            }

            Statement(const Statement&) = delete;
            Statement& operator=(const Statement&) = delete;

            // Whether there is a row to read
            bool step()
            {
                switch (sqlite3_step(m_stmt)) {
                case SQLITE_ROW:
                    return true;
                case SQLITE_DONE:
                    return false;
                default:
                    fail(m_db, "Cannot run a query");
                }
            }

            // Runs a statement that returns no rows and returns how many rows it changed
            int run()
            {
                while (step()) {
                }
                return sqlite3_changes(m_db);
            }

            // NULL reads as an empty string
            std::string text(int column) const
            {
                auto text = sqlite3_column_text(m_stmt, column);
                return text ? std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(m_stmt, column))
                            : std::string();
            }

        private:
            void bind(int index, const std::string& value)
            {
                check(sqlite3_bind_text(m_stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
            }

            void bind(int index, const char* value)
            {
                bind(index, std::string(value));
            }

            void bind(int index, int64_t value)
            {
                check(sqlite3_bind_int64(m_stmt, index, value));
            }

            void check(int rc)
            {
                if (rc != SQLITE_OK) {
                    fail(m_db, "Cannot bind a query parameter");
                }
            }

            sqlite3* m_db;
            sqlite3_stmt* m_stmt = nullptr;
        };

        // Takes the write lock at once, so two processes never both read and then both try to write.
        // Rolls back unless committed.
        class Transaction
        {
        public:
            explicit Transaction(sqlite3* db)
                : m_db(db)
            {
                exec(db, "BEGIN IMMEDIATE");
            }

            ~Transaction()
            {
                if (!m_done) {
                    sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
                }
            }

            Transaction(const Transaction&) = delete;
            Transaction& operator=(const Transaction&) = delete;

            void commit()
            {
                exec(m_db, "COMMIT");
                m_done = true;
            }

        private:
            sqlite3* m_db;
            bool m_done = false;
        };

        bool userExists(sqlite3* db, const std::string& name)
        {
            return Statement(db, "SELECT 1 FROM users WHERE name = ?", name).step();
        }

        void insertDevice(sqlite3* db, const StoredDevice& device, const std::string& user)
        {
            Statement(db,
                      "INSERT INTO devices (id, user_name, name, token_hash, created_at, last_seen_at) "
                      "VALUES (?, ?, ?, ?, ?, nullif(?, ''))",
                      device.id,
                      user,
                      device.name,
                      device.tokenHash,
                      device.createdAt,
                      device.lastSeenAt)
                .run();
        }

        StoredDevice newDevice(const std::string& id, const std::string& name, const std::string& token)
        {
            return {id, name, sha256Hex(token), isoTimestamp(), {}};
        }

        // users.json as older servers wrote it, legacy per-user tokens turned into devices called "legacy".
        // Pairing codes are left out: they are only valid for minutes.
        std::vector<StoredUser> readUsersJson(const fs::path& file)
        {
            Json::Value data;
            Json::CharReaderBuilder builder;
            std::string errors;
            auto text = readFile(file);
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if (!reader->parse(text.data(), text.data() + text.size(), &data, &errors) || !data.isObject()) {
                throw std::runtime_error("Invalid " + file.string() + ": " + errors);
            }

            std::vector<StoredUser> result;
            const auto& users = data["users"];
            if (!users.isObject()) {
                return result;
            }
            const auto now = isoTimestamp();
            auto orNow = [&](const Json::Value& value) {
                return value.asString().empty() ? now : value.asString();
            };
            for (const auto& name : users.getMemberNames()) {
                const auto& user = users[name];
                StoredUser stored{name, orNow(user["createdAt"]), {}};
                if (user.isMember("tokenHash")) {
                    stored.devices.push_back(
                        {"legacy", "legacy", user["tokenHash"].asString(), stored.createdAt, {}});
                }
                const auto& devices = user["devices"];
                if (devices.isObject()) {
                    for (const auto& id : devices.getMemberNames()) {
                        const auto& device = devices[id];
                        stored.devices.push_back({id,
                                                  device["name"].asString(),
                                                  device["tokenHash"].asString(),
                                                  orNow(device["createdAt"]),
                                                  device["lastSeenAt"].asString()});
                    }
                }
                result.push_back(std::move(stored));
            }
            return result;
        }
    } // namespace

    UserStore::UserStore(const fs::path& dataDir)
        : m_dataDir(dataDir)
    {
        ensurePrivateDirectory(dataDir);
        const auto file = dataDir / "users.sqlite";
        // Created first so it is private; SQLite gives its -wal and -shm files the same mode. On Windows it inherits
        // the data directory's access control list instead.
#ifdef _WIN32
        int fd = ::_wopen(file.c_str(), _O_RDWR | _O_CREAT | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "Cannot open " + file.string());
        }
        ::_close(fd);
#else
        int fd = ::open(file.c_str(), O_RDWR | O_CREAT, 0600);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "Cannot open " + file.string());
        }
        ::close(fd);
#endif

        // SQLite takes UTF-8 file names everywhere, also on Windows
        if (sqlite3_open_v2(file.u8string().c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            std::string error = m_db ? sqlite3_errmsg(m_db) : "out of memory";
            sqlite3_close(m_db);
            throw std::runtime_error("Cannot open " + file.string() + ": " + error);
        }
        try {
            sqlite3_busy_timeout(m_db, BUSY_TIMEOUT_MS);
            exec(m_db, "PRAGMA journal_mode = WAL");
            exec(m_db, "PRAGMA foreign_keys = ON");
            migrate();
            importJson();
        } catch (...) {
            sqlite3_close(m_db);
            throw;
        }
    }

    UserStore::~UserStore()
    {
        sqlite3_close(m_db);
    }

    void UserStore::migrate()
    {
        Transaction trans(m_db);
        Statement version(m_db, "PRAGMA user_version");
        version.step();
        const auto current = std::stoi(version.text(0));
        if (current > static_cast<int>(MIGRATIONS.size())) {
            throw std::runtime_error("users.sqlite (version " + std::to_string(current) + ") is newer than this server ("
                                     + std::to_string(MIGRATIONS.size()) + "). Upgrade the server.");
        }
        for (auto next = current + 1; next <= static_cast<int>(MIGRATIONS.size()); ++next) {
            for (const auto* statement : MIGRATIONS[next - 1]) {
                exec(m_db, statement);
            }
            // PRAGMA takes no parameters
            exec(m_db, ("PRAGMA user_version = " + std::to_string(next)).c_str());
        }
        trans.commit();
    }

    void UserStore::importJson()
    {
        const auto json = m_dataDir / "users.json";
        std::error_code ec;
        if (!fs::exists(json, ec)) {
            return;
        }
        {
            Transaction trans(m_db);
            // Checked again under the lock: another process may have just imported and renamed it
            if (!fs::exists(json, ec)) {
                return;
            }
            // Users already here are skipped, so an import that stopped before the rename can run again
            for (const auto& user : readUsersJson(json)) {
                if (Statement(m_db, "INSERT OR IGNORE INTO users (name, created_at) VALUES (?, ?)", user.name, user.createdAt)
                        .run()
                    == 0) {
                    continue;
                }
                for (const auto& device : user.devices) {
                    insertDevice(m_db, device, user.name);
                }
            }
            trans.commit();
        }
        fs::rename(json, m_dataDir / "users.json.imported", ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw std::system_error(ec, "Cannot rename " + json.string() + " after importing it");
        }
    }

    std::optional<Identity> UserStore::authenticate(const std::string& token)
    {
        if (token.empty()) {
            return std::nullopt;
        }
        // As in Postgres: an indexed lookup by the hash of a 256-bit random token tells an attacker nothing useful
        std::lock_guard lock(m_mutex);
        auto staleBefore = isoTimestamp(std::chrono::system_clock::now() - m_lastSeenInterval);
        Statement select(m_db,
                         "SELECT user_name, id, coalesce(last_seen_at, '') < ? FROM devices WHERE token_hash = ?",
                         staleBefore,
                         sha256Hex(token));
        if (!select.step()) {
            return std::nullopt;
        }
        Identity identity{select.text(0), select.text(1)};
        // ISO timestamps in UTC sort like the times they stand for
        if (select.text(2) == "1") {
            Statement(m_db,
                      "UPDATE devices SET last_seen_at = ? WHERE user_name = ? AND id = ?",
                      isoTimestamp(),
                      identity.user,
                      identity.deviceId)
                .run();
        }
        return identity;
    }

    PairingCode UserStore::createPairingCode(const std::string& user, const std::string& createdBy)
    {
        auto code = newPairingCode();
        auto now = isoTimestamp();
        auto expiresAt = isoTimestamp(std::chrono::system_clock::now() + m_pairingLifetime);

        std::lock_guard lock(m_mutex);
        Transaction trans(m_db);
        if (!userExists(m_db, user)) {
            throw std::invalid_argument("No such user: " + user);
        }
        Statement(m_db, "DELETE FROM pairing_codes WHERE user_name = ? AND expires_at <= ?", user, now).run();
        // Keep a user's waiting codes few, dropping the ones that expire first
        Statement(m_db,
                  "DELETE FROM pairing_codes WHERE code_hash IN ("
                  "SELECT code_hash FROM pairing_codes WHERE user_name = ? "
                  "ORDER BY expires_at DESC LIMIT -1 OFFSET ?)",
                  user,
                  static_cast<int64_t>(MaxPairingsPerUser - 1))
            .run();
        Statement(m_db,
                  "INSERT INTO pairing_codes (code_hash, user_name, created_by, created_at, expires_at) "
                  "VALUES (?, ?, nullif(?, ''), ?, ?)",
                  sha256Hex(code),
                  user,
                  createdBy,
                  now,
                  expiresAt)
            .run();
        trans.commit();
        return {formatPairingCode(code), expiresAt};
    }

    std::optional<PairedDevice> UserStore::claimPairingCode(const std::string& code, const std::string& deviceName)
    {
        validateDeviceName(deviceName);
        auto hash = sha256Hex(normalizePairingCode(code));

        std::lock_guard lock(m_mutex);
        Transaction trans(m_db);
        Statement(m_db, "DELETE FROM pairing_codes WHERE expires_at <= ?", isoTimestamp()).run();
        Statement select(m_db, "SELECT user_name FROM pairing_codes WHERE code_hash = ?", hash);
        if (!select.step()) {
            trans.commit();
            return std::nullopt;
        }

        // Each code adds one device only
        PairedDevice paired{select.text(0), newDeviceId(), newToken()};
        Statement(m_db, "DELETE FROM pairing_codes WHERE code_hash = ?", hash).run();
        insertDevice(m_db, newDevice(paired.deviceId, deviceName, paired.token), paired.user);
        trans.commit();
        return paired;
    }

    std::string UserStore::addUser(const std::string& name, const std::string& deviceName)
    {
        validateUserName(name);
        validateDeviceName(deviceName);
        auto token = newToken();

        std::lock_guard lock(m_mutex);
        Transaction trans(m_db);
        if (Statement(m_db, "INSERT OR IGNORE INTO users (name, created_at) VALUES (?, ?)", name, isoTimestamp()).run()
            == 0) {
            throw std::invalid_argument("User \"" + name + "\" already exists. Add a device with: devices add " + name
                                        + " <device-name>");
        }
        insertDevice(m_db, newDevice(newDeviceId(), deviceName, token), name);
        trans.commit();
        return token;
    }

    bool UserStore::removeUser(const std::string& name)
    {
        std::lock_guard lock(m_mutex);
        return Statement(m_db, "DELETE FROM users WHERE name = ?", name).run() > 0;
    }

    std::vector<std::string> UserStore::listUsers()
    {
        std::lock_guard lock(m_mutex);
        std::vector<std::string> names;
        Statement select(m_db, "SELECT name FROM users ORDER BY name");
        while (select.step()) {
            names.push_back(select.text(0));
        }
        return names;
    }

    std::pair<std::string, std::string> UserStore::addDevice(const std::string& user, const std::string& deviceName)
    {
        validateDeviceName(deviceName);
        auto id = newDeviceId();
        auto token = newToken();

        std::lock_guard lock(m_mutex);
        Transaction trans(m_db);
        if (!userExists(m_db, user)) {
            throw std::invalid_argument("No such user: " + user);
        }
        insertDevice(m_db, newDevice(id, deviceName, token), user);
        trans.commit();
        return {id, token};
    }

    bool UserStore::revokeDevice(const std::string& user, const std::string& deviceId)
    {
        std::lock_guard lock(m_mutex);
        return Statement(m_db, "DELETE FROM devices WHERE user_name = ? AND id = ?", user, deviceId).run() > 0;
    }

    std::vector<Device> UserStore::listDevices(const std::string& user)
    {
        std::lock_guard lock(m_mutex);
        if (!userExists(m_db, user)) {
            throw std::invalid_argument("No such user: " + user);
        }
        std::vector<Device> devices;
        Statement select(m_db,
                         "SELECT id, name, created_at, last_seen_at FROM devices WHERE user_name = ? "
                         "ORDER BY created_at, id",
                         user);
        while (select.step()) {
            devices.push_back({select.text(0), select.text(1), select.text(2), select.text(3)});
        }
        return devices;
    }

    std::vector<StoredUser> UserStore::exportUsers()
    {
        std::lock_guard lock(m_mutex);
        std::vector<StoredUser> users;
        Statement selectUsers(m_db, "SELECT name, created_at FROM users ORDER BY name");
        while (selectUsers.step()) {
            StoredUser user{selectUsers.text(0), selectUsers.text(1), {}};
            Statement selectDevices(m_db,
                                    "SELECT id, name, token_hash, created_at, last_seen_at FROM devices "
                                    "WHERE user_name = ? ORDER BY created_at, id",
                                    user.name);
            while (selectDevices.step()) {
                user.devices.push_back({selectDevices.text(0),
                                        selectDevices.text(1),
                                        selectDevices.text(2),
                                        selectDevices.text(3),
                                        selectDevices.text(4)});
            }
            users.push_back(std::move(user));
        }
        return users;
    }
} // namespace syncserver
