#include "Util.h"

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

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
        std::tm tm{};
        gmtime_r(&seconds, &tm);
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
        std::time_t seconds = timegm(&tm);
        gmtime_r(&seconds, &tm);
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
} // namespace syncserver
