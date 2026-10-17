#include "Migrate.h"

#include "Jwt.h"
#include "Util.h"

#include <stdexcept>

namespace syncserver
{
    void migrateToVault(Storage& storage,
                        const std::string& user,
                        const std::string& databaseId,
                        const std::string& vaultId,
                        std::ostream& out)
    {
        if (!isValidName(user) || !isValidName(databaseId)) {
            throw std::invalid_argument("Invalid user name or database id");
        }
        if (!isUuid(vaultId)) {
            throw std::invalid_argument("The vault id must be a lowercase UUID such as "
                                        "3f1c2a9e-7b4d-4e8a-9c21-5d6e7f809a1b");
        }
        auto history = storage.databases->history(user, databaseId);
        if (!history || history->empty()) {
            throw std::invalid_argument("User " + user + " has no database " + databaseId);
        }
        std::vector<DatabaseRepository::Entry> versions;
        for (const auto& version : *history) {
            if (auto entry = storage.databases->readRevision(user, databaseId, version.revision)) {
                versions.push_back(std::move(*entry));
            }
        }
        if (versions.empty() || versions.front().meta.revision != history->front().revision) {
            throw std::invalid_argument("Database " + user + "/" + databaseId
                                        + " changed while it was being read; try again");
        }
        const auto etag = versions.front().meta.etag;
        if (!storage.vaults->importDatabase(VaultScope, vaultId, versions)) {
            throw std::invalid_argument("Vault " + vaultId + " already exists");
        }

        // Only delete the version that was copied; an upload in the meantime undoes the move
        bool changed = false;
        try {
            storage.databases->remove(user, databaseId, [&](const std::optional<DatabaseMeta>& current) {
                if (!current || current->etag != etag) {
                    changed = true;
                    throw std::runtime_error("changed");
                }
            });
        } catch (...) {
            if (!changed) {
                throw;
            }
            storage.vaults->remove(VaultScope, vaultId, [](const auto&) {});
            throw std::invalid_argument("Database " + user + "/" + databaseId
                                        + " changed while it was being copied; nothing was moved, try again");
        }
        out << "Moved database " << user << "/" << databaseId << " to vault " << vaultId << ": revision "
            << versions.front().meta.revision << ", " << versions.size() - 1 << " kept version(s)\n";
    }
} // namespace syncserver
