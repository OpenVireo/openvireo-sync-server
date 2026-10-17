#include "Jwt.h"

#include <drogon/HttpClient.h>
#include <json/json.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/utils/Logger.h>

#include <openssl/evp.h>

#include <chrono>

namespace syncserver
{
    namespace
    {
        constexpr std::size_t ED25519_KEY_SIZE = 32;
        constexpr std::size_t ED25519_SIGNATURE_SIZE = 64;

        std::optional<Json::Value> parseObject(std::string_view text)
        {
            Json::CharReaderBuilder builder;
            // Duplicate claims could make two verifiers read one token differently
            builder["rejectDupKeys"] = true;
            builder["failIfExtra"] = true;
            Json::Value value;
            std::string errors;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors) || !value.isObject()) {
                return std::nullopt;
            }
            return value;
        }

        std::optional<int64_t> numericDate(const Json::Value& value)
        {
            if (value.isInt64()) {
                return value.asInt64();
            }
            if (value.isDouble()) {
                // NumericDate may have a fraction; whole seconds are enough here
                double d = value.asDouble();
                if (d > -1e15 && d < 1e15) {
                    return static_cast<int64_t>(d);
                }
            }
            return std::nullopt;
        }

        bool isPrincipal(const std::string& subject)
        {
            // openapi.yaml: ^(user|sa):[0-9a-f-]{36}$
            std::string_view rest;
            if (subject.rfind("user:", 0) == 0) {
                rest = std::string_view(subject).substr(5);
            } else if (subject.rfind("sa:", 0) == 0) {
                rest = std::string_view(subject).substr(3);
            } else {
                return false;
            }
            return rest.size() == 36 && rest.find_first_not_of("0123456789abcdef-") == std::string_view::npos;
        }

        std::optional<VaultRole> roleFromString(const std::string& role)
        {
            if (role == "read") {
                return VaultRole::Read;
            }
            if (role == "write") {
                return VaultRole::Write;
            }
            if (role == "manage") {
                return VaultRole::Manage;
            }
            return std::nullopt;
        }
    } // namespace

    struct JwtVerifier::Key
    {
        explicit Key(EVP_PKEY* key)
            : pkey(key)
        {
        }
        ~Key()
        {
            EVP_PKEY_free(pkey);
        }
        Key(const Key&) = delete;
        Key& operator=(const Key&) = delete;

        bool verify(std::string_view message, std::string_view signature) const
        {
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            if (!ctx) {
                return false;
            }
            bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1
                      && EVP_DigestVerify(ctx,
                                          reinterpret_cast<const unsigned char*>(signature.data()),
                                          signature.size(),
                                          reinterpret_cast<const unsigned char*>(message.data()),
                                          message.size())
                             == 1;
            EVP_MD_CTX_free(ctx);
            return ok;
        }

        EVP_PKEY* pkey;
    };

    bool isUuid(std::string_view text)
    {
        if (text.size() != 36) {
            return false;
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (c != '-') {
                    return false;
                }
            } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    std::optional<std::string> base64UrlDecode(std::string_view text)
    {
        while (!text.empty() && text.back() == '=') {
            text.remove_suffix(1);
        }
        if (text.size() % 4 == 1) {
            return std::nullopt;
        }
        std::string out;
        out.reserve(text.size() * 3 / 4);
        uint32_t buffer = 0;
        int bits = 0;
        for (char c : text) {
            int value;
            if (c >= 'A' && c <= 'Z') {
                value = c - 'A';
            } else if (c >= 'a' && c <= 'z') {
                value = c - 'a' + 26;
            } else if (c >= '0' && c <= '9') {
                value = c - '0' + 52;
            } else if (c == '-') {
                value = 62;
            } else if (c == '_') {
                value = 63;
            } else {
                return std::nullopt;
            }
            buffer = (buffer << 6) | static_cast<uint32_t>(value);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += static_cast<char>((buffer >> bits) & 0xff);
            }
        }
        return out;
    }

    JwtVerifier::JwtVerifier(std::string issuer, std::string audience, Fetcher fetcher, Clock clock)
        : m_issuer(std::move(issuer))
        , m_audience(std::move(audience))
        , m_fetcher(std::move(fetcher))
        , m_clock(std::move(clock))
    {
    }

    JwtVerifier::~JwtVerifier() = default;

    JwtVerifier::Clock JwtVerifier::systemClock()
    {
        return [] {
            using namespace std::chrono;
            return static_cast<int64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
        };
    }

    int JwtVerifier::fetchCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_fetches;
    }

    bool JwtVerifier::refresh()
    {
        m_lastAttempt = m_clock();
        ++m_fetches;
        std::optional<std::string> document;
        try {
            document = m_fetcher();
        } catch (const std::exception& e) {
            LOG_WARN << "Cannot fetch the JWKS: " << e.what();
        }
        if (!document) {
            return false;
        }
        auto jwks = parseObject(*document);
        if (!jwks || !(*jwks)["keys"].isArray()) {
            LOG_WARN << "The JWKS is not a JSON object with \"keys\"";
            return false;
        }
        std::map<std::string, std::shared_ptr<Key>> keys;
        for (const auto& jwk : (*jwks)["keys"]) {
            if (!jwk.isObject() || jwk["kty"] != "OKP" || jwk["crv"] != "Ed25519" || !jwk["kid"].isString()
                || !jwk["x"].isString()) {
                continue;
            }
            // Keys for encryption, or for another algorithm, never verify tokens
            if ((jwk.isMember("use") && jwk["use"] != "sig") || (jwk.isMember("alg") && jwk["alg"] != "EdDSA")) {
                continue;
            }
            auto x = base64UrlDecode(jwk["x"].asString());
            if (!x || x->size() != ED25519_KEY_SIZE) {
                continue;
            }
            EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
                EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(x->data()), x->size());
            if (pkey) {
                keys[jwk["kid"].asString()] = std::make_shared<Key>(pkey);
            }
        }
        if (keys.empty()) {
            LOG_WARN << "The JWKS has no Ed25519 signing key";
            return false;
        }
        m_keys = std::move(keys);
        m_lastSuccess = m_lastAttempt;
        return true;
    }

    std::shared_ptr<JwtVerifier::Key> JwtVerifier::keyFor(const std::string& kid)
    {
        std::lock_guard lock(m_mutex);
        const auto now = m_clock();
        const bool mayFetch = !m_lastAttempt || now - *m_lastAttempt >= RefetchInterval || now < *m_lastAttempt;
        const bool stale = !m_lastSuccess || now - *m_lastSuccess >= MaxCacheAge;
        auto it = m_keys.find(kid);
        if ((it == m_keys.end() || stale) && mayFetch) {
            refresh();
            it = m_keys.find(kid);
        }
        return it == m_keys.end() ? nullptr : it->second;
    }

    std::optional<JwtVerifier::Claims> JwtVerifier::verify(std::string_view token)
    {
        auto firstDot = token.find('.');
        auto secondDot = firstDot == std::string_view::npos ? firstDot : token.find('.', firstDot + 1);
        if (secondDot == std::string_view::npos || token.find('.', secondDot + 1) != std::string_view::npos) {
            return std::nullopt;
        }
        auto headerText = base64UrlDecode(token.substr(0, firstDot));
        auto payloadText = base64UrlDecode(token.substr(firstDot + 1, secondDot - firstDot - 1));
        auto signature = base64UrlDecode(token.substr(secondDot + 1));
        if (!headerText || !payloadText || !signature) {
            return std::nullopt;
        }
        auto header = parseObject(*headerText);
        // Only EdDSA: never "none", never an algorithm chosen by whoever made the token
        if (!header || (*header)["alg"] != "EdDSA" || !(*header)["kid"].isString() || header->isMember("crit")
            || signature->size() != ED25519_SIGNATURE_SIZE) {
            return std::nullopt;
        }

        auto key = keyFor((*header)["kid"].asString());
        if (!key || !key->verify(token.substr(0, secondDot), *signature)) {
            return std::nullopt;
        }

        auto claims = parseObject(*payloadText);
        if (!claims) {
            return std::nullopt;
        }
        if (!(*claims)["iss"].isString() || (*claims)["iss"].asString() != m_issuer) {
            return std::nullopt;
        }
        const auto& aud = (*claims)["aud"];
        bool audienceOk = aud.isString() && aud.asString() == m_audience;
        if (aud.isArray()) {
            for (const auto& item : aud) {
                audienceOk = audienceOk || (item.isString() && item.asString() == m_audience);
            }
        }
        if (!audienceOk) {
            return std::nullopt;
        }

        const auto now = m_clock();
        auto exp = numericDate((*claims)["exp"]);
        if (!exp || now >= *exp + ClockSkew) {
            return std::nullopt;
        }
        if (claims->isMember("nbf")) {
            auto nbf = numericDate((*claims)["nbf"]);
            if (!nbf || now < *nbf - ClockSkew) {
                return std::nullopt;
            }
        }

        Claims result;
        result.expiresAt = *exp;
        if (!(*claims)["sub"].isString() || !isPrincipal((*claims)["sub"].asString())) {
            return std::nullopt;
        }
        result.subject = (*claims)["sub"].asString();
        const auto& vlt = (*claims)["vlt"];
        if (!vlt.isNull() && !vlt.isArray()) {
            return std::nullopt;
        }
        for (const auto& grant : vlt) {
            // An entry we can't read grants nothing
            if (!grant.isObject() || !grant["id"].isString() || !grant["r"].isString()) {
                continue;
            }
            auto id = grant["id"].asString();
            auto role = roleFromString(grant["r"].asString());
            if (!isUuid(id) || !role) {
                continue;
            }
            auto& current = result.vaults[id];
            current = std::max(current, *role);
        }
        return result;
    }

    JwtVerifier::Fetcher JwtVerifier::httpFetcher(const std::string& url)
    {
        auto schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
            throw std::runtime_error("SYNC_AUTH_JWKS_URL must be an http:// or https:// URL");
        }
        auto pathStart = url.find('/', schemeEnd + 3);
        std::string origin = pathStart == std::string::npos ? url : url.substr(0, pathStart);
        std::string path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
        std::string query;
        if (auto q = path.find('?'); q != std::string::npos) {
            query = path.substr(q + 1);
            path.erase(q);
        }
        auto loop = std::make_shared<trantor::EventLoopThread>("jwks");
        loop->run();
        return [loop, origin, path, query]() -> std::optional<std::string> {
            auto client = drogon::HttpClient::newHttpClient(origin, loop->getLoop());
            auto req = drogon::HttpRequest::newHttpRequest();
            req->setMethod(drogon::Get);
            req->setPath(path);
            size_t start = 0;
            while (start < query.size()) {
                auto end = query.find('&', start);
                if (end == std::string::npos) {
                    end = query.size();
                }
                auto pair = query.substr(start, end - start);
                auto eq = pair.find('=');
                req->setParameter(pair.substr(0, eq), eq == std::string::npos ? "" : pair.substr(eq + 1));
                start = end + 1;
            }
            auto [result, resp] = client->sendRequest(req, 10);
            if (result != drogon::ReqResult::Ok || !resp) {
                LOG_WARN << "Cannot fetch the JWKS from " << origin << path << ": " << drogon::to_string(result);
                return std::nullopt;
            }
            if (resp->statusCode() != drogon::k200OK) {
                LOG_WARN << "Cannot fetch the JWKS from " << origin << path << ": HTTP " << resp->statusCode();
                return std::nullopt;
            }
            return std::string(resp->body());
        };
    }
} // namespace syncserver
