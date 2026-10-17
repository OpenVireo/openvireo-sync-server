#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace syncserver
{
    // An S3-compatible bucket such as DigitalOcean Spaces or MinIO
    struct S3Config
    {
        // e.g. https://nyc3.digitaloceanspaces.com
        std::string endpoint;
        std::string region;
        std::string bucket;
        // Prepended to every key, so several servers can share a bucket
        std::string prefix;
        std::string accessKey;
        std::string secretKey;
        // https://endpoint/bucket/key instead of https://bucket.endpoint/key, for MinIO
        bool pathStyle = false;
    };

    struct Config
    {
        std::string host = "127.0.0.1";
        uint16_t port = 8787;
        // Where users and databases are stored when there is no databaseUrl
        std::filesystem::path dataDir = "data";
        // Maximum accepted upload size in bytes (default 50 MiB)
        size_t maxDatabaseBytes = 50 * 1024 * 1024;
        // How many previous versions of each database to keep as a safety net
        unsigned historySize = 10;
        // Number of I/O threads, 0 means one per CPU core
        unsigned threads = 0;
        // TLS is terminated in front of the server, so a non-loopback address is expected
        bool behindProxy = false;

        // Postgres connection string; when set, state lives in Postgres and blobs in the bucket
        std::string databaseUrl;
        // Schema for the tables, created when missing (default: public)
        std::string databaseSchema;
        // Connections per instance, 0 means one per I/O thread
        unsigned databaseConnections = 0;
        S3Config s3;

        bool usesPostgres() const
        {
            return !databaseUrl.empty();
        }

        // Reads SYNC_* environment variables, throwing on invalid values
        static Config fromEnvironment();
    };
} // namespace syncserver
