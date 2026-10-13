# OpenXR port: implementation handoff

Goal: replace the OpenVR dependency with native OpenXR so the mod works on **Virtual Desktop (VDXR)**. OpenComposite is not an option for this target. The port is feasible without a redesign: OpenVR use is concentrated in four files plus the DXVK fork, and everything downstream consumes `Mat4` and the helpers in `src/vr/vr_session.h`.

Estimated size: about 25 distinct OpenVR API calls to replace, roughly 1.5-2k new lines.

Nothing in the repo has been modified yet. This document is the audit result and the plan. Items marked **UNVERIFIED** must be checked by the implementer, not assumed.

---

## 1. Architecture recap

- `trl.exe` is **32-bit**. The mod ships a 32-bit D3D9 proxy `d3d9.dll`, a DXVK fork `d3d9_dxvk.dll` (D3D9 to Vulkan), and `trlvr_launcher.exe`, which patches a copy `trl_vr.exe` to be large-address-aware.
- OpenVR cannot take D3D9 textures, hence DXVK. The `VkImage` behind a D3D9 surface is handed to the compositor through the custom `IDirect3DVR9` interface (`Direct3DCreateVR9`, `dxvk-trlvr/src/d3d9/d3d9_vr.{h,cpp}`).
- All HUD and menus are rendered by the mod into the eye image. There is no IVROverlay, chaperone, render-models or settings usage, so no quad layers are needed.
- Rendering is side-by-side stereo in one surface (back buffer width x2). Same-frame stereo is the main path. An alternating-eye path exists too.

## 2. OpenVR touchpoints to replace

| File | What | Notes |
|---|---|---|
| `src/vr/vr_session.cpp` (3957 lines) | `load_dll` (217-248), `read_device_info` (250-296), `vr_init` (299-378), `vr_get_interface` (1496), `vr_render_target_size` (3605), `vr_update_pose` (3625), `vr_set_render_poses` (3671), `ingest_poses` (3685), controller pose lookups (~2036-2200, 3004-3016, 3476-3487) | `g_tracked_poses[k_unMaxTrackedDeviceCount]` and about 60 uses of `vr::HmdMatrix34_t` |
| `src/vr/vr_submit.cpp` (4152 lines) | `g_comp`, `prepare_surface` (3596-3653), `submit_prepared` (3655-3677), `submit_side_by_side` / `submit_pair` (3680-3715), `resolved_back_buffer` (3722-3761), `vr_submit_init` (3790-3862), `GetFrameTiming` stats and `WaitGetPoses` (~3920-4020) | |
| `src/vr/vr_input.cpp` (4793 lines) | `IVRInput` use: `digital()`/`analog()` (1976-2063), controller type enumeration (2077-2119), init (~2138-2210), skeletal (2371-2470), pose actions (2576-2600), haptics (2652-3028), `UpdateActionState` (3118), profile decision via `Prop_ControllerType_String` (3409) | |
| `src/proxy/proxy_d3d9.cpp` | `vr_init()` at 112, `vr_render_target_size` at 175, `vr_eye_aspect` at 203, `vr_submit_init(device)` at 298 | Defines the init ordering |
| `src/proxy/proxy_device.cpp` | line 114 | |
| `dxvk-trlvr/src/dxvk/dxvk_openvr.{cpp,h}` | `VrInstance`, a `DxvkExtensionProvider` registered at `dxvk_instance.cpp:45` | Supplies Vulkan instance and device extensions today |
| `dxvk-trlvr/src/dxvk/meson.build:95` | lists `dxvk_openvr.cpp` | |
| `dxvk-trlvr/include/openvr/openvr.hpp`, `third_party/openvr/headers/openvr.h` | vendored headers | Replace with OpenXR headers |
| `README.md`, `install.bat`, `package.bat`, `build.bat` | mention `openvr_api.dll`, `trlvr_actions.json`, `bindings_*.json` | Update after the port |
| `src/xinput/xinput_proxy.cpp` | stale comment only | |

## 3. Frame loop mapping

| OpenVR | OpenXR |
|---|---|
| `VR_InitInternal2` (Scene if `config().vr_submit`, else Background) | `xrCreateInstance` + `xrGetSystem`, then session creation (see section 4) |
| `GetRecommendedRenderTargetSize` | `xrEnumerateViewConfigurationViews` (`recommendedImageRectWidth/Height`) |
| `GetProjectionRaw` | `XrView.fov` (angleLeft/Right/Up/Down), take `tanf` once and convert to `EyeInfo` (`tan_left/right/top/bottom`). Mind the sign and axis convention: OpenVR top/bottom has bottom larger, and `vr_eye_aspect` uses `tan_bottom - tan_top` |
| `GetEyeToHeadTransform` | `XrView.pose` located in the VIEW space; offset to `EyeInfo.offset_*`, orientation to `rot[3][3]` |
| IPD | distance between the two view poses |
| `WaitGetPoses` | `xrWaitFrame` then `xrBeginFrame` then `xrLocateViews` and `xrLocateSpace` (HMD and hands) at `predictedDisplayTime` |
| `Submit` x2 | one `xrEndFrame` with one `XrCompositionLayerProjection` |
| Seated tracking universe | `XR_REFERENCE_SPACE_TYPE_LOCAL` |
| `GetFrameTiming` perf lines | no equivalent. Use own timing around `xrWaitFrame` / `xrEndFrame` |
| `SetTrackingSpace(Seated)` | create the LOCAL reference space |

Coordinate systems: OpenVR and OpenXR are both right-handed, Y up, -Z forward, metres. `XrPosef` is quaternion plus vector, so add one helper that converts to the 3x4 row-major layout the existing code reads. **Recommended:** define a local `Mat34` type with the same layout as `vr::HmdMatrix34_t` and keep a pose cache (HMD, left hand, right hand), so the roughly 60 call sites and all the math stay untouched.

Per-eye render pose: the camera hook bakes the head pose mid-frame (`vr_note_main_camera_head_applied`). The pose used there must be remembered and passed in `XrCompositionLayerProjectionView.pose` at `xrEndFrame`, otherwise reprojection is wrong. The `WaitGetPoses`-then-`vr_set_render_poses` handshake (`vr_submit.cpp` ~4011, `g_render_poses_active`) maps to: `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews` at the predicted time, then feed `ingest_poses`.

## 4. Init-order problem (the main design risk)

Current order in `ProxyD3D9::CreateDevice` (`proxy_d3d9.cpp`):
1. `vr_init()` (needs FOV, IPD and render size here, to pick the back buffer size and `stereo_set_eye_size`).
2. Back buffer is sized from `vr_render_target_size` / `vr_eye_aspect`.
3. `_d3d9->CreateDevice(...)`. DXVK creates its own Vulkan instance and device.
4. `vr_submit_init(device)` (real device) gets `IDirect3DVR9`.

OpenXR constraint: a session needs a Vulkan device, and the device is created by DXVK in step 3. FOV and view poses (`xrLocateViews`) are only reliable once a session exists and is running.

Proposed solution:
- Step 1 (`vr_init`): `xrCreateInstance`, `xrGetSystem`, `xrEnumerateViewConfigurationViews` (render size, available pre-session). Fill provisional `EyeInfo`. Recommended: provisional symmetric FOV derived from the aspect of the recommended size (or a config default), replaced with real values after the session starts. The `EyeInfo` consumers (`vr_max_tangent`, `vr_eye_aspect`, culling) must tolerate the late update; check which of them cache values at init. `vr_eye_aspect` is used in `CreateDevice` to choose the back buffer height, so a wrong provisional aspect means a wrong buffer shape. Prefer deriving the aspect from the recommended image rect and verify.
- Step 3: DXVK needs the Vulkan extension lists. Replace `VrInstance` with an OpenXR provider (see section 5).
- Step 4 (`vr_submit_init`): create the session from the DXVK device (`XrGraphicsBindingVulkanKHR` or `...2KHR`), create the reference space, swapchain, then update `EyeInfo`/IPD from the first `xrLocateViews`.
- The `XrInstance` has to be shared between `d3d9.dll` (the proxy, 32-bit, the mod) and `d3d9_dxvk.dll`. Simplest: the proxy creates the `XrInstance` and `XrSystemId` and passes them to the DXVK fork, for example via an exported `trlvr_set_xr_instance(XrInstance, XrSystemId, PFN_xrGetInstanceProcAddr)` called right after `LoadLibrary` and before `CreateDevice`.

## 5. Vulkan interop

Which Vulkan extension does VDXR support? **UNVERIFIED.** Check at runtime with `xrEnumerateInstanceExtensionProperties` and log the list. Keep the code able to pick whichever is present.

- **`XR_KHR_vulkan_enable` (v1), recommended first.** DXVK keeps creating its own `VkInstance`/`VkDevice`. The runtime only tells us what to add:
  - `xrGetVulkanInstanceExtensionsKHR`, `xrGetVulkanDeviceExtensionsKHR`, `xrGetVulkanGraphicsDeviceKHR` (picks the physical device, so `DxvkAdapter` selection must match).
  - This matches the existing `DxvkExtensionProvider::getInstanceExtensions()` / `getDeviceExtensions(adapterId)` flow in `dxvk_openvr.cpp`: replace the OpenVR query code with these calls and keep `parseExtensionList`. Delete the Wine registry path (`Software\Wine\VR`) and the `openvr_api.dll` loading.
  - Call `xrGetVulkanGraphicsRequirementsKHR` before session creation (mandatory).
- **`XR_KHR_vulkan_enable2`.** The instance and device must be created through `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR`. This forces DXVK's `vkCreateInstance` (`dxvk_instance.cpp:258`) and `vkCreateDevice` (`dxvk_adapter.cpp:290`) to go through the runtime. More invasive. Only do this if the runtime offers no v1.

Session creation: `XrGraphicsBindingVulkanKHR` takes `instance`, `physicalDevice`, `device`, `queueFamilyIndex`, `queueIndex`. Most are already returned by `GetVRDesc` (`D3D9_TEXTURE_VR_DESC`: `Instance`, `PhysicalDevice`, `Device`, `Queue`, `QueueFamilyIndex`). The queue index within the family is not in the desc and must be added.

### Swapchain copy design (the other hard part)

OpenXR swapchain images are owned by the runtime, unlike OpenVR where the app's `VkImage` was submitted directly. Per frame:
1. `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`.
2. On DXVK's queue: copy the eye surface image into the swapchain `VkImage` (`vkCmdCopyImage` or `vkCmdBlitImage`, with barriers: source `TRANSFER_SRC_OPTIMAL`, which `TransferSurface` already provides; destination into `TRANSFER_DST_OPTIMAL` and back to what the runtime expects). Submit and wait for the fence before `xrReleaseSwapchainImage`.
3. `xrReleaseSwapchainImage`, then `xrEndFrame`.

Design points:
- Use **one swapchain of width 2x eye width** with two `imageRect`s, matching the existing side-by-side bounds (left 0 to 0.5, right 0.5 to 1; `tune_swap_eyes()` swaps them). If the runtime rejects the size, fall back to two swapchains.
- Sample count must be 1. FSAA is already resolved first via `resolved_back_buffer` (`vr_submit.cpp` 3722-3761).
- Formats: enumerate `xrEnumerateSwapchainFormats`. The current code passes the linear format and tells OpenVR the colour space separately (`ColorSpace_Auto`). With OpenXR choose an `_SRGB` or `UNORM` swapchain format so the copy is bit-exact; a format mismatch needs a blit. Get this wrong and colours come out washed out or too dark.
- **Add a new method to `IDirect3DVR9`** (append at the end of the vtable) or a separate export in the DXVK fork, for example `CopyToExternalImage(pSurface, VkImage dst, VkImageLayout dstLayout, srcRect, dstRect)`, which records the copy through DXVK's own context so DXVK's resource tracking and layout management stay correct, inside the existing `LockDevice` / `Flush` discipline. Safer than recording raw Vulkan on DXVK's queue from outside.
- Locking rules from the existing code (must be preserved; violating them deadlocks): prepare both eyes first (`prepare_surface` does `TransferSurface(surface, TRUE)`), then hold `LockSubmissionQueue` only around the actual queue submit, and never wait on the GPU while holding it. `VkQueue` is externally synchronised and DXVK's submit thread uses it. The device must be created with `D3DCREATE_MULTITHREADED` (the proxy forces it, `proxy_d3d9.cpp` 254-259).
- `GetVRDesc` sets `QueueFamilyIndex` from `queues().graphics.queueIndex`. Check whether that field is the family index or the queue index and name it correctly.
- The image pinning against relocation in `GetVRDesc` (SiN VR NVIDIA crash lesson) only matters if a raw handle leaves DXVK. If the copy is done inside DXVK it is unnecessary but harmless.

## 6. Input

Current: SteamVR Input. Action set `/actions/gameplay` in `input/actions.json`: 17 booleans, 2 vector2 (move, look), 2 poses (`hand_left`, `hand_right`), 2 skeletons, 2 haptics. Four binding files: `bindings_oculus_touch.json`, `bindings_knuckles.json`, `bindings_holographic_controller.json`, `bindings_hpmotioncontroller.json`.

Port to the OpenXR action system, all in code (no manifest files):
- One `XrActionSet` "gameplay"; boolean, float, vector2f, pose and vibration actions mirroring `actions.json`. Create subaction paths `/user/hand/left` and `/user/hand/right` where actions are per-hand.
- `xrSuggestInteractionProfileBindings` for each profile worth supporting. For VDXR the primary profile is expected to be `/interaction_profiles/oculus/touch_controller` (**UNVERIFIED**; log what `xrGetCurrentInteractionProfile` returns). Also suggest `valve/index_controller`, `microsoft/motion_controller`, `hp/mixed_reality_controller` (extension) and `khr/simple_controller` as a fallback. Translate the existing JSON binding files to this table; they are the source of truth for which physical input maps to which action.
- Per frame: `xrSyncActions`, then `xrGetActionStateBoolean/Float/Vector2f`, `xrLocateSpace` on grip action spaces (`xrCreateActionSpace`). Replaces `UpdateActionState`, `GetDigitalActionData`, `GetAnalogActionData` and `GetPoseActionDataRelativeToNow`.
- `GetOriginTrackedDeviceInfo` and `GetTrackedDeviceIndexForControllerRole` go away. Hand identity comes from the subaction path.
- Haptics (`vr_input.cpp` 2652-3028): `xrApplyHapticFeedback` with `XrHapticVibration` (duration ns, frequency, amplitude); `xrStopHapticFeedback` for cancellation.
- **Press-mode state machine:** SteamVR bindings currently give single, double and long press (`long_press_delay` 0.65 s). OpenXR has no equivalent, so the mod must implement it. Known uses: left-stick click (single = gear menu, double = recentre, long = medkit/pause), right-stick click (double = PDA, long = binoculars), a light long-press. Re-read the binding JSONs to find every `mode` other than a plain button and implement a small per-action tracker (press time, click count, long-press timer) that feeds the same boolean the rest of the code reads.
- Controller type: `Prop_ControllerType_String` (vr_input.cpp 3409, enumeration 2077-2119) decides profile-specific behaviour. Replace with the interaction profile from `xrGetCurrentInteractionProfile` (handle `XrEventDataInteractionProfileChanged`).
- Grip pose and hand calibration (`vr_set_controller_grip_offset`, `vr_controller_grip_head_pose`, hand calibration menu) assume SteamVR's grip pose. OpenXR's grip is defined similarly but not identically per runtime. Keep the calibration code, and expect small offsets versus SteamVR; saved `hand_calibration_*` values may need a sanity check.
- **Skeletal / finger tracking** (`finger_tracking`, default off, `vr_input.cpp` 2371-2470): no core-OpenXR equivalent. **Defer.** Stub it to report unavailable. Later option: `XR_EXT_hand_tracking` (**UNVERIFIED** on VDXR), or approximate finger curls from trigger, grip and thumb-touch values.
- The OpenXR event loop (`xrPollEvent`: READY then `xrBeginSession`, STOPPING then `xrEndSession`, EXITING, LOSS_PENDING) has no OpenVR counterpart in the current code. Add it to the per-frame path.

## 7. 32-bit runtime and loader

- 32-bit OpenXR on Windows reads `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime`. The `XR_RUNTIME_JSON` environment variable overrides it, and the launcher (`src/launcher/main.cpp`) is the place to set it.
- VDXR ships a 32-bit manifest, `virtualdesktop-openxr-32.json`, under `C:\Program Files\Virtual Desktop Streamer\OpenXR\` (**UNVERIFIED** exact path and whether the Streamer registers it in WOW6432Node). Recommended launcher behaviour: if `XR_RUNTIME_JSON` is unset and the WOW6432Node ActiveRuntime is missing or not VDXR, look for the VDXR `-32.json` and set the variable for the child process. Provide a `[vr] xr_runtime_json` config override.
- SteamVR 2.17+ beta ships `steamxr_win32.json` (reportedly flaky). Meta and WMR 32-bit runtimes are **UNVERIFIED**. Only VDXR is a supported target for now.
- Need a **32-bit `openxr_loader.dll`**. Build from Khronos OpenXR-SDK-Source for Win32 (`cmake -A Win32`), or load it dynamically from the exe directory the way `openvr_api.dll` is loaded today (`load_dll`, vr_session.cpp 217-248). The proxy is the game's d3d9.dll, so a missing or broken runtime must still result in the game running flat: on any OpenXR failure, log and return false from `vr_init`.
- Headers: `openxr.h`, `openxr_platform.h` (needs `XR_USE_GRAPHICS_API_VULKAN` and the Vulkan headers; `build.bat` already includes the Vulkan SDK from `C:\VulkanSDK`). Replace `third_party/openvr` with `third_party/openxr` and add the loader licence (Apache-2.0 / MIT) to the third-party notices.
- Instance extensions to enable: `XR_KHR_vulkan_enable` (or `_enable2`), and `XR_EXT_hand_tracking` later.

## 8. Build and packaging

- `build.bat`: `cl /MT /std:c++17`, change `/I third_party\openvr\headers` to `/I third_party\openxr\include`. It links `dxguid.lib user32.lib kernel32.lib` and not `d3d9.lib`. If the loader is linked statically add `openxr_loader.lib`, otherwise load it dynamically.
- `build_dxvk.bat` and `dxvk-trlvr/src/dxvk/meson.build`: drop `dxvk_openvr.cpp`, add the new OpenXR provider file and the OpenXR include path. The DXVK fork is built as 32-bit.
- `install.bat` and `package.bat`: stop copying `openvr_api.dll`, `trlvr_actions.json` and `bindings_*.json`; add `openxr_loader.dll` (x86).
- `README.md`: requirements, install file list, uninstall list, licences, thanks.
- Add `[vr]` config keys in `src/common/config.h` (`vr_enabled` is at line 18, `vr_submit` at 19; parsing lives in `src/common/config.cpp`), for example `xr_runtime_json` and `xr_vulkan_ext` (auto/1/2).

## 9. Suggested phases

1. **Probe (no game changes).** Small standalone x86 exe that loads the 32-bit loader, calls `xrCreateInstance`, lists extensions (`XR_KHR_vulkan_enable`, `_enable2`, `XR_EXT_hand_tracking`, interaction profiles), calls `xrGetSystem` and prints the recommended render size. Confirms the VDXR 32-bit runtime, the manifest path and which Vulkan extension it supports. Do this first; the answers change section 5.
2. **Instance plumbing.** `vr_init` using OpenXR, provisional `EyeInfo`, share `XrInstance` with the DXVK fork, replace `VrInstance` with an OpenXR extension provider.
3. **Session, swapchain and frame loop.** Session creation in `vr_submit_init`, LOCAL space, event loop, `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame`, copy path in the DXVK fork, real FOV/IPD update. Goal: stereo image in the headset with head tracking, keyboard and mouse input still working.
4. **Pose plumbing.** Replace `g_tracked_poses` with the `Mat34` pose cache, controller poses from action spaces.
5. **Input.** Actions, bindings, press-mode state machine, haptics, controller type.
6. **Cleanup and packaging.** Remove OpenVR code and headers, update scripts and README, stub finger tracking.
7. **Optional later.** `XR_EXT_hand_tracking`, more interaction profiles, 32-bit SteamVR runtime.

## 10. Verification

- Log the runtime name, extension list, chosen Vulkan extension, swapchain format, recommended size, and real FOV and IPD at startup.
- Compare with an OpenVR baseline: `EyeInfo` values (tangents, IPD), render target size, per-eye aspect, so culling, `stereo_set_eye_size` and the back buffer shape stay equivalent.
- Visual checks: no eye swap (`tune_swap_eyes`), correct handedness, no colour shift (sRGB vs UNORM), world-locked stability while turning (render pose passed correctly at `xrEndFrame`), no stretch (aspect).
- Check the deadlock rules under load: no GPU wait while holding `LockSubmissionQueue`. Test pause menu, cutscene theatre, board diorama and loading screens (frames stop; handle `xrWaitFrame` cadence and session state, and never call `xrEndFrame` without `xrBeginFrame`).
- Input: every action in `actions.json` triggers, including single, double and long presses, both sticks, grip and trigger values, haptics on both hands, recentre.
- Failure paths: runtime not installed, headset not connected, loader missing, session lost (`XR_SESSION_LOSS_PENDING`). The game must still run flat and not crash.

## 11. Risks

- VDXR support for `XR_KHR_vulkan_enable` vs `_enable2` and the 32-bit manifest location are unverified (phase 1 answers both).
- Init order: FOV and IPD are not known when the back buffer is sized. A wrong provisional aspect gives a wrongly shaped buffer; consider a device reset if the real aspect differs noticeably.
- Copying into runtime-owned swapchain images adds a GPU copy per frame and a synchronisation point, and is the most likely source of deadlocks or flicker.
- Single, double and long-press behaviour was provided by SteamVR bindings and now lives in mod code.
- Finger tracking is dropped initially.
- OpenXR has no `GetFrameTiming`, so code in `vr_submit.cpp` that reads those stats (about 3920-4020) needs its own timers or removal.
- Streaming runtimes can change predicted display time and pose latency compared with SteamVR, so the render-pose handshake needs testing on VDXR specifically.