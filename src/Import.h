#pragma once

#include "Storage.h"

#include <filesystem>
#include <ostream>

namespace syncserver
{
    /**
     * Copies a data directory (users, devices, legacy tokens, databases and kept versions) into Postgres
     * and the bucket, keeping revisions and ETags so existing clients see no change. Users and databases
     * that already exist are left alone and reported. Returns whether everything was copied.
     */
    bool importDataDirectory(const std::filesystem::path& dataDir, Storage& target, std::ostream& out);
} // namespace syncserver
