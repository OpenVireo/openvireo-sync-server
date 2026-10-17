#include "BlobStore.h"

#include "Util.h"

#include <drogon/utils/Utilities.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <stdexcept>
#include <thread>

namespace syncserver
{
    namespace
    {
        constexpr double REQUEST_TIMEOUT_SECONDS = 120;
        constexpr int ATTEMPTS = 3;

        std::string hmac(std::string_view key, std::string_view data)
        {
            unsigned char out[EVP_MAX_MD_SIZE];
            unsigned int length = 0;
            if (!HMAC(EVP_sha256(),
                      key.data(),
                      static_cast<int>(key.size()),
                      reinterpret_cast<const unsigned char*>(data.data()),
                      data.size(),
                      out,
                      &length)) {
                throw std::runtime_error("HMAC-SHA256 failed");
            }
            return std::string(reinterpret_cast<const char*>(out), length);
        }

        std::string hex(std::string_view bytes)
        {
            return drogon::utils::binaryStringToHex(
                reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), true);
        }

        // RFC 3986 unreserved characters stay, everything else is %XX; "/" stays only in paths
        std::string uriEncode(std::string_view text, bool keepSlash)
        {
            static const char* digits = "0123456789ABCDEF";
            std::string out;
            for (unsigned char c : text) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) {
                    out += static_cast<char>(c);
                } else {
                    out += '%';
                    out += digits[c >> 4];
                    out += digits[c & 0xF];
                }
            }
            return out;
        }

        std::string xmlUnescape(std::string text)
        {
            static const std::pair<std::string_view, std::string_view> entities[] = {
                {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
            for (const auto& [entity, plain] : entities) {
                for (auto pos = text.find(entity); pos != std::string::npos; pos = text.find(entity, pos + 1)) {
                    text.replace(pos, entity.size(), plain);
                }
            }
            return text;
        }

        // The text of the first <tag> in [from, to), if any
        std::optional<std::string>
        element(const std::string& xml, const std::string& tag, size_t from = 0, size_t to = std::string::npos)
        {
            auto open = "<" + tag + ">";
            auto start = xml.find(open, from);
            if (start == std::string::npos || start >= to) {
                return std::nullopt;
            }
            start += open.size();
            auto end = xml.find("</" + tag + ">", start);
            if (end == std::string::npos || end > to) {
                return std::nullopt;
            }
            return xmlUnescape(xml.substr(start, end - start));
        }

        std::chrono::system_clock::time_point parseIsoTime(const std::string& iso)
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
            return std::chrono::system_clock::from_time_t(timegm(&tm));
        }
    } // namespace

    BlobStore::BlobStore(S3Config config)
        : m_config(std::move(config))
        , m_loop("s3")
    {
        auto endpoint = m_config.endpoint;
        while (!endpoint.empty() && endpoint.back() == '/') {
            endpoint.pop_back();
        }
        auto schemeEnd = endpoint.find("://");
        if (schemeEnd == std::string::npos) {
            throw std::runtime_error("SYNC_S3_ENDPOINT must start with https:// or http://");
        }
        auto scheme = endpoint.substr(0, schemeEnd);
        auto host = endpoint.substr(schemeEnd + 3);
        if (host.find('/') != std::string::npos || host.empty()) {
            throw std::runtime_error("SYNC_S3_ENDPOINT must be scheme://host[:port], without a path");
        }
        if (m_config.pathStyle) {
            m_host = host;
            m_bucketPath = "/" + m_config.bucket;
        } else {
            m_host = m_config.bucket + "." + host;
        }
        m_origin = scheme + "://" + m_host;
        m_loop.run();
    }

    BlobStore::~BlobStore()
    {
        m_idle.clear();
        m_loop.getLoop()->quit();
        m_loop.wait();
    }

    std::string BlobStore::describe() const
    {
        return m_config.bucket + " at " + m_config.endpoint
               + (m_config.prefix.empty() ? std::string() : " (prefix " + m_config.prefix + ")");
    }

    drogon::HttpClientPtr BlobStore::acquire()
    {
        {
            std::lock_guard lock(m_mutex);
            if (!m_idle.empty()) {
                auto client = std::move(m_idle.back());
                m_idle.pop_back();
                return client;
            }
        }
        return drogon::HttpClient::newHttpClient(m_origin, m_loop.getLoop());
    }

    void BlobStore::release(drogon::HttpClientPtr client)
    {
        std::lock_guard lock(m_mutex);
        m_idle.push_back(std::move(client));
    }

    BlobStore::Response BlobStore::send(drogon::HttpMethod method,
                                        const std::string& objectKey,
                                        const std::vector<std::pair<std::string, std::string>>& query,
                                        std::string_view body)
    {
        const auto path = m_bucketPath + "/" + uriEncode(objectKey, true);

        auto sortedQuery = query;
        std::sort(sortedQuery.begin(), sortedQuery.end());
        std::string canonicalQuery;
        for (const auto& [name, value] : sortedQuery) {
            canonicalQuery +=
                (canonicalQuery.empty() ? "" : "&") + uriEncode(name, false) + "=" + uriEncode(value, false);
        }

        const char* methodName = method == drogon::Put ? "PUT" : method == drogon::Delete ? "DELETE" : "GET";
        const auto payloadHash = sha256Hex(body);

        for (int attempt = 1;; ++attempt) {
            // Signed anew each attempt, since the signature covers the time
            char amzDate[17];
            char date[9];
            auto now = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&now, &tm);
            std::strftime(amzDate, sizeof(amzDate), "%Y%m%dT%H%M%SZ", &tm);
            std::strftime(date, sizeof(date), "%Y%m%d", &tm);

            const std::string signedHeaders = "host;x-amz-content-sha256;x-amz-date";
            const auto canonicalRequest = std::string(methodName) + "\n" + path + "\n" + canonicalQuery + "\n"
                                          + "host:" + m_host + "\n" + "x-amz-content-sha256:" + payloadHash + "\n"
                                          + "x-amz-date:" + amzDate + "\n\n" + signedHeaders + "\n" + payloadHash;
            const auto scope = std::string(date) + "/" + m_config.region + "/s3/aws4_request";
            const auto stringToSign =
                std::string("AWS4-HMAC-SHA256\n") + amzDate + "\n" + scope + "\n" + sha256Hex(canonicalRequest);
            auto signingKey = hmac("AWS4" + m_config.secretKey, date);
            signingKey = hmac(signingKey, m_config.region);
            signingKey = hmac(signingKey, "s3");
            signingKey = hmac(signingKey, "aws4_request");
            const auto signature = hex(hmac(signingKey, stringToSign));

            auto req = drogon::HttpRequest::newHttpRequest();
            req->setMethod(method);
            // Already encoded, and drogon would encode it again
            req->setPathEncode(false);
            req->setPath(canonicalQuery.empty() ? path : path + "?" + canonicalQuery);
            req->addHeader("host", m_host);
            req->addHeader("x-amz-date", amzDate);
            req->addHeader("x-amz-content-sha256", payloadHash);
            req->addHeader("authorization",
                           "AWS4-HMAC-SHA256 Credential=" + m_config.accessKey + "/" + scope
                               + ", SignedHeaders=" + signedHeaders + ", Signature=" + signature);
            if (method == drogon::Put) {
                req->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
                req->setBody(std::string(body));
            }

            auto client = acquire();
            auto [result, resp] = client->sendRequest(req, REQUEST_TIMEOUT_SECONDS);
            if (result == drogon::ReqResult::Ok && resp) {
                release(std::move(client));
                Response response{static_cast<int>(resp->statusCode()), std::string(resp->body())};
                // A server error or throttling (503 Slow Down): worth another try
                if (response.status >= 500 && attempt < ATTEMPTS) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
                    continue;
                }
                return response;
            }
            // A broken connection: drop the client, so the next one connects afresh
            if (attempt >= ATTEMPTS) {
                throw std::runtime_error("Request to the bucket failed: " + drogon::to_string(result));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
        }
    }

    void BlobStore::put(const std::string& key, std::string_view data)
    {
        auto resp = send(drogon::Put, m_config.prefix + key, {}, data);
        if (resp.status != 200) {
            throw std::runtime_error("Cannot store " + key + " in the bucket: HTTP " + std::to_string(resp.status) + " "
                                     + element(resp.body, "Code").value_or(""));
        }
    }

    std::optional<std::string> BlobStore::get(const std::string& key)
    {
        auto resp = send(drogon::Get, m_config.prefix + key, {}, {});
        if (resp.status == 404) {
            return std::nullopt;
        }
        if (resp.status != 200) {
            throw std::runtime_error("Cannot read " + key + " from the bucket: HTTP " + std::to_string(resp.status)
                                     + " " + element(resp.body, "Code").value_or(""));
        }
        return std::move(resp.body);
    }

    void BlobStore::remove(const std::string& key)
    {
        auto resp = send(drogon::Delete, m_config.prefix + key, {}, {});
        if (resp.status != 204 && resp.status != 200 && resp.status != 404) {
            throw std::runtime_error("Cannot delete " + key + " from the bucket: HTTP " + std::to_string(resp.status)
                                     + " " + element(resp.body, "Code").value_or(""));
        }
    }

    std::vector<BlobStore::Object> BlobStore::list(const std::string& keyPrefix)
    {
        std::vector<Object> objects;
        std::string continuation;
        do {
            std::vector<std::pair<std::string, std::string>> query{{"list-type", "2"},
                                                                   {"prefix", m_config.prefix + keyPrefix}};
            if (!continuation.empty()) {
                query.emplace_back("continuation-token", continuation);
            }
            // The bucket's root, "/" (or "/bucket/")
            auto resp = send(drogon::Get, "", query, {});
            if (resp.status != 200) {
                throw std::runtime_error("Cannot list the bucket: HTTP " + std::to_string(resp.status) + " "
                                         + element(resp.body, "Code").value_or(""));
            }
            for (auto pos = resp.body.find("<Contents>"); pos != std::string::npos;
                 pos = resp.body.find("<Contents>", pos + 1)) {
                auto end = resp.body.find("</Contents>", pos);
                auto key = element(resp.body, "Key", pos, end);
                if (!key || key->rfind(m_config.prefix, 0) != 0) {
                    continue;
                }
                objects.push_back({key->substr(m_config.prefix.size()),
                                   parseIsoTime(element(resp.body, "LastModified", pos, end).value_or(""))});
            }
            continuation = element(resp.body, "IsTruncated").value_or("false") == "true"
                               ? element(resp.body, "NextContinuationToken").value_or("")
                               : std::string();
        } while (!continuation.empty());
        return objects;
    }
} // namespace syncserver
