#include "Config.h"

#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace syncserver
{
    namespace
    {
        unsigned long long uintFromEnv(const char* name,
                                       unsigned long long fallback,
                                       unsigned long long max = std::numeric_limits<unsigned long long>::max())
        {
            const char* raw = std::getenv(name);
            if (!raw || !*raw) {
                return fallback;
            }
            std::string value(raw);
            size_t pos = 0;
            unsigned long long result = 0;
            try {
                if (value.find_first_not_of("0123456789") != std::string::npos) {
                    throw std::invalid_argument(value);
                }
                result = std::stoull(value, &pos);
            } catch (const std::exception&) {
                pos = 0;
            }
            if (pos != value.size() || result > max) {
                throw std::runtime_error(std::string("Environment variable ") + name
                                         + " must be a non-negative integer, got \"" + value + "\"");
            }
            return result;
        }
    } // namespace

    Config Config::fromEnvironment()
    {
        Config config;
        if (const char* host = std::getenv("SYNC_HOST"); host && *host) {
            config.host = host;
        }
        config.port = static_cast<uint16_t>(uintFromEnv("SYNC_PORT", config.port, 65535));
        if (const char* dir = std::getenv("SYNC_DATA_DIR"); dir && *dir) {
            config.dataDir = dir;
        }
        config.dataDir = std::filesystem::absolute(config.dataDir).lexically_normal();
        config.maxDatabaseBytes = static_cast<size_t>(uintFromEnv("SYNC_MAX_DB_BYTES", config.maxDatabaseBytes));
        config.historySize = static_cast<unsigned>(uintFromEnv("SYNC_HISTORY_SIZE", config.historySize, 1000000));
        config.threads = static_cast<unsigned>(uintFromEnv("SYNC_THREADS", config.threads, 1024));
        return config;
    }
} // namespace syncserver
