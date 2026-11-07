#pragma once

#include <string>

namespace SteamDiagnostics {

    // Capture the Steam build id and DLL hashes used in support popups.
    void Initialize(const std::string& steamclientPath,
                    const std::string& steamUIPath);

    // Reuse captured hashes for known Steam DLLs and hash other files on demand.
    std::string Sha256Of(const std::string& path);

    // Display a warning asynchronously with the captured Steam diagnostics.
    void ShowWarning(std::string title, std::string message);

    // Version/build report for the in-client "version info" dialog: OpenSteamTool
    // version and build date, the DLL it is running from, the Steam build id and
    // the SHA-256 of the two Steam binaries the hooks target. Safe to call from
    // any thread; the text is built on the caller and does no file I/O.
    std::string VersionReport();

    // Modal info box with VersionReport(), raised on a detached thread so the
    // caller is never blocked. Used by the in-client Ctrl+Shift+? shortcut.
    void ShowVersionInfo(std::string title);

} // namespace SteamDiagnostics
