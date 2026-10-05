#include "proxy_device.h"
#include "msaa_cap.h"
#include "gpu_profile.h"
#include "proxy_swapchain.h"
#include "window.h"
#include "../vr/vr_session.h"
#include "../vr/stereo.h"
#include "../vr/vr_submit.h"

#include "../common/config.h"
#include "../common/log.h"

namespace trlvr
{
    HRESULT ProxyDevice::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
    {
        if (!pSwapChain)
            return D3DERR_INVALIDCALL;

        IDirect3DSwapChain9* chain = nullptr;
        const HRESULT hr = _device->GetSwapChain(iSwapChain, &chain);
        if (FAILED(hr) || !chain)
        {
            *pSwapChain = nullptr;
            return hr;
        }
        *pSwapChain = new ProxySwapChain(chain, this);
        return hr;
    }

    HRESULT ProxyDevice::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                   IDirect3DSwapChain9** pSwapChain)
    {
        if (!pSwapChain)
            return D3DERR_INVALIDCALL;

        IDirect3DSwapChain9* chain = nullptr;
        const HRESULT hr = _device->CreateAdditionalSwapChain(pPresentationParameters, &chain);
        if (FAILED(hr) || !chain)
        {
            *pSwapChain = nullptr;
            return hr;
        }
        log("device: CreateAdditionalSwapChain wrapped");
        *pSwapChain = new ProxySwapChain(chain, this);
        return hr;
    }

    HRESULT ProxyDevice::QueryInterface(REFIID riid, void** ppvObj)
    {
        if (!ppvObj)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9)
        {
            AddRef();
            *ppvObj = this;
            return S_OK;
        }

        // Anything else -- IDirect3DDevice9Ex in particular -- we do not wrap.
        // Handing back the real object for those would let a caller drive the
        // device behind our back, so refuse instead of lying.
        HRESULT hr = _device->QueryInterface(riid, ppvObj);
        if (SUCCEEDED(hr))
        {
            ((IUnknown*)*ppvObj)->Release();
            *ppvObj = nullptr;
            log("device: refused QueryInterface for an interface we do not wrap");
            return E_NOINTERFACE;
        }
        return hr;
    }

    HRESULT ProxyDevice::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                 HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
    {
        // The frame boundary. Everything the mod does per frame hangs off here.
        if (config().force_windowed)
            window_tick();
        if (vr_ready())
            vr_update_pose();

        camera_first_person_gpu_frame_end();

        return _device->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    HRESULT ProxyDevice::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
    {
        gpu_profile_set_device(nullptr);
        if (pPresentationParameters)
        {
            msaa_clamp(&pPresentationParameters->MultiSampleType,
                       &pPresentationParameters->MultiSampleQuality);
            log("device: Reset to %ux%u windowed=%d",
                pPresentationParameters->BackBufferWidth,
                pPresentationParameters->BackBufferHeight,
                pPresentationParameters->Windowed);

            if (config().force_windowed && !pPresentationParameters->Windowed)
            {
                pPresentationParameters->Windowed = TRUE;
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }
            // Same sizing as CreateDevice (proxy_d3d9.cpp): SteamVR's
            // recommended per-eye size, then both eyes side by side. A reset
            // (a graphics option change) used to fall back to the window
            // size with one eye's width (2026-10-04).
            bool sized_from_headset = false;
            if (vr_ready())
            {
                unsigned rw = 0, rh = 0;
                vr_render_target_size(&rw, &rh);
                if (rw && rh)
                {
                    pPresentationParameters->BackBufferWidth = rw & ~1u;
                    pPresentationParameters->BackBufferHeight = rh & ~1u;
                    sized_from_headset = true;
                    log("device: reset back buffer %ux%u per eye from "
                        "SteamVR's recommended render size", rw, rh);
                }
            }
            if (config().force_windowed && !sized_from_headset)
            {
                UINT w = 0, h = 0;
                window_choose_size(pPresentationParameters->hDeviceWindow,
                                   pPresentationParameters->BackBufferWidth,
                                   pPresentationParameters->BackBufferHeight,
                                   &w, &h);
                if (w && h)
                {
                    pPresentationParameters->BackBufferWidth = w;
                    pPresentationParameters->BackBufferHeight = h;
                }
            }
            if (pPresentationParameters->BackBufferWidth &&
                pPresentationParameters->BackBufferHeight)
            {
                stereo_set_eye_size(pPresentationParameters->BackBufferWidth,
                                    pPresentationParameters->BackBufferHeight);
                if (stereo_same_frame_active())
                {
                    pPresentationParameters->BackBufferWidth *= 2;
                    log("device: reset back buffer widened to %ux%u -- two "
                        "eyes side by side",
                        pPresentationParameters->BackBufferWidth,
                        pPresentationParameters->BackBufferHeight);
                }
            }
        }

        if (pPresentationParameters)
            vr_set_scene_size(pPresentationParameters->BackBufferWidth,
                              pPresentationParameters->BackBufferHeight);

        vr_submit_release_targets();
        const HRESULT hr = _device->Reset(pPresentationParameters);
        gpu_profile_set_device(_device);

        // A reset can put the fullscreen styling back, so redo it.
        if (SUCCEEDED(hr) && config().force_windowed && pPresentationParameters)
        {
            HWND target = pPresentationParameters->hDeviceWindow;
            if (target)
                window_configure(target,
                                 pPresentationParameters->BackBufferWidth,
                                 pPresentationParameters->BackBufferHeight);
        }
        return hr;
    }

    HRESULT ProxyDevice::SetVertexShaderConstantF(UINT StartRegister,
                                                  CONST float* pConstantData,
                                                  UINT Vector4fCount)
    {
        // c0..c3 is the projection, and this is where the game's camera is
        // replaced by the headset's.
        //
        // projection_from_registers is the filter as well as the parser: it
        // only accepts a matrix with w = z_view, which is what a perspective
        // projection has and a 2D or orthographic pass does not. So HUD and
        // post-processing draws that also touch c0..c3 are left alone without
        // needing to identify them individually.
        if (vr_ready() && StartRegister == 0 && Vector4fCount == 4 && pConstantData)
        {
            const GameProjection gp = projection_from_registers(pConstantData);

            // Two kinds of upload arrive here. View-space geometry gets the
            // projection alone; world-space geometry gets a full
            // world-view-projection. Both need replacing, differently, and 2D
            // passes need leaving alone -- vr_substitute_c0 decides which.
            float regs[16];
            if (vr_substitute_c0(pConstantData, regs))
            {
                vr_note_c0(pConstantData, regs);
                return _device->SetVertexShaderConstantF(StartRegister, regs, 4);
            }
            vr_note_c0(pConstantData, pConstantData);
        }

        return _device->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }

    HRESULT ProxyDevice::SetViewport(CONST D3DVIEWPORT9* pViewport)
    {
        return _device->SetViewport(pViewport);
    }

    HRESULT ProxyDevice::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
    {
        if (RenderTargetIndex == 0)
        {
            gpu_profile_render_target(pRenderTarget);
            D3DSURFACE_DESC desc{};
            if (pRenderTarget && SUCCEEDED(pRenderTarget->GetDesc(&desc)))
                vr_set_render_target_size(desc.Width, desc.Height);
        }
        return _device->SetRenderTarget(RenderTargetIndex, pRenderTarget);
    }

    HRESULT ProxyDevice::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
    {
        return _device->SetTransform(State, pMatrix);
    }
}
