#include "Util.h"

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace syncserver
{
    bool isValidName(std::string_view name)
    {
        if (name.empty() || name.size() > 64 || !std::isalnum(static_cast<unsigned char>(name.front()))) {
            return false;
        }
        return std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
        });
    }

    std::string sha256Hex(std::string_view data)
    {
        auto hex = drogon::utils::getSha256(data.data(), data.size());
        std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) { return std::tolower(c); });
        return hex;
    }

    bool constantTimeEquals(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        }
        return diff == 0;
    }

    std::string isoTimestamp(std::chrono::system_clock::time_point time)
    {
        using namespace std::chrono;
        auto ms = duration_cast<milliseconds>(time.time_since_epoch()).count();
        std::time_t seconds = ms / 1000;
        auto tm = utcTime(seconds);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        char out[40];
        std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms % 1000));
        return out;
    }

    std::string httpDateFromIso(const std::string& iso)
    {
        std::tm tm{};
        if (std::sscanf(iso.c_str(),
                        "%d-%d-%dT%d:%d:%d",
                        &tm.tm_year,
                        &tm.tm_mon,
                        &tm.tm_mday,
                        &tm.tm_hour,
                        &tm.tm_min,
                        &tm.tm_sec)
            != 6) {
            return {};
        }
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm = utcTime(fromUtcTime(tm));
        static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char out[40];
        std::snprintf(out,
                      sizeof(out),
                      "%s, %02d %s %04d %02d:%02d:%02d GMT",
                      days[tm.tm_wday],
                      tm.tm_mday,
                      months[tm.tm_mon],
                      tm.tm_year + 1900,
                      tm.tm_hour,
                      tm.tm_min,
                      tm.tm_sec);
        return out;
    }

    std::tm utcTime(std::time_t time)
    {
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        return tm;
    }

    std::time_t fromUtcTime(std::tm tm)
    {
#ifdef _WIN32
        return _mkgmtime(&tm);
#else
        return timegm(&tm);
#endif
    }

#ifdef _WIN32
    namespace
    {
        [[noreturn]] void throwLastError(const std::string& what)
        {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), what);
        }

        // Closes a Windows file handle when it goes out of scope
        struct Handle
        {
            HANDLE handle;
            ~Handle()
            {
                if (handle != INVALID_HANDLE_VALUE) {
                    ::CloseHandle(handle);
                }
            }
        };
    } // namespace

    // Shares delete access, unlike std::ifstream, so that atomicWriteFile can replace a file while it's being read,
    // as rename() can on POSIX
    std::string readFile(const fs::path& file)
    {
        Handle in{::CreateFileW(file.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr)};
        if (in.handle == INVALID_HANDLE_VALUE) {
            throwLastError("Cannot read " + file.string());
        }
        std::string contents;
        char buffer[65536];
        DWORD read = 0;
        do {
            if (!::ReadFile(in.handle, buffer, sizeof(buffer), &read, nullptr)) {
                throwLastError("Cannot read " + file.string());
            }
            contents.append(buffer, read);
        } while (read > 0);
        return contents;
    }

    // The temporary file inherits the directory's permissions (see ensurePrivateDirectory). MoveFileEx with
    // MOVEFILE_REPLACE_EXISTING replaces the target in one step, like rename() on POSIX; it fails while another
    // program holds the target open without sharing delete access (e.g. a virus scanner), so it's retried briefly.
    void atomicWriteFile(const fs::path& file, std::string_view contents)
    {
        auto tmp = file;
        tmp += "." + std::to_string(::_getpid()) + "." + drogon::utils::secureRandomString(8) + ".tmp";
        {
            Handle out{::CreateFileW(
                tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (out.handle == INVALID_HANDLE_VALUE) {
                throwLastError("Cannot create " + tmp.string());
            }
            const char* ptr = contents.data();
            size_t remaining = contents.size();
            while (remaining > 0) {
                DWORD written = 0;
                const auto chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 30));
                if (!::WriteFile(out.handle, ptr, chunk, &written, nullptr)) {
                    auto err = ::GetLastError();
                    ::CloseHandle(out.handle);
                    out.handle = INVALID_HANDLE_VALUE;
                    ::DeleteFileW(tmp.c_str());
                    throw std::system_error(static_cast<int>(err), std::system_category(), "Cannot write " + tmp.string());
                }
                ptr += written;
                remaining -= written;
            }
        }

        for (int attempt = 1;; ++attempt) {
            if (::MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return;
            }
            auto err = ::GetLastError();
            if ((err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) && attempt < 50) {
                ::Sleep(10);
                continue;
            }
            ::DeleteFileW(tmp.c_str());
            throw std::system_error(static_cast<int>(err), std::system_category(), "Cannot replace " + file.string());
        }
    }

    // POSIX modes don't apply: the directory inherits its parent's access control list, so the data directory
    // belongs somewhere only the server's account can read (see docs/install.md)
    void ensurePrivateDirectory(const fs::path& dir)
    {
        fs::create_directories(dir);
    }
#else
    std::string readFile(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            throw std::system_error(errno, std::generic_category(), "Cannot read " + file.string());
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void atomicWriteFile(const fs::path& file, std::string_view contents)
    {
        auto tmp = file;
        tmp += "." + std::to_string(::getpid()) + "." + drogon::utils::secureRandomString(8) + ".tmp";

        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "Cannot create " + tmp.string());
        }
        const char* ptr = contents.data();
        size_t remaining = contents.size();
        while (remaining > 0) {
            auto written = ::write(fd, ptr, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                int err = errno;
                ::close(fd);
                ::unlink(tmp.c_str());
                throw std::system_error(err, std::generic_category(), "Cannot write " + tmp.string());
            }
            ptr += written;
            remaining -= static_cast<size_t>(written);
        }
        ::close(fd);

        std::error_code ec;
        fs::rename(tmp, file, ec);
        if (ec) {
            fs::remove(tmp);
            throw std::system_error(ec, "Cannot replace " + file.string());
        }
    }

    void ensurePrivateDirectory(const fs::path& dir)
    {
        if (fs::create_directories(dir)) {
            fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
        }
    }
#endif

    FailureLimiter::FailureLimiter(std::size_t perKey, std::size_t total, std::chrono::seconds window)
        : m_perKey(perKey)
        , m_total(total)
        , m_window(window)
    {
    }

    void FailureLimiter::dropOld(Clock::time_point now)
    {
        while (!m_failures.empty() && m_failures.front().first + m_window <= now) {
            m_failures.pop_front();
        }
    }

    long long FailureLimiter::retryAfter(const std::string& key, Clock::time_point now)
    {
        std::lock_guard lock(m_mutex);
        dropOld(now);
        // The failure whose expiry brings the count back under the limit
        std::optional<Clock::time_point> until;
        if (m_failures.size() >= m_total) {
            until = m_failures[m_failures.size() - m_total].first;
        }
        std::vector<Clock::time_point> forKey;
        for (const auto& [time, failedKey] : m_failures) {
            if (failedKey == key) {
                forKey.push_back(time);
            }
        }
        if (forKey.size() >= m_perKey) {
            auto keyUntil = forKey[forKey.size() - m_perKey];
            until = until ? std::max(*until, keyUntil) : keyUntil;
        }
        if (!until) {
            return 0;
        }
        auto wait = std::chrono::duration_cast<std::chrono::seconds>(*until + m_window - now).count();
        return std::max<long long>(wait, 1);
    }

    void FailureLimiter::recordFailure(const std::string& key, Clock::time_point now)
    {
        std::lock_guard lock(m_mutex);
        dropOld(now);
        m_failures.emplace_back(now, key);
    }
} // namespace syncserver
