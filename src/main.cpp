#include "Config.h"
#include "Migrate.h"
#include "Storage.h"
#include "SyncApi.h"

#ifdef SYNC_SERVER_WITH_CLOUD
#include "Import.h"
#include "PgDatabaseRepository.h"
#endif

#include <drogon/drogon.h>
#ifdef SYNC_SERVER_PORTABLE
#include <openssl/crypto.h>
#endif

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* USAGE =
        R"(Usage: openvireo-sync-server [users|devices <command> ... | migrate | import <data-dir> | sweep |
                             migrate-to-vaults <user> <database-id> <vault-uuid>]

Without arguments, runs the sync server.

User commands:
  users add <name> [device-name]     Create a user with a first device and print its access token
  users remove <name>                Delete a user and their devices (their stored databases are kept)
  users list                         List users

Device commands (each device has its own access token):
  devices add <user> <device-name>   Add a device and print its access token
  devices pair <user>                Print a one-time code that adds a device when entered in OpenVireo
                                     instead of a token (valid for 10 minutes)
  devices list <user>                List a user's devices
  devices revoke <user> <device-id>  Revoke a device; its token stops working at once

Storage commands (Postgres and a bucket, see SYNC_DATABASE_URL):
  migrate                            Create or upgrade the tables and exit, e.g. as a pre-deploy job
  import <data-dir>                  Copy a data directory's users, devices and databases (with their
                                     kept versions, revisions and ETags) into Postgres and the bucket
  sweep                              Delete files in the bucket that no database version refers to

Account commands (either storage):
  migrate-to-vaults <user> <database-id> <vault-uuid>
                                     Move a user's database, with its kept versions, revisions and ETags,
                                     to the account vault vaults/<vault-uuid> (the vault id the auth server
                                     gave it) and delete the user's copy. With the data directory, stop the
                                     server first.

Commands use the same storage as the server: Postgres when SYNC_DATABASE_URL is set, otherwise
SYNC_DATA_DIR (default: ./data).)";

    void printToken(const std::string& user, const std::string& device, const std::string& token)
    {
        std::cout << "Access token for \"" << device << "\" of \"" << user
                  << "\" (shown only once, enter it in OpenVireo on that device):\n\n  " << token << "\n\n";
    }

    int manageUsers(syncserver::UserRepository& users, const std::vector<std::string>& args)
    {
        const auto& command = args.at(0);

        if (command == "add" && (args.size() == 2 || args.size() == 3)) {
            const auto device = args.size() == 3 ? args[2] : std::string("first device");
            printToken(args[1], device, users.addUser(args[1], device));
            return 0;
        }
        if (command == "remove" && args.size() == 2) {
            if (!users.removeUser(args[1])) {
                std::cerr << "No such user: " << args[1] << "\n";
                return 1;
            }
            std::cout << "Removed user \"" << args[1] << "\".\n";
            return 0;
        }
        if (command == "list" && args.size() == 1) {
            for (const auto& user : users.listUsers()) {
                std::cout << user << "\n";
            }
            return 0;
        }
        std::cerr << USAGE << "\n";
        return 2;
    }

    int manageDevices(syncserver::UserRepository& users, const std::vector<std::string>& args)
    {
        const auto& command = args.at(0);

        if (command == "add" && args.size() == 3) {
            auto [id, token] = users.addDevice(args[1], args[2]);
            printToken(args[1], args[2], token);
            std::cout << "Device id: " << id << "\n";
            return 0;
        }
        if (command == "pair" && args.size() == 2) {
            auto pairing = users.createPairingCode(args[1]);
            std::cout << "Pairing code for a new device of \"" << args[1]
                      << "\" (enter it in OpenVireo instead of an access token):\n\n  " << pairing.code
                      << "\n\nIt works once, until " << pairing.expiresAt << ". The running server accepts it.\n";
            return 0;
        }
        if (command == "list" && args.size() == 2) {
            for (const auto& device : users.listDevices(args[1])) {
                std::cout << device.id << "\t" << device.name << "\tadded " << device.createdAt << "\tlast seen "
                          << (device.lastSeenAt.empty() ? std::string("never") : device.lastSeenAt) << "\n";
            }
            return 0;
        }
        if (command == "revoke" && args.size() == 3) {
            if (!users.revokeDevice(args[1], args[2])) {
                std::cerr << "No device " << args[2] << " for user " << args[1] << "\n";
                return 1;
            }
            std::cout << "Revoked device " << args[2] << " of \"" << args[1] << "\".\n";
            return 0;
        }
        std::cerr << USAGE << "\n";
        return 2;
    }

    bool isLoopback(const std::string& host)
    {
        return host == "127.0.0.1" || host == "localhost" || host == "::1";
    }

    int
    manageStorage(const syncserver::Config& config, const std::string& command, const std::vector<std::string>& args)
    {
        if (!config.usesPostgres()) {
            std::cerr << command << " works with Postgres and a bucket only: set SYNC_DATABASE_URL and SYNC_S3_*\n";
            return 2;
        }
#ifdef SYNC_SERVER_WITH_CLOUD
        if (command == "migrate" && args.empty()) {
            // Opening the storage applies the migrations
            auto storage = syncserver::openStorage(config);
            std::cout << "The schema in " << storage.description << " is up to date.\n";
            return 0;
        }
        if (command == "import" && args.size() == 1) {
            auto storage = syncserver::openStorage(config);
            if (!syncserver::importDataDirectory(args[0], storage, std::cout)) {
                std::cerr << "Some users or databases already existed and were not copied.\n";
                return 1;
            }
            return 0;
        }
        if (command == "sweep" && args.empty()) {
            auto storage = syncserver::openStorage(config);
            auto databases = std::dynamic_pointer_cast<syncserver::PgDatabaseRepository>(storage.databases);
            auto removed = databases->sweep();
            if (!removed) {
                std::cerr << "Another instance is sweeping right now.\n";
                return 1;
            }
            std::cout << "Deleted " << *removed << " unreferenced file(s).\n";
            return 0;
        }
        std::cerr << USAGE << "\n";
        return 2;
#else
        (void)args;
        std::cerr << "This build has no Postgres and S3 support.\n";
        return 2;
#endif
    }
} // namespace

#ifdef SYNC_SERVER_PORTABLE
// The linked-in OpenSSL can't use another build's openssl.cnf: Fedora's names providers and crypto policies it can't
// load, and then every TLS connection fails. The CA certificates are still read from /etc/ssl. A constructor with a
// priority, because static initialisers in the libraries already use OpenSSL (and load the config) before main().
__attribute__((constructor(101))) static void ignoreSystemOpenSslConfig()
{
    if (!std::getenv("OPENSSL_CONF")) {
        OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr);
    }
}
#endif

int main(int argc, char* argv[])
{
    try {
        auto config = syncserver::Config::fromEnvironment();

        if (argc > 1) {
            const std::string group = argv[1];
            const std::vector<std::string> args(argv + 2, argv + argc);
            if (group == "migrate" || group == "import" || group == "sweep") {
                return manageStorage(config, group, args);
            }
            if (group == "migrate-to-vaults") {
                if (args.size() != 3) {
                    std::cerr << USAGE << "\n";
                    return 2;
                }
                auto storage = syncserver::openStorage(config);
                try {
                    syncserver::migrateToVault(storage, args[0], args[1], args[2], std::cout);
                } catch (const std::invalid_argument& e) {
                    std::cerr << e.what() << "\n";
                    return 1;
                }
                return 0;
            }
            if (args.empty() || (group != "users" && group != "devices")) {
                std::cerr << USAGE << "\n";
                return 2;
            }
            auto storage = syncserver::openStorage(config);
            return group == "users" ? manageUsers(*storage.users, args) : manageDevices(*storage.users, args);
        }

        auto storage = syncserver::openStorage(config);
        const auto description = storage.description;
#ifdef SYNC_SERVER_WITH_CLOUD
        if (auto databases = std::dynamic_pointer_cast<syncserver::PgDatabaseRepository>(storage.databases)) {
            databases->startPeriodicSweep(std::chrono::hours(6));
        }
#endif
        syncserver::registerSyncApi(config, std::move(storage));
        drogon::app()
            .setLogLevel(trantor::Logger::kWarn)
            .setThreadNum(config.threads)
            .addListener(config.host, config.port)
            .registerBeginningAdvice([config, description] {
                std::cout << "OpenVireo sync server listening on http://" << config.host << ":" << config.port
                          << "\nStorage: " << description << std::endl;
                if (!isLoopback(config.host) && !config.behindProxy) {
                    std::cerr << "Warning: listening on a non-loopback address. Put the server behind HTTPS "
                                 "(e.g. a reverse proxy) and set SYNC_BEHIND_PROXY=1."
                              << std::endl;
                }
            })
            .run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
