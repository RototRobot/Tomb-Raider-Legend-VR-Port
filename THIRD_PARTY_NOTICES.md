# Third-party notices

## DXVK (`dxvk-trlvr/`)

A fork of [DXVK](https://github.com/doitsujin/dxvk) 3.1 with a D3D9 VR
interop extension (`src/d3d9/d3d9_vr.*`) and a profile for `trl_vr.exe`.
zlib/libpng licence; see `dxvk-trlvr/LICENSE`. Its bundled headers and
subprojects (Vulkan-Headers, SPIRV-Headers, mingw-directx-headers,
dxbc-spirv, libdisplay-info) keep their own licences in their folders.

## OpenXR SDK (`third_party/openxr/`)

The OpenXR headers and a 32-bit static build of the OpenXR loader from the
Khronos [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK), release
1.1.49. The loader is linked into `d3d9.dll`. Apache-2.0 licence (the loader
bundles jsoncpp, MIT / public domain); see `third_party/openxr/LICENSE` and
`third_party/openxr/COPYING.adoc`. `third_party/openxr/README.md` says how
the library was built.

## TRAWindowed

The D3D9 proxy wrapper in `src/proxy/` started from
[TRAWindowed](https://github.com/chreden/trawindowed) by chreden.

MIT License

Copyright (c) 2018 chreden

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
