#include "proxy_d3d9.h"
#include "msaa_cap.h"
#include "proxy_device.h"
#include "window.h"
#include "../vr/vr_session.h"
#include "../vr/vr_submit.h"
#include "../vr/cull.h"
#include "../vr/stereo.h"
#include "../vr/camera_head.h"
#include "../vr/ui_space.h"
#include "../vr/water_stereo.h"
#include "../vr/input_labels.h"
#include "../vr/hook.h"

#include "../common/config.h"
#include "../common/log.h"

#include <cwchar>

namespace trlvr
{
    namespace
    {
        // The camera, culling and UI hooks below patch or read
        // fixed addresses in the retail executable.  The D3D proxy also has a
        // standalone load test, and may be loaded by unrelated programs while
        // troubleshooting; neither process owns those addresses.
        bool is_trl_process()
        {
            wchar_t path[MAX_PATH]{};
            const DWORD count = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (!count || count == MAX_PATH)
                return false;

            const wchar_t* leaf = wcsrchr(path, L'\\');
            leaf = leaf ? leaf + 1 : path;
            return _wcsicmp(leaf, L"trl.exe") == 0 ||
                   _wcsicmp(leaf, L"trl_vr.exe") == 0;
        }
    }

    namespace
    {
        const char* devtype_name(D3DDEVTYPE t)
        {
            switch (t)
            {
            case D3DDEVTYPE_HAL:      return "HAL";
            case D3DDEVTYPE_REF:      return "REF";
            case D3DDEVTYPE_SW:       return "SW";
            case D3DDEVTYPE_NULLREF:  return "NULLREF";
            default:                  return "?";
            }
        }
    }

    HRESULT ProxyD3D9::QueryInterface(REFIID riid, void** ppvObj)
    {
        if (!ppvObj)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IDirect3D9)
        {
            AddRef();
            *ppvObj = this;
            return S_OK;
        }

        // IDirect3D9Ex would let the caller create a device we never see.
        HRESULT hr = _d3d9->QueryInterface(riid, ppvObj);
        if (SUCCEEDED(hr))
        {
            ((IUnknown*)*ppvObj)->Release();
            *ppvObj = nullptr;
            return E_NOINTERFACE;
        }
        return hr;
    }

    // The qualities offered to the game stop at the [vr] msaa cap
    // (msaa_cap.h), so it never asks for more.
    HRESULT ProxyD3D9::CheckDeviceMultiSampleType(UINT Adapter,
        D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed,
        D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
    {
        const int max_quality = msaa_max_quality();
        if (MultiSampleType != D3DMULTISAMPLE_NONE)
        {
            if (max_quality < 0)
                return D3DERR_NOTAVAILABLE;
            if (MultiSampleType != D3DMULTISAMPLE_NONMASKABLE &&
                int(MultiSampleType) > (1 << max_quality))
                return D3DERR_NOTAVAILABLE;
        }
        const HRESULT hr = _d3d9->CheckDeviceMultiSampleType(Adapter,
            DeviceType, SurfaceFormat, Windowed, MultiSampleType,
            pQualityLevels);
        if (SUCCEEDED(hr) && pQualityLevels &&
            MultiSampleType == D3DMULTISAMPLE_NONMASKABLE &&
            *pQualityLevels > DWORD(max_quality + 1))
            *pQualityLevels = DWORD(max_quality + 1);
        return hr;
    }

    HRESULT ProxyD3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                    DWORD BehaviorFlags,
                                    D3DPRESENT_PARAMETERS* pPresentationParameters,
                                    IDirect3DDevice9** ppReturnedDeviceInterface)
    {
        // Before the device, not after: the headset's eye shape decides what
        // size back buffer to ask for, and that has to be known now.
        vr_init();

        // Install the engine hooks once, before the game draws anything.
        static bool engine_init_done = false;
        if (!engine_init_done)
        {
            engine_init_done = true;
            if (is_trl_process())
            {
                cull_init();
                camera_head_init();
                ui_space_init();
                water_stereo_init();
                input_labels_init();
                hook_apply_gog_crash_guard();
            }
            else
            {
                log("engine hooks: skipped -- host is not trl.exe or trl_vr.exe");
            }
        }

        if (pPresentationParameters)
        {
            log("CreateDevice: adapter=%u type=%s hwnd=0x%p flags=0x%08lX",
                Adapter, devtype_name(DeviceType), (void*)hFocusWindow, BehaviorFlags);
            log("  backbuffer %ux%u fmt=%d count=%u windowed=%d refresh=%u interval=0x%X",
                pPresentationParameters->BackBufferWidth,
                pPresentationParameters->BackBufferHeight,
                (int)pPresentationParameters->BackBufferFormat,
                pPresentationParameters->BackBufferCount,
                pPresentationParameters->Windowed,
                pPresentationParameters->FullScreen_RefreshRateInHz,
                pPresentationParameters->PresentationInterval);
            log("  depth fmt=%d autodepth=%d swapeffect=%d multisample=%d "
                "quality=%lu",
                (int)pPresentationParameters->AutoDepthStencilFormat,
                pPresentationParameters->EnableAutoDepthStencil,
                (int)pPresentationParameters->SwapEffect,
                (int)pPresentationParameters->MultiSampleType,
                pPresentationParameters->MultiSampleQuality);

            if (msaa_clamp(&pPresentationParameters->MultiSampleType,
                           &pPresentationParameters->MultiSampleQuality))
                log("  multisample capped by [vr] msaa = %d: type %d, "
                    "quality %lu", config().msaa,
                    (int)pPresentationParameters->MultiSampleType,
                    pPresentationParameters->MultiSampleQuality);

            if (config().force_windowed && !pPresentationParameters->Windowed)
            {
                log("  forcing windowed: exclusive fullscreen would fight the compositor");
                pPresentationParameters->Windowed = TRUE;
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }

            // SteamVR's recommended size already includes its global and
            // per-application resolution setting. Ask once before device creation;
            // changing it therefore takes effect the next time the game starts.
            bool sized_from_headset = false;
            if (vr_ready())
            {
                unsigned rw = 0, rh = 0;
                vr_render_target_size(&rw, &rh);
                if (rw && rh)
                {
                    log("  back buffer %ux%u per eye from SteamVR's recommended render size",
                        rw, rh);
                    pPresentationParameters->BackBufferWidth = rw & ~1u;
                    pPresentationParameters->BackBufferHeight = rh & ~1u;
                    sized_from_headset = true;
                }
            }

            // Render at the window's size rather than the desktop's. Leaving
            // the back buffer at the desktop resolution inside a smaller window
            // means the runtime stretches every frame for nothing.
            if (config().force_windowed && !sized_from_headset)
            {
                UINT w = 0, h = 0;
                window_choose_size(hFocusWindow,
                                   pPresentationParameters->BackBufferWidth,
                                   pPresentationParameters->BackBufferHeight,
                                   &w, &h);
                if (w && h)
                {
                    // Submit with no bounds tells the compositor the texture is
                    // the whole eye frustum, so a texture of a different shape
                    // gets stretched to fit. Rendering at the eye's own aspect
                    // avoids that: 1440x1080 is 1.333 where the G2's frustum is
                    // about 1.04, which was a 28% horizontal stretch.
                    const float eye = vr_eye_aspect();
                    if (eye > 0.0f)
                    {
                        const unsigned nh = (unsigned)((float)w / eye) & ~1u;
                        if (nh > 0)
                        {
                            log("  back buffer %ux%u to match the eye frustum "
                                "(aspect %.3f) rather than the window", w, nh, eye);
                            h = nh;
                        }
                    }
                    else
                    {
                        log("  back buffer resized to %ux%u to match the window", w, h);
                    }
                    pPresentationParameters->BackBufferWidth = w;
                    pPresentationParameters->BackBufferHeight = h;
                }
            }
        }

        // Same-frame stereo draws both eyes side by side, so the surface has
        // to be twice as wide as an eye -- and that has to be decided here,
        // before the device exists. Setting it afterwards changes nothing but
        // our own idea of the size: the first attempt did exactly that, and
        // the result was the right eye's viewport landing entirely outside a
        // surface that had never grown, with the compositor handed the two
        // halves of a single mono image.
        //
        // The eye size is recorded before doubling. Everything that reasons
        // about the shape of a *view* -- the aspect the x prescale comes from
        // -- means one eye, not the pair.
        if (pPresentationParameters && pPresentationParameters->BackBufferWidth &&
            pPresentationParameters->BackBufferHeight)
        {
            stereo_set_eye_size(pPresentationParameters->BackBufferWidth,
                                pPresentationParameters->BackBufferHeight);
            if (stereo_same_frame_active())
            {
                pPresentationParameters->BackBufferWidth *= 2;
                log("  back buffer widened to %ux%u -- two eyes side by side",
                    pPresentationParameters->BackBufferWidth,
                    pPresentationParameters->BackBufferHeight);
            }
        }

        // DXVK's device lock is a no-op unless the device asked for
        // D3DCREATE_MULTITHREADED, and this game does not (its flags are
        // HARDWARE_VERTEXPROCESSING | PUREDEVICE). Submitting touches the
        // device from the present path, so the flag has to be on or the locks
        // in the VR interop protect nothing at all.
        if (vr_ready() && config().vr_submit &&
            !(BehaviorFlags & D3DCREATE_MULTITHREADED))
        {
            BehaviorFlags |= D3DCREATE_MULTITHREADED;
            log("  forcing D3DCREATE_MULTITHREADED: the VR submit path needs real locking");
        }

        IDirect3DDevice9* device = nullptr;
        const HRESULT hr = _d3d9->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                               pPresentationParameters, &device);
        if (FAILED(hr) || !device)
        {
            log("CreateDevice: FAILED hr=0x%08lX", hr);
            if (ppReturnedDeviceInterface)
                *ppReturnedDeviceInterface = nullptr;
            return hr;
        }

        // The wrapper owns the reference the runtime just gave us.
        AddRef();
        *ppReturnedDeviceInterface = new ProxyDevice(device, this);
        log("CreateDevice: ok, device wrapped");

        if (config().force_windowed && pPresentationParameters)
        {
            // hDeviceWindow is the one actually presented to; the focus window
            // is only the one D3D listens to for mode changes.
            HWND target = pPresentationParameters->hDeviceWindow
                        ? pPresentationParameters->hDeviceWindow
                        : hFocusWindow;
            window_configure(target,
                             pPresentationParameters->BackBufferWidth,
                             pPresentationParameters->BackBufferHeight);
        }

        if (pPresentationParameters && pPresentationParameters->BackBufferHeight)
        {
            // The size the main scene is drawn at. Anything targeting a
            // different surface -- shadow maps, most obviously -- is left with
            // the camera the game gave it.
            vr_set_scene_size(pPresentationParameters->BackBufferWidth,
                              pPresentationParameters->BackBufferHeight);
        }
        if (vr_ready())
            vr_submit_init(device);        // the real device, not the wrapper
        return hr;
    }
}
