# FORK_CHANGES: what this fork adds to upstream

Audience: the upstream author's coding agents, choosing which features to merge.
Status below is per the fork author; verify on a headset before relying on it.

## (a) Overview

- **Base:** upstream tag `upstream-base` (= f500659). Branch `god-mode-backport`
  is the fork tip; `main` is upstream's branch.
- **How to diff:** `git diff upstream-base..god-mode-backport -- <paths>`
  (for example `-- src/vr/physics src/vr/godmode` for the new modules, or
  `-- src/vr/camera_head.cpp` for glue). Shared glue files are heavily
  interleaved, so locate a feature by function or section name, not by hunk.
- **Commits (oldest first):**

| Commit | Subject | Feature(s) |
|---|---|---|
| dd4a740 | port to openXR | 1 (removes OpenVR, `third_party/openvr`, `dxvk_openvr.*`) |
| 1a2ecc7 | port to openxr | 1 (`xr_runtime.*`, `xrprobe`, `third_party/openxr`) |
| 9f222e7 | improved inputs, settings, physics | 2, 3, 4, 5 |
| 8aa78fc | cleanup | 15 (removed handoff notes and `input/*.json`) |
| 4d714b4 | physics modules | 5, 9, 10 (`src/vr/physics/*` + tests) |
| 664f6c3 | godmode modules | 6, 7, 11 (`src/vr/godmode/*` + tests) |
| 9a636c0 | finger maths + in-VR menu panel | 8, 13 (`finger_input.*`, `menu_panel.*` + tests) |
| 11bd8b2 | integration | glue for 6-14 (`camera_head.*`, `vr_input.*`, `vr_session.*`, `vr_submit.cpp`, `config.*`, `overlay.*`, `vr_math.*`, `mathtest.cpp`, `build.bat`, `mathtest.bat`) |
| (this commit) | menu fixes, scale input option, rear physics tuning | 10, 11, 13 (`ui_space.cpp`, `vr_session.cpp`, `vr_input.cpp`, `camera_head.cpp`, `config.*`, `overlay.cpp`, `physics/rear_*`) |

- **Cherry-pick story:** modules under `src/vr/physics/` and `src/vr/godmode/`,
  and `src/vr/finger_input.*` and `src/vr/menu_panel.*`, are pure maths on
  `vr_math.h` with unit tests in `src/test/*_tests.inc` (run with `mathtest.bat`).
  They cherry-pick cleanly. The glue (commit 11bd8b2, and 9f222e7 for overlay
  and camera) must be ported by function name, as described per feature below.
- **Gotcha:** Legend already has a `SoftSpring` inside `camera_head.cpp`;
  `physics/soft_body.h` defines its own. Do not merge them blindly.

## (b) Dependency graph

```
upstream-base
└─ 1 OpenXR port (xr_runtime, vr_session / vr_submit / vr_input)
   ├─ 2 ImGui overlay + ini write-back (edits other features' keys; not a code prerequisite)
   ├─ 3 Smooth camera / view glide
   ├─ 4 Picture adjust
   ├─ 5 Third-person chest soft physics
   ├─ 9 Body push (physics/body_volumes, physics/body_push)
   │   └─ 10 Rear physics (physics/rear_jiggle, rear_shape, soft_body)
   ├─ 6 God mode free roam / carry (godmode/god_carry)
   │   ├─ 7 Throw + pick-up (godmode/god_pluck)
   │   ├─ 8 Pose / finger grabs (finger_input, vr_input pose glue)
   │   └─ 14 Hands near Lara withhold grip/trigger (claim latch)
   ├─ 11 Board leash, zoom, scale (godmode/board_leash)
   │   └─ 12 Right-stick rework (uses board_stick_axis, camera_set_tp_view)
   └─ 13 VR menu fix + in-VR settings panel (menu_panel)
15 Removed: OpenVR, input/*.json, mission_prep_backdrop_* (no dependency)
```

## (c) Features

### 1. OpenXR port

- **What:** Replaces OpenVR/SteamVR with OpenXR. Controller bindings are
  suggested in code per interaction profile (`xr_runtime.cpp`, `suggest`,
  `xr_input_profile`), so no `input/*.json` manifests ship. `xrprobe` is a
  standalone runtime diagnostic.
- **Commits:** dd4a740, 1a2ecc7.
- **New files:** `src/vr/xr_runtime.{cpp,h}`, `src/test/xrprobe.cpp`,
  `xrprobe.bat` (output `build\test\xrprobe.log`, headset must be connected),
  `third_party/openxr/`.
- **Glue:** `vr_session.cpp`, `vr_submit.cpp` (`xr_copy_wait` use),
  `vr_input.*`, `proxy_d3d9.cpp`, `proxy_device.cpp`, `dllmain.cpp`,
  `build.bat` / `install.bat` / `package.bat`.
- **Log prefix:** `xr:`.
- **Ini keys:**
  - `vr.xr_runtime_json` (default empty: use the registered runtime; sets `XR_RUNTIME_JSON`)
  - `vr.xr_copy_wait` (default `1`; wait for the GPU copy before handing the image over)
- **Depends on:** nothing. Everything depends on it.
- **Status:** tested on headset.

### 2. ImGui desktop overlay and ini write-back

- **What:** Dear ImGui settings window (Insert, or hold the VR menu button
  per feature 13) that edits live config values and writes them back to
  `trlvr.ini`.
- **Commits:** 9f222e7 (overlay), 11bd8b2 (wiring).
- **New files:** `src/proxy/overlay.{cpp,h}`, `third_party/imgui/`.
- **Glue:** `src/proxy/window.cpp` `window_block_game_mouse` (blocks game mouse
  input while the overlay is open); `src/common/config.cpp`
  `config_edit`, `config_save_setting`.
- **Overlay sections (functions in `overlay.cpp`):** `scale_section`,
  `camera_section`, `body_section`, `god_section`, `theatre_section`,
  `diorama_section`, `controls_section`.
- **Ini keys:** none of its own; it edits all keys in other features.
- **Depends on:** 1.
- **Status:** tested on headset.

### 3. Smooth camera and view glide

- **What:** Eases the view between first and third person and between
  third-person presets instead of cutting.
- **Commits:** 9f222e7; 11bd8b2 (glue).
- **Glue (`camera_head.cpp`):** `ViewTween` struct and `g_view_tween`,
  `view_tween_elapsed`; `ThirdPersonCamera` struct and `g_tp_camera`;
  `tp_view_load` / `tp_view_save` (persisted third-person view);
  `camera_set_tp_view`, `camera_tp_scale`, `camera_tp_zoom`.
- **Ini keys:** `vr.view_transition_seconds` (default `0.3`, `0` = cut).
- **Depends on:** 1.
- **Status:** tested on headset.

### 4. Picture adjust

- **What:** Brightness and contrast applied as a final pass on the VR image,
  optionally following the game's own gamma ramp.
- **Commits:** 9f222e7; 11bd8b2 (overlay controls).
- **Glue (`vr_submit.cpp`):** `picture_pass` (blend-state helper),
  `draw_picture_adjust` (called after `draw_settings_panel` in the frame).
- **Ini keys:**
  - `vr.picture_brightness` (default `0`, range -0.5..0.5)
  - `vr.picture_contrast` (default `1`, range 0.5..2)
  - `vr.picture_follow_game` (default `1`; use the game's ramp when seen)
- **Depends on:** 1.
- **Status:** tested on headset.

### 5. Third-person chest soft physics

- **What:** Soft secondary motion on Lara's chest in third person (spring
  sway, stretch, ripple).
- **Commits:** 9f222e7 (first version), 4d714b4 (`physics/soft_body`), 11bd8b2 (glue).
- **New files:** `src/vr/physics/soft_body.{cpp,h}`,
  `src/test/soft_body_tests.inc`.
- **Glue (`camera_head.cpp`):** `third_person_soft_frame` (per frame, state
  `g_tp_soft`), `soft_springs`, `SoftSpring` (Legend's own; see gotcha in (a)).
- **Ini keys (defaults):** `vr.body_soft_sway_hz` 1.8 (0.8..4),
  `vr.body_soft_damping` 0.14 (0.03..0.6), `vr.body_soft_stretch` 0.5 (0..1),
  `vr.body_soft_ripple` 0.4 (0..1).
- **Depends on:** 1.
- **Status:** tested on headset.

### 6. God mode free roam and carry

- **What:** Upstream already has the board/god view and its pick-up
  (`camera_board_pluck_update`, `camera_board_flick_update`, `board_god_hand`).
  This fork reworks the carry: head and arm pick-up zones, hang pose, and
  free roam with the board anchored round Lara (see 11). Diff those
  functions against `upstream-base` to see exactly what changed.
- **Commits:** 664f6c3 (module), 11bd8b2 (glue).
- **New files:** `src/vr/godmode/god_carry.{cpp,h}`,
  `src/test/god_carry_tests.inc`.
- **Glue (`camera_head.cpp`):** `carry_apply` and `carry_restore`, called from
  `detour_draw_instance_body` around Lara's draw; the board/god-view code that
  calls them is in the camera board functions (grep `camera_board_`).
- **Ini keys (defaults):**
  - `vr.god_head_zone` 0.3 (0.05..1): pick-up zone round her head, metres at her size
  - `vr.god_claim_zone` 0.6 (0..3): see feature 14
  - `vr.god_arm_zone` 0.25 (0..0.5): a grip this close to an arm holds the arm instead of the head; 0 = no arm grab
  - `vr.god_arm_lead` 1: holding her arm, only the arm follows your hand until you pull past her reach; then she hangs from that wrist. 0 = lifted by the arm at once
  - `vr.god_hang_arm` 0: 1 = Legend's dangle animation hangs her by the wrist; 0 = she hangs from where you pinched her
  - `vr.god_arms_limp` 0: 1 = arms hang limp while held
  - `vr.god_drag_hand` 0 (EXPERIMENTAL): drag her by a hand or arm, hanging by IK; 0 = head grab and dangle
- **Depends on:** 1, 2 (keys are overlay-editable).
- **Status:** built, headset-tested in an earlier iteration. `god_drag_hand`: never headset-tested.

### 7. Throw physics and pick-up

- **What:** Let go mid-swing and she flies off the hand, arcs under gravity
  and lands where she hits the level.
- **Commits:** 664f6c3 (module), 11bd8b2 (glue).
- **New files:** `src/vr/godmode/god_pluck.{cpp,h}`,
  `src/test/god_pluck_tests.inc`.
- **Module functions:** `pluck_zone`, `pluck_near`, `pluck_step`,
  `pluck_land`, `pluck_bounce`.
- **Glue (`camera_head.cpp`):** `god_floor`, `god_put_down`,
  `pluck_read_bones`, `pluck_flight`, `camera_board_pluck_update`
  (called from `vr_input.cpp` each frame).
- **Ini keys (defaults):**
  - `vr.god_throw` 1: 0 = always put down where you let go
  - `vr.god_throw_strength` 1 (0.2..3): hand speed multiplier
  - `vr.god_throw_gravity` 9.81 (1..30): m/s^2 at her size
  - `vr.god_land_anim` -1 (-1..4000): landing animation number; -1 = none
  - `vr.god_drop_height` 1 (0..5): max height above the floor for a set-down drop
- **Depends on:** 6.
- **Status:** built, headset-tested in an earlier iteration.

### 8. Pose and finger grabs

- **What:** Grab with the hand's pose: a pinch (trigger and thumb down) or
  a fist (trigger and grip). Grip alone points and presses.
- **Commits:** 9a636c0 (maths), 11bd8b2 (glue).
- **New files:** `src/vr/finger_input.{cpp,h}` (curl and gesture maths:
  `finger_curls_wanted`, `finger_curls_step`, `finger_gesture`,
  `finger_grabbing`), `src/test/finger_input_tests.inc`.
- **Glue:** `camera_head.cpp`: `apply_finger_tracking`, `pose_fingers`,
  `pose_arm_reach`, `finger_status` (log prefix `fingers:`);
  `vr_input.cpp`: `vr_input_finger_pose`, `pose_fingers` use (per-hand grab
  decision); `vr_submit.cpp` uses `vr_input_finger_pose` for the drawn hand.
- **Ini keys:** `vr.god_grab_pose` (default `1`; needs `controls.finger_tracking`;
  0 = the grip button grabs).
- **Depends on:** 1, 6.
- **Status:** built, headset-tested in an earlier iteration.

### 9. Body push

- **What:** A hand leaning on Lara pushes her away: head, chest, waist,
  arms, hips and legs tilt a few degrees on springs and return.
- **Commits:** 4d714b4 (`physics/body_push`, `physics/body_volumes`), 11bd8b2 (glue).
- **New files:** `src/vr/physics/body_push.{cpp,h}`,
  `src/vr/physics/body_volumes.{cpp,h}`, `src/test/body_push_tests.inc`,
  `src/test/body_volumes_tests.inc`.
- **Glue (`camera_head.cpp`):** `push_map` (per-model bone map),
  `push_step_frame` (once per frame at her first draw), `push_turn`,
  `push_pose`, `push_apply`, `push_restore` (called around the draw).
- **Ini keys (defaults):**
  - `vr.body_push` 1
  - `vr.body_push_strength` 1 (0..3)
  - `vr.body_push_hz` 2.5 (0.5..6)
  - `vr.body_push_damping` 0.9 (0.2..1.5)
  - `vr.body_push_head_degrees` 6, `_chest_degrees` 4, `_waist_degrees` 3, `_arm_degrees` 8, `_hips_degrees` 3, `_leg_degrees` 2 (per-part maxima)
- **Depends on:** 6 (acts only while she is free: not held, thrown or on a palm).
- **Status:** built, headset-tested in an earlier iteration.

### 10. Rear physics (buttocks)

- **What:** Legend has no buttock bone, so the cheeks are CPU edits of
  Lara's skinned source vertices around her draw: each cheek is a mass on a
  spring that lags and settles as she moves, with optional hand dent.
- **Commits:** 4d714b4 (`physics/rear_jiggle`, `rear_shape`, `soft_body`
  maths), 11bd8b2 (glue).
- **New files:** `src/vr/physics/rear_jiggle.{cpp,h}`,
  `src/vr/physics/rear_shape.{cpp,h}`, `src/test/rear_jiggle_tests.inc`,
  `src/test/rear_shape_tests.inc`.
- **Glue:** `camera_head.cpp`: `rear_find_pelvis`, `rear_build`,
  `rear_hands`, `rear_frame`, `rear_soft_apply`, `rear_soft_begin`
  (`RearDrawGuard` wraps the draw). `vr_submit.cpp`: `draw_rear_debug`
  (line overlay when `body_rear_debug` is on).
- **Tuning (this commit):** the rear offset limit is now applied once with a
  smooth ease instead of twice; no new keys.
- **Ini keys:** 24 `vr.body_rear_*` keys in total. Main ones (defaults):
  `body_rear` 1 (on), `body_rear_hz` 4, `body_rear_damping` 0.3,
  `body_rear_gain` 1, `body_rear_max_cm` 3, `body_rear_side` 0.6,
  `body_rear_back` 1, `body_rear_up` 1, `body_rear_limit_cm` 3,
  `body_rear_size` 1, `body_rear_width` 1, `body_rear_height` 1,
  `body_rear_spread_cm` 8, `body_rear_core` 0.5, `body_rear_inward` 0.5,
  `body_rear_dent` 1.
  Debug and tuning keys: default 0 are `body_rear_debug`, `body_rear_test_cm`,
  `body_rear_fullness_cm`, `body_rear_lift_cm`, `body_rear_touch_cm`,
  `body_rear_cleft_cm`. Default non-zero tuning keys are `body_rear_cleft_width_cm` (2)
  and `body_rear_seam_cm` (1).
- **Depends on:** 9 (`body_volumes`).
- **Status:** built, headset-tested in an earlier iteration.

### 11. Board leash, zoom and scale

- **What:** In the board view, Lara can move a set distance from the board's
  centre before the table follows her; the right stick zooms and scales
  her; the anchor turns round her with the game camera heading.
- **Commits:** 664f6c3 (module), 11bd8b2 (glue).
- **New files:** `src/vr/godmode/board_leash.{cpp,h}`,
  `src/test/board_leash_tests.inc`.
- **Module functions:** `board_leash`, `board_stick_axis` (stick value with dead zone).
- **Glue:** `camera_head.cpp` (board anchor update, `board_leash(g_tp_camera.anchor, ...)`);
  `vr_input.cpp` (`board_stick_axis` for zoom and scale).
- **Ini keys:**
  - `vr.board_leash` 0.35 (0..2 table metres)
  - `vr.board_leash_height` 0.1 (0..1)
  - `vr.board_zoom_speed` 1 (0..5; 0 = off)
  - `vr.board_scale_speed` 0.7 (0..5; 0 = off)
- **Scale / zoom input choice (this commit):** a setting picks how scale and
  zoom are changed: the right stick, the palm sliders (the off hand held
  palm-up opens them), or both (default).
  - **Glue:** `camera_head.cpp`: `camera_view_stick` ignores zoom and scale
    when the choice is sliders only; `vr_input.cpp`: `update_tp_sliders`
    never opens the sliders when it is stick only. Overlay combo "Scale / zoom
    control" in the God mode section; `config_set_tp_scale_control`.
  - **Ini key:** `vr.tp_scale_control` `both` (`both`, `stick`, `sliders`)
- **Depends on:** 3 (board view camera), 6.
- **Status:** built, headset-tested in an earlier iteration; the input choice
  is headset-tested.

### 12. Right-stick rework in immersive third person

- **What:** Holding the right stick's click for `view_switch_hold` seconds
  switches the view; a quick click recentres. In the shoulder view, holding
  the stick in and pushing up or down raises or lowers the seat.
- **Commits:** 11bd8b2.
- **Glue:** `vr_input.cpp` (right-stick click handling; `view_switch_hold`);
  `camera_head.cpp` (seat height via `stick_height_speed`).
- **Ini keys:**
  - `vr.view_switch_hold` 0.4 (0.15..2 seconds)
  - `vr.stick_height_speed` 0.4 (0..3 metres a second at full tilt; 0 = off)
- **Depends on:** 3, 11 (`board_stick_axis`).
- **Status:** built, NOT headset-tested.

### 13. VR menu fix and in-VR settings panel

- **What:** Holding the left menu button opens the settings panel in the
  headset; a quick press still pauses the game. The pause menu keeps the
  frozen scene anchored (no drift with residual head motion).
- **Commits:** 9a636c0 (`menu_panel`), 11bd8b2 (glue).
- **New files:** `src/vr/menu_panel.{cpp,h}`, `src/test/menu_panel_tests.inc`.
- **Module functions:** `menu_panel_place`, `menu_panel_corners`,
  `menu_panel_half_size`.
- **Glue:** `vr_submit.cpp`: `draw_settings_panel` (called each frame);
  `vr_session.cpp`: `menu:` log (frozen-scene anchor follows residual HMD
  motion); `vr_input.cpp`: hold-to-open handling near `overlay_vr_open_hold`.
- **Ini keys:**
  - `vr.overlay_vr_distance` 1.0 (0.3..5 m)
  - `vr.overlay_vr_width_degrees` 38 (10..120)
  - `vr.overlay_vr_yaw_degrees` 15 (-90..90; negative = right)
  - `vr.overlay_vr_pitch_degrees` -10 (-60..60; negative = down)
  - `vr.overlay_vr_open_hold` 0.6 (0..3 s; 0 = off)
- **Caveat:** on Index and WMR the panel opens only with Insert, and cannot
  be closed from the controllers.
- **Pause menu fixed in the room (this commit):** the in-game pause menu can
  be pinned level in the room, like the main menu, at the heading the head had
  when it opened (a recentre re-anchors it). The gameplay HUD is told apart
  from menus by the game's menu stack, so it stays head-locked.
  - **Glue:** `ui_space.cpp`: `draw_detour` (the `g_world_locked_hud` choice
    uses `pause_world_locked` for the pause root screen, `menu_world_locked`
    for the rest); `vr_session.cpp`: `vr_substitute_c0_for_eye` (menu anchor
    re-taken when the menu kind changes; pause anchors at the opening yaw
    via `steady_yaw_of`/`set_yaw`).
  - **Known issue:** the controller hands drawn under the pinned pause menu
    swung against the head and sat off their centres, so they are not drawn
    there (`vr_substitute_c0_for_eye`, the `hand >= 0` early return). Cause
    not found: likely the pause head correction applied on top of hands that
    are already posed live.
  - **Ini key:** `vr.pause_world_locked` 1
- **Hide the gameplay HUD (this commit):** skips drawing the head-locked HUD
  outright. World markers, the binocular view and menus still draw.
  - **Glue:** `ui_space.cpp`: `draw_detour` (the `hidden` test).
  - **Ini key:** `vr.hide_gameplay_hud` 0
- **Both toggles** are in the overlay under "Scale, height and distance".
- **Depends on:** 1, 2.
- **Status:** built, headset-tested in an earlier iteration; pause pinning and
  HUD hide headset-tested this commit.

### 14. Hands near Lara withhold grip and trigger from the game

- **What:** A hand within `god_claim_zone` of her body (feet to top of head)
  keeps its grip and trigger for god mode, so the game does not also act on
  them. The claim latches until release.
- **Commits:** 11bd8b2.
- **Glue:** `camera_head.cpp` (claim-zone test near `god_claim_zone`, in the
  board god-hand code); `vr_input.cpp` (per-hand claim state, the
  `God mode's claim on each physical hand` block).
- **Ini keys:** `vr.god_claim_zone` 0.6 (0..3 m at her size).
- **Depends on:** 6.
- **Status:** built, headset-tested in an earlier iteration.

### 15. Removed

- OpenVR (`third_party/openvr`, `dxvk_openvr.*`, commit dd4a740).
- SteamVR input manifests: `input/*.json`, `trlvr_actions.json`, and bindings (commit 8aa78fc).
- Ini keys `vr.mission_prep_backdrop_scale` and `vr.mission_prep_backdrop_lag`
  (present in `upstream-base`'s `config.cpp`, gone in the fork).
- `OPENXR_PORT_HANDOFF.md`.

## Unattributed keys

None. Every key added to `src/common/config.cpp` since `upstream-base` maps to a
feature above. `vr.body_soft` and `vr.body_soft_angle` are upstream keys (present in
`upstream-base`) that the fork's soft-physics code reads; they are not fork additions.
