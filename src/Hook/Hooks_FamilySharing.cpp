#include "Hooks_FamilySharing.h"

#include "Steam/Enums.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <mutex>
#include <unordered_set>

namespace {

    // Shared apps seen through CheckAppOwnership, read by FillInAppOverview.
    std::mutex g_sharedAppsMutex;
    std::unordered_set<AppId_t> g_sharedApps;

    // Steam sets bFamilyShared on every app whose license came from another
    // member of the Steam Family; bBorrowed covers the borrow/return flow of
    // the same family (a temporary loan of an app the lender still holds).
    // Either source means "this account does not own the app locally".
    bool HasSharedSource(const AppOwnership* pOwn)
    {
        return pOwn && (pOwn->bFamilyShared || pOwn->bBorrowed);
    }

    bool IsBypassEnabled()
    {
        // On by default; only an explicit [family_sharing] bypass = false in
        // opensteamtool.toml turns it off.
        return LuaConfig::GetFamilySharingBypass();
    }

}

namespace Hooks_FamilySharing {

    bool IsSharedLicense(const AppOwnership* pOwn)
    {
        return HasSharedSource(pOwn);
    }

    bool NormalizeSharedOwnership(AppId_t appId, AppOwnership* pOwn)
    {
        if (!HasSharedSource(pOwn))
            return false;
        if (!IsBypassEnabled())
            return false;

        // Only the "nobody owns it, it is ours" case is rewritten. When the
        // result already says the app is owned (exact title owned by this
        // account) the shared bit is informational and dropping it is still
        // correct, but the rest of the record is left as Steam produced it.
        pOwn->bFamilyShared   = false;
        pOwn->bBorrowed       = false;

        if (!pOwn->bOwnsLicense) {
            // Present as a locally held permanent license bought by this
            // account. Time-limited borrows (TimeExpire != 0) would otherwise
            // start ticking and lock the app mid-session, so they are made
            // permanent as well.
            pOwn->bOwnsLicense    = true;
            pOwn->bLicenseExpired = false;
            pOwn->bLicenseLocked  = false;
            pOwn->bLicensePending = false;
            pOwn->bIsPermanent    = true;
            pOwn->bLicensePermanent = true;
            pOwn->bRetailLicense  = true;
            pOwn->bFromFreeWeekend = false;
            pOwn->TrialSeconds   = 0;
            pOwn->TimeExpire      = 0;

            if (pOwn->ReleaseState == EAppReleaseState::UnknownApp ||
                pOwn->ReleaseState == EAppReleaseState::Unavailable)
                pOwn->ReleaseState = EAppReleaseState::Released;
        }

        // Also remember it here so the UI-side flag pass (FillInAppOverview,
        // Hooks_SteamUI) can mark the app overview: the Play button and the
        // "already in use" toast read that record, not this function's return.
        Hooks_FamilySharing::NoteSharedApp(appId);

        LOG_PACKAGE_INFO("FamilySharing: AppId={} shared license normalized (package={} lender={})",
                         appId, pOwn->PackageId, pOwn->SteamId32);
        return true;
    }

    void NoteSharedApp(AppId_t appId) {
        if (!appId) return;
        std::lock_guard<std::mutex> lock(g_sharedAppsMutex);
        g_sharedApps.insert(appId);
    }

    bool IsBypassedSharedApp(AppId_t appId) {
        if (!appId) return false;
        std::lock_guard<std::mutex> lock(g_sharedAppsMutex);
        return g_sharedApps.count(appId) != 0;
    }

} // namespace Hooks_FamilySharing
