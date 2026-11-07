#include "Hooks_Hotkey.h"

#include "OSTPlatform/include/Process.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

namespace {

    // Ctrl+Shift+? — '?' is Shift+/ on a US layout, so the physical key is
    // VK_OEM_2 (the /? key) with Ctrl and Shift held.
    constexpr int kKeyQuestionMark = VK_OEM_2;
    constexpr int kKeyControl = VK_CONTROL;
    constexpr int kKeyShift = VK_SHIFT;

    // The dialog is modal on its own thread; while it is up another press must
    // not stack a second one.
    std::atomic<bool> g_dialogOpen{false};
    std::atomic<bool> g_stopping{false};

    // The Steam client UI is not drawn by this process: this DLL lives in
    // steam.exe, but the window the user clicks belongs to steamwebhelper.exe
    // (client UI) or gameoverlayui.exe (overlay). So "Steam is in front" means
    // the foreground window belongs to one of those, not a PID match with us.
    bool IsSteamUiImageName(std::string_view imagePath)
    {
        const size_t slash = imagePath.find_last_of("\\/");
        const std::string_view name = slash == std::string_view::npos
            ? imagePath
            : imagePath.substr(slash + 1);

        static constexpr std::string_view kSteamUiNames[] = {
            "steamwebhelper.exe",
            "gameoverlayui.exe",
            "gameoverlayui64.exe",
        };
        return std::any_of(std::begin(kSteamUiNames), std::end(kSteamUiNames),
                           [name](std::string_view candidate) {
                               return _stricmp(std::string(name).c_str(),
                                               std::string(candidate).c_str()) == 0;
                           });
    }

    bool WindowBelongsToSteamUi(HWND window)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) return false;

        const auto image = OSTPlatform::Process::GetImagePath(pid);
        return image && IsSteamUiImageName(*image);
    }

    // On a slow machine or while the client is busy, GetForegroundWindow can
    // point at a window whose process no longer answers; in that case ask the
    // desktop itself instead of giving up, because being asked "why doesn't it
    // fire while I have the Steam window in front" is worse than one extra
    // EnumWindows per key press.
    bool SteamUiInForeground()
    {
        const HWND foreground = GetForegroundWindow();
        if (!foreground) return false;
        if (WindowBelongsToSteamUi(foreground)) return true;

        struct Search {
            bool found = false;
        } search;

        EnumWindows([](HWND window, LPARAM param) -> BOOL {
            auto& state = *reinterpret_cast<Search*>(param);
            if (!IsWindowVisible(window)) return TRUE;
            if (WindowBelongsToSteamUi(window))
            {
                state.found = true;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));

        return search.found;
    }

    // Polled every 50 ms. GetAsyncKeyState's low bit is "pressed since the
    // last call", so the edge is detected per key; requiring the high bit as
    // well keeps a stale low bit (set while another window had focus) from
    // opening the dialog as soon as a Steam window comes back to front.
    uint32_t WatchLoop()
    {
        bool armed = false;
        while (!g_stopping.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            const bool ctrl = (GetAsyncKeyState(kKeyControl) & 0x8000) != 0;
            const bool shift = (GetAsyncKeyState(kKeyShift) & 0x8000) != 0;
            const SHORT question = GetAsyncKeyState(kKeyQuestionMark);
            const bool down = (question & 0x8000) != 0;
            const bool pressedSinceLast = (question & 0x0001) != 0;

            if (!ctrl || !shift)
            {
                // Forget the key while modifiers are not held, so that holding
                // Ctrl+Shift and then tapping /? is one deliberate press.
                armed = false;
                continue;
            }

            if (!armed && down && pressedSinceLast && SteamUiInForeground())
            {
                armed = true;
                if (!g_dialogOpen.exchange(true))
                {
                    LOG_INFO("Hooks_Hotkey: version dialog requested");
                    std::thread([]() {
                        SteamDiagnostics::ShowVersionInfo(
                            "BetterSteamTools (Custom crstlnz) - version info");
                        g_dialogOpen.store(false);
                    }).detach();
                }
                continue;
            }

            if (!down)
                armed = false;
        }
        return 0;
    }

} // namespace

namespace Hooks_Hotkey {

    void Install()
    {
        LOG_INFO("Hooks_Hotkey: shortcut watcher starting (Ctrl+Shift+?)");
        g_stopping.store(false);
        if (!OSTPlatform::Thread::StartDetached(&WatchLoop))
            LOG_WARN("Hooks_Hotkey: could not start the shortcut watcher");
    }

    void Uninstall()
    {
        // The poller is a detached thread with its own stop flag; it exits on
        // its next tick. No Steam state is touched, so nothing else to undo.
        g_stopping.store(true);
    }

} // namespace Hooks_Hotkey
