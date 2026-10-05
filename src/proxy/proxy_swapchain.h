#pragma once
// IDirect3DSwapChain9 wrapper.
//
// The game does not call IDirect3DDevice9::Present. It asks for swap chain 0
// and presents through that, so handing back the real swap chain would let
// every frame boundary past us unseen -- which is exactly what happened on the
// first run against the game. Wrapping it is also how the finished mod will get
// at the back buffer to hand to the compositor.

#include <d3d9.h>
#include <windows.h>

namespace trlvr
{
    struct ProxySwapChain : IDirect3DSwapChain9
    {
        IDirect3DSwapChain9* _chain;
        IDirect3DDevice9* _device;      // the wrapper, not the real device
        volatile LONG _ref;

        ProxySwapChain(IDirect3DSwapChain9* chain, IDirect3DDevice9* device)
            : _chain(chain), _device(device), _ref(1)
        {
        }

        ~ProxySwapChain()
        {
            if (_chain)
                _chain->Release();
        }

        STDMETHOD(QueryInterface)(THIS_ REFIID riid, void** ppvObj);

        STDMETHOD_(ULONG, AddRef)(THIS)
        {
            return (ULONG)InterlockedIncrement(&_ref);
        }

        STDMETHOD_(ULONG, Release)(THIS)
        {
            const LONG count = InterlockedDecrement(&_ref);
            if (count == 0)
                delete this;
            return (ULONG)count;
        }

        STDMETHOD(Present)(THIS_ CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags);

        STDMETHOD(GetFrontBufferData)(THIS_ IDirect3DSurface9* pDestSurface) { return _chain->GetFrontBufferData(pDestSurface); }
        STDMETHOD(GetBackBuffer)(THIS_ UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) { return _chain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer); }
        STDMETHOD(GetRasterStatus)(THIS_ D3DRASTER_STATUS* pRasterStatus) { return _chain->GetRasterStatus(pRasterStatus); }
        STDMETHOD(GetDisplayMode)(THIS_ D3DDISPLAYMODE* pMode) { return _chain->GetDisplayMode(pMode); }
        STDMETHOD(GetPresentParameters)(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters) { return _chain->GetPresentParameters(pPresentationParameters); }

        STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9** ppDevice)
        {
            if (!ppDevice)
                return D3DERR_INVALIDCALL;
            if (_device)
            {
                _device->AddRef();
                *ppDevice = _device;
                return D3D_OK;
            }
            return _chain->GetDevice(ppDevice);
        }
    };
}
