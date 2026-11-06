#include "SteamToolsPipeline.h"

#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "dllmain.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace SteamToolsPipeline {

namespace {

    // Manifest sanity check, kept in step with ManifestCache::LooksLikeManifest:
    // a real depot manifest starts with the payload magic and ends with the EOF
    // marker. Anything else is copied only if the caller asked for it as-is.
    constexpr unsigned char kPayloadMagic[4] = { 0xD0, 0x17, 0xF6, 0x71 }; // 0x71F617D0
    constexpr unsigned char kEofMagic[4] = { 0xAB, 0x15, 0xC4, 0x32 };     // 0x32C415AB
    constexpr size_t kMinManifestBytes = 16;

    // Import sub-folders, mirroring SteamTools' suffix dispatch. The empty key
    // is the import root, which only accepts the text/URL app-ID lists.
    constexpr const char* kManifestDir = "manifest";
    constexpr const char* kLuaDirName = "lua";
    constexpr const char* kBinDirName = "bin";

    // Destination sub-paths under <steam>.
    constexpr const char* kDepotCacheSubdir = "\\config\\depotcache";
    constexpr const char* kPluginSubdir = "\\config\\stplug-in";
    constexpr const char* kStatsExportSubdir = "\\config\\StatsExport";

    // Serialises Import()/GenerateLua() so two watchers firing at once cannot
    // interleave a copy with a regeneration.
    std::mutex g_pipelineMutex;

    struct Stats {
        uint32_t manifestAdded = 0;
        uint32_t manifestUpdated = 0;
        uint32_t luaAdded = 0;
        uint32_t luaUpdated = 0;
        uint32_t binAdded = 0;
        uint32_t binUpdated = 0;
        uint32_t luaRejected = 0;
        uint32_t appIdsParsed = 0;
        uint32_t appIdsUpdated = 0;
    };

    std::string SteamRoot() {
        return SteamInstallPath[0] ? std::string(SteamInstallPath) : std::string();
    }

    std::string ToLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string ExtensionOf(const fs::path& p) {
        return ToLower(p.extension().string());
    }

    bool IsDigits(std::string_view s) {
        return !s.empty() && std::all_of(s.begin(), s.end(),
                                         [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    bool IsManifestBytes(const std::string& body) {
        if (body.size() < kMinManifestBytes) return false;
        const auto* head = reinterpret_cast<const unsigned char*>(body.data());
        const auto* tail = head + body.size() - 4;
        return std::equal(head, head + 4, kPayloadMagic) &&
               std::equal(tail, tail + 4, kEofMagic);
    }

    bool ReadWholeFile(const fs::path& path, std::string* out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return in.good() || in.eof();
    }

    bool WriteWholeFile(const fs::path& path, const std::string& body) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        const fs::path tmp = fs::path(path).concat(".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
            out.flush();
            if (!out) {
                out.close();
                fs::remove(tmp, ec);
                return false;
            }
        }
        if (!MoveFileExA(tmp.string().c_str(), path.string().c_str(), MOVEFILE_REPLACE_EXISTING)) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    }

    // `<digits>_<digits>.manifest`; the extension's case is irrelevant.
    bool ParseConventionalName(const std::string& name, uint32_t* depot, uint64_t* gid) {
        const std::string lower = ToLower(name);
        constexpr std::string_view kSuffix = ".manifest";
        if (lower.size() <= kSuffix.size() ||
            lower.compare(lower.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            return false;

        const std::string stem = name.substr(0, name.size() - kSuffix.size());
        const size_t sep = stem.find('_');
        if (sep == std::string::npos || sep == 0 || sep + 1 >= stem.size()) return false;

        const std::string_view depotPart(stem.data(), sep);
        const std::string_view gidPart(stem.data() + sep + 1, stem.size() - sep - 1);
        if (!IsDigits(depotPart) || !IsDigits(gidPart)) return false;

        uint32_t d = 0;
        uint64_t g = 0;
        const auto dRes = std::from_chars(depotPart.data(), depotPart.data() + depotPart.size(), d);
        const auto gRes = std::from_chars(gidPart.data(), gidPart.data() + gidPart.size(), g);
        if (dRes.ec != std::errc() || gRes.ec != std::errc()) return false;

        if (depot) *depot = d;
        if (gid) *gid = g;
        return true;
    }

    // Drop-in replacement for SteamTools' FUN_140013200: make `<steam><subdir>`,
    // delete an existing target, copy, and report whether it was already there.
    bool CopyInto(const fs::path& source, const std::string& subdir,
                  bool* outExisted, std::string* outDest)
    {
        if (outExisted) *outExisted = false;
        const std::string root = SteamRoot();
        if (root.empty()) return false;

        const fs::path destDir = fs::path(root) / fs::path(subdir);
        std::error_code ec;
        fs::create_directories(destDir, ec);

        const fs::path dest = destDir / source.filename();
        const bool existed = fs::exists(dest, ec);
        if (existed) {
            fs::remove(dest, ec);
            if (ec) {
                LOG_MANIFEST_WARN("SteamTools: cannot replace {}", dest.string());
                return false;
            }
        }
        fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_MANIFEST_WARN("SteamTools: copy {} -> {} failed: {}",
                              source.string(), dest.string(), ec.message());
            return false;
        }
        if (outExisted) *outExisted = existed;
        if (outDest) *outDest = dest.string();
        return true;
    }

    void ImportManifest(const fs::path& file, Stats& stats) {
        std::string body;
        if (!ReadWholeFile(file, &body)) {
            LOG_MANIFEST_WARN("SteamTools: cannot read manifest {}", file.string());
            return;
        }
        if (!IsManifestBytes(body)) {
            LOG_MANIFEST_WARN("SteamTools: skipping {} (not a depot manifest: {} bytes)",
                              file.filename().string(), body.size());
            return;
        }

        bool existed = false;
        std::string dest;
        if (!CopyInto(file, kDepotCacheSubdir, &existed, &dest)) return;

        existed ? ++stats.manifestUpdated : ++stats.manifestAdded;

        // A manifest in depotcache only short-circuits Steam's request-code path
        // when the depot entry points at that gid, so pin it as well.
        uint32_t depot = 0;
        uint64_t gid = 0;
        if (ParseConventionalName(file.filename().string(), &depot, &gid)) {
            LuaConfig::SetManifestOverrideImmediate(depot, gid);
            LOG_MANIFEST_INFO("SteamTools: {} depot={} gid={}",
                              existed ? "updated" : "imported", depot, gid);
        } else {
            LOG_MANIFEST_INFO("SteamTools: {} {} (no depot_gid in name; add a "
                              "setmanifestid() line to pin it)",
                              existed ? "updated" : "imported", dest);
        }
    }

    void ImportLua(const fs::path& file, Stats& stats) {
        std::string body;
        if (!ReadWholeFile(file, &body) || ContainsAddSubId(body)) {
            ++stats.luaRejected;
            LOG_MANIFEST_WARN("SteamTools: rejected lua {} (contains addsubid)",
                              file.filename().string());
            return;
        }
        bool existed = false;
        if (!CopyInto(file, kPluginSubdir, &existed, nullptr)) return;
        existed ? ++stats.luaUpdated : ++stats.luaAdded;
    }

    void ImportBin(const fs::path& file, Stats& stats) {
        bool existed = false;
        if (!CopyInto(file, kStatsExportSubdir, &existed, nullptr)) return;
        existed ? ++stats.binUpdated : ++stats.binAdded;
    }

    // App-ID list: one record per line, `appid` or `appid,type[,decryptionkey]`.
    // "#" and "//" start a comment, blank lines are ignored. Returns the number
    // of IDs registered.
    uint32_t ImportAppIdList(const fs::path& file, Stats& stats) {
        std::ifstream in(file);
        if (!in) {
            LOG_MANIFEST_WARN("SteamTools: cannot read app-id list {}", file.string());
            return 0;
        }

        uint32_t count = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto firstNonSpace = line.find_first_not_of(" \t");
            if (firstNonSpace == std::string::npos) continue;
            if (line.compare(firstNonSpace, 1, "#") == 0 ||
                line.compare(firstNonSpace, 2, "//") == 0)
                continue;

            std::vector<std::string> fields;
            size_t start = firstNonSpace;
            while (start <= line.size()) {
                const size_t comma = line.find(',', start);
                const size_t end = comma == std::string::npos ? line.size() : comma;
                std::string field = line.substr(start, end - start);
                const auto b = field.find_first_not_of(" \t");
                const auto e = field.find_last_not_of(" \t");
                field = (b == std::string::npos) ? std::string() : field.substr(b, e - b + 1);
                fields.push_back(std::move(field));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (fields.empty() || !IsDigits(fields[0])) continue;

            uint32_t id = 0;
            const std::string& idText = fields[0];
            if (std::from_chars(idText.data(), idText.data() + idText.size(), id).ec != std::errc())
                continue;

            const std::string key = fields.size() >= 3 ? fields[2] : std::string();
            const bool hadEntry = LuaConfig::HasDepot(id, false);
            LuaConfig::AddDepotImmediate(id, key);
            hadEntry ? ++stats.appIdsUpdated : ++stats.appIdsParsed;
            ++count;
        }
        return count;
    }

    // Walk one import sub-folder, dispatching on the file extension.
    void ImportSubdir(const fs::path& dir, const std::string& kind, Stats& stats) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;

        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;

            const fs::path& file = entry.path();
            const std::string ext = ExtensionOf(file);

            if (kind == kManifestDir) {
                if (ext == ".manifest") ImportManifest(file, stats);
            } else if (kind == kLuaDirName) {
                if (ext == ".lua") ImportLua(file, stats);
            } else if (kind == kBinDirName) {
                ImportBin(file, stats);            // any extension, like SteamTools
            } else if (ext == ".txt" || ext == ".url") {
                ImportAppIdList(file, stats);
            }
        }
    }

    // Build the file body from every depot currently registered with addappid()
    // plus every pinned manifest. Deterministic order keeps rewrites idempotent.
    std::string BuildLuaBody() {
        std::string out = kGeneratedLuaHeader;

        std::vector<AppId_t> depots = LuaConfig::GetAllDepotIds();
        std::sort(depots.begin(), depots.end());
        for (AppId_t id : depots) {
            const std::vector<uint8> key = LuaConfig::GetDecryptionKey(id);
            if (key.size() == 64) {
                out += std::format("addappid({}, 1, \"{}\")\n", id,
                                   std::string(reinterpret_cast<const char*>(key.data()), key.size()));
            } else {
                out += std::format("addappid({}, 1)\n", id);
            }
        }

        const auto& overrides = LuaConfig::GetManifestOverrides();
        std::map<uint64_t, uint64_t> ordered;
        for (const auto& [depot, pinned] : overrides) ordered.emplace(depot, pinned.gid);
        for (const auto& [depot, gid] : ordered) {
            out += std::format("setmanifestid({}, \"{}\")\n", depot, gid);
        }
        return out;
    }

} // namespace

std::string DefaultImportDir() {
    const std::string root = SteamRoot();
    if (root.empty()) return {};
    return (fs::path(root) / "opensteamtool" / "import").string();
}

std::string GeneratedLuaPath() {
    const std::string root = SteamRoot();
    if (root.empty()) return {};
    return (fs::path(root) / std::string(kPluginSubdir).substr(1) / kGeneratedLuaName).string();
}

bool IsConventionalManifestName(const std::string& name) {
    return ParseConventionalName(name, nullptr, nullptr);
}

bool ContainsAddSubId(const std::string& text) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + pos, end - pos);
        pos = end + 1;

        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);

        if (line.starts_with("#") || line.starts_with("--")) continue;
        if (!line.starts_with("addsubid")) continue;

        std::string_view rest = line.substr(std::string_view("addsubid").size());
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
        if (rest.empty() || rest.front() != '(') continue;
        rest.remove_prefix(1);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);

        size_t digits = 0;
        while (digits < rest.size() && std::isdigit(static_cast<unsigned char>(rest[digits]))) ++digits;
        if (digits == 0) continue;
        rest.remove_prefix(digits);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
        if (!rest.empty() && rest.front() == ')') return true;
    }
    return false;
}

uint32_t Import(const std::string& importDir) {
    if (importDir.empty()) return 0;
    std::lock_guard<std::mutex> lock(g_pipelineMutex);

    std::error_code ec;
    if (!fs::is_directory(importDir, ec)) {
        LOG_MANIFEST_DEBUG("SteamTools: no import folder at {}", importDir);
        return 0;
    }

    Stats stats;
    const fs::path root(importDir);
    for (const char* sub : { kManifestDir, kLuaDirName, kBinDirName })
        ImportSubdir(root / sub, sub, stats);
    ImportSubdir(root, std::string(), stats);

    const uint32_t total = stats.manifestAdded + stats.manifestUpdated + stats.luaAdded +
                           stats.luaUpdated + stats.binAdded + stats.binUpdated +
                           stats.appIdsParsed + stats.appIdsUpdated;
    if (total) {
        LOG_MANIFEST_INFO("SteamTools: import from {} - appids +{}/{} lua +{}/{} "
                          "manifests +{}/{} bin +{}/{} ({} lua rejected)",
                          importDir,
                          stats.appIdsParsed, stats.appIdsUpdated,
                          stats.luaAdded, stats.luaUpdated,
                          stats.manifestAdded, stats.manifestUpdated,
                          stats.binAdded, stats.binUpdated,
                          stats.luaRejected);
    }

    // The generated file is a mirror of the live set, so refresh it whenever the
    // import may have changed it. GenerateLua() takes the lock itself (mutex is
    // not recursive), so release first.
    if (total) {
        g_pipelineMutex.unlock();
        GenerateLua();
        g_pipelineMutex.lock();
    }
    return total;
}

bool GenerateLua() {
    std::string path = GeneratedLuaPath();
    if (path.empty()) return false;

    std::lock_guard<std::mutex> lock(g_pipelineMutex);

    const std::string body = BuildLuaBody();
    std::string existing;
    if (ReadWholeFile(path, &existing) && existing == body) return false;

    if (!WriteWholeFile(path, body)) {
        LOG_MANIFEST_WARN("SteamTools: cannot write {}", path);
        return false;
    }
    LOG_MANIFEST_INFO("SteamTools: generated {} ({} bytes)", path, body.size());
    return true;
}

} // namespace SteamToolsPipeline
