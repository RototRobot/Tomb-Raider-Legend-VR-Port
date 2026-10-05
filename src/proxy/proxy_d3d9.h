#pragma once
// IDirect3D9 wrapper. Its only real job is to catch CreateDevice so we can see
// the presentation parameters and wrap the device that comes back.
//
// Forked from TRAWindowed (MIT, (c) 2018 chreden).

#include <d3d9.h>
#include <windows.h>

namespace trlvr
{
    struct ProxyD3D9 : IDirect3D9
    {
        IDirect3D9* _d3d9;
        volatile LONG _ref;

        explicit ProxyD3D9(IDirect3D9* d3d9)
            : _d3d9(d3d9), _ref(1)
        {
        }

        ~ProxyD3D9()
        {
            if (_d3d9)
                _d3d9->Release();
        }

        /*** IUnknown ***/
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

        /*** instrumented ***/
        STDMETHOD(CreateDevice)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface);

        /*** forwarded ***/
        STDMETHOD(RegisterSoftwareDevice)(THIS_ void* pInitializeFunction) { return _d3d9->RegisterSoftwareDevice(pInitializeFunction); }
        STDMETHOD_(UINT, GetAdapterCount)(THIS) { return _d3d9->GetAdapterCount(); }
        STDMETHOD(GetAdapterIdentifier)(THIS_ UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) { return _d3d9->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
        STDMETHOD_(UINT, GetAdapterModeCount)(THIS_ UINT Adapter, D3DFORMAT Format) { return _d3d9->GetAdapterModeCount(Adapter, Format); }
        STDMETHOD(EnumAdapterModes)(THIS_ UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) { return _d3d9->EnumAdapterModes(Adapter, Format, Mode, pMode); }
        STDMETHOD(GetAdapterDisplayMode)(THIS_ UINT Adapter, D3DDISPLAYMODE* pMode) { return _d3d9->GetAdapterDisplayMode(Adapter, pMode); }
        STDMETHOD(CheckDeviceType)(THIS_ UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) { return _d3d9->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
        STDMETHOD(CheckDeviceFormat)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) { return _d3d9->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
        STDMETHOD(CheckDeviceMultiSampleType)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels);
        STDMETHOD(CheckDepthStencilMatch)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) { return _d3d9->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
        STDMETHOD(CheckDeviceFormatConversion)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) { return _d3d9->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
        STDMETHOD(GetDeviceCaps)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) { return _d3d9->GetDeviceCaps(Adapter, DeviceType, pCaps); }
        STDMETHOD_(HMONITOR, GetAdapterMonitor)(THIS_ UINT Adapter) { return _d3d9->GetAdapterMonitor(Adapter); }
    };
}
