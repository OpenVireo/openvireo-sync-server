#pragma once

// What the tests need from the operating system beyond the server's own code: a plain TCP client for the event
// stream, changing environment variables and removing temporary directories, on POSIX and Windows alike.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace testplatform
{
#ifdef _WIN32
    using Socket = SOCKET;
    constexpr Socket NoSocket = INVALID_SOCKET;
#else
    using Socket = int;
    constexpr Socket NoSocket = -1;
#endif

    // A blocking connection to 127.0.0.1 whose reads give up after `timeout`
    inline Socket connectLoopback(int port, std::chrono::milliseconds timeout)
    {
#ifdef _WIN32
        static const bool started = [] {
            WSADATA data;
            return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        (void)started;
#endif
        Socket s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == NoSocket) {
            throw std::runtime_error("cannot create a socket");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("cannot connect");
        }
#ifdef _WIN32
        DWORD value = static_cast<DWORD>(timeout.count());
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
        timeval value{static_cast<time_t>(timeout.count() / 1000), static_cast<suseconds_t>(timeout.count() % 1000 * 1000)};
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
        return s;
    }

    inline void sendAll(Socket s, const std::string& data)
    {
        ::send(s, data.data(), static_cast<int>(data.size()), 0);
    }

    // Bytes read, 0 when the peer closed the connection, negative on a timeout or error
    inline long receive(Socket s, char* buffer, std::size_t size)
    {
        return static_cast<long>(::recv(s, buffer, static_cast<int>(size), 0));
    }

    inline void closeSocket(Socket s)
    {
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
    }

    inline void setEnv(const char* name, const char* value)
    {
#ifdef _WIN32
        ::_putenv_s(name, value);
#else
        ::setenv(name, value, 1);
#endif
    }

    inline void unsetEnv(const char* name)
    {
#ifdef _WIN32
        // An empty value removes the variable
        ::_putenv_s(name, "");
#else
        ::unsetenv(name);
#endif
    }

    // Best effort: on Windows files the server still holds open (e.g. users.sqlite) can't be deleted, so they stay
    // behind in the temporary directory instead of failing the test run
    inline void removeAll(const std::filesystem::path& dir)
    {
#ifdef _WIN32
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
#else
        std::filesystem::remove_all(dir);
#endif
    }
} // namespace testplatform
