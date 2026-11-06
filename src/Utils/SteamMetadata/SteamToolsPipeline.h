#pragma once

#include <string>

// ─────────────────────────────────────────────────────────────────
//  SteamTools-compatible import pipeline.
//
//  Reimplements what SteamTools.exe does with a user-supplied import folder,
//  mapped onto the pieces OpenSteamTool already owns:
//
//    <import>\manifest\*.manifest  -> <steam>\config\depotcache\<file>
//    <import>\lua\*.lua            -> <steam>\config\stplug-in\<file>
//    <import>\bin\*                -> <steam>\config\StatsExport\<file>
//    <import>\*.txt / *.url        -> parsed as an app-ID list (id,type,key)
//
//  SteamTools then rewrites config/stplug-in/Steamtools.lua from a SQLite
//  table. OST has no database here: the app/depot set is the Lua config
//  itself, so the generator mirrors the *live* set instead — one addappid()
//  line per registered ID — and pins the depots whose manifest ended up in
//  depotcache.
//
//  Fidelity notes (observed in SteamTools.exe, sha256 992d547b…3eaa):
//    * the destination name is the source file name unless it already matches
//      the expected shape (see NormalizedFileName),
//    * an existing destination is removed first and the copy is counted as
//      "updated" instead of "added",
//    * `.lua` files containing an `addsubid(<digits>)` line are rejected,
//    * the generated Lua starts with "-- Steamtools auto-generated!".
//
//  Everything here is best-effort: a missing folder or a failed copy logs and
//  is skipped, and nothing throws.
// ─────────────────────────────────────────────────────────────────
namespace SteamToolsPipeline {

    // Canonical output file inside config/stplug-in. Written atomically.
    inline constexpr const char* kGeneratedLuaName = "Steamtools.lua";
    inline constexpr const char* kGeneratedLuaHeader = "-- Steamtools auto-generated!\n\n";

    // Default import folder when [steamtools] import = false? No: enabled with
    // no explicit path resolves here, so dropping files means one obvious place.
    //   <steam>\opensteamtool\import
    std::string DefaultImportDir();

    // Copy every supported file from `importDir` into the Steam tree and pin the
    // depots of the imported manifests. Returns the number of files copied.
    // Safe to call from a worker thread, never from DllMain.
    uint32_t Import(const std::string& importDir);

    // Rewrite <steam>\config\stplug-in\Steamtools.lua from the live Lua config
    // (every addappid-declared depot + a setmanifestid line per pinned manifest).
    // Returns true when the file content changed on disk.
    bool GenerateLua();

    // <steam>\config\stplug-in\Steamtools.lua
    std::string GeneratedLuaPath();

    // True when `name` is already `<digits>_<digits>.manifest`. Steam only looks
    // at the depot/gid inside the file, but keeping the conventional name makes
    // duplicate manifests collapse instead of piling up.
    bool IsConventionalManifestName(const std::string& name);

    // Steam rejects a .lua carrying addsubid(); detection is line based, with
    // leading whitespace tolerated, and "#"/"--" lines ignored — same rule as
    // SteamTools' filter.
    bool ContainsAddSubId(const std::string& text);

} // namespace SteamToolsPipeline
