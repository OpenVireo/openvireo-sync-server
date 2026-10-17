#pragma once

#include "Config.h"
#include "Storage.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace syncserver
{
    // Hooks for tests; the defaults are the real clock and an HTTP GET of SYNC_AUTH_JWKS_URL
    struct SyncApiOptions
    {
        // Seconds since the Unix epoch, for checking tokens and closing streams when their token expires
        std::function<int64_t()> clock;
        // Returns the JWKS document, nullopt when it can't be fetched
        std::function<std::optional<std::string>()> jwksFetcher;
    };

    // Registers the /api/v1 routes, error handlers and response headers on drogon::app()
    void registerSyncApi(const Config& config, Storage storage, SyncApiOptions options = {});
} // namespace syncserver
