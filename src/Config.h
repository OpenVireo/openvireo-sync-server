#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace syncserver
{
    struct Config
    {
        std::string host = "127.0.0.1";
        uint16_t port = 8787;
        // Where users and databases are stored
        std::filesystem::path dataDir = "data";
        // Maximum accepted upload size in bytes (default 50 MiB)
        size_t maxDatabaseBytes = 50 * 1024 * 1024;
        // How many previous versions of each database to keep on disk as a safety net
        unsigned historySize = 10;
        // Number of I/O threads, 0 means one per CPU core
        unsigned threads = 0;

        // Reads SYNC_* environment variables, throwing on invalid values
        static Config fromEnvironment();
    };
} // namespace syncserver
