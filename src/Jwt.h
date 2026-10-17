#pragma once

#include "Storage.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace syncserver
{
    /**
     * Verifies account access tokens issued by the auth server (SPEC.md §9): compact JWS with alg EdDSA
     * (Ed25519), checked against the auth server's JWKS. Accepts only EdDSA; checks iss, aud (must contain
     * the configured audience), exp and nbf with 60 seconds of clock skew.
     *
     * The JWKS is fetched on first use and cached. A token with an unknown kid triggers a refetch, at most
     * once every 30 seconds; the cache is also refreshed when it is older than an hour. A failed fetch keeps
     * the keys we have.
     */
    class JwtVerifier
    {
    public:
        // Seconds since the Unix epoch
        using Clock = std::function<int64_t()>;
        // Returns the JWKS document, nullopt when it couldn't be fetched
        using Fetcher = std::function<std::optional<std::string>()>;

        struct Claims
        {
            // "user:<uuid>" or "sa:<uuid>"
            std::string subject;
            std::map<std::string, VaultRole> vaults;
            int64_t expiresAt = 0;
        };

        static constexpr int64_t ClockSkew = 60;
        static constexpr int64_t RefetchInterval = 30;
        static constexpr int64_t MaxCacheAge = 3600;

        JwtVerifier(std::string issuer, std::string audience, Fetcher fetcher, Clock clock = systemClock());
        ~JwtVerifier();

        // nullopt when the token is malformed, not signed by a known key, or fails a claim check
        std::optional<Claims> verify(std::string_view token);

        // How many times the JWKS was fetched, for tests
        int fetchCount() const;

        static Clock systemClock();
        // GETs the URL (http or https) on an event loop of its own, so it is safe to call from a request handler
        static Fetcher httpFetcher(const std::string& url);

    private:
        struct Key;

        std::shared_ptr<Key> keyFor(const std::string& kid);
        // Replaces the cached keys; false when the document has no usable key
        bool refresh();

        std::string m_issuer;
        std::string m_audience;
        Fetcher m_fetcher;
        Clock m_clock;

        mutable std::mutex m_mutex;
        std::map<std::string, std::shared_ptr<Key>> m_keys;
        std::optional<int64_t> m_lastAttempt;
        std::optional<int64_t> m_lastSuccess;
        int m_fetches = 0;
    };

    // Lowercase hyphenated UUID, as used for vault ids and principals
    bool isUuid(std::string_view text);
    // Decodes base64url, with or without padding; nullopt on any other character
    std::optional<std::string> base64UrlDecode(std::string_view text);
} // namespace syncserver
