# OpenXR SDK (vendored)

From [KhronosGroup/OpenXR-SDK-Source](https://github.com/KhronosGroup/OpenXR-SDK-Source),
tag `release-1.1.63`.

- `include/openxr/` is the SDK's `include/openxr/`, unmodified.
- `bin/win32/openxr_loader.dll` and `lib/win32/openxr_loader.lib` are the
  loader, built 32-bit as a DLL (unmodified source) with its import library.

`d3d9.dll` delay-loads `openxr_loader.dll`: without it beside the game the
mod still starts and falls back to OpenVR (`src/vr/vr_runtime.cpp`).

## Rebuilding the loader

```
cmake -S OpenXR-SDK-Source-release-1.1.63 -B xrbuild -G "Visual Studio 17 2022" -A Win32 ^
      -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF ^
      -DBUILD_API_LAYERS=OFF
cmake --build xrbuild --config Release --target openxr_loader
```

The results are in `xrbuild/src/loader/Release/`.
