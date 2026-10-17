#pragma once

#include "Config.h"

#include <drogon/orm/DbClient.h>

#include <functional>
#include <string>

namespace syncserver
{
    // ISO 8601 in UTC with milliseconds, like isoTimestamp(), for a timestamptz column
#define SYNC_PG_ISO(column) "to_char(" column " AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')"

    /**
     * The connections to Postgres shared by the repositories. Queries block the calling thread
     * (execSqlSync); the client runs them on its own event loops, so that is safe from a request handler.
     */
    class Postgres
    {
    public:
        explicit Postgres(const Config& config);

        drogon::orm::DbClient& client() const
        {
            return *m_client;
        }

        // The connection string with the schema applied, for a listener connection
        const std::string& connectionInfo() const
        {
            return m_connInfo;
        }

        // The LISTEN/NOTIFY channel for changes, one per schema since channels are database-wide
        const std::string& channel() const
        {
            return m_channel;
        }

        // Host and database, without credentials
        std::string describe() const;

        // Applies the missing migrations under an advisory lock, so instances starting together don't race
        void migrate();

        // Runs fn in a transaction and returns once it is committed. An exception from fn rolls it back.
        void transaction(const std::function<void(drogon::orm::Transaction&)>& fn);

        // Runs fn in a transaction if no other session holds the advisory lock named `name`; returns whether it ran
        bool withTryLock(const std::string& name, const std::function<void(drogon::orm::Transaction&)>& fn);

    private:
        std::string m_url;
        std::string m_schema;
        std::string m_connInfo;
        std::string m_channel;
        drogon::orm::DbClientPtr m_client;
    };
} // namespace syncserver
