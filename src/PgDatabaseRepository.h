#pragma once

#include "Storage.h"

#include <drogon/orm/DbClient.h>
#include <drogon/orm/DbListener.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <memory>
#include <optional>

namespace syncserver
{
    class BlobStore;
    class Postgres;

    /**
     * Database metadata and history in Postgres, blobs in the bucket. Postgres decides every commit:
     * a row lock on the database replaces the in-process lock, so several instances can serve the same
     * databases. Blob keys are never reused, so a reader never sees a partly written or replaced blob:
     *   <prefix><user>/<id>/<sha256>-<random>.kdbx
     *
     * Account vaults (Namespace::Vaults) live in tables of their own, vaults and vault_versions, with the
     * empty scope (VaultScope) in place of a user, and blobs under a prefix no user name can start with:
     *   <prefix>_vaults/<uuid>/<sha256>-<random>.kdbx
     */
    class PgDatabaseRepository : public DatabaseRepository
    {
    public:
        enum class Namespace
        {
            Databases,
            Vaults,
        };

        PgDatabaseRepository(std::shared_ptr<Postgres> postgres,
                             std::shared_ptr<BlobStore> blobs,
                             unsigned historySize,
                             Namespace ns = Namespace::Databases);
        ~PgDatabaseRepository() override;

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

        // Listens for changes from every instance through Postgres LISTEN/NOTIFY
        void setChangeListener(ChangeListener listener) override;
        // Through Postgres NOTIFY, so it reaches every instance
        void broadcast(const Json::Value& message) override;
        void setBroadcastListener(BroadcastListener listener) override;

        // Deletes blobs that no row references and that are older than minAge (younger ones may belong to a
        // commit in progress). nullopt when another instance is sweeping.
        std::optional<std::size_t> sweep(std::chrono::seconds minAge = std::chrono::hours(1));
        // Sweeps every `interval` on a thread of its own, on whichever instance gets there first
        void startPeriodicSweep(std::chrono::seconds interval);

        // Copies a database with its kept versions (newest first, the current one first), keeping revisions
        // and ETags; false when it already exists
        bool
        importDatabase(const std::string& user, const std::string& id, const std::vector<Entry>& versions) override;

    private:
        std::string newBlobKey(const std::string& user, const std::string& id, const std::string& etag) const;
        void notify(drogon::orm::Transaction& trans, const std::string& user, const Json::Value& event);
        void removeBlobs(const std::vector<std::string>& keys);

        // Marks notifications of this namespace; legacy databases send none, as before
        std::string notifyNamespace() const;

        Namespace m_ns;
        std::string m_table;
        std::string m_versions;
        std::string m_owner;
        std::string m_keyPrefix;
        BroadcastListener m_broadcastListener;
        std::shared_ptr<Postgres> m_pg;
        std::shared_ptr<BlobStore> m_blobs;
        unsigned m_historySize;
        drogon::orm::DbListenerPtr m_listener;
        std::unique_ptr<trantor::EventLoopThread> m_sweeper;
    };
} // namespace syncserver
