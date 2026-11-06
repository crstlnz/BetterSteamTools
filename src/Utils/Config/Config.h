#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "Steam/Types.h"

namespace Config {

    enum class LogLevel { Trace, Debug, Info, Warn, Error };

    struct ManifestTimeouts {
        uint32_t resolve = 5000;
        uint32_t connect = 5000;
        uint32_t send    = 10000;
        uint32_t recv    = 10000;
    };

    // [[inject]] entry: a DLL loaded into a matching game process at the IPC handshake.
    struct InjectDll {
        std::string                 path;        // resolved absolute path
        std::string                 whenCmdline; // substring required in the game command line
        std::unordered_set<AppId_t> whenAppids;  // appids this entry applies to
        bool                        allGames = false;  // false: only Lua-unlocked games
    };

    struct CloudSettings {
        bool enabled = false;
        std::string library;
    };

    // [family_sharing] — decide whether the published AppOwnership struct in
    // CheckAppOwnership may be rewritten for apps lent by another Family member.
    // On by default: the bypass needs no entry in the TOML at all, and setting
    // `bypass = false` is what turns it off.
    struct FamilySharingSettings {
        bool bypass = true;
    };

    // [steamtools] — SteamTools-compatible import pipeline. On by default with an
    // empty path, which resolves to <steam>\opensteamtool\import; `enabled`
    // turns the copy/generate steps off without deleting anything already there.
    struct SteamToolsSettings {
        bool        enabled = true;
        std::string importPath;   // empty = <steam>\opensteamtool\import
    };

    struct LoadResult {
        bool applied = false;
        bool luaPathsChanged = false;
    };

    LoadResult Load(const std::string& configPath);

    ManifestTimeouts GetManifestTimeouts();
    LogLevel GetLogLevel();
    std::string GetLogDir();
    std::vector<std::string> GetLuaPaths();
    std::vector<std::string> GetRemoteUrlTemplates();
    CloudSettings GetCloudSettings();
    bool GetStatsEnableApi();
    bool GetUpdateEnabled();
    FamilySharingSettings GetFamilySharingSettings();
    SteamToolsSettings GetSteamToolsSettings();

    // [donate] — contribute manifest request codes for depots this account owns.
    struct DonateSettings {
        bool        enabled  = true;
        std::string url;                       // base; empty = built-in default
        uint32_t    intervalSecs        = 30;
        uint32_t    maxMintsPerCycle    = 25;
        uint32_t    minMintIntervalMs   = 2000;
        uint32_t    maxMintsPerSession  = 0;     // 0 = unlimited (mint all session)
        uint32_t    wantedRefreshSecs   = 300;   // re-pull the (large) wanted list only this often; minting still runs every intervalSecs
    };
    DonateSettings GetDonateSettings();

    // [manifest] — provider selection lives in ManifestClient (table-driven).
    inline uint32_t manifestTimeoutResolve = 5000;
    inline uint32_t manifestTimeoutConnect = 5000;
    inline uint32_t manifestTimeoutSend    = 10000;
    inline uint32_t manifestTimeoutRecv    = 10000;

    // [log]
    inline LogLevel logLevel = LogLevel::Debug;

    // derived from configPath: <steam>/opensteamtool/
    inline std::string logDir;

    // [lua]
    inline std::vector<std::string> luaPaths;

    // [remote] — one or more mirror templates, tried in order. Empty = built-in defaults.
    inline std::vector<std::string> remoteUrlTemplates;

    // [stats]
    inline bool statsEnableApi = true;

    // [update] - self-update check on startup (staged for next Steam launch).
    inline bool updateEnabled = true;

    // [donate] - mint manifest request codes on request for depots this account
    // owns. Codes are bound to (depot, manifest) and rotate within minutes, so
    // they are minted on demand and sent straight on, never stored. The caps
    // exist because this calls Steam as the signed-in user.
    inline DonateSettings donate;

    // [[inject]] - optional DLL injection into matching game processes.
    inline std::vector<InjectDll> injectDlls;

    // [cloud] - optional Steam Cloud save redirection via CloudRedirect.
    inline bool cloudEnabled = false;
    inline std::string cloudLibrary;

    // [family_sharing] - default settings; nothing in the TOML is required.
    inline FamilySharingSettings familySharing;

    // [steamtools] - import pipeline defaults (see Config.h).
    inline SteamToolsSettings steamTools;

}
