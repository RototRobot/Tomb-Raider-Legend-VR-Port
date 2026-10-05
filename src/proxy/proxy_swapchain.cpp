#include "proxy_swapchain.h"
#include "gpu_profile.h"
#include "../vr/perf_cpu.h"
#include "window.h"
#include "../vr/vr_session.h"
#include "../vr/vr_submit.h"
#include "../vr/tune.h"
#include "../vr/vr_input.h"
#include "../vr/camera_head.h"

#include "../common/config.h"
#include "../common/log.h"

namespace trlvr
{
    HRESULT ProxySwapChain::QueryInterface(REFIID riid, void** ppvObj)
    {
        if (!ppvObj)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IDirect3DSwapChain9)
        {
            AddRef();
            *ppvObj = this;
            return S_OK;
        }

        HRESULT hr = _chain->QueryInterface(riid, ppvObj);
        if (SUCCEEDED(hr))
        {
            ((IUnknown*)*ppvObj)->Release();
            *ppvObj = nullptr;
            return E_NOINTERFACE;
        }
        return hr;
    }

    HRESULT ProxySwapChain::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                    HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion,
                                    DWORD dwFlags)
    {
        // This, not IDirect3DDevice9::Present, is the game's frame boundary.
        perf_cpu_present_begin();
        camera_first_person_gpu_frame_end();
        if (config().force_windowed)
            window_tick();
        if (vr_ready())
        {
            PerfCpuScope perf_scope(PerfInput);
            tune_update();
            vr_update_pose();
            // Gestures consume the controller poses refreshed immediately
            // above; their keyboard events are naturally applied to the next
            // game frame.
            vr_input_update();
        }

        gpu_profile_mark("mod: VR submit (resolve, overlays, hands)");
        if (vr_submit_ready())
        {
            IDirect3DSurface9* back = nullptr;
            if (SUCCEEDED(_chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
            {
                vr_submit_frame(back);
                back->Release();
            }
            gpu_profile_frame_end();
            // After the frame is in, not before: this blocks until the
            // compositor is ready for the next one, which is what paces the
            // game to the headset instead of to the monitor.
            vr_submit_wait();
        }

        else
            gpu_profile_frame_end();

        const HRESULT hr = _chain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        perf_cpu_present_end();
        return hr;
    }
}
