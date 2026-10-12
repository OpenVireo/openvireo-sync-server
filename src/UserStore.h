#pragma once

#include <json/json.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace syncserver
{
    /**
     * Users live in <dataDir>/users.json as { "users": { "<name>": { "tokenHash": "<sha256 hex>" } } }.
     * Only token hashes are stored; the plain token is shown once when it is created.
     * The file is re-read when it changes, so users can be added without a restart.
     */
    class UserStore
    {
    public:
        explicit UserStore(const std::filesystem::path& dataDir);

        // Returns the user name that owns the token
        std::optional<std::string> authenticate(const std::string& token);

        // Creates the user or rotates its token. Returns the new plain token.
        std::string setUser(const std::string& name);
        bool removeUser(const std::string& name);
        std::vector<std::string> listUsers();

    private:
        Json::Value readLocked();
        void writeLocked(const Json::Value& data);

        std::filesystem::path m_file;
        std::mutex m_mutex;
        std::optional<Json::Value> m_cache;
        std::filesystem::file_time_type m_cacheMtime;
    };
} // namespace syncserver
