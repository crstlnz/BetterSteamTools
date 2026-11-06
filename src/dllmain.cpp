#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/ManifestDonor.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/SteamMetadata/SteamToolsPipeline.h"
#include "Utils/Tokeer/TokeerBridge.h"
#ifdef OST_ENABLE_UPDATER
#include "Utils/Update/AppUpdater.h"
#endif
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DirectoryWatch.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Thread.h"

#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <windows.h>

// prepare key runtime paths.
bool InitializeSteamComponents()
{
    const std::string steamInstallPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    if (steamInstallPath.empty()) {
        return false;
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", steamInstallPath.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    sprintf_s(DiversionPath,   kRuntimePathCapacity, "%s\\bin\\diversion.dll", SteamInstallPath);
    sprintf_s(LuaDir,          kRuntimePathCapacity, "%s\\config\\stplug-in",  SteamInstallPath);
    sprintf_s(ConfigPath,      kRuntimePathCapacity, "%s\\opensteamtool.toml", SteamInstallPath);
    
    client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
    if (!client_hModule) {
        LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                  SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    LOG_INFO("Loaded steamclient64.dll from {}", SteamclientPath);
    
    ui_hModule = OSTPlatform::DynamicLibrary::Load(SteamUIPath);
    if(!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    Log::Init(selfModule);
    LOG_INFO("OpenSteamTool init thread started");

    if (!InitializeSteamComponents()) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // SteamTools-compatible import: copy <import>\{manifest,lua,bin} into the
    // Steam tree and pin the imported depots. Runs before the Lua directories are
    // parsed so files it drops into config\stplug-in are picked up in this pass.
    // The folder is a user drop point, so it is also watched for later drops.
    const Config::SteamToolsSettings steamTools = Config::GetSteamToolsSettings();
    std::string steamToolsImportDir;
    if (steamTools.enabled) {
        steamToolsImportDir = steamTools.importPath.empty()
            ? SteamToolsPipeline::DefaultImportDir()
            : steamTools.importPath;
        SteamToolsPipeline::Import(steamToolsImportDir);
    } else {
        LOG_INFO("[steamtools] import disabled");
    }

    // Load pattern files for steamclient64.dll and steamui.dll.
    // Each call computes the SHA-256 of the DLL on disk, checks the local
    // cache, and downloads from GitHub if needed.  Both calls are synchronous
    // but run on this worker thread, never under the loader lock.
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs =
        LuaConfig::MergeWatchDirs(Config::GetLuaPaths(), std::string(LuaDir));
    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    // Regenerate config\stplug-in\Steamtools.lua from the live set, and re-run the
    // import whenever the drop folder changes (both on detached workers).
    if (steamTools.enabled) {
        OSTPlatform::Thread::StartDetached([]() -> uint32_t {
            SteamToolsPipeline::GenerateLua();
            return 0;
        });
        if (!steamToolsImportDir.empty()) {
            // Watch::Open fails when the folder does not exist yet, so make the
            // drop point first (the import above already declines a missing one).
            std::error_code ec;
            std::filesystem::create_directories(steamToolsImportDir, ec);
            const std::string importDir = steamToolsImportDir;
            auto watch = std::make_shared<OSTPlatform::DirectoryWatch::Watch>();
            if (watch->Open(importDir, 64 * 1024) && watch->IssueRead()) {
                // shared_ptr: Watch is move-only, StartDetached takes a copyable
                // callable, and the thread outlives this scope.
                OSTPlatform::Thread::StartDetached([importDir, watch]() -> uint32_t {
                    for (;;) {
                        OSTPlatform::DirectoryWatch::Watch* watches[] = { watch.get() };
                        const auto result = OSTPlatform::DirectoryWatch::WaitAny(
                            std::span<OSTPlatform::DirectoryWatch::Watch*>(watches), 5000);
                        if (result.status == OSTPlatform::DirectoryWatch::WaitStatus::Failed) return 0;
                        if (result.status != OSTPlatform::DirectoryWatch::WaitStatus::Signaled) continue;

                        // Give the writer a moment to close the file before copying.
                        ::Sleep(500);
                        const bool changed = !watch->Drain().empty();
                        watch->IssueRead();
                        if (!changed) continue;

                        if (SteamToolsPipeline::Import(importDir)) {
                            LuaConfig::ReloadDirectories(
                                LuaConfig::MergeWatchDirs(Config::GetLuaPaths(), std::string(LuaDir)));
                        }
                    }
                });
            } else {
                LOG_WARN("[steamtools] cannot watch import folder {}", importDir);
            }
        }
    }

    SteamUI::CoreHook();
    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    // Contributes manifest request codes for depots this account owns, on
    // request. Started after the hooks are in place because it needs the
    // netpacket send path; it idles until the license list resolves anyway.
    ManifestDonor::Start();

    // Register the bst:// URI scheme so the website can drive code redemption via this
    // DLL (rundll32 handler). HKCU, no admin; idempotent.
    TokeerBridge::RegisterUriScheme(std::string(SteamInstallPath) + "\\OpenSteamTool.dll");

    // Optional self-update check. Runs on its own detached thread so the network
    // round-trip never delays hook installation; a staged DLL applies next launch.
    //
    // Compiled out entirely by -DOST_ENABLE_UPDATER=OFF. That is deliberately a
    // build-time cut rather than a runtime one: a pinned or private build should
    // not be replaceable by flipping [update] in opensteamtool.toml, and with the
    // updater absent the DLL makes no update request at all.
#ifdef OST_ENABLE_UPDATER
    if (Config::GetUpdateEnabled()) {
        OSTPlatform::Thread::StartDetached([] () -> uint32_t {
            const std::string self = std::string(SteamInstallPath) + "\\OpenSteamTool.dll";
            AppUpdater::CleanupStagedBackup(self);

            const AppUpdater::CheckResult upd = AppUpdater::Check();
            if (!upd.updateAvailable) return 0;
            if (!AppUpdater::DownloadAndStage(upd, self)) return 0;

            const bool restart = OSTPlatform::Dialog::ShowConfirm(
                "BetterSteamTools Updated!",
                upd.oldVersion + " -> " + upd.newVersion +
                "\n\nRestart Steam now to apply?");
            if (restart) AppUpdater::RestartSteam();
            return 0;
        });
    }
#else
    LOG_INFO("Self-updater not compiled in (OST_ENABLE_UPDATER=OFF)");
#endif

    LOG_INFO("OpenSteamTool init complete");
    return 0;
}

// True only when the host process is steam.exe. The proxy DLLs already gate injection to
// Steam, but rundll32 loads this DLL directly to service a bst:// link — there we must NOT
// run the Steam-injection machinery (steamclient load, hooks, watchers); the TokeerUri
// export does its work standalone.
static bool IsSteamHost()
{
    char exePath[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) return false;
    const char* name = strrchr(exePath, '\\');
    name = name ? name + 1 : exePath;
    return _stricmp(name, "steam.exe") == 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        if (!IsSteamHost())
            return TRUE;   // e.g. rundll32 bst:// handler — no injection here
        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH && IsSteamHost())
    {
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
