#include "DatabaseStore.h"

#include "Util.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace fs = std::filesystem;

namespace syncserver
{
    namespace
    {
        constexpr std::string_view META_SUFFIX = ".meta.json";

        std::optional<Json::Value> parseJson(const std::string& text)
        {
            Json::Value json;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if (!reader->parse(text.data(), text.data() + text.size(), &json, &errors) || !json.isObject()) {
                return std::nullopt;
            }
            return json;
        }

        std::string toJsonString(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "  ";
            return Json::writeString(builder, value);
        }

        DatabaseMeta metaFromJson(const Json::Value& json, const std::string& id)
        {
            DatabaseMeta meta;
            meta.id = json.get("id", id).asString();
            meta.revision = json["revision"].asInt64();
            meta.etag = json["etag"].asString();
            meta.size = json["size"].asUInt64();
            meta.updatedAt = json["updatedAt"].asString();
            return meta;
        }

        // Kept versions are named <revision>.kdbx
        std::optional<int64_t> revisionOf(const fs::path& path)
        {
            if (path.extension() != ".kdbx") {
                return std::nullopt;
            }
            try {
                return std::stoll(path.stem().string());
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }

        std::chrono::system_clock::time_point toSystemTime(fs::file_time_type time)
        {
            // C++17 has no clock_cast; both clocks advance together, so carry the offset over
            using namespace std::chrono;
            return time_point_cast<system_clock::duration>(time - fs::file_time_type::clock::now()
                                                           + system_clock::now());
        }
    } // namespace

    DatabaseStore::DatabaseStore(const fs::path& dataDir, unsigned historySize, const char* folder)
        : m_root(dataDir / folder)
        , m_historySize(historySize)
    {
    }

    DatabaseStore::Paths DatabaseStore::paths(const std::string& user, const std::string& id) const
    {
        // The empty user (vaults) keeps its files right in the root folder
        auto dir = user.empty() ? m_root : m_root / user;
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
        auto json = parseJson(readFile(file));
        if (!json) {
            throw std::runtime_error("Invalid " + file.string());
        }
        return metaFromJson(*json, id);
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
        for (const auto& item : fs::directory_iterator(user.empty() ? m_root : m_root / user, ec)) {
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
            const auto name = std::to_string(current->revision);
            fs::copy_file(p.blob, p.history / (name + ".kdbx"), fs::copy_options::overwrite_existing);
            atomicWriteFile(p.history / (name + ".json"), toJsonString(current->toJson()));
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
        atomicWriteFile(p.meta, toJsonString(meta.toJson()));
        if (m_listener) {
            m_listener(user, changeEvent(meta));
        }
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
        if (m_listener) {
            m_listener(user, deletedEvent(id));
        }
        return true;
    }

    void DatabaseStore::setChangeListener(ChangeListener listener)
    {
        m_listener = std::move(listener);
    }

    void DatabaseStore::broadcast(const Json::Value& message)
    {
        // One process: every listener is here
        if (m_broadcastListener) {
            m_broadcastListener(message);
        }
    }

    void DatabaseStore::setBroadcastListener(BroadcastListener listener)
    {
        m_broadcastListener = std::move(listener);
    }

    bool
    DatabaseStore::importDatabase(const std::string& user, const std::string& id, const std::vector<Entry>& versions)
    {
        if (versions.empty()) {
            return false;
        }
        auto mutex = lockFor(user, id);
        std::lock_guard lock(*mutex);
        if (readMeta(user, id)) {
            return false;
        }
        auto p = paths(user, id);
        ensurePrivateDirectory(p.dir);
        // Under its new id when it moves, e.g. from a user's databases to a vault
        auto metaOf = [&id](const Entry& entry) {
            auto meta = entry.meta;
            meta.id = id;
            meta.size = entry.data.size();
            if (meta.updatedAt.empty()) {
                meta.updatedAt = isoTimestamp();
            }
            return meta;
        };
        for (std::size_t i = 1; i < versions.size(); ++i) {
            ensurePrivateDirectory(p.history);
            const auto name = std::to_string(versions[i].meta.revision);
            atomicWriteFile(p.history / (name + ".kdbx"), versions[i].data);
            atomicWriteFile(p.history / (name + ".json"), toJsonString(metaOf(versions[i]).toJson()));
        }
        auto meta = metaOf(versions.front());
        atomicWriteFile(p.blob, versions.front().data);
        atomicWriteFile(p.meta, toJsonString(meta.toJson()));
        return true;
    }

    std::vector<std::string> DatabaseStore::owners() const
    {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& item : fs::directory_iterator(m_root, ec)) {
            auto name = item.path().filename().string();
            if (item.is_directory() && isValidName(name)) {
                names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    void DatabaseStore::pruneHistory(const fs::path& historyDir) const
    {
        std::vector<int64_t> revisions;
        for (const auto& item : fs::directory_iterator(historyDir)) {
            if (auto revision = revisionOf(item.path())) {
                revisions.push_back(*revision);
            }
        }
        std::sort(revisions.begin(), revisions.end(), std::greater<>());
        for (size_t i = m_historySize; i < revisions.size(); ++i) {
            const auto name = std::to_string(revisions[i]);
            fs::remove(historyDir / (name + ".kdbx"));
            fs::remove(historyDir / (name + ".json"));
        }
    }

    DatabaseMeta DatabaseStore::historyMeta(const std::string& id, const fs::path& blob)
    {
        auto jsonFile = blob;
        jsonFile.replace_extension(".json");
        if (fs::exists(jsonFile)) {
            if (auto json = parseJson(readFile(jsonFile))) {
                return metaFromJson(*json, id);
            }
        }
        auto data = readFile(blob);
        DatabaseMeta meta;
        meta.id = id;
        meta.revision = revisionOf(blob).value_or(0);
        meta.etag = "\"" + sha256Hex(data) + "\"";
        meta.size = data.size();
        meta.updatedAt = isoTimestamp(toSystemTime(fs::last_write_time(blob)));
        return meta;
    }

    std::optional<std::vector<DatabaseMeta>> DatabaseStore::history(const std::string& user, const std::string& id)
    {
        auto mutex = lockFor(user, id);
        std::lock_guard lock(*mutex);
        auto current = readMeta(user, id);
        if (!current) {
            return std::nullopt;
        }
        std::vector<DatabaseMeta> versions{*current};
        std::error_code ec;
        for (const auto& item : fs::directory_iterator(paths(user, id).history, ec)) {
            if (revisionOf(item.path())) {
                versions.push_back(historyMeta(id, item.path()));
            }
        }
        std::sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) {
            return a.revision > b.revision;
        });
        return versions;
    }

    std::optional<DatabaseStore::Entry>
    DatabaseStore::readRevision(const std::string& user, const std::string& id, int64_t revision)
    {
        auto mutex = lockFor(user, id);
        std::lock_guard lock(*mutex);
        auto current = readMeta(user, id);
        if (!current) {
            return std::nullopt;
        }
        auto p = paths(user, id);
        if (revision == current->revision) {
            return Entry{std::move(*current), readFile(p.blob)};
        }
        auto blob = p.history / (std::to_string(revision) + ".kdbx");
        if (!fs::exists(blob)) {
            return std::nullopt;
        }
        return Entry{historyMeta(id, blob), readFile(blob)};
    }
} // namespace syncserver
