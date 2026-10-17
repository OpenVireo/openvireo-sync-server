#pragma once

#include "Storage.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncserver
{
    /**
     * Stores database blobs on disk.
     *
     * Layout:
     *   <dataDir>/databases/<user>/<id>.kdbx        current version
     *   <dataDir>/databases/<user>/<id>.meta.json   { id, revision, etag, size, updatedAt }
     *   <dataDir>/databases/<user>/.history/<id>/<revision>.kdbx   previous versions
     *   <dataDir>/databases/<user>/.history/<id>/<revision>.json   their metadata
     */
    class DatabaseStore : public DatabaseRepository
    {
    public:
        DatabaseStore(const std::filesystem::path& dataDir, unsigned historySize);

        std::optional<DatabaseMeta> meta(const std::string& user, const std::string& id) override;
        std::optional<Entry> read(const std::string& user, const std::string& id) override;
        std::vector<DatabaseMeta> list(const std::string& user) override;
        WriteResult write(const std::string& user,
                          const std::string& id,
                          std::string_view data,
                          const Precondition& precondition) override;
        bool remove(const std::string& user, const std::string& id, const Precondition& precondition) override;

        std::optional<std::vector<DatabaseMeta>> history(const std::string& user, const std::string& id) override;
        std::optional<Entry> readRevision(const std::string& user, const std::string& id, int64_t revision) override;

        // Only this process writes the directory, so it hears about every change itself
        void setChangeListener(ChangeListener listener) override;

        // Users with a databases folder, including removed users whose databases were kept
        std::vector<std::string> owners() const;

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
        ChangeListener m_listener;
    };
} // namespace syncserver
