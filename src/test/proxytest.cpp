// Load test for the d3d9 proxy.
//
// Stands in for the game: loads d3d9.dll out of its own directory (so it picks
// up the proxy, exactly as trl.exe would), creates a device, and exercises the
// wrapped constant-upload and present paths.
//
// Run it from a directory containing the built d3d9.dll.

#include <d3d9.h>
#include <windows.h>
#include <cstdio>

namespace
{
    int g_failures = 0;

    void check(bool ok, const char* what)
    {
        printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    // A D3D projection matrix: last column (0,0,1,0), not affine.
    const float kProjection[16] = {
        1.3f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.7f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 1.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
    };

    // A world transform: last column (0,0,0,1), affine.
    const float kWorld[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        5.0f, 6.0f, 7.0f, 1.0f,
    };
}

int main()
{
    printf("d3d9 proxy load test\n\n");

    HMODULE mod = LoadLibraryA("d3d9.dll");
    check(mod != nullptr, "LoadLibrary(\"d3d9.dll\")");
    if (!mod)
    {
        printf("\n  (error %lu)\n", GetLastError());
        return 1;
    }

    char path[MAX_PATH]{};
    GetModuleFileNameA(mod, path, MAX_PATH);
    printf("  loaded: %s\n", path);

    typedef IDirect3D9*(WINAPI * CreateFn)(UINT);
    CreateFn create = (CreateFn)GetProcAddress(mod, "Direct3DCreate9");
    check(create != nullptr, "GetProcAddress(\"Direct3DCreate9\")");
    if (!create)
        return 1;

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    check(d3d != nullptr, "Direct3DCreate9 returned an object");
    if (!d3d)
        return 1;

    const UINT adapters = d3d->GetAdapterCount();
    check(adapters > 0, "GetAdapterCount forwarded");
    printf("  adapters: %u\n", adapters);

    D3DADAPTER_IDENTIFIER9 id{};
    check(SUCCEEDED(d3d->GetAdapterIdentifier(0, 0, &id)), "GetAdapterIdentifier forwarded");
    printf("  adapter0: %s\n", id.Description);

    // QueryInterface for IDirect3D9 must return the wrapper, not the real one.
    IDirect3D9* qi = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(IID_IDirect3D9, (void**)&qi)))
    {
        check(qi == d3d, "QueryInterface(IID_IDirect3D9) returns the wrapper");
        if (qi)
            qi->Release();
    }
    else
    {
        check(false, "QueryInterface(IID_IDirect3D9) succeeded");
    }

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "TrlVrProxyTest";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("TrlVrProxyTest", "TrlVrProxyTest", WS_OVERLAPPEDWINDOW,
                              0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    check(hwnd != nullptr, "created a test window");

    // A windowed NULLREF device is enough to exercise the wrapper without
    // depending on the desktop driver's supported fullscreen modes.
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    check(SUCCEEDED(hr) && dev != nullptr, "CreateDevice");
    if (FAILED(hr) || !dev)
    {
        printf("  hr=0x%08lX\n", hr);
        d3d->Release();
        return 1;
    }

    check(pp.Windowed == TRUE, "proxy forced windowed mode");

    // GetDirect3D must hand back the wrapper too.
    IDirect3D9* parent = nullptr;
    if (SUCCEEDED(dev->GetDirect3D(&parent)))
    {
        check(parent == d3d, "GetDirect3D returns the wrapper");
        if (parent)
            parent->Release();
    }

    // Exercise constant uploads that must pass through unchanged outside TRL.
    for (int frame = 0; frame < 5; ++frame)
    {
        float proj[16];
        memcpy(proj, kProjection, sizeof(proj));
        proj[12] = (float)frame;              // make it change, as a camera does
        dev->SetVertexShaderConstantF(10, proj, 4);

        for (int obj = 0; obj < 100; ++obj)
            dev->SetVertexShaderConstantF(20, kWorld, 4);

        dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    check(true, "pushed 5 frames through the wrapped device");

    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s");
    printf("Check trlvr.log for proxy startup status.\n");
    return g_failures ? 1 : 0;
}
