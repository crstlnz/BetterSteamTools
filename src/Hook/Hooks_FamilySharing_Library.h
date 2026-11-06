#pragma once

#include "Steam/Types.h"

// Family Sharing — shared-library arbitration.
//
// Whenever more than one member of a Steam Family wants the same app at the
// same time, Valve arbitrates "who may play" with two CM messages:
//
//   CMsgClientSharedLibraryLockStatus            (eMsg 9405)
//       Sent to every member of the Family when play/lock state changes. Each
//       library carries the SteamID currently holding the lock (all zeros =
//       free). A member whose library is locked by somebody else cannot launch
//       the shared app, and a running session is told to stop via 9406.
//
//   CFamilyGroupsClient_NotifyRunningApps_Notification   (eMsg 147, service
//   method "FamilyGroupsClient.NotifyRunningApps#1")
//       Sent when a Family member starts an app. The Family Groups backend
//       registers the play session; a session the backend did not accept is
//       killed ("Someone else started playing...").
//
// This module answers both directions locally while the bypass is on (the
// default; [family_sharing] bypass = false disables it): locks held by other
// members are published as free, and play notifications are made to look like
// this account's own app owned by this account. Every reply whose layout this
// module cannot reproduce exactly falls back to Steam's own handling.
namespace Hooks_FamilySharing_Library {

    // eMsg 9405 — incoming. Returns true if the body was rewritten (and
    // SerializeToArray'd into outBody/outSize).
    bool PatchSharedLibraryLockStatus(const uint8* pBody, uint32 cbBody,
                                      uint8* outBody, uint32 outCapacity,
                                      uint32* outSize);

    // eMsg 147 service method. Incoming notification: returns true if the body
    // was rewritten. Outgoing request: returns true if it should be suppressed
    // (answered locally instead of being sent to Valve).
    bool PatchRunningAppsNotification(const uint8* pBody, uint32 cbBody,
                                      uint8* outBody, uint32 outCapacity,
                                      uint32* outSize);

    bool PatchRunningAppsRequest(const uint8* pBody, uint32 cbBody);

    // eMsg 9406 CMsgClientSharedLibraryStopPlaying — incoming. While the bypass
    // is on, a stop request is published with no apps and zero seconds so the UI
    // never starts its "this shared game is already in use" countdown. Returns
    // false (and leaves the message alone) when the bypass is off or the body
    // does not parse.
    bool NeutralizeStopPlaying(const uint8* pBody, uint32 cbBody,
                               uint8* outBody, uint32 outCapacity,
                               uint32* outSize);

    // True if the service method name belongs to this module.
    bool IsRunningAppsJob(const char* jobName);

    // "FamilyGroupsClient.GetSharedLibraryApps#1" — the Family lend list.
    bool IsSharedLibraryAppsJob(const char* jobName);

    // eMsg 147 reply to GetSharedLibraryApps. While the bypass is on, every
    // SharedApp whose exclude_reason is not Included is republished as Included,
    // so an app whose lender is currently playing no longer disables Play.
    bool PatchSharedLibraryApps(const uint8* pBody, uint32 cbBody,
                                uint8* outBody, uint32 outCapacity,
                                uint32* outSize);

    // Build a CMsgClientSharedLibraryLockStatus body with the 9405 header
    // unset, for the request Steam makes while starting up.
    bool StripLockStatusRequest(const uint8* pBody, uint32 cbBody,
                                uint8* outBody, uint32 outCapacity,
                                uint32* outSize);

    // Local SteamID, captured from CheckAppOwnership (SteamId32 is the
    // account id that requested the answer). 0 until the first call.
    void SetLocalSteamId(uint64 steamId);
    uint64 GetLocalSteamId();

    bool IsPatchEnabled();

} // namespace Hooks_FamilySharing_Library
