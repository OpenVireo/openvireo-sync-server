#include "UserStore.h"

#include "Util.h"

#include <drogon/utils/Utilities.h>

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

    std::optional<std::string> UserStore::authenticate(const std::string& token)
    {
        if (token.empty()) {
            return std::nullopt;
        }
        auto candidate = sha256Hex(token);

        std::lock_guard lock(m_mutex);
        auto users = readLocked()["users"];
        std::optional<std::string> match;
        // Compare against every user, each comparison in constant time
        for (const auto& name : users.getMemberNames()) {
            if (constantTimeEquals(users[name]["tokenHash"].asString(), candidate)) {
                match = name;
            }
        }
        return match;
    }

    std::string UserStore::setUser(const std::string& name)
    {
        if (!isValidName(name)) {
            throw std::invalid_argument("Invalid user name \"" + name
                                        + "\" (allowed: letters, digits, \".\", \"_\", \"-\"; max 64 chars)");
        }
        std::lock_guard lock(m_mutex);
        auto data = readLocked();
        auto token = generateToken();
        Json::Value user;
        user["tokenHash"] = sha256Hex(token);
        user["createdAt"] = isoTimestamp();
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
} // namespace syncserver
