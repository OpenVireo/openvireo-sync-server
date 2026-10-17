#include "PgUserRepository.h"

#include "Postgres.h"
#include "Util.h"

#include <stdexcept>

using drogon::orm::Transaction;

namespace syncserver
{
    PgUserRepository::PgUserRepository(std::shared_ptr<Postgres> postgres)
        : m_pg(std::move(postgres))
    {
    }

    std::optional<Identity> PgUserRepository::authenticate(const std::string& token)
    {
        if (token.empty()) {
            return std::nullopt;
        }
        // An indexed lookup by the hash of a 256-bit random token tells an attacker nothing useful,
        // so there is no need for the constant-time scan the data directory does
        auto staleBefore = isoTimestamp(std::chrono::system_clock::now() - m_lastSeenInterval);
        auto rows = m_pg->client().execSqlSync(
            "SELECT user_name, id, (last_seen_at IS NULL OR last_seen_at < $2::timestamptz) AS stale "
            "FROM devices WHERE token_hash = $1",
            sha256Hex(token),
            staleBefore);
        if (rows.empty()) {
            return std::nullopt;
        }
        Identity identity{rows[0]["user_name"].as<std::string>(), rows[0]["id"].as<std::string>()};
        if (rows[0]["stale"].as<bool>()) {
            m_pg->client().execSqlSync(
                "UPDATE devices SET last_seen_at = $3::timestamptz WHERE user_name = $1 AND id = $2",
                identity.user,
                identity.deviceId,
                isoTimestamp());
        }
        return identity;
    }

    std::string PgUserRepository::addUser(const std::string& name, const std::string& deviceName)
    {
        validateUserName(name);
        validateDeviceName(deviceName);
        auto token = newToken();
        m_pg->transaction([&](Transaction& trans) {
            auto created = trans.execSqlSync(
                "INSERT INTO users (name, created_at) VALUES ($1, $2::timestamptz) ON CONFLICT DO NOTHING",
                name,
                isoTimestamp());
            if (created.affectedRows() == 0) {
                throw std::invalid_argument("User \"" + name + "\" already exists. Add a device with: devices add "
                                            + name + " <device-name>");
            }
            trans.execSqlSync("INSERT INTO devices (id, user_name, name, token_hash, created_at) "
                              "VALUES ($1, $2, $3, $4, $5::timestamptz)",
                              newDeviceId(),
                              name,
                              deviceName,
                              sha256Hex(token),
                              isoTimestamp());
        });
        return token;
    }

    bool PgUserRepository::removeUser(const std::string& name)
    {
        return m_pg->client().execSqlSync("DELETE FROM users WHERE name = $1", name).affectedRows() > 0;
    }

    std::vector<std::string> PgUserRepository::listUsers()
    {
        std::vector<std::string> names;
        for (const auto& row : m_pg->client().execSqlSync("SELECT name FROM users ORDER BY name")) {
            names.push_back(row["name"].as<std::string>());
        }
        return names;
    }

    std::pair<std::string, std::string> PgUserRepository::addDevice(const std::string& user,
                                                                    const std::string& deviceName)
    {
        validateDeviceName(deviceName);
        auto id = newDeviceId();
        auto token = newToken();
        auto added = m_pg->client().execSqlSync(
            "INSERT INTO devices (id, user_name, name, token_hash, created_at) "
            "SELECT $1, $2, $3, $4, $5::timestamptz WHERE EXISTS (SELECT 1 FROM users WHERE name = $2)",
            id,
            user,
            deviceName,
            sha256Hex(token),
            isoTimestamp());
        if (added.affectedRows() == 0) {
            throw std::invalid_argument("No such user: " + user);
        }
        return {id, token};
    }

    bool PgUserRepository::revokeDevice(const std::string& user, const std::string& deviceId)
    {
        return m_pg->client()
                   .execSqlSync("DELETE FROM devices WHERE user_name = $1 AND id = $2", user, deviceId)
                   .affectedRows()
               > 0;
    }

    std::vector<Device> PgUserRepository::listDevices(const std::string& user)
    {
        if (m_pg->client().execSqlSync("SELECT 1 FROM users WHERE name = $1", user).empty()) {
            throw std::invalid_argument("No such user: " + user);
        }
        auto rows = m_pg->client().execSqlSync(
            "SELECT id, name, " SYNC_PG_ISO(
                "created_at") " AS created_at, "
                              "coalesce(" SYNC_PG_ISO(
                                  "last_seen_at") ", '') AS last_seen_at "
                                                  "FROM devices WHERE user_name = $1 ORDER BY created_at, id",
            user);
        std::vector<Device> devices;
        for (const auto& row : rows) {
            devices.push_back({row["id"].as<std::string>(),
                               row["name"].as<std::string>(),
                               row["created_at"].as<std::string>(),
                               row["last_seen_at"].as<std::string>()});
        }
        return devices;
    }

    PairingCode PgUserRepository::createPairingCode(const std::string& user, const std::string& createdBy)
    {
        auto code = newPairingCode();
        auto now = isoTimestamp();
        auto expiresAt = isoTimestamp(std::chrono::system_clock::now() + m_pairingLifetime);
        m_pg->transaction([&](Transaction& trans) {
            // Locking the user serialises codes created at the same time, so they stay few
            if (trans.execSqlSync("SELECT 1 FROM users WHERE name = $1 FOR UPDATE", user).empty()) {
                throw std::invalid_argument("No such user: " + user);
            }
            trans.execSqlSync(
                "DELETE FROM pairing_codes WHERE user_name = $1 AND expires_at <= $2::timestamptz", user, now);
            // Keep a user's waiting codes few, dropping the ones that expire first
            trans.execSqlSync("DELETE FROM pairing_codes WHERE code_hash IN ("
                              "SELECT code_hash FROM pairing_codes WHERE user_name = $1 "
                              "ORDER BY expires_at DESC OFFSET $2)",
                              user,
                              static_cast<int64_t>(MaxPairingsPerUser - 1));
            trans.execSqlSync("INSERT INTO pairing_codes (code_hash, user_name, created_by, created_at, expires_at) "
                              "VALUES ($1, $2, nullif($3, ''), $4::timestamptz, $5::timestamptz)",
                              sha256Hex(code),
                              user,
                              createdBy,
                              now,
                              expiresAt);
        });
        return {formatPairingCode(code), expiresAt};
    }

    std::optional<PairedDevice> PgUserRepository::claimPairingCode(const std::string& code,
                                                                   const std::string& deviceName)
    {
        validateDeviceName(deviceName);
        auto now = isoTimestamp();
        std::optional<PairedDevice> device;
        m_pg->transaction([&](Transaction& trans) {
            trans.execSqlSync("DELETE FROM pairing_codes WHERE expires_at <= $1::timestamptz", now);
            // Deleting the code claims it, so two devices can't both use it
            auto claimed = trans.execSqlSync("DELETE FROM pairing_codes WHERE code_hash = $1 RETURNING user_name",
                                             sha256Hex(normalizePairingCode(code)));
            if (claimed.empty()) {
                return;
            }
            PairedDevice paired{claimed[0]["user_name"].as<std::string>(), newDeviceId(), newToken()};
            trans.execSqlSync("INSERT INTO devices (id, user_name, name, token_hash, created_at) "
                              "VALUES ($1, $2, $3, $4, $5::timestamptz)",
                              paired.deviceId,
                              paired.user,
                              deviceName,
                              sha256Hex(paired.token),
                              now);
            device = std::move(paired);
        });
        return device;
    }

    bool PgUserRepository::importUser(const StoredUser& user)
    {
        bool imported = false;
        m_pg->transaction([&](Transaction& trans) {
            auto created = trans.execSqlSync("INSERT INTO users (name, created_at) "
                                             "VALUES ($1, coalesce(nullif($2, '')::timestamptz, now())) "
                                             "ON CONFLICT DO NOTHING",
                                             user.name,
                                             user.createdAt);
            if (created.affectedRows() == 0) {
                return;
            }
            for (const auto& device : user.devices) {
                trans.execSqlSync("INSERT INTO devices (id, user_name, name, token_hash, created_at, last_seen_at) "
                                  "VALUES ($1, $2, $3, $4, coalesce(nullif($5, '')::timestamptz, now()), "
                                  "nullif($6, '')::timestamptz)",
                                  device.id,
                                  user.name,
                                  device.name,
                                  device.tokenHash,
                                  device.createdAt,
                                  device.lastSeenAt);
            }
            imported = true;
        });
        return imported;
    }
} // namespace syncserver
