#include "Postgres.h"

#include <drogon/orm/Exception.h>
#include <drogon/utils/Utilities.h>

#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace syncserver
{
    namespace
    {
        // Each migration is a list of statements, since one query can't hold several with parameters.
        // Append new ones; never change one that has shipped.
        const std::vector<std::vector<std::string>> MIGRATIONS = {
            {
                R"(CREATE TABLE users (
                       name        text PRIMARY KEY,
                       created_at  timestamptz NOT NULL DEFAULT now()
                   ))",
                R"(CREATE TABLE devices (
                       user_name     text NOT NULL REFERENCES users(name) ON DELETE CASCADE,
                       id            text NOT NULL,
                       name          text NOT NULL,
                       token_hash    text NOT NULL UNIQUE,
                       created_at    timestamptz NOT NULL DEFAULT now(),
                       last_seen_at  timestamptz,
                       PRIMARY KEY (user_name, id)
                   ))",
                R"(CREATE TABLE pairing_codes (
                       code_hash   text PRIMARY KEY,
                       user_name   text NOT NULL REFERENCES users(name) ON DELETE CASCADE,
                       created_by  text,
                       created_at  timestamptz NOT NULL DEFAULT now(),
                       expires_at  timestamptz NOT NULL
                   ))",
                "CREATE INDEX pairing_codes_user_name ON pairing_codes (user_name)",
                // No foreign key to users: removing a user keeps their databases, as with the data directory
                R"(CREATE TABLE databases (
                       user_name   text NOT NULL,
                       id          text NOT NULL,
                       revision    bigint NOT NULL,
                       etag        text NOT NULL,
                       size        bigint NOT NULL,
                       blob_key    text NOT NULL,
                       updated_at  timestamptz NOT NULL,
                       PRIMARY KEY (user_name, id)
                   ))",
                R"(CREATE TABLE versions (
                       user_name   text NOT NULL,
                       id          text NOT NULL,
                       revision    bigint NOT NULL,
                       etag        text NOT NULL,
                       size        bigint NOT NULL,
                       blob_key    text NOT NULL,
                       updated_at  timestamptz NOT NULL,
                       PRIMARY KEY (user_name, id, revision)
                   ))",
            },
            // Account vaults: shared by their members, so not under a user. scope is always '' for now; it
            // keeps the key shape of the databases tables so both use the same code.
            {
                R"(CREATE TABLE vaults (
                       scope       text NOT NULL DEFAULT '',
                       id          text NOT NULL,
                       revision    bigint NOT NULL,
                       etag        text NOT NULL,
                       size        bigint NOT NULL,
                       blob_key    text NOT NULL,
                       updated_at  timestamptz NOT NULL,
                       PRIMARY KEY (scope, id)
                   ))",
                R"(CREATE TABLE vault_versions (
                       scope       text NOT NULL DEFAULT '',
                       id          text NOT NULL,
                       revision    bigint NOT NULL,
                       etag        text NOT NULL,
                       size        bigint NOT NULL,
                       blob_key    text NOT NULL,
                       updated_at  timestamptz NOT NULL,
                       PRIMARY KEY (scope, id, revision)
                   ))",
            },
        };

        bool isUri(const std::string& url)
        {
            return url.rfind("postgres://", 0) == 0 || url.rfind("postgresql://", 0) == 0;
        }

        std::string withSchema(const std::string& url, const std::string& schema)
        {
            if (schema.empty()) {
                return url;
            }
            if (isUri(url)) {
                return url + (url.find('?') == std::string::npos ? "?" : "&") + "options=-c%20search_path%3D" + schema;
            }
            return url + " options='-c search_path=" + schema + "'";
        }
    } // namespace

    Postgres::Postgres(const Config& config)
        : m_url(config.databaseUrl)
        , m_schema(config.databaseSchema)
        , m_connInfo(withSchema(config.databaseUrl, config.databaseSchema))
        , m_channel("keepassxc_sync" + (config.databaseSchema.empty() ? std::string() : "_" + config.databaseSchema))
    {
        auto connections = config.databaseConnections;
        if (connections == 0) {
            connections = config.threads ? config.threads : std::max(1u, std::thread::hardware_concurrency());
        }
        m_client = drogon::orm::DbClient::newPgClient(m_connInfo, connections);
        if (!m_client) {
            throw std::runtime_error("This Drogon build has no PostgreSQL support");
        }
        // Connections come up in the background; wait for one so a wrong URL fails at startup
        for (int attempt = 0;; ++attempt) {
            try {
                m_client->execSqlSync("SELECT 1");
                break;
            } catch (const drogon::orm::DrogonDbException& e) {
                if (attempt >= 50) {
                    throw std::runtime_error(std::string("Cannot connect to Postgres at ") + describe() + ": "
                                             + e.base().what());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

    std::string Postgres::describe() const
    {
        if (!isUri(m_url)) {
            return "(connection string)";
        }
        // postgresql://user:password@host:port/db?params -> host:port/db
        auto rest = m_url.substr(m_url.find("://") + 3);
        if (auto at = rest.rfind('@', rest.find('/')); at != std::string::npos) {
            rest = rest.substr(at + 1);
        }
        rest = rest.substr(0, rest.find('?'));
        return rest + (m_schema.empty() ? std::string() : " (schema " + m_schema + ")");
    }

    void Postgres::transaction(const std::function<void(drogon::orm::Transaction&)>& fn)
    {
        std::promise<bool> committed;
        auto done = committed.get_future();
        {
            auto trans = m_client->newTransaction([&committed](bool ok) { committed.set_value(ok); });
            try {
                fn(*trans);
            } catch (...) {
                // A failed query has already rolled back; the callback never runs after a rollback
                trans->rollback();
                throw;
            }
        } // committed when the last reference goes away
        if (!done.get()) {
            throw std::runtime_error("Cannot commit the transaction");
        }
    }

    bool Postgres::withTryLock(const std::string& name, const std::function<void(drogon::orm::Transaction&)>& fn)
    {
        bool ran = false;
        transaction([&](drogon::orm::Transaction& trans) {
            auto lock =
                trans.execSqlSync("SELECT pg_try_advisory_xact_lock(hashtext($1)) AS locked", name + ":" + m_schema);
            if (lock[0]["locked"].as<bool>()) {
                fn(trans);
                ran = true;
            }
        });
        return ran;
    }

    void Postgres::migrate()
    {
        transaction([&](drogon::orm::Transaction& trans) {
            trans.execSqlSync("SELECT pg_advisory_xact_lock(hashtext($1))", "keepassxc-sync-migrate:" + m_schema);
            // Checked first rather than with IF NOT EXISTS, which logs a notice on every start
            if (!m_schema.empty()
                && trans.execSqlSync("SELECT 1 FROM pg_namespace WHERE nspname = $1", m_schema).empty()) {
                // Checked in Config: lower case letters, digits and "_" only
                trans.execSqlSync("CREATE SCHEMA " + m_schema);
            }
            if (trans.execSqlSync("SELECT to_regclass('schema_migrations') IS NOT NULL AS present")[0]["present"]
                    .as<bool>()
                == false) {
                trans.execSqlSync(R"(CREATE TABLE schema_migrations (
                                         version     integer PRIMARY KEY,
                                         applied_at  timestamptz NOT NULL DEFAULT now()
                                     ))");
            }
            auto applied = trans.execSqlSync("SELECT coalesce(max(version), 0) AS version FROM schema_migrations");
            auto current = applied[0]["version"].as<int>();
            if (current > static_cast<int>(MIGRATIONS.size())) {
                throw std::runtime_error("The database schema (version " + std::to_string(current)
                                         + ") is newer than this server (" + std::to_string(MIGRATIONS.size())
                                         + "). Upgrade the server.");
            }
            for (auto version = current + 1; version <= static_cast<int>(MIGRATIONS.size()); ++version) {
                for (const auto& statement : MIGRATIONS[version - 1]) {
                    trans.execSqlSync(statement);
                }
                trans.execSqlSync("INSERT INTO schema_migrations (version) VALUES ($1)", version);
            }
        });
    }
} // namespace syncserver
