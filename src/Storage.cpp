#include "Storage.h"

#include "DatabaseStore.h"
#include "UserStore.h"
#include "Util.h"

#ifdef SYNC_SERVER_WITH_CLOUD
#include "BlobStore.h"
#include "PgDatabaseRepository.h"
#include "PgUserRepository.h"
#include "Postgres.h"
#endif

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace syncserver
{
    namespace
    {
        // No 0/O or 1/I, which are easy to mix up when reading a code off another screen
        constexpr std::string_view PAIRING_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        constexpr std::size_t PAIRING_CODE_LENGTH = 8;
    } // namespace

    Json::Value DatabaseMeta::toJson() const
    {
        Json::Value json;
        json["id"] = id;
        json["revision"] = static_cast<Json::Int64>(revision);
        json["etag"] = etag;
        json["size"] = static_cast<Json::UInt64>(size);
        json["updatedAt"] = updatedAt;
        return json;
    }

    void UserRepository::setLastSeenInterval(std::chrono::seconds interval)
    {
        m_lastSeenInterval = interval;
    }

    void UserRepository::setPairingLifetime(std::chrono::seconds lifetime)
    {
        m_pairingLifetime = lifetime;
    }

    std::string UserRepository::normalizePairingCode(std::string_view code)
    {
        std::string normalized;
        for (unsigned char c : code) {
            if (c == '-' || std::isspace(c)) {
                continue;
            }
            normalized += static_cast<char>(std::toupper(c));
        }
        return normalized;
    }

    std::string UserRepository::newToken()
    {
        unsigned char bytes[32];
        if (!drogon::utils::secureRandomBytes(bytes, sizeof(bytes))) {
            throw std::runtime_error("Cannot generate a random token");
        }
        return drogon::utils::base64EncodeUnpadded(bytes, sizeof(bytes), true);
    }

    std::string UserRepository::newDeviceId()
    {
        unsigned char bytes[6];
        if (!drogon::utils::secureRandomBytes(bytes, sizeof(bytes))) {
            throw std::runtime_error("Cannot generate a device id");
        }
        return "d-" + drogon::utils::binaryStringToHex(bytes, sizeof(bytes), true);
    }

    std::string UserRepository::newPairingCode()
    {
        unsigned char bytes[PAIRING_CODE_LENGTH];
        if (!drogon::utils::secureRandomBytes(bytes, sizeof(bytes))) {
            throw std::runtime_error("Cannot generate a pairing code");
        }
        std::string code;
        for (auto byte : bytes) {
            // 256 is a multiple of the alphabet's 32 letters, so every letter is equally likely
            code += PAIRING_ALPHABET[byte % PAIRING_ALPHABET.size()];
        }
        return code;
    }

    std::string UserRepository::formatPairingCode(const std::string& normalized)
    {
        return normalized.substr(0, 4) + "-" + normalized.substr(4);
    }

    void UserRepository::validateUserName(const std::string& name)
    {
        if (!isValidName(name)) {
            throw std::invalid_argument("Invalid user name \"" + name
                                        + "\" (allowed: letters, digits, \".\", \"_\", \"-\"; max 64 chars)");
        }
    }

    void UserRepository::validateDeviceName(const std::string& name)
    {
        const bool printable = std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 0x20; });
        if (name.empty() || name.size() > 64 || !printable) {
            throw std::invalid_argument("Invalid device name (1 to 64 characters, no control characters)");
        }
    }

    Json::Value DatabaseRepository::changeEvent(const DatabaseMeta& meta)
    {
        Json::Value event;
        event["id"] = meta.id;
        event["etag"] = meta.etag;
        event["revision"] = static_cast<Json::Int64>(meta.revision);
        return event;
    }

    Json::Value DatabaseRepository::deletedEvent(const std::string& id)
    {
        Json::Value event;
        event["id"] = id;
        event["deleted"] = true;
        return event;
    }

    Storage openStorage(const Config& config)
    {
        if (!config.usesPostgres()) {
            return {std::make_shared<UserStore>(config.dataDir),
                    std::make_shared<DatabaseStore>(config.dataDir, config.historySize),
                    std::make_shared<DatabaseStore>(config.dataDir, config.historySize, DatabaseStore::VaultsFolder),
                    nullptr,
                    "data directory " + config.dataDir.string()};
        }
#ifdef SYNC_SERVER_WITH_CLOUD
        auto postgres = std::make_shared<Postgres>(config);
        postgres->migrate();
        auto blobs = std::make_shared<BlobStore>(config.s3);
        auto backend =
            std::make_shared<std::pair<std::shared_ptr<Postgres>, std::shared_ptr<BlobStore>>>(postgres, blobs);
        return {std::make_shared<PgUserRepository>(postgres),
                std::make_shared<PgDatabaseRepository>(postgres, blobs, config.historySize),
                std::make_shared<PgDatabaseRepository>(
                    postgres, blobs, config.historySize, PgDatabaseRepository::Namespace::Vaults),
                backend,
                "Postgres " + postgres->describe() + ", bucket " + blobs->describe()};
#else
        throw std::runtime_error("SYNC_DATABASE_URL is set, but this build has no Postgres and S3 support "
                                 "(build it with a Drogon that has PostgreSQL, see README.md)");
#endif
    }
} // namespace syncserver
