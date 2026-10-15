#pragma once

#include <json/json.h>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncserver
{
    struct Device
    {
        std::string id;
        std::string name;
        std::string createdAt;
        // Empty until the device first connects
        std::string lastSeenAt;
    };

    // Who sent a request: the user owns the databases, the device holds the token
    struct Identity
    {
        std::string user;
        std::string deviceId;
    };

    struct PairingCode
    {
        // Eight letters and digits as "ABCD-EFGH"
        std::string code;
        std::string expiresAt;
    };

    struct PairedDevice
    {
        std::string user;
        std::string deviceId;
        std::string token;
    };

    /**
     * Users and their devices live in <dataDir>/users.json as
     *   { "users": { "<name>": { "createdAt": "...",
     *                            "devices": { "<id>": { "name": "...", "tokenHash": "<sha256 hex>",
     *                                                   "createdAt": "...", "lastSeenAt": "..." } } } } }
     *
     * Every device has its own token, so one lost device can be revoked without touching the others.
     * Only token hashes are stored; the plain token is shown once when it is created.
     * The file is re-read when it changes, so changes take effect without a restart.
     *
     * Files from before devices existed have one "tokenHash" per user; that token keeps working
     * as a device called "legacy".
     */
    class UserStore
    {
    public:
        explicit UserStore(const std::filesystem::path& dataDir);

        // Also records when the device was last seen, at most once per lastSeenInterval
        std::optional<Identity> authenticate(const std::string& token);

        // Creates a user with a first device and returns that device's token
        std::string addUser(const std::string& name, const std::string& deviceName);
        bool removeUser(const std::string& name);
        std::vector<std::string> listUsers();

        // Returns the new device's id and token
        std::pair<std::string, std::string> addDevice(const std::string& user, const std::string& deviceName);
        bool revokeDevice(const std::string& user, const std::string& deviceId);
        std::vector<Device> listDevices(const std::string& user);

        // A one-time code that a new device trades for a token of its own, so adding a device
        // doesn't mean copying a token around. createdBy is the device that asked, if any.
        PairingCode createPairingCode(const std::string& user, const std::string& createdBy = {});
        // Adds a device for the code's user; nullopt when the code is unknown, used or expired
        std::optional<PairedDevice> claimPairingCode(const std::string& code, const std::string& deviceName);
        // Upper case without separators, as stored; codes are typed by hand
        static std::string normalizePairingCode(std::string_view code);

        void setLastSeenInterval(std::chrono::seconds interval);
        void setPairingLifetime(std::chrono::seconds lifetime);

        static constexpr std::size_t MaxPairingsPerUser = 5;

    private:
        Json::Value readLocked();
        void writeLocked(const Json::Value& data);

        std::filesystem::path m_file;
        std::mutex m_mutex;
        std::optional<Json::Value> m_cache;
        std::filesystem::file_time_type m_cacheMtime;
        std::chrono::seconds m_lastSeenInterval = std::chrono::hours(1);
        std::chrono::seconds m_pairingLifetime = std::chrono::minutes(10);
    };
} // namespace syncserver
