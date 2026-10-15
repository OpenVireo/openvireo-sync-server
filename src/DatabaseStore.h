#pragma once

#include <json/json.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncserver
{
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
     * Stores opaque database blobs on disk. The server never decrypts anything;
     * KeePassXC uploads the already-encrypted .kdbx file.
     *
     * Layout:
     *   <dataDir>/databases/<user>/<id>.kdbx        current version
     *   <dataDir>/databases/<user>/<id>.meta.json   { id, revision, etag, size, updatedAt }
     *   <dataDir>/databases/<user>/.history/<id>/<revision>.kdbx   previous versions
     *   <dataDir>/databases/<user>/.history/<id>/<revision>.json   their metadata
     */
    class DatabaseStore
    {
    public:
        // Runs under the database's lock with its current metadata and may throw to abort
        using Precondition = std::function<void(const std::optional<DatabaseMeta>&)>;

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

        DatabaseStore(const std::filesystem::path& dataDir, unsigned historySize);

        std::optional<DatabaseMeta> meta(const std::string& user, const std::string& id);
        std::optional<Entry> read(const std::string& user, const std::string& id);
        std::vector<DatabaseMeta> list(const std::string& user);
        WriteResult
        write(const std::string& user, const std::string& id, std::string_view data, const Precondition& precondition);
        bool remove(const std::string& user, const std::string& id, const Precondition& precondition);

        // The current version and the kept previous ones, newest first; nullopt when the database doesn't exist
        std::optional<std::vector<DatabaseMeta>> history(const std::string& user, const std::string& id);
        // One version, the current one or a kept previous one
        std::optional<Entry> readRevision(const std::string& user, const std::string& id, int64_t revision);

    private:
        struct Paths
        {
            std::filesystem::path dir;
            std::filesystem::path blob;
            std::filesystem::path meta;
            std::filesystem::path history;
        };

        Paths paths(const std::string& user, const std::string& id) const;
        std::optional<DatabaseMeta> readMeta(const std::string& user, const std::string& id) const;
        // Metadata of a kept previous version, from its .json or, for versions kept before those existed, the file
        static DatabaseMeta historyMeta(const std::string& id, const std::filesystem::path& blob);
        // Serialises operations on one database so check-then-write is atomic
        std::shared_ptr<std::mutex> lockFor(const std::string& user, const std::string& id);
        void pruneHistory(const std::filesystem::path& historyDir) const;

        std::filesystem::path m_root;
        unsigned m_historySize;
        std::mutex m_locksMutex;
        std::map<std::string, std::shared_ptr<std::mutex>> m_locks;
    };
} // namespace syncserver
