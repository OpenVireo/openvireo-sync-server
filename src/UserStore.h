#pragma once

#include "Storage.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace syncserver
{
    /**
     * Users, their devices and pairing codes in <dataDir>/users.sqlite, in the same tables as in Postgres.
     * The server and the users and devices commands use the file at the same time; SQLite's locking keeps
     * their changes apart, so changes take effect without a restart.
     *
     * A users.json from an older server is copied in when the file is first opened and then renamed to
     * users.json.imported. Its per-user tokens from before devices existed keep working as a device
     * called "legacy".
     */
    class UserStore : public UserRepository
    {
    public:
        explicit UserStore(const std::filesystem::path& dataDir);
        ~UserStore() override;
        UserStore(const UserStore&) = delete;
        UserStore& operator=(const UserStore&) = delete;

        std::optional<Identity> authenticate(const std::string& token) override;

        std::string addUser(const std::string& name, const std::string& deviceName) override;
        bool removeUser(const std::string& name) override;
        std::vector<std::string> listUsers() override;

        std::pair<std::string, std::string> addDevice(const std::string& user, const std::string& deviceName) override;
        bool revokeDevice(const std::string& user, const std::string& deviceId) override;
        std::vector<Device> listDevices(const std::string& user) override;

        PairingCode createPairingCode(const std::string& user, const std::string& createdBy = {}) override;
        std::optional<PairedDevice> claimPairingCode(const std::string& code, const std::string& deviceName) override;

        // Every user with their devices and token hashes, for import into Postgres
        std::vector<StoredUser> exportUsers();

    private:
        void migrate();
        void importJson();

        std::filesystem::path m_dataDir;
        sqlite3* m_db = nullptr;
        // One connection, used by one thread at a time
        std::mutex m_mutex;
    };
} // namespace syncserver
