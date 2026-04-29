#pragma once

#include <windows.h>

namespace DustGUI
{
    // DirectInput8 hooks: block keyboard/mouse from reaching the game when the
    // overlay is open, and emit compensating key-up/-down events on transitions
    // so nothing stays stuck.
    bool InstallDInputHooks();

    // Tracks WM_KEY{DOWN,UP} the game has actually seen, so OnOverlayClose() can
    // send compensating WM_KEYUPs and the WndProc can avoid eating an unmatched
    // WM_KEYUP. Updated by DustWndProc, read by HookedKbGetDeviceData.
    extern BYTE gGameKeyHeld[256];

    // Internal DInput state, exposed only so DustGUI::Shutdown() can reset it.
    extern BYTE gKbTrackedState[256];
    extern bool gKbWasBlocking;
}
