#pragma once

#include "Steam/Types.h"
#include "Steam/Structs.h"

// Steam Family Sharing normalization.
//
// Steam reports apps that were lent by another account in the Family through
// CheckAppOwnership by setting AppOwnership::bFamilyShared (and, for the
// borrowee, bBorrowed) while leaving bOwnsLicense false. Games therefore see
// "shared" ownership and Steam applies the sharing restrictions on top of it:
// the LicenseLocked/RegionRestricted/Trial gates, PlaytimeLimit on the lender's
// side, and the runtime checks for apps that were not part of the share.
//
// The ownership result is normalized before anyone (game, Steam UI, depot
// builder) reads it. On by default — the TOML needs no entry; only an explicit
// [family_sharing] bypass = false turns it off.
namespace Hooks_FamilySharing {

    // Rewrite a shared AppOwnership in place. Returns true when pOwn was a
    // shared license and has been normalized, false (and leaves it untouched)
    // in every other case, including when the feature is disabled.
    bool NormalizeSharedOwnership(AppId_t appId, AppOwnership* pOwn);

    // True if pOwn describes an app lent by another account of the Family.
    bool IsSharedLicense(const AppOwnership* pOwn);

    // Apps whose ownership Steam reported as a shared license and whose bypass
    // normalization is applied. The ownership result itself has no flags field;
    // the UI reads AppOverview's ownership flags, so FillInAppOverview asks this
    // list whether to raise k_EAppOwnershipFlags_FamilyBypass on the overview.
    void NoteSharedApp(AppId_t appId);
    bool IsBypassedSharedApp(AppId_t appId);

} // namespace Hooks_FamilySharing
