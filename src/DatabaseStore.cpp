#include "DatabaseStore.h"

#include "Util.h"

#include <algorithm>
#include <stdexcept>

namespace fs = std::filesystem;

namespace syncserver
{
    namespace
    {
        constexpr std::string_view META_SUFFIX = ".meta.json";
    } // namespace

    Json::Value DatabaseMeta::toJson() const
    {
        Json::Value json;
        json["id"] = id;
        json["revision"] = static_cast<Json::Int64>(revision);
        json["etag"] = etag;
        json["size"] = static_cast<Json::UInt64>(size);
        json["updatedAt"] = updatedAt;
        return json;
    }

    DatabaseStore::DatabaseStore(const fs::path& dataDir, unsigned historySize)
        : m_root(dataDir / "databases")
        , m_historySize(historySize)
    {
    }

    DatabaseStore::Paths DatabaseStore::paths(const std::string& user, const std::string& id) const
    {
        auto dir = m_root / user;
        return {dir, dir / (id + ".kdbx"), dir / (id + std::string(META_SUFFIX)), dir / ".history" / id};
    }

    std::shared_ptr<std::mutex> DatabaseStore::lockFor(const std::string& user, const std::string& id)
    {
        std::lock_guard lock(m_locksMutex);
        auto& mutex = m_locks[user + "/" + id];
        if (!mutex) {
            mutex = std::make_shared<std::mutex>();
        }
        return mutex;
    }

    std::optional<DatabaseMeta> DatabaseStore::readMeta(const std::string& user, const std::string& id) const
    {
        auto file = paths(user, id).meta;
        if (!fs::exists(file)) {
            return std::nullopt;
        }
        auto text = readFile(file);
        Json::Value json;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        if (!reader->parse(text.data(), text.data() + text.size(), &json, &errors) || !json.isObject()) {
            throw std::runtime_error("Invalid " + file.string() + ": " + errors);
        }
        DatabaseMeta meta;
        meta.id = json.get("id", id).asString();
        meta.revision = json["revision"].asInt64();
        meta.etag = json["etag"].asString();
        meta.size = json["size"].asUInt64();
        meta.updatedAt = json["updatedAt"].asString();
        return meta;
    }

    std::optional<DatabaseMeta> DatabaseStore::meta(const std::string& user, const std::string& id)
    {
        auto mutex = lockFor(user, id);
        std::lock_guard lock(*mutex);
        return readMeta(user, id);
    }

    std::optional<DatabaseStore::Entry> DatabaseStore::read(const std::string& user, const std::string& id)
    {
        // Read metadata and blob under the lock so they always belong together
        auto mutex = lockFor(user, id);
        std::lock_guard lock(*mutex);
        auto meta = readMeta(user, id);
        if (!meta) {
            return std::nullopt;
        }
        return Entry{std::move(*meta), readFile(paths(user, id).blob)};
    }

    std::vector<DatabaseMeta> DatabaseStore::list(const std::string& user)
    {
        std::vector<DatabaseMeta> metas;
        std::error_code ec;
        for (const auto& item : fs::directory_iterator(m_root / user, ec)) {
            auto name = item.path().filename().string();
            if (name.size() <= META_SUFFIX.size()
                || name.compare(name.size() - META_SUFFIX.size(), META_SUFFIX.size(), META_SUFFIX) != 0) {
                continue;
            }
            auto id = name.substr(0, name.size() - META_SUFFIX.size());
            if (auto meta = this->meta(user, id)) {
                metas.push_back(std::move(*meta));
            }
        }
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw std::system_error(ec, "Cannot list databases of " + user);
        }
        std::sort(metas.begin(), metas.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        return metas;
    }

    DatabaseStore::WriteResult DatabaseStore::write(const std::string& user,
                                                    const std::string& id,
                                                    std::string_view data,
                                                    const Precondition& precondition)
    {
        auto mutex = lockFor(user, id);
        std::lock_guard lock(*mutex);

        auto current = readMeta(user, id);
        precondition(current);

        auto p = paths(user, id);
        ensurePrivateDirectory(p.dir);

        if (current && m_historySize > 0) {
            ensurePrivateDirectory(p.history);
            fs::copy_file(p.blob,
                          p.history / (std::to_string(current->revision) + ".kdbx"),
                          fs::copy_options::overwrite_existing);
            pruneHistory(p.history);
        }

        DatabaseMeta meta;
        meta.id = id;
        meta.revision = (current ? current->revision : 0) + 1;
        meta.etag = "\"" + sha256Hex(data) + "\"";
        meta.size = data.size();
        meta.updatedAt = isoTimestamp();

        // Blob first, then metadata, each via rename so readers never see a partial file
        atomicWriteFile(p.blob, data);
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        atomicWriteFile(p.meta, Json::writeString(builder, meta.toJson()));
        return {std::move(meta), !current};
    }

    bool DatabaseStore::remove(const std::string& user, const std::string& id, const Precondition& precondition)
    {
        auto mutex = lockFor(user, id);
        std::lock_guard lock(*mutex);

        auto current = readMeta(user, id);
        precondition(current);
        if (!current) {
            return false;
        }
        auto p = paths(user, id);
        fs::remove(p.meta);
        fs::remove(p.blob);
        fs::remove_all(p.history);
        return true;
    }

    void DatabaseStore::pruneHistory(const fs::path& historyDir) const
    {
        std::vector<int64_t> revisions;
        for (const auto& item : fs::directory_iterator(historyDir)) {
            const auto& path = item.path();
            if (path.extension() != ".kdbx") {
                continue;
            }
            try {
                revisions.push_back(std::stoll(path.stem().string()));
            } catch (const std::exception&) {
                // Not one of ours
            }
        }
        std::sort(revisions.begin(), revisions.end(), std::greater<>());
        for (size_t i = m_historySize; i < revisions.size(); ++i) {
            fs::remove(historyDir / (std::to_string(revisions[i]) + ".kdbx"));
        }
    }
} // namespace syncserver
