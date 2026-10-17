#include "Import.h"

#include "DatabaseStore.h"
#include "PgDatabaseRepository.h"
#include "PgUserRepository.h"
#include "UserStore.h"

#include <limits>
#include <stdexcept>

namespace syncserver
{
    bool importDataDirectory(const std::filesystem::path& dataDir, Storage& target, std::ostream& out)
    {
        auto users = std::dynamic_pointer_cast<PgUserRepository>(target.users);
        auto databases = std::dynamic_pointer_cast<PgDatabaseRepository>(target.databases);
        if (!users || !databases) {
            throw std::runtime_error("import copies into Postgres and the bucket: set SYNC_DATABASE_URL and SYNC_S3_*");
        }
        if (!std::filesystem::is_directory(dataDir)) {
            throw std::runtime_error("No such directory: " + dataDir.string());
        }

        bool complete = true;
        UserStore sourceUsers(dataDir);
        // Legacy tokens are already devices called "legacy" in the store
        for (const auto& user : sourceUsers.exportUsers()) {
            if (users->importUser(user)) {
                out << "User " << user.name << ": " << user.devices.size() << " device(s)\n";
            } else {
                out << "User " << user.name << ": already exists, skipped\n";
                complete = false;
            }
        }

        // Every kept version, however many the source kept
        DatabaseStore sourceDatabases(dataDir, std::numeric_limits<unsigned>::max());
        for (const auto& owner : sourceDatabases.owners()) {
            for (const auto& meta : sourceDatabases.list(owner)) {
                std::vector<DatabaseRepository::Entry> versions;
                for (const auto& version :
                     sourceDatabases.history(owner, meta.id).value_or(std::vector<DatabaseMeta>{})) {
                    if (auto entry = sourceDatabases.readRevision(owner, meta.id, version.revision)) {
                        versions.push_back(std::move(*entry));
                    }
                }
                if (databases->importDatabase(owner, meta.id, versions)) {
                    out << "Database " << owner << "/" << meta.id << ": revision " << meta.revision << ", "
                        << versions.size() - 1 << " kept version(s)\n";
                } else {
                    out << "Database " << owner << "/" << meta.id << ": already exists, skipped\n";
                    complete = false;
                }
            }
        }
        return complete;
    }
} // namespace syncserver
