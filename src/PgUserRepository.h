#pragma once

#include "Storage.h"

#include <memory>

namespace syncserver
{
    class Postgres;

    // Users, devices and pairing codes in the users, devices and pairing_codes tables
    class PgUserRepository : public UserRepository
    {
    public:
        explicit PgUserRepository(std::shared_ptr<Postgres> postgres);

        std::optional<Identity> authenticate(const std::string& token) override;

        std::string addUser(const std::string& name, const std::string& deviceName) override;
        bool removeUser(const std::string& name) override;
        std::vector<std::string> listUsers() override;

        std::pair<std::string, std::string> addDevice(const std::string& user, const std::string& deviceName) override;
        bool revokeDevice(const std::string& user, const std::string& deviceId) override;
        std::vector<Device> listDevices(const std::string& user) override;

        PairingCode createPairingCode(const std::string& user, const std::string& createdBy = {}) override;
        std::optional<PairedDevice> claimPairingCode(const std::string& code, const std::string& deviceName) override;

        // Copies a user with their devices and token hashes as they are; false when the user already exists
        bool importUser(const StoredUser& user);

    private:
        std::shared_ptr<Postgres> m_pg;
    };
} // namespace syncserver
