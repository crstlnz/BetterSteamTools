#pragma once

#include "dllmain.h"

// Hidden in-client shortcut: Ctrl+Shift+? opens a dialog with this build's
// version and the Steam binaries the hooks are bound to.
//
// The key state is polled on a background thread rather than installed as a
// registered hotkey or a detour: Steam runs its own accelerator handling, and
// RegisterHotKey from an injected DLL would take the combination away from it
// (and needs a message loop to receive WM_HOTKEY). Polling GetAsyncKeyState
// touches no Steam code at all, so it cannot destabilise the client.
//
// The DLL is loaded by steam.exe, but the Steam window belongs to
// steamwebhelper.exe (and the overlay to gameoverlayui.exe), so the pose check
// looks at the foreground window's process, not at our own PID. See
// Hooks_Hotkey.cpp.
namespace Hooks_Hotkey {
    void Install();
    void Uninstall();
}
