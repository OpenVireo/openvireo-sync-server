#pragma once

#include "Config.h"

#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncserver
{
    /**
     * Blobs in an S3-compatible bucket (DigitalOcean Spaces, MinIO, AWS), with plain PUT, GET, DELETE and
     * LIST requests signed with AWS Signature Version 4. Calls block the calling thread; the requests run
     * on the store's own event loop, so that is safe from a request handler.
     */
    class BlobStore
    {
    public:
        struct Object
        {
            std::string key;
            std::chrono::system_clock::time_point lastModified;
        };

        explicit BlobStore(S3Config config);
        ~BlobStore();

        // Keys are relative to the configured prefix
        void put(const std::string& key, std::string_view data);
        // nullopt when there is no such key
        std::optional<std::string> get(const std::string& key);
        // Succeeds when the key doesn't exist
        void remove(const std::string& key);
        std::vector<Object> list(const std::string& keyPrefix = {});

        // Bucket and endpoint, without credentials
        std::string describe() const;

    private:
        struct Response
        {
            int status = 0;
            std::string body;
        };

        // objectKey includes the prefix
        Response send(drogon::HttpMethod method,
                      const std::string& objectKey,
                      const std::vector<std::pair<std::string, std::string>>& query,
                      std::string_view body);
        drogon::HttpClientPtr acquire();
        void release(drogon::HttpClientPtr client);

        S3Config m_config;
        // scheme://host[:port] the clients connect to, and the Host header that is signed
        std::string m_origin;
        std::string m_host;
        // Path to the bucket's root: "" for virtual-hosted style, "/bucket" for path style
        std::string m_bucketPath;

        trantor::EventLoopThread m_loop;
        std::mutex m_mutex;
        // One request at a time per client (one connection each), so idle ones are kept and reused
        std::vector<drogon::HttpClientPtr> m_idle;
    };
} // namespace syncserver
