#include "Hooks_FamilySharing_Library.h"

#include "Steam/Enums.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "steam_messages.pb.h"

#include <atomic>
#include <cstring>
#include <string>

namespace {

    // Account part of a SteamID64. Steam IDs are "<universe><type><instance>"
    // followed by a 32-bit account id, so the members of one Family share the
    // high half and differ in the low one.
    uint64 AccountFromSteamId(uint64 steamId)
    {
        return steamId & 0xFFFFFFFFull;
    }

    std::atomic<uint64> g_localSteamId{0};

    // The 9405 message carries account ids, not SteamID64s. Remember the
    // high half seen alongside the local account so injected ids stay 64-bit
    // consistent if a client ever expects that; the account id is what is
    // actually written into locked_by/owner_id.
    bool SerializeInto(const google::protobuf::MessageLite& msg,
                       uint8* outBody, uint32 outCapacity, uint32* outSize)
    {
        if (!outBody || !outSize) return false;
        const size_t size = msg.ByteSizeLong();
        if (size > outCapacity) return false;
        if (!msg.SerializeToArray(outBody, static_cast<int>(outCapacity)))
            return false;
        *outSize = static_cast<uint32>(size);
        return true;
    }

}

namespace Hooks_FamilySharing_Library {

    bool IsPatchEnabled()
    {
        return LuaConfig::GetFamilySharingBypass();
    }

    void SetLocalSteamId(uint64 steamId)
    {
        const uint64 previous = g_localSteamId.exchange(steamId);
        if (previous != steamId)
            LOG_PACKAGE_INFO("FamilySharing: local SteamID captured as {}", steamId);
    }

    uint64 GetLocalSteamId()
    {
        return g_localSteamId.load();
    }

    bool IsRunningAppsJob(const char* jobName)
    {
        return jobName && std::strcmp(jobName, "FamilyGroupsClient.NotifyRunningApps#1") == 0;
    }

    // The service method is registered on the "FamilyGroups" service while the
    // notifications come from "FamilyGroupsClient", and which prefix the
    // client puts in target_job_name has not been observed yet: netpacket.log
    // shows FamilyGroups.GetFamilyGroup#1 for the service and
    // FamilyGroupsClient.NotifyRunningApps#1 for the notification. Accept both
    // spellings so the lend list is patched whichever one Steam sends.
    bool IsSharedLibraryAppsJob(const char* jobName)
    {
        if (!jobName) return false;
        return std::strcmp(jobName, "FamilyGroups.GetSharedLibraryApps#1") == 0 ||
               std::strcmp(jobName, "FamilyGroupsClient.GetSharedLibraryApps#1") == 0;
    }

    // ── FamilyGroupsClient.GetSharedLibraryApps#1 ───────────────────────
    //
    // Reply (eMsg 147) to the client's request for the Family lend list. Each
    // SharedApp carries exclude_reason; anything but Included drops the app out
    // of the playable set and the Play button. With the bypass on, excluded
    // apps are published as Included so a copy the lender is currently playing
    // does not disable the button. Field numbers come from the descriptor in
    // steamclient64.dll (see steam_messages.proto).
    bool PatchSharedLibraryApps(const uint8* pBody, uint32 cbBody,
                                uint8* outBody, uint32 outCapacity,
                                uint32* outSize)
    {
        if (!IsPatchEnabled()) return false;

        CFamilyGroups_GetSharedLibraryApps_Response msg;
        if (!msg.ParseFromArray(pBody, cbBody)) {
            LOG_PACKAGE_WARN("FamilySharing: failed to parse GetSharedLibraryApps response ({} bytes)", cbBody);
            return false;
        }

        bool changed = false;
        for (int i = 0; i < msg.apps_size(); ++i) {
            auto* app = msg.mutable_apps(i);
            const auto reason = app->has_exclude_reason()
                ? app->exclude_reason() : k_ESharedLibrary_Included;
            if (reason == k_ESharedLibrary_Included) continue;

            LOG_PACKAGE_INFO("FamilySharing: shared app {} exclude_reason={} -> included",
                             app->appid(), static_cast<int>(reason));
            app->set_exclude_reason(k_ESharedLibrary_Included);
            changed = true;
        }

        if (!changed) return false;
        return SerializeInto(msg, outBody, outCapacity, outSize);
    }

    // ── 9405: nobody else holds a shared-library lock ──────────────────
    //
    // The message points at play-session state: LockedLibrary is
    // [owner account id, locker account id] for a Family library that is
    // currently held, and my_library_locked_by is the account holding *our*
    // library. When another member starts a shared game, Steam's launch path
    // asks whether the app can be played; a locker other than this account makes
    // the answer "no", which is what leaves the button as "Play" but dead.
    //
    // While the bypass is on, every library is published as unlocked — including
    // our own, because a game launched from a shared license runs as if the app
    // belonged to us. Field numbers for both entries are confirmed by the BST
    // proto.
    bool PatchSharedLibraryLockStatus(const uint8* pBody, uint32 cbBody,
                                      uint8* outBody, uint32 outCapacity,
                                      uint32* outSize)
    {
        if (!IsPatchEnabled()) return false;

        CMsgClientSharedLibraryLockStatus msg;
        if (!msg.ParseFromArray(pBody, cbBody)) {
            LOG_PACKAGE_WARN("FamilySharing: failed to parse CMsgClientSharedLibraryLockStatus");
            return false;
        }

        const uint64 localSteamId = GetLocalSteamId();
        const uint32 localAccount  = localSteamId ? static_cast<uint32>(AccountFromSteamId(localSteamId)) : 0xFFFFFFFFu;

        bool changed = false;
        for (int i = 0; i < msg.locked_library_size(); ++i) {
            auto* lib = msg.mutable_locked_library(i);
            const uint32 lockedBy = lib->has_locked_by() ? lib->locked_by() : 0;
            if (!lockedBy) continue;

            LOG_PACKAGE_INFO("FamilySharing: 9405 owner={} locked_by={} -> released", lib->owner_id(), lockedBy);
            lib->set_locked_by(0);
            if (localSteamId && lib->has_owner_id())
                lib->set_owner_id(static_cast<uint32>(AccountFromSteamId(localSteamId)));
            changed = true;
        }

        const uint32 myLocker = msg.has_my_library_locked_by() ? msg.my_library_locked_by() : 0;
        if (myLocker) {
            LOG_PACKAGE_INFO("FamilySharing: 9405 my_library_locked_by={} -> released", myLocker);
            msg.set_my_library_locked_by(0);
            changed = true;
        }

        if (!changed) return false;
        return SerializeInto(msg, outBody, outCapacity, outSize);
    }

    // ── FamilyGroupsClient.NotifyRunningApps#1 ──────────────────────────
    //
    // Incoming (eMsg 147, Response) — the Family broadcast a play session for
    // one or more apps. This is what makes the client believe a copy is "in
    // use" by another member, which is one half of the disabled-Play state.
    //
    // While the bypass is on, every entry that names a lender other than this
    // account is neutralized: is_locked is cleared, is_running is cleared and
    // the playing-member list is emptied, so no member — including this one —
    // is recorded as playing the app.
    //
    // No Lua/depot filter here: the whole point is that an app lent by Family
    // (which by definition is not in the Lua list) stops being arbitrated. The
    // earlier version gated on LuaConfig::HasDepot and therefore never touched
    // a real Family-shared app, which is why Play stayed disabled.
    bool PatchRunningAppsNotification(const uint8* pBody, uint32 cbBody,
                                      uint8* outBody, uint32 outCapacity,
                                      uint32* outSize)
    {
        if (!IsPatchEnabled()) return false;

        CFamilyGroupsClient_NotifyRunningApps_Notification msg;
        if (!msg.ParseFromArray(pBody, cbBody)) {
            LOG_PACKAGE_WARN("FamilySharing: failed to parse NotifyRunningApps notification "
                             "({} bytes) - layout not confirmed for this client", cbBody);
            return false;
        }

        const uint64 localSteamId = GetLocalSteamId();
        bool changed = false;

        for (int i = 0; i < msg.running_apps_size(); ++i) {
            auto* app = msg.mutable_running_apps(i);
            const AppId_t appId = app->appid();
            if (!appId) continue;

            // Only entries that are actually in use are rewritten; an app that
            // nobody is playing is already in the state we want.
            bool busy = app->is_locked()
                     || app->playing_members_size() > 0;

            // Everybody else's session on a copy we share is exactly what the
            // bypass must hide. A session already attributed to this account is
            // left alone so our own play state keeps working.
            for (int m = 0; m < app->playing_members_size(); ++m) {
                const uint64 member = app->playing_members(m).member_steamid();
                if (!localSteamId || member != localSteamId) { busy = true; break; }
            }
            if (!busy) continue;

            LOG_PACKAGE_INFO("FamilySharing: running-app appid={} locked={} members={} -> released",
                             appId, app->is_locked(), app->playing_members_size());
            app->set_is_locked(false);
            app->clear_playing_members();
            changed = true;
        }

        if (!changed) return false;
        return SerializeInto(msg, outBody, outCapacity, outSize);
    }

    // Outgoing (eMsg 151, Request) — Steam wants to register a play session for
    // a Library-shared app. While the bypass is on it is answered locally: the
    // Family backend must not arbitrate it, and suppressing the request avoids
    // the "Someone else started playing" stop.
    //
    // Not holding the request hostage: the body was never sent, so no reply is
    // coming, and returning true here only makes Steam treat the frame as sent.
    bool PatchRunningAppsRequest(const uint8* pBody, uint32 cbBody)
    {
        if (!IsPatchEnabled()) return false;

        CFamilyGroups_NotifyRunningAppsRequest msg;
        if (!msg.ParseFromArray(pBody, cbBody)) {
            LOG_PACKAGE_WARN("FamilySharing: failed to parse NotifyRunningApps request ({} bytes)", cbBody);
            return false;
        }
        std::string apps;
        for (int i = 0; i < msg.running_apps_size(); ++i) {
            if (i) apps += ", ";
            apps += std::to_string(msg.running_apps(i).appid());
        }
        LOG_PACKAGE_INFO("FamilySharing: NotifyRunningApps request suppressed locally (appids=[{}], group={})",
                         apps, msg.has_family_groupid() ? msg.family_groupid() : 0);
        return true;
    }

    // eMsg 9406. A stop request carries a countdown and the apps to stop; the
    // bypass answers it with "nothing to stop" instead of rewriting apps one by
    // one, which also stops the UI toast (it is fed by the same message).
    bool NeutralizeStopPlaying(const uint8* pBody, uint32 cbBody,
                               uint8* outBody, uint32 outCapacity,
                               uint32* outSize)
    {
        if (!IsPatchEnabled()) return false;

        CMsgClientSharedLibraryStopPlaying msg;
        if (!msg.ParseFromArray(pBody, cbBody)) {
            LOG_PACKAGE_WARN("FamilySharing: failed to parse CMsgClientSharedLibraryStopPlaying (9406, {} bytes)", cbBody);
            return false;
        }

        LOG_PACKAGE_INFO("FamilySharing: 9406 stop request ({} app(s), {}s left) -> neutralized",
                         msg.stop_apps_size(), msg.has_seconds_left() ? msg.seconds_left() : 0);
        msg.clear_stop_apps();
        msg.set_seconds_left(0);
        return SerializeInto(msg, outBody, outCapacity, outSize);
    }

    // Steam also asks for the lock status at start-up (the same 9405 message,
    // sent client -> server with the header fields unset). The reply is then
    // whichever libraries are locked. Nothing in that request needs rewriting
    // for the bypass, so it is passed through untouched; the hook keeps this
    // exported for symmetry with the other entry points.
    bool StripLockStatusRequest(const uint8* pBody, uint32 cbBody,
                                uint8* outBody, uint32 outCapacity,
                                uint32* outSize)
    {
        (void)pBody; (void)cbBody;
        return SerializeInto(CMsgClientSharedLibraryLockStatus{}, outBody, outCapacity, outSize);
    }

} // namespace Hooks_FamilySharing_Library
