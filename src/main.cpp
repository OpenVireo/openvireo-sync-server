#include "Config.h"
#include "SyncApi.h"
#include "UserStore.h"

#include <drogon/drogon.h>

#include <iostream>
#include <string>

namespace
{
    constexpr const char* USAGE = R"(Usage: keepassxc-sync-server [users <command> [name]]

Without arguments, runs the sync server.

User commands:
  users add <name>      Create a user (or rotate its token) and print the new access token
  users remove <name>   Delete a user (their stored databases are kept on disk)
  users list            List users

Uses SYNC_DATA_DIR (default: ./data) to locate users.json.)";

    int manageUsers(const syncserver::Config& config, const std::string& command, const std::string& name)
    {
        syncserver::UserStore users(config.dataDir);

        if (command == "add" && !name.empty()) {
            auto token = users.setUser(name);
            std::cout << "Access token for \"" << name << "\" (shown only once, enter it in KeePassXC):\n\n  " << token
                      << "\n\n";
            return 0;
        }
        if (command == "remove" && !name.empty()) {
            if (!users.removeUser(name)) {
                std::cerr << "No such user: " << name << "\n";
                return 1;
            }
            std::cout << "Removed user \"" << name << "\".\n";
            return 0;
        }
        if (command == "list") {
            for (const auto& user : users.listUsers()) {
                std::cout << user << "\n";
            }
            return 0;
        }
        std::cerr << USAGE << "\n";
        return 2;
    }

    bool isLoopback(const std::string& host)
    {
        return host == "127.0.0.1" || host == "localhost" || host == "::1";
    }
} // namespace

int main(int argc, char* argv[])
{
    try {
        auto config = syncserver::Config::fromEnvironment();

        if (argc > 1) {
            if (std::string(argv[1]) != "users" || argc < 3) {
                std::cerr << USAGE << "\n";
                return 2;
            }
            return manageUsers(config, argv[2], argc > 3 ? argv[3] : "");
        }

        syncserver::registerSyncApi(config);
        drogon::app()
            .setLogLevel(trantor::Logger::kWarn)
            .setThreadNum(config.threads)
            .addListener(config.host, config.port)
            .registerBeginningAdvice([config] {
                std::cout << "KeePassXC sync server listening on http://" << config.host << ":" << config.port
                          << "\nData directory: " << config.dataDir.string() << std::endl;
                if (!isLoopback(config.host)) {
                    std::cerr << "Warning: listening on a non-loopback address. Put the server behind HTTPS "
                                 "(e.g. a reverse proxy)."
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
