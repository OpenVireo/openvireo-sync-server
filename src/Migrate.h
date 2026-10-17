#pragma once

#include "Storage.h"

#include <ostream>
#include <string>

namespace syncserver
{
    /**
     * Moves a legacy database of a user (databases/{id}) to an account vault (vaults/{uuid}) with its kept
     * versions, revisions and ETags, then deletes the legacy copy. Works with either storage. Throws
     * std::invalid_argument with a message for the operator when the database is missing, the vault already
     * exists, or the database changed while it was being copied (then the vault copy is removed again).
     */
    void migrateToVault(Storage& storage,
                        const std::string& user,
                        const std::string& databaseId,
                        const std::string& vaultId,
                        std::ostream& out);
} // namespace syncserver
