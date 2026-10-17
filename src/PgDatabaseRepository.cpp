#include "PgDatabaseRepository.h"

#include "BlobStore.h"
#include "Postgres.h"
#include "Util.h"

#include <drogon/utils/Utilities.h>
#include <trantor/utils/Logger.h>

#include <set>
#include <stdexcept>

using drogon::orm::Row;
using drogon::orm::Transaction;

namespace syncserver
{
    namespace
    {
        const std::string COLUMNS = "id, revision, etag, size, blob_key, " SYNC_PG_ISO("updated_at") " AS updated_at";

        struct Stored
        {
            DatabaseMeta meta;
            std::string blobKey;
        };

        Stored fromRow(const Row& row)
        {
            Stored stored;
            stored.meta.id = row["id"].as<std::string>();
            stored.meta.revision = row["revision"].as<int64_t>();
            stored.meta.etag = row["etag"].as<std::string>();
            stored.meta.size = static_cast<uint64_t>(row["size"].as<int64_t>());
            stored.meta.updatedAt = row["updated_at"].as<std::string>();
            stored.blobKey = row["blob_key"].as<std::string>();
            return stored;
        }

        std::string compactJson(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }
    } // namespace

    PgDatabaseRepository::PgDatabaseRepository(std::shared_ptr<Postgres> postgres,
                                               std::shared_ptr<BlobStore> blobs,
                                               unsigned historySize,
                                               Namespace ns)
        : m_ns(ns)
        , m_table(ns == Namespace::Vaults ? "vaults" : "databases")
        , m_versions(ns == Namespace::Vaults ? "vault_versions" : "versions")
        , m_owner(ns == Namespace::Vaults ? "scope" : "user_name")
        // User names start with a letter or digit, so "_vaults/" is never a user's folder
        , m_keyPrefix(ns == Namespace::Vaults ? "_vaults/" : "")
        , m_pg(std::move(postgres))
        , m_blobs(std::move(blobs))
        , m_historySize(historySize)
    {
    }

    PgDatabaseRepository::~PgDatabaseRepository()
    {
        if (m_sweeper) {
            m_sweeper->getLoop()->quit();
            m_sweeper->wait();
        }
    }

    std::string
    PgDatabaseRepository::newBlobKey(const std::string& user, const std::string& id, const std::string& etag) const
    {
        // A random part makes every upload's key its own, so pruning one version can never delete
        // the blob of another with the same contents
        auto hash = etag.substr(1, etag.size() - 2);
        return m_keyPrefix + (user.empty() ? "" : user + "/") + id + "/" + hash + "-"
               + drogon::utils::secureRandomString(16) + ".kdbx";
    }

    void PgDatabaseRepository::notify(Transaction& trans, const std::string& user, const Json::Value& event)
    {
        Json::Value payload;
        payload["user"] = user;
        payload["event"] = event;
        if (auto ns = notifyNamespace(); !ns.empty()) {
            payload["ns"] = ns;
        }
        // Postgres only delivers it if the transaction commits
        trans.execSqlSync("SELECT pg_notify($1, $2)", m_pg->channel(), compactJson(payload));
    }

    std::string PgDatabaseRepository::notifyNamespace() const
    {
        return m_ns == Namespace::Vaults ? "vaults" : "";
    }

    void PgDatabaseRepository::broadcast(const Json::Value& message)
    {
        Json::Value payload;
        payload["ns"] = notifyNamespace().empty() ? "databases" : notifyNamespace();
        payload["broadcast"] = message;
        auto text = compactJson(payload);
        // NOTIFY payloads must stay under 8000 bytes
        if (text.size() >= 7900) {
            throw std::invalid_argument("The message is too large to broadcast");
        }
        m_pg->client().execSqlSync("SELECT pg_notify($1, $2)", m_pg->channel(), text);
    }

    void PgDatabaseRepository::setBroadcastListener(BroadcastListener listener)
    {
        m_broadcastListener = std::move(listener);
    }

    void PgDatabaseRepository::removeBlobs(const std::vector<std::string>& keys)
    {
        for (const auto& key : keys) {
            try {
                m_blobs->remove(key);
            } catch (const std::exception& e) {
                // Left for the sweep
                LOG_WARN << e.what();
            }
        }
    }

    std::optional<DatabaseMeta> PgDatabaseRepository::meta(const std::string& user, const std::string& id)
    {
        auto rows = m_pg->client().execSqlSync(
            "SELECT " + COLUMNS + " FROM " + m_table + " WHERE " + m_owner + " = $1 AND id = $2", user, id);
        if (rows.empty()) {
            return std::nullopt;
        }
        return fromRow(rows[0]).meta;
    }

    std::optional<DatabaseRepository::Entry> PgDatabaseRepository::read(const std::string& user, const std::string& id)
    {
        // A newer upload may prune the blob between reading the row and the blob; then read the row again
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto rows = m_pg->client().execSqlSync(
                "SELECT " + COLUMNS + " FROM " + m_table + " WHERE " + m_owner + " = $1 AND id = $2", user, id);
            if (rows.empty()) {
                return std::nullopt;
            }
            auto stored = fromRow(rows[0]);
            if (auto data = m_blobs->get(stored.blobKey)) {
                return Entry{std::move(stored.meta), std::move(*data)};
            }
        }
        throw std::runtime_error("The file of database " + user + "/" + id + " is missing from the bucket");
    }

    std::vector<DatabaseMeta> PgDatabaseRepository::list(const std::string& user)
    {
        std::vector<DatabaseMeta> metas;
        for (const auto& row : m_pg->client().execSqlSync(
                 "SELECT " + COLUMNS + " FROM " + m_table + " WHERE " + m_owner + " = $1 ORDER BY id", user)) {
            metas.push_back(fromRow(row).meta);
        }
        return metas;
    }

    DatabaseRepository::WriteResult PgDatabaseRepository::write(const std::string& user,
                                                                const std::string& id,
                                                                std::string_view data,
                                                                const Precondition& precondition)
    {
        const auto etag = "\"" + sha256Hex(data) + "\"";
        const auto key = newBlobKey(user, id, etag);
        // Uploaded before the transaction, so the row lock is held only for the database's own work
        m_blobs->put(key, data);

        WriteResult result;
        std::vector<std::string> pruned;
        bool committing = false;
        try {
            m_pg->transaction([&](Transaction& trans) {
                const auto updatedAt = isoTimestamp();
                // A second round when another writer created the database between our SELECT and INSERT
                for (int attempt = 0;; ++attempt) {
                    // The row lock makes check-and-commit atomic across instances
                    auto rows = trans.execSqlSync("SELECT " + COLUMNS + " FROM " + m_table + " WHERE " + m_owner
                                                      + " = $1 AND id = $2 FOR UPDATE",
                                                  user,
                                                  id);
                    std::optional<Stored> current;
                    if (!rows.empty()) {
                        current = fromRow(rows[0]);
                    }
                    precondition(current ? std::optional<DatabaseMeta>(current->meta) : std::nullopt);

                    result.meta.id = id;
                    result.meta.etag = etag;
                    result.meta.size = data.size();
                    result.meta.updatedAt = updatedAt;
                    if (current) {
                        if (m_historySize > 0) {
                            trans.execSqlSync("INSERT INTO " + m_versions
                                                  + " "
                                                    "SELECT "
                                                  + m_owner
                                                  + ", id, revision, etag, size, blob_key, updated_at "
                                                    "FROM "
                                                  + m_table + " WHERE " + m_owner + " = $1 AND id = $2",
                                              user,
                                              id);
                        } else {
                            pruned.push_back(current->blobKey);
                        }
                        trans.execSqlSync("UPDATE " + m_table
                                              + " SET revision = $3, etag = $4, size = $5, blob_key = $6, "
                                                "updated_at = $7::timestamptz WHERE "
                                              + m_owner + " = $1 AND id = $2",
                                          user,
                                          id,
                                          current->meta.revision + 1,
                                          etag,
                                          static_cast<int64_t>(data.size()),
                                          key,
                                          updatedAt);
                        result.meta.revision = current->meta.revision + 1;
                        result.created = false;
                        break;
                    }
                    auto inserted =
                        trans.execSqlSync("INSERT INTO " + m_table + " (" + m_owner
                                              + ", id, revision, etag, size, blob_key, updated_at) "
                                                "VALUES ($1, $2, 1, $3, $4, $5, $6::timestamptz) ON CONFLICT ("
                                              + m_owner + ", id) DO NOTHING",
                                          user,
                                          id,
                                          etag,
                                          static_cast<int64_t>(data.size()),
                                          key,
                                          updatedAt);
                    if (inserted.affectedRows() == 1) {
                        result.meta.revision = 1;
                        result.created = true;
                        break;
                    }
                    if (attempt > 0) {
                        throw std::runtime_error("Cannot create database " + user + "/" + id);
                    }
                }

                if (m_historySize > 0) {
                    auto old = trans.execSqlSync("DELETE FROM " + m_versions + " WHERE " + m_owner
                                                     + " = $1 AND id = $2 AND revision NOT IN ("
                                                       "SELECT revision FROM "
                                                     + m_versions + " WHERE " + m_owner
                                                     + " = $1 AND id = $2 "
                                                       "ORDER BY revision DESC LIMIT $3) RETURNING blob_key",
                                                 user,
                                                 id,
                                                 static_cast<int64_t>(m_historySize));
                    for (const auto& row : old) {
                        pruned.push_back(row["blob_key"].as<std::string>());
                    }
                }
                notify(trans, user, changeEvent(result.meta));
                committing = true;
            });
        } catch (...) {
            // Nothing references the new blob unless the commit may have gone through; then the sweep decides
            if (!committing) {
                removeBlobs({key});
            }
            throw;
        }
        removeBlobs(pruned);
        return result;
    }

    bool PgDatabaseRepository::remove(const std::string& user, const std::string& id, const Precondition& precondition)
    {
        bool removed = false;
        std::vector<std::string> keys;
        m_pg->transaction([&](Transaction& trans) {
            auto rows = trans.execSqlSync("SELECT " + COLUMNS + " FROM " + m_table + " WHERE " + m_owner
                                              + " = $1 AND id = $2 FOR UPDATE",
                                          user,
                                          id);
            std::optional<DatabaseMeta> current;
            if (!rows.empty()) {
                current = fromRow(rows[0]).meta;
            }
            precondition(current);
            if (!current) {
                return;
            }
            for (const auto& table : {m_table, m_versions}) {
                auto deleted = trans.execSqlSync(
                    "DELETE FROM " + table + " WHERE " + m_owner + " = $1 AND id = $2 RETURNING blob_key", user, id);
                for (const auto& row : deleted) {
                    keys.push_back(row["blob_key"].as<std::string>());
                }
            }
            notify(trans, user, deletedEvent(id));
            removed = true;
        });
        removeBlobs(keys);
        return removed;
    }

    std::optional<std::vector<DatabaseMeta>> PgDatabaseRepository::history(const std::string& user,
                                                                           const std::string& id)
    {
        auto rows = m_pg->client().execSqlSync("SELECT " + COLUMNS + ", 0 AS kept FROM " + m_table + " WHERE " + m_owner
                                                   + " = $1 AND id = $2 "
                                                     "UNION ALL SELECT "
                                                   + COLUMNS + ", 1 AS kept FROM " + m_versions + " WHERE " + m_owner
                                                   + " = $1 AND id = $2 "
                                                     "ORDER BY revision DESC",
                                               user,
                                               id);
        bool exists = false;
        std::vector<DatabaseMeta> versions;
        for (const auto& row : rows) {
            exists = exists || row["kept"].as<int>() == 0;
            versions.push_back(fromRow(row).meta);
        }
        if (!exists) {
            return std::nullopt;
        }
        return versions;
    }

    std::optional<DatabaseRepository::Entry>
    PgDatabaseRepository::readRevision(const std::string& user, const std::string& id, int64_t revision)
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto rows = m_pg->client().execSqlSync("SELECT " + COLUMNS + " FROM " + m_table + " WHERE " + m_owner
                                                       + " = $1 AND id = $2 AND revision = $3 "
                                                         "UNION ALL SELECT "
                                                       + COLUMNS + " FROM " + m_versions + " WHERE " + m_owner
                                                       + " = $1 AND id = $2 AND revision = $3 "
                                                         "AND EXISTS (SELECT 1 FROM "
                                                       + m_table + " WHERE " + m_owner + " = $1 AND id = $2)",
                                                   user,
                                                   id,
                                                   revision);
            if (rows.empty()) {
                return std::nullopt;
            }
            auto stored = fromRow(rows[0]);
            if (auto data = m_blobs->get(stored.blobKey)) {
                return Entry{std::move(stored.meta), std::move(*data)};
            }
        }
        // Pruned by a newer upload in the meantime
        return std::nullopt;
    }

    void PgDatabaseRepository::setChangeListener(ChangeListener listener)
    {
        m_listener = drogon::orm::DbListener::newPgListener(m_pg->connectionInfo());
        if (!m_listener) {
            throw std::runtime_error("This Drogon build has no PostgreSQL listener");
        }
        m_listener->listen(
            m_pg->channel(),
            [listener = std::move(listener), broadcastListener = m_broadcastListener, ns = notifyNamespace()](
                const std::string&, const std::string& payload) {
                Json::Value message;
                Json::CharReaderBuilder builder;
                std::string errors;
                std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                if (!reader->parse(payload.data(), payload.data() + payload.size(), &message, &errors)
                    || !message.isObject()) {
                    LOG_WARN << "Ignoring a malformed change notification";
                    return;
                }
                // Every namespace shares the channel; changes to legacy databases carry no "ns"
                const auto messageNs = message.get("ns", "").asString();
                const bool mine = ns.empty() ? (messageNs.empty() || messageNs == "databases") : messageNs == ns;
                if (!mine) {
                    return;
                }
                if (message.isMember("broadcast")) {
                    if (broadcastListener && message["broadcast"].isObject()) {
                        broadcastListener(message["broadcast"]);
                    }
                    return;
                }
                if (!message["user"].isString() || !message["event"].isObject()) {
                    LOG_WARN << "Ignoring a malformed change notification";
                    return;
                }
                listener(message["user"].asString(), message["event"]);
            });
    }

    std::optional<std::size_t> PgDatabaseRepository::sweep(std::chrono::seconds minAge)
    {
        std::size_t removed = 0;
        bool ran = m_pg->withTryLock("keepassxc-sync-sweep", [&](Transaction& trans) {
            // Listed before reading the rows: a blob uploaded after the listing isn't in it, and one committed
            // before the query is in the rows
            auto objects = m_blobs->list();
            std::set<std::string> referenced;
            for (const auto& row :
                 trans.execSqlSync("SELECT blob_key FROM databases UNION SELECT blob_key FROM versions "
                                   "UNION SELECT blob_key FROM vaults UNION SELECT blob_key FROM vault_versions")) {
                referenced.insert(row["blob_key"].as<std::string>());
            }
            const auto cutoff = std::chrono::system_clock::now() - minAge;
            std::vector<std::string> orphans;
            for (const auto& object : objects) {
                if (!referenced.count(object.key) && object.lastModified < cutoff) {
                    orphans.push_back(object.key);
                }
            }
            removeBlobs(orphans);
            removed = orphans.size();
        });
        if (!ran) {
            return std::nullopt;
        }
        return removed;
    }

    void PgDatabaseRepository::startPeriodicSweep(std::chrono::seconds interval)
    {
        if (!m_sweeper) {
            m_sweeper = std::make_unique<trantor::EventLoopThread>("sweep");
            m_sweeper->run();
        }
        m_sweeper->getLoop()->runEvery(static_cast<double>(interval.count()), [this] {
            try {
                if (auto removed = sweep(); removed && *removed > 0) {
                    LOG_INFO << "Deleted " << *removed << " unreferenced files from the bucket";
                }
            } catch (const std::exception& e) {
                LOG_ERROR << "Sweep failed: " << e.what();
            }
        });
    }

    bool PgDatabaseRepository::importDatabase(const std::string& user,
                                              const std::string& id,
                                              const std::vector<Entry>& versions)
    {
        if (versions.empty() || meta(user, id)) {
            return false;
        }
        std::vector<std::string> keys;
        for (const auto& version : versions) {
            keys.push_back(newBlobKey(user, id, version.meta.etag));
            m_blobs->put(keys.back(), version.data);
        }
        bool imported = false;
        try {
            m_pg->transaction([&](Transaction& trans) {
                for (std::size_t i = 0; i < versions.size(); ++i) {
                    const auto& meta = versions[i].meta;
                    std::string updatedAt = meta.updatedAt.empty() ? isoTimestamp() : meta.updatedAt;
                    auto added = trans.execSqlSync(
                        "INSERT INTO " + (i == 0 ? m_table : m_versions) + " (" + m_owner
                            + ", id, revision, etag, size, blob_key, updated_at) "
                              "VALUES ($1, $2, $3, $4, $5, $6, $7::timestamptz) ON CONFLICT DO NOTHING",
                        user,
                        id,
                        meta.revision,
                        meta.etag,
                        static_cast<int64_t>(versions[i].data.size()),
                        keys[i],
                        updatedAt);
                    if (i == 0 && added.affectedRows() == 0) {
                        // Created in the meantime: leave it alone
                        throw std::invalid_argument("exists");
                    }
                }
                imported = true;
            });
        } catch (const std::invalid_argument&) {
        } catch (...) {
            removeBlobs(keys);
            throw;
        }
        if (!imported) {
            removeBlobs(keys);
        }
        return imported;
    }
} // namespace syncserver
