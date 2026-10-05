// d3d9.dll proxy for Tomb Raider: Legend.
//
// The game calls LoadLibrary("d3d9.dll"), which searches its own directory
// first, so this file sitting next to trl.exe is the whole injection story --
// no launcher, no injector, nothing patched on disk.

#include "../vr/hook.h"
#include "backend.h"
#include "proxy_d3d9.h"

#include "../common/config.h"
#include "../common/log.h"

#include <d3d9.h>
#include <windows.h>

using namespace trlvr;

namespace
{
    template <typename T>
    T backend_fn(const char* name)
    {
        return reinterpret_cast<T>(backend_proc(name));
    }
}

extern "C"
{
    IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
    {
        typedef IDirect3D9*(WINAPI * fn)(UINT);
        fn real = backend_fn<fn>("Direct3DCreate9");
        if (!real)
        {
            log("Direct3DCreate9: backend has no Direct3DCreate9");
            return nullptr;
        }

        IDirect3D9* d3d9 = real(SDKVersion);
        if (!d3d9)
        {
            log("Direct3DCreate9: backend returned null (SDKVersion=%u)", SDKVersion);
            return nullptr;
        }

        log("Direct3DCreate9: SDKVersion=%u, wrapped", SDKVersion);
        return new ProxyD3D9(d3d9);
    }

    // The game itself never asks for this, but overlays do. It is deliberately
    // not wrapped: an Ex device would bypass the proxy, so anything that takes
    // this path is on its own.
    HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
    {
        typedef HRESULT(WINAPI * fn)(UINT, IDirect3D9Ex**);
        fn real = backend_fn<fn>("Direct3DCreate9Ex");
        if (!real)
            return E_NOTIMPL;
        log("Direct3DCreate9Ex: passed through unwrapped");
        return real(SDKVersion, ppD3D);
    }

    int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName)
    {
        typedef int(WINAPI * fn)(D3DCOLOR, LPCWSTR);
        fn real = backend_fn<fn>("D3DPERF_BeginEvent");
        return real ? real(col, wszName) : 0;
    }

    int WINAPI D3DPERF_EndEvent(void)
    {
        typedef int(WINAPI * fn)(void);
        fn real = backend_fn<fn>("D3DPERF_EndEvent");
        return real ? real() : 0;
    }

    void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName)
    {
        typedef void(WINAPI * fn)(D3DCOLOR, LPCWSTR);
        fn real = backend_fn<fn>("D3DPERF_SetMarker");
        if (real)
            real(col, wszName);
    }

    void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName)
    {
        typedef void(WINAPI * fn)(D3DCOLOR, LPCWSTR);
        fn real = backend_fn<fn>("D3DPERF_SetRegion");
        if (real)
            real(col, wszName);
    }

    BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
    {
        typedef BOOL(WINAPI * fn)(void);
        fn real = backend_fn<fn>("D3DPERF_QueryRepeatFrame");
        return real ? real() : FALSE;
    }

    void WINAPI D3DPERF_SetOptions(DWORD dwOptions)
    {
        typedef void(WINAPI * fn)(DWORD);
        fn real = backend_fn<fn>("D3DPERF_SetOptions");
        if (real)
            real(dwOptions);
    }

    DWORD WINAPI D3DPERF_GetStatus(void)
    {
        typedef DWORD(WINAPI * fn)(void);
        fn real = backend_fn<fn>("D3DPERF_GetStatus");
        return real ? real() : 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(module);
        load_config();
        if (config().logging)
        {
            log_open();
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            log("host: %S", exe);
            if (config_defaults_added() != 0)
            {
                log("config: added %u missing setting%s to trlvr.ini",
                    config_defaults_added(),
                    config_defaults_added() == 1 ? "" : "s");
            }
            log("config: backend=%S force_windowed=%d",
                config().backend, config().force_windowed);

            // Echo the settings that materially change runtime behaviour.
            log("config: [vr] stereo=%d same_frame=%d head_tracking=%d "
                "hmd_drives_camera=%d hmd_aim=%d",
                config().stereo, config().stereo_same_frame,
                config().head_tracking, config().hmd_drives_camera,
                config().hmd_aim);
            log("config: [vr] auto_center=%d shake=%d first_person=%d "
                "tracked_hands=%d "
                "first_person_smoothing=%.2f level_horizon=%d "
                "ui_passthrough=%d menu_world_locked=%d ui_scale=%.2f "
                "hud_follow=%.2f "
                "cull_fov=%.0f world_scale=%.4g",
                config().camera_auto_center, config().camera_shake,
                config().first_person,
                config().first_person_tracked_hands,
                config().first_person_smoothing,
                config().level_horizon, config().ui_passthrough,
                config().menu_world_locked, config().ui_scale,
                config().hud_follow,
                config().cull_fov_degrees,
                config().world_scale);
            log("config: [vr] head_position=%d head_position_scale=%.2f "
                "drives_position=%d",
                config().head_position, config().head_position_scale,
                config().hmd_drives_position);
            log("config: [controls] controllers=%d immersive_controls=%d "
                "look_speed=%.0f look_vertical=%d",
                config().controllers, config().immersive_controls,
                config().look_speed, config().look_vertical);
        }
        hook_apply_settings_overrides();
        break;
    }
    case DLL_PROCESS_DETACH:
        log("unloading");
        log_close();
        break;
    }
    return TRUE;
}
