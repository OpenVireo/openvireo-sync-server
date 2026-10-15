#include "Config.h"
#include "SyncApi.h"
#include "UserStore.h"

#include <drogon/drogon.h>

#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* USAGE = R"(Usage: keepassxc-sync-server [users|devices <command> ...]

Without arguments, runs the sync server.

User commands:
  users add <name> [device-name]     Create a user with a first device and print its access token
  users remove <name>                Delete a user and their devices (stored databases are kept on disk)
  users list                         List users

Device commands (each device has its own access token):
  devices add <user> <device-name>   Add a device and print its access token
  devices list <user>                List a user's devices
  devices revoke <user> <device-id>  Revoke a device; its token stops working at once

Uses SYNC_DATA_DIR (default: ./data) to locate users.json.)";

    void printToken(const std::string& user, const std::string& device, const std::string& token)
    {
        std::cout << "Access token for \"" << device << "\" of \"" << user
                  << "\" (shown only once, enter it in KeePassXC on that device):\n\n  " << token << "\n\n";
    }

    int manageUsers(const syncserver::Config& config, const std::vector<std::string>& args)
    {
        syncserver::UserStore users(config.dataDir);
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

    int manageDevices(const syncserver::Config& config, const std::vector<std::string>& args)
    {
        syncserver::UserStore users(config.dataDir);
        const auto& command = args.at(0);

        if (command == "add" && args.size() == 3) {
            auto [id, token] = users.addDevice(args[1], args[2]);
            printToken(args[1], args[2], token);
            std::cout << "Device id: " << id << "\n";
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
} // namespace

int main(int argc, char* argv[])
{
    try {
        auto config = syncserver::Config::fromEnvironment();

        if (argc > 1) {
            const std::string group = argv[1];
            const std::vector<std::string> args(argv + 2, argv + argc);
            if (args.empty() || (group != "users" && group != "devices")) {
                std::cerr << USAGE << "\n";
                return 2;
            }
            return group == "users" ? manageUsers(config, args) : manageDevices(config, args);
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
