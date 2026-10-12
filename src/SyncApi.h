#pragma once

#include "Config.h"

namespace syncserver
{
    // Registers the /api/v1 routes, error handlers and response headers on drogon::app()
    void registerSyncApi(const Config& config);
} // namespace syncserver
