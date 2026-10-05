// NOT BUILT since 2026-09-10. The controllers now drive the keyboard and
// mouse from the d3d9 side (src/vr/vr_input.cpp): the game polls
// XInputGetState once at startup and never again, and a stick caps how
// fast aim can turn. This file still calls the old vr_input API and will
// not compile against the new one without work. It is kept for the day
// analog movement is wanted, since a gamepad is the only route to that.
//
// xinput9_1_0.dll -- the game's gamepad, driven by the VR controllers.
//
// Tomb Raider: Legend reads its gamepad through XInput, not DirectInput as the
// early survey assumed: the executable carries `xinput9_1_0.dll`,
// `XInputGetState` and `XInputSetState` and nothing from dinput at all. XInput
// is not a KnownDLL, and the executable's own directory is searched first, so a
// file of that name beside the game wins -- the same trick d3d9.dll already
// uses, and for the same reason.
//
// That makes motion controllers straightforward. The game already has a full
// gamepad control scheme; it does not need to know anything has changed.
//
// Real pads keep working. Whatever the system DLL reports is read first and the
// controllers are merged into it, so a pad plugged in alongside is simply
// another input. If the VR runtime is absent or has no controllers, this
// forwards and nothing is lost.
//
// Legacy OpenVR input (GetControllerState) rather than the Input API: no action
// manifest to install, no bindings UI to get wrong, and SteamVR already maps
// every headset's controllers onto the legacy buttons. For a debugging aid that
// wants to work the moment it is dropped in, that is the right trade.

#include <windows.h>
#include <xinput.h>

#include "../vr/vr_input.h"

namespace
{
    typedef DWORD(WINAPI* PFN_GetState)(DWORD, XINPUT_STATE*);
    typedef DWORD(WINAPI* PFN_SetState)(DWORD, XINPUT_VIBRATION*);
    typedef DWORD(WINAPI* PFN_GetCaps)(DWORD, DWORD, XINPUT_CAPABILITIES*);

    HMODULE      g_real = nullptr;
    PFN_GetState g_get_state = nullptr;
    PFN_SetState g_set_state = nullptr;
    PFN_GetCaps  g_get_caps = nullptr;

    void load_real()
    {
        if (g_real)
            return;

        // By full path. Loading "xinput9_1_0.dll" by name from inside a file of
        // that name would find this one again.
        wchar_t path[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return;
        wcscat_s(path, MAX_PATH, L"\\xinput9_1_0.dll");

        trlvr::input_log("xinput9_1_0.dll proxy is loaded -- the game found it");

        g_real = LoadLibraryW(path);
        if (!g_real)
        {
            trlvr::input_log("no system xinput9_1_0.dll at %S -- real pads will "
                             "not work, VR controllers still will", path);
            return;
        }

        g_get_state = (PFN_GetState)GetProcAddress(g_real, "XInputGetState");
        g_set_state = (PFN_SetState)GetProcAddress(g_real, "XInputSetState");
        g_get_caps  = (PFN_GetCaps)GetProcAddress(g_real, "XInputGetCapabilities");
    }
}

extern "C"
{
    DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE* state) noexcept
    {
        load_real();
        if (!state)
            return ERROR_BAD_ARGUMENTS;

        DWORD result = ERROR_DEVICE_NOT_CONNECTED;
        if (g_get_state)
            result = g_get_state(index, state);
        if (result != ERROR_SUCCESS)
        {
            ZeroMemory(state, sizeof(*state));
            result = ERROR_DEVICE_NOT_CONNECTED;
        }

        // Only the first pad. A second controller is a real second player as
        // far as the game is concerned, and there is one player wearing the
        // headset.
        if (index != 0)
            return result;

        if (trlvr::vr_input_merge(&state->Gamepad))
        {
            // The packet number has to move or the game is entitled to skip
            // reading the state at all.
            state->dwPacketNumber = trlvr::vr_input_packet();
            result = ERROR_SUCCESS;
        }
        return result;
    }

    DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION* vibration) noexcept
    {
        load_real();
        if (vibration)
            trlvr::vr_input_rumble(vibration->wLeftMotorSpeed,
                                   vibration->wRightMotorSpeed);
        if (g_set_state)
            return g_set_state(index, vibration);
        return ERROR_SUCCESS;
    }

    DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags,
                                       XINPUT_CAPABILITIES* caps) noexcept
    {
        load_real();
        DWORD result = ERROR_DEVICE_NOT_CONNECTED;
        if (g_get_caps)
            result = g_get_caps(index, flags, caps);

        // A game that asks whether a pad exists before reading it has to be
        // told yes, or the controllers never get a chance to say anything.
        if (result != ERROR_SUCCESS && index == 0 && caps &&
            trlvr::vr_input_available())
        {
            ZeroMemory(caps, sizeof(*caps));
            caps->Type = XINPUT_DEVTYPE_GAMEPAD;
            caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
            caps->Flags = 0;
            caps->Gamepad.wButtons = 0xFFFF;
            caps->Gamepad.bLeftTrigger = 0xFF;
            caps->Gamepad.bRightTrigger = 0xFF;
            caps->Gamepad.sThumbLX = (SHORT)0x7FFF;
            caps->Gamepad.sThumbLY = (SHORT)0x7FFF;
            caps->Gamepad.sThumbRX = (SHORT)0x7FFF;
            caps->Gamepad.sThumbRY = (SHORT)0x7FFF;
            caps->Vibration.wLeftMotorSpeed = 0xFFFF;
            caps->Vibration.wRightMotorSpeed = 0xFFFF;
            result = ERROR_SUCCESS;
        }
        return result;
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        load_real();
    return TRUE;
}
