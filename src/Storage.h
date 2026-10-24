#pragma once

#include "Config.h"

#include <json/json.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

    // What an account access token allows on a vault (SPEC.md §9); ordered so a higher role includes the lower
    enum class VaultRole
    {
        Read = 1,
        Write = 2,
        Manage = 3,
    };

    /**
     * Who sent a request. With a device token, the user owns the databases and the device holds the token.
     * With an account access token (JWT), subject is "user:<uuid>" or "sa:<uuid>" and vaults lists every
     * vault the token opens with its role; user and deviceId are empty.
     */
    struct Identity
    {
        std::string user;
        std::string deviceId;

        std::string subject;
        std::map<std::string, VaultRole> vaults;
        // The token's exp, seconds since the Unix epoch; 0 for device tokens
        int64_t expiresAt = 0;

        bool isAccount() const
        {
            return !subject.empty();
        }
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

    // A device as stored, with the hash of its token, for copying users from one storage to another
    struct StoredDevice
    {
        std::string id;
        std::string name;
        std::string tokenHash;
        std::string createdAt;
        std::string lastSeenAt;
    };

    struct StoredUser
    {
        std::string name;
        std::string createdAt;
        std::vector<StoredDevice> devices;
    };

    struct DatabaseMeta
    {
        std::string id;
        int64_t revision = 0;
        // Quoted SHA-256 of the stored blob
        std::string etag;
        uint64_t size = 0;
        std::string updatedAt;

        Json::Value toJson() const;
    };

    /**
     * Users, their devices and pairing codes. Every device has its own token, so one lost device can be
     * revoked without touching the others. Only hashes of tokens and pairing codes are stored; the plain
     * token is shown once when it is created.
     */
    class UserRepository
    {
    public:
        virtual ~UserRepository() = default;

        // Also records when the device was last seen, at most once per lastSeenInterval
        virtual std::optional<Identity> authenticate(const std::string& token) = 0;

        // Creates a user with a first device and returns that device's token
        virtual std::string addUser(const std::string& name, const std::string& deviceName) = 0;
        virtual bool removeUser(const std::string& name) = 0;
        virtual std::vector<std::string> listUsers() = 0;

        // Returns the new device's id and token
        virtual std::pair<std::string, std::string> addDevice(const std::string& user,
                                                              const std::string& deviceName) = 0;
        virtual bool revokeDevice(const std::string& user, const std::string& deviceId) = 0;
        virtual std::vector<Device> listDevices(const std::string& user) = 0;

        // A one-time code that a new device trades for a token of its own, so adding a device
        // doesn't mean copying a token around. createdBy is the device that asked, if any.
        virtual PairingCode createPairingCode(const std::string& user, const std::string& createdBy = {}) = 0;
        // Adds a device for the code's user; nullopt when the code is unknown, used or expired
        virtual std::optional<PairedDevice> claimPairingCode(const std::string& code,
                                                             const std::string& deviceName) = 0;

        void setLastSeenInterval(std::chrono::seconds interval);
        void setPairingLifetime(std::chrono::seconds lifetime);

        // Upper case without separators, as stored; codes are typed by hand
        static std::string normalizePairingCode(std::string_view code);

        static constexpr std::size_t MaxPairingsPerUser = 5;

    protected:
        static std::string newToken();
        static std::string newDeviceId();
        // Normalised, see formatPairingCode() for how it is shown
        static std::string newPairingCode();
        static std::string formatPairingCode(const std::string& normalized);
        static void validateUserName(const std::string& name);
        static void validateDeviceName(const std::string& name);

        std::chrono::seconds m_lastSeenInterval = std::chrono::hours(1);
        std::chrono::seconds m_pairingLifetime = std::chrono::minutes(10);
    };

    /**
     * Opaque database blobs with their metadata and a bounded history of previous versions. The
     * server never decrypts anything; OpenVireo uploads the already-encrypted .kdbx file.
     */
    class DatabaseRepository
    {
    public:
        // Runs while the database is locked against other writers, with its current metadata, and may throw to abort
        using Precondition = std::function<void(const std::optional<DatabaseMeta>&)>;
        // Called with the user and the event for every committed change, see changeEvent()
        using ChangeListener = std::function<void(const std::string& user, const Json::Value& event)>;

        struct Entry
        {
            DatabaseMeta meta;
            std::string data;
        };

        struct WriteResult
        {
            DatabaseMeta meta;
            bool created = false;
        };

        virtual ~DatabaseRepository() = default;

        virtual std::optional<DatabaseMeta> meta(const std::string& user, const std::string& id) = 0;
        virtual std::optional<Entry> read(const std::string& user, const std::string& id) = 0;
        virtual std::vector<DatabaseMeta> list(const std::string& user) = 0;
        virtual WriteResult write(const std::string& user,
                                  const std::string& id,
                                  std::string_view data,
                                  const Precondition& precondition) = 0;
        virtual bool remove(const std::string& user, const std::string& id, const Precondition& precondition) = 0;

        // The current version and the kept previous ones, newest first; nullopt when the database doesn't exist
        virtual std::optional<std::vector<DatabaseMeta>> history(const std::string& user, const std::string& id) = 0;
        // One version, the current one or a kept previous one
        virtual std::optional<Entry> readRevision(const std::string& user, const std::string& id, int64_t revision) = 0;

        // Changes from every instance sharing this storage, not only this one
        virtual void setChangeListener(ChangeListener listener) = 0;

        // Hands a message to the broadcast listener of every instance sharing this storage (this one included),
        // e.g. an access change from the auth server for streams connected to other instances. Best effort.
        using BroadcastListener = std::function<void(const Json::Value& message)>;
        virtual void broadcast(const Json::Value& message) = 0;
        // Set before setChangeListener()
        virtual void setBroadcastListener(BroadcastListener listener) = 0;

        // Copies a database with its kept versions (the current one first, then older ones), keeping revisions,
        // ETags and times; false when it already exists. Sends no change event.
        virtual bool
        importDatabase(const std::string& user, const std::string& id, const std::vector<Entry>& versions) = 0;

        // {"id", "etag", "revision"} after an upload, {"id", "deleted": true} after a delete
        static Json::Value changeEvent(const DatabaseMeta& meta);
        static Json::Value deletedEvent(const std::string& id);
    };

    struct Storage
    {
        std::shared_ptr<UserRepository> users;
        // Legacy databases, per user (databases/{id} of a device token's user)
        std::shared_ptr<DatabaseRepository> databases;
        // Account vaults (vaults/{id}), in a namespace of their own that never shares a key with a user's
        // databases. Vault ids are global, so every call passes VaultScope as the user.
        std::shared_ptr<DatabaseRepository> vaults;
        // Something to keep alive with the repositories, such as shared connections
        std::shared_ptr<void> backend;
        // Human-readable, for the startup message
        std::string description;
    };

    // The "user" of every vault in Storage::vaults
    inline const std::string VaultScope;

    // Postgres and S3 when SYNC_DATABASE_URL is set, otherwise the data directory
    Storage openStorage(const Config& config);
} // namespace syncserver
