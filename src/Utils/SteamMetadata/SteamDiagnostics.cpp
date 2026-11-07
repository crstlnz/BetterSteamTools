#include "SteamDiagnostics.h"
#include "OpenSteamToolBuildInfo.h"
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Hash.h"
#include "Utils/Logging/Log.h"

#include <cstdint>
#include <thread>
#include <utility>

#include <windows.h>

namespace SteamDiagnostics {

namespace {

    struct Snapshot {
        std::string openSteamToolVersion = OPENSTEAMTOOL_VERSION;
        std::string openSteamToolBuildDate = OPENSTEAMTOOL_BUILD_DATE;
        std::string openSteamToolPath;
        std::string buildID = "(unavailable)";
        std::string steamclientPath;
        std::string steamclientSha256 = "(unavailable)";
        std::string steamUIPath;
        std::string steamUISha256 = "(unavailable)";
    };

    Snapshot g_snapshot;

    static std::string DetectSteamBuildID()
    {
        using GetBootstrapperVersion_t = int64_t (*)();

        const auto steam = OSTPlatform::DynamicLibrary::GetLoaded("steam.exe");
        if (!steam) {
            LOG_WARN("SteamDiagnostics: steam.exe module not loaded; build id unavailable");
            return "(unavailable)";
        }

        const auto getBootstrapperVersion =
            reinterpret_cast<GetBootstrapperVersion_t>(
                OSTPlatform::DynamicLibrary::GetSymbol(steam, "GetBootstrapperVersion"));
        if (!getBootstrapperVersion) {
            LOG_WARN("SteamDiagnostics: steam.exe!GetBootstrapperVersion not exported");
            return "(unavailable)";
        }

        return std::to_string(getBootstrapperVersion());
    }

    static std::string HashOrUnavailable(const std::string& path)
    {
        std::string sha256 = OSTPlatform::Hash::Sha256OfFile(path);
        return sha256.empty() ? "(unavailable)" : std::move(sha256);
    }

    static std::string AppendSnapshot(std::string message)
    {
        message +=
            "\n\nSteam diagnostics:\n"
            "  OpenSteamTool version: " + g_snapshot.openSteamToolVersion + "\n"
            "  Build ID:              " + g_snapshot.buildID + "\n"
            "  steamclient64.dll SHA: " + g_snapshot.steamclientSha256 + "\n"
            "  steamui.dll SHA:       " + g_snapshot.steamUISha256;
        return message;
    }

    std::string BuildVersionReport()
    {
        const std::string& dll = g_snapshot.openSteamToolPath;
        const std::string& client = g_snapshot.steamclientPath;
        const std::string& ui = g_snapshot.steamUIPath;
        std::string report;
        report += "OpenSteamTool Crstlnz " + g_snapshot.openSteamToolVersion +
                  "  (build " + g_snapshot.openSteamToolBuildDate + ")\n";
        report += "DLL: " + (dll.empty() ? "(unknown)" : dll) + "\n";
        report += "Steam Build ID: " + g_snapshot.buildID + "\n";
        report += "steamclient64 SHA-256: " + g_snapshot.steamclientSha256 + "\n";
        report += "steamui SHA-256: " + g_snapshot.steamUISha256 + "\n";
        report += "\nCtrl+Shift+? opens this dialog.";
        return report;
    }

} // namespace

void Initialize(const std::string& steamclientPath,
                const std::string& steamUIPath)
{
    g_snapshot.buildID = DetectSteamBuildID();
    {
        // Our own module, for the DLL path shown in the version dialog.
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&Initialize), &self) && self) {
            char path[MAX_PATH] = {};
            if (GetModuleFileNameA(self, path, MAX_PATH))
                g_snapshot.openSteamToolPath = path;
        }
    }
    g_snapshot.steamclientPath = steamclientPath;
    g_snapshot.steamclientSha256 = HashOrUnavailable(steamclientPath);
    g_snapshot.steamUIPath = steamUIPath;
    g_snapshot.steamUISha256 = HashOrUnavailable(steamUIPath);

    LOG_INFO("SteamDiagnostics: ost.version={} build={} steamclient64.sha256={} steamui.sha256={}",
             g_snapshot.openSteamToolVersion,
             g_snapshot.buildID,
             g_snapshot.steamclientSha256,
             g_snapshot.steamUISha256);
}

std::string Sha256Of(const std::string& path)
{
    if (path == g_snapshot.steamclientPath)
        return g_snapshot.steamclientSha256 == "(unavailable)"
            ? std::string{}
            : g_snapshot.steamclientSha256;

    if (path == g_snapshot.steamUIPath)
        return g_snapshot.steamUISha256 == "(unavailable)"
            ? std::string{}
            : g_snapshot.steamUISha256;

    return OSTPlatform::Hash::Sha256OfFile(path);
}

void ShowWarning(std::string title, std::string message)
{
    std::thread([title = std::move(title),
                 message = AppendSnapshot(std::move(message))]() {
        OSTPlatform::Dialog::ShowWarning(title, message);
    }).detach();
}

std::string VersionReport()
{
    return BuildVersionReport();
}

void ShowVersionInfo(std::string title)
{
    std::thread([title = std::move(title)]() {
        OSTPlatform::Dialog::ShowInfo(title, BuildVersionReport());
    }).detach();
}

} // namespace SteamDiagnostics
