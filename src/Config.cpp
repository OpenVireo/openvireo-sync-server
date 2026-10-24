#include "Config.h"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

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

        std::string stringFromEnv(const char* name)
        {
            const char* raw = std::getenv(name);
            return raw ? std::string(raw) : std::string();
        }

        bool flagFromEnv(const char* name)
        {
            auto value = stringFromEnv(name);
            if (value.empty() || value == "0" || value == "false") {
                return false;
            }
            if (value == "1" || value == "true") {
                return true;
            }
            throw std::runtime_error(std::string("Environment variable ") + name + " must be 0 or 1, got \"" + value
                                     + "\"");
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
        config.behindProxy = flagFromEnv("SYNC_BEHIND_PROXY");

        config.databaseUrl = stringFromEnv("SYNC_DATABASE_URL");
        config.databaseSchema = stringFromEnv("SYNC_DATABASE_SCHEMA");
        config.databaseConnections =
            static_cast<unsigned>(uintFromEnv("SYNC_DATABASE_CONNECTIONS", config.databaseConnections, 1024));

        auto& s3 = config.s3;
        s3.endpoint = stringFromEnv("SYNC_S3_ENDPOINT");
        s3.region = stringFromEnv("SYNC_S3_REGION");
        s3.bucket = stringFromEnv("SYNC_S3_BUCKET");
        s3.prefix = stringFromEnv("SYNC_S3_PREFIX");
        s3.accessKey = stringFromEnv("SYNC_S3_ACCESS_KEY");
        s3.secretKey = stringFromEnv("SYNC_S3_SECRET_KEY");
        s3.pathStyle = flagFromEnv("SYNC_S3_PATH_STYLE");

        const bool anyS3 = !s3.endpoint.empty() || !s3.region.empty() || !s3.bucket.empty() || !s3.prefix.empty()
                           || !s3.accessKey.empty() || !s3.secretKey.empty();
        const bool allS3 = !s3.endpoint.empty() && !s3.region.empty() && !s3.bucket.empty() && !s3.accessKey.empty()
                           && !s3.secretKey.empty();
        if (config.usesPostgres() && !allS3) {
            throw std::runtime_error("SYNC_DATABASE_URL needs SYNC_S3_ENDPOINT, SYNC_S3_REGION, SYNC_S3_BUCKET, "
                                     "SYNC_S3_ACCESS_KEY and SYNC_S3_SECRET_KEY for the database files");
        }
        if (!config.usesPostgres() && anyS3) {
            throw std::runtime_error("SYNC_S3_* is only used together with SYNC_DATABASE_URL");
        }
        if (!config.databaseSchema.empty()
            && config.databaseSchema.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
            throw std::runtime_error("SYNC_DATABASE_SCHEMA may only contain lower case letters, digits and \"_\"");
        }
        auto mode = stringFromEnv("SYNC_AUTH_MODE");
        // "legacy" is the old name of standalone
        if (mode.empty() || mode == "standalone" || mode == "legacy") {
            config.authMode = AuthMode::Standalone;
        } else if (mode == "jwt") {
            config.authMode = AuthMode::Jwt;
        } else if (mode == "both") {
            config.authMode = AuthMode::Both;
        } else {
            throw std::runtime_error("SYNC_AUTH_MODE must be standalone, jwt or both, got \"" + mode + "\"");
        }
        config.authJwksUrl = stringFromEnv("SYNC_AUTH_JWKS_URL");
        config.authIssuer = stringFromEnv("SYNC_AUTH_ISSUER");
        if (auto audience = stringFromEnv("SYNC_AUTH_AUDIENCE"); !audience.empty()) {
            config.authAudience = audience;
        }
        config.internalToken = stringFromEnv("SYNC_INTERNAL_TOKEN");
        config.allowedOrigins = parseOrigins(stringFromEnv("SYNC_ALLOWED_ORIGINS"));
        config.validateAuth();
        return config;
    }

    std::vector<std::string> Config::parseOrigins(const std::string& list)
    {
        std::vector<std::string> origins;
        size_t start = 0;
        while (start <= list.size()) {
            auto end = list.find(',', start);
            if (end == std::string::npos) {
                end = list.size();
            }
            auto origin = list.substr(start, end - start);
            origin.erase(0, origin.find_first_not_of(" \t"));
            origin.erase(origin.find_last_not_of(" \t") + 1);
            // Browsers send the origin without a path or trailing slash
            while (!origin.empty() && origin.back() == '/') {
                origin.pop_back();
            }
            if (!origin.empty()) {
                if (origin != "*" && origin.rfind("http://", 0) != 0 && origin.rfind("https://", 0) != 0) {
                    throw std::runtime_error(
                        "SYNC_ALLOWED_ORIGINS holds origins such as https://app.example.org, got \"" + origin + "\"");
                }
                origins.push_back(origin);
            }
            start = end + 1;
        }
        return origins;
    }

    void Config::validateAuth() const
    {
        if (authMode == AuthMode::Standalone) {
            return;
        }
        if (authJwksUrl.empty() || authIssuer.empty()) {
            throw std::runtime_error("SYNC_AUTH_MODE=jwt or both needs SYNC_AUTH_JWKS_URL (the auth server's "
                                     "/.well-known/jwks.json) and SYNC_AUTH_ISSUER (its issuer URL)");
        }
        if (authJwksUrl.rfind("http://", 0) != 0 && authJwksUrl.rfind("https://", 0) != 0) {
            throw std::runtime_error("SYNC_AUTH_JWKS_URL must be an http:// or https:// URL");
        }
    }
} // namespace syncserver
