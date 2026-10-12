#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace syncserver
{
    // User names and database ids end up as path components, so keep them strict.
    bool isValidName(std::string_view name);

    std::string sha256Hex(std::string_view data);
    bool constantTimeEquals(std::string_view a, std::string_view b);

    // ISO 8601 with milliseconds in UTC, e.g. 2026-10-12T08:30:00.000Z
    std::string isoTimestamp(std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
    // RFC 9110 IMF-fixdate for an ISO timestamp produced by isoTimestamp()
    std::string httpDateFromIso(const std::string& iso);

    std::string readFile(const std::filesystem::path& file);
    // Writes to a temporary file (mode 0600) and renames it over `file`, so readers never see a partial file
    void atomicWriteFile(const std::filesystem::path& file, std::string_view contents);
    // Creates the directory and any missing parents with mode 0700
    void ensurePrivateDirectory(const std::filesystem::path& dir);
} // namespace syncserver
