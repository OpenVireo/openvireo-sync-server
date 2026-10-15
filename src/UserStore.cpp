#include "UserStore.h"

#include "Util.h"

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace fs = std::filesystem;

namespace syncserver
{
    namespace
    {
        std::string generateToken()
        {
            unsigned char bytes[32];
            if (!drogon::utils::secureRandomBytes(bytes, sizeof(bytes))) {
                throw std::runtime_error("Cannot generate a random token");
            }
            return drogon::utils::base64EncodeUnpadded(bytes, sizeof(bytes), true);
        }

        std::string generateDeviceId()
        {
            unsigned char bytes[6];
            if (!drogon::utils::secureRandomBytes(bytes, sizeof(bytes))) {
                throw std::runtime_error("Cannot generate a device id");
            }
            return "d-" + drogon::utils::binaryStringToHex(bytes, sizeof(bytes), true);
        }

        // No 0/O or 1/I, which are easy to mix up when reading a code off another screen
        constexpr std::string_view PAIRING_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        constexpr std::size_t PAIRING_CODE_LENGTH = 8;

        std::string generatePairingCode()
        {
            unsigned char bytes[PAIRING_CODE_LENGTH];
            if (!drogon::utils::secureRandomBytes(bytes, sizeof(bytes))) {
                throw std::runtime_error("Cannot generate a pairing code");
            }
            std::string code;
            for (auto byte : bytes) {
                // 256 is a multiple of the alphabet's 32 letters, so every letter is equally likely
                code += PAIRING_ALPHABET[byte % PAIRING_ALPHABET.size()];
            }
            return code;
        }

        // Drops a user's expired codes; returns whether any were dropped
        bool dropExpiredPairings(Json::Value& user)
        {
            auto& pairings = user["pairings"];
            if (!pairings.isObject()) {
                user.removeMember("pairings");
                return false;
            }
            const auto now = isoTimestamp();
            bool dropped = false;
            for (const auto& hash : pairings.getMemberNames()) {
                if (pairings[hash]["expiresAt"].asString() <= now) {
                    pairings.removeMember(hash);
                    dropped = true;
                }
            }
            if (pairings.empty()) {
                user.removeMember("pairings");
            }
            return dropped;
        }

        void validateDeviceName(const std::string& name)
        {
            const bool printable = std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 0x20; });
            if (name.empty() || name.size() > 64 || !printable) {
                throw std::invalid_argument("Invalid device name (1 to 64 characters, no control characters)");
            }
        }

        Json::Value newDevice(const std::string& name, const std::string& token)
        {
            Json::Value device;
            device["name"] = name;
            device["tokenHash"] = sha256Hex(token);
            device["createdAt"] = isoTimestamp();
            device["lastSeenAt"] = "";
            return device;
        }

        // Files from before devices existed: one token per user, kept as a device called "legacy"
        void migrateUsers(Json::Value& users)
        {
            for (const auto& name : users.getMemberNames()) {
                auto& user = users[name];
                if (!user["devices"].isObject()) {
                    user["devices"] = Json::objectValue;
                }
                if (user.isMember("tokenHash")) {
                    Json::Value device;
                    device["name"] = "legacy";
                    device["tokenHash"] = user["tokenHash"];
                    device["createdAt"] = user["createdAt"];
                    device["lastSeenAt"] = "";
                    user["devices"]["legacy"] = device;
                    user.removeMember("tokenHash");
                }
            }
        }

        std::string toJsonString(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "  ";
            return Json::writeString(builder, value);
        }
    } // namespace

    UserStore::UserStore(const fs::path& dataDir)
        : m_file(dataDir / "users.json")
    {
    }

    Json::Value UserStore::readLocked()
    {
        std::error_code ec;
        auto mtime = fs::last_write_time(m_file, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                Json::Value empty;
                empty["users"] = Json::objectValue;
                return empty;
            }
            throw std::system_error(ec, "Cannot read " + m_file.string());
        }

        if (!m_cache || mtime != m_cacheMtime) {
            Json::Value data;
            Json::CharReaderBuilder builder;
            std::string errors;
            auto text = readFile(m_file);
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if (!reader->parse(text.data(), text.data() + text.size(), &data, &errors) || !data.isObject()) {
                throw std::runtime_error("Invalid " + m_file.string() + ": " + errors);
            }
            if (!data["users"].isObject()) {
                data["users"] = Json::objectValue;
            }
            migrateUsers(data["users"]);
            m_cache = std::move(data);
            m_cacheMtime = mtime;
        }
        return *m_cache;
    }

    void UserStore::writeLocked(const Json::Value& data)
    {
        ensurePrivateDirectory(m_file.parent_path());
        atomicWriteFile(m_file, toJsonString(data));
        m_cache.reset();
    }

    void UserStore::setLastSeenInterval(std::chrono::seconds interval)
    {
        m_lastSeenInterval = interval;
    }

    std::optional<Identity> UserStore::authenticate(const std::string& token)
    {
        if (token.empty()) {
            return std::nullopt;
        }
        auto candidate = sha256Hex(token);

        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        auto& users = data["users"];
        std::optional<Identity> match;
        // Compare against every device of every user, each comparison in constant time
        for (const auto& user : users.getMemberNames()) {
            auto& devices = users[user]["devices"];
            for (const auto& id : devices.getMemberNames()) {
                if (constantTimeEquals(devices[id]["tokenHash"].asString(), candidate)) {
                    match = Identity{user, id};
                }
            }
        }

        // ISO timestamps in UTC sort like the times they stand for
        if (match) {
            auto& device = users[match->user]["devices"][match->deviceId];
            auto staleBefore = isoTimestamp(std::chrono::system_clock::now() - m_lastSeenInterval);
            if (device["lastSeenAt"].asString() < staleBefore) {
                device["lastSeenAt"] = isoTimestamp();
                writeLocked(data);
            }
        }
        return match;
    }

    void UserStore::setPairingLifetime(std::chrono::seconds lifetime)
    {
        m_pairingLifetime = lifetime;
    }

    std::string UserStore::normalizePairingCode(std::string_view code)
    {
        std::string normalized;
        for (unsigned char c : code) {
            if (c == '-' || std::isspace(c)) {
                continue;
            }
            normalized += static_cast<char>(std::toupper(c));
        }
        return normalized;
    }

    PairingCode UserStore::createPairingCode(const std::string& user, const std::string& createdBy)
    {
        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        if (!data["users"].isMember(user)) {
            throw std::invalid_argument("No such user: " + user);
        }
        auto& entry = data["users"][user];
        dropExpiredPairings(entry);

        // Keep a user's waiting codes few, dropping the ones that expire first
        auto& pairings = entry["pairings"];
        while (pairings.isObject() && pairings.size() >= MaxPairingsPerUser) {
            auto names = pairings.getMemberNames();
            auto oldest = *std::min_element(names.begin(), names.end(), [&](const auto& a, const auto& b) {
                return pairings[a]["expiresAt"].asString() < pairings[b]["expiresAt"].asString();
            });
            pairings.removeMember(oldest);
        }

        auto code = generatePairingCode();
        auto expiresAt = isoTimestamp(std::chrono::system_clock::now() + m_pairingLifetime);
        Json::Value pairing;
        pairing["expiresAt"] = expiresAt;
        pairing["createdBy"] = createdBy;
        entry["pairings"][sha256Hex(code)] = pairing;
        writeLocked(data);
        return {code.substr(0, 4) + "-" + code.substr(4), expiresAt};
    }

    std::optional<PairedDevice> UserStore::claimPairingCode(const std::string& code, const std::string& deviceName)
    {
        validateDeviceName(deviceName);
        auto candidate = sha256Hex(normalizePairingCode(code));

        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        auto& users = data["users"];
        bool changed = false;
        std::optional<std::pair<std::string, std::string>> match;
        for (const auto& name : users.getMemberNames()) {
            auto& user = users[name];
            changed = dropExpiredPairings(user) || changed;
            if (!user.isMember("pairings")) {
                continue;
            }
            auto& pairings = user["pairings"];
            for (const auto& hash : pairings.getMemberNames()) {
                if (constantTimeEquals(hash, candidate)) {
                    match = {name, hash};
                }
            }
        }
        if (!match) {
            if (changed) {
                writeLocked(data);
            }
            return std::nullopt;
        }

        // Each code adds one device only
        auto& user = users[match->first];
        user["pairings"].removeMember(match->second);
        if (user["pairings"].empty()) {
            user.removeMember("pairings");
        }
        auto id = generateDeviceId();
        auto token = generateToken();
        user["devices"][id] = newDevice(deviceName, token);
        writeLocked(data);
        return PairedDevice{match->first, id, token};
    }

    std::string UserStore::addUser(const std::string& name, const std::string& deviceName)
    {
        if (!isValidName(name)) {
            throw std::invalid_argument("Invalid user name \"" + name
                                        + "\" (allowed: letters, digits, \".\", \"_\", \"-\"; max 64 chars)");
        }
        validateDeviceName(deviceName);
        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        if (data["users"].isMember(name)) {
            throw std::invalid_argument("User \"" + name + "\" already exists. Add a device with: devices add " + name
                                        + " <device-name>");
        }
        Json::Value user;
        user["createdAt"] = isoTimestamp();
        user["devices"] = Json::objectValue;
        auto token = generateToken();
        user["devices"][generateDeviceId()] = newDevice(deviceName, token);
        data["users"][name] = user;
        writeLocked(data);
        return token;
    }

    bool UserStore::removeUser(const std::string& name)
    {
        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        if (!data["users"].isMember(name)) {
            return false;
        }
        data["users"].removeMember(name);
        writeLocked(data);
        return true;
    }

    std::vector<std::string> UserStore::listUsers()
    {
        std::lock_guard lock(m_mutex);
        auto names = readLocked()["users"].getMemberNames();
        std::sort(names.begin(), names.end());
        return names;
    }

    std::pair<std::string, std::string> UserStore::addDevice(const std::string& user, const std::string& deviceName)
    {
        validateDeviceName(deviceName);
        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        if (!data["users"].isMember(user)) {
            throw std::invalid_argument("No such user: " + user);
        }
        auto id = generateDeviceId();
        auto token = generateToken();
        data["users"][user]["devices"][id] = newDevice(deviceName, token);
        writeLocked(data);
        return {id, token};
    }

    bool UserStore::revokeDevice(const std::string& user, const std::string& deviceId)
    {
        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        // Check before indexing: operator[] would create the user
        if (!data["users"].isMember(user) || !data["users"][user]["devices"].isMember(deviceId)) {
            return false;
        }
        data["users"][user]["devices"].removeMember(deviceId);
        writeLocked(data);
        return true;
    }

    std::vector<Device> UserStore::listDevices(const std::string& user)
    {
        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        if (!data["users"].isMember(user)) {
            throw std::invalid_argument("No such user: " + user);
        }
        const auto& devices = data["users"][user]["devices"];
        std::vector<Device> result;
        for (const auto& id : devices.getMemberNames()) {
            const auto& device = devices[id];
            result.push_back({id,
                              device["name"].asString(),
                              device["createdAt"].asString(),
                              device["lastSeenAt"].asString()});
        }
        std::sort(result.begin(), result.end(), [](const Device& a, const Device& b) {
            return a.createdAt < b.createdAt;
        });
        return result;
    }
} // namespace syncserver
