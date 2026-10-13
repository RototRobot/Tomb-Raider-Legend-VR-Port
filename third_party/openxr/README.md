# OpenXR SDK (vendored)

From [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK),
tag `release-1.1.49`.

- `include/openxr/` is the SDK's `include/openxr/`, unmodified.
- `lib/win32/openxr_loader.lib` is the loader built as a 32-bit static
  library against the static CRT, to match the proxy's `/MT`.

## Rebuilding the loader

The SDK builds a static loader against the DLL CRT (`/MD`) and has no option
for the other, so one line is changed before configuring: in
`src/loader/CMakeLists.txt`, the `MSVC_RUNTIME_LIBRARY` property in the
non-`DYNAMIC_LOADER` branch loses its `DLL` suffix
(`"MultiThreaded$<$<CONFIG:Debug>:Debug>"`).

```
cmake -S OpenXR-SDK -B xrbuild -G "Visual Studio 17 2022" -A Win32 ^
      -DDYNAMIC_LOADER=OFF -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF ^
      -DBUILD_API_LAYERS=OFF
cmake --build xrbuild --config Release --target openxr_loader
```

The result is `xrbuild/src/loader/Release/openxr_loader.lib`.
