# Tomb Raider: Legend VR

A virtual reality mod for **Tomb Raider: Legend** (PC, Steam or GOG), adding
stereoscopic rendering, 6DoF head tracking and motion controller support, a
VR third-person camera, and an optional first-person mode where you climb,
shoot and swing with your own hands.

It runs as a Direct3D 9 proxy (`d3d9.dll`) beside a small fork of DXVK, with a
launcher that makes the game large-address-aware.

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

> [!NOTE]
> **This mod was made with heavy AI (Claude) assistance.** I want to be upfront
> about that.

> [!WARNING]
> **This is a preview.** It is playable from start to finish in third person
> and most of the way in first person, but expect rough edges. Bug reports with
> logs are very welcome; see [Troubleshooting](#troubleshooting).

> [!IMPORTANT]
> **Windows may block this mod from running.**
>
> **Smart App Control** (Windows 11) refuses unsigned code outright. This mod is
> unsigned, so on a machine where SAC is enforced the game will start and the VR
> mod will not load. SAC has no exclusion list; the only option is to turn it
> off: `Windows Security → App & browser control → Smart App Control → Off`.
>
> Ordinary antivirus may also object. What this mod does, replacing a system
> DLL name and patching a running game's code, looks a lot like what malware
> does. An exclusion for the game folder fixes that.

---

## About the game

**Tomb Raider: Legend** (Crystal Dynamics, 2006) rebooted the series with a
new engine, fluid traversal and a grapple. It has no VR support of any kind:
one camera, a flat HUD and a renderer built for a 4:3 or 16:9 monitor.

It turned out to be a good fit anyway. Its traversal (ledges, poles, vines,
swinging bars, grapple swings) maps naturally onto your hands, and the
third-person camera works as a comfortable way to watch Lara from just behind
her shoulder.

---

## Requirements

- **Tomb Raider: Legend** for PC: the Steam or the GOG release
- A SteamVR-compatible headset with two tracked motion controllers
- **SteamVR** installed and running
- A GPU with Vulkan support (DXVK translates the game's Direct3D 9 to Vulkan)
- Windows

Developed and tested on an **HP Reverb G2**. Bindings ship for Oculus Touch,
Valve Index, WMR and Reverb G2 controllers. Nothing is specific to one headset:
field of view, IPD and render resolution are all read from the runtime at
start-up.

---

## Features

### Rendering

- **True stereo, both eyes every frame.** Each draw is issued once per eye into
  a side-by-side target. No alternate-eye rendering or reprojection tricks.
- **Per-eye asymmetric projection** from the runtime, so geometry is correct
  rather than approximated.
- **Automatic render resolution.** The game renders at SteamVR's recommended
  size, so SteamVR's per-game resolution slider is the control.
- **VR-correct culling.** The game's object and terrain culling is rebuilt
  around the wide headset view, so nothing pops out at the edge of your vision
  and nothing behind you is drawn for nothing.
- **Level horizon.** The game camera's pitch and roll stay out of your view;
  only your head moves it.
- **A stable compositor hand-off.** The image given to SteamVR is pinned in
  video memory (the lesson from the SiN VR mod's NVIDIA crash).


### Third person (default)

- **Three camera styles:** `classic` (the game's camera, smoothed, no
  auto-centring or shake), `shoulder` (close behind Lara at life size) and
  `board` (Lara a few inches tall on a tabletop world).
- **Camera collision** keeps the view out of rock and walls.
- **A 3D gear cross** of the game's own item icons floats in front of you with
  immersive controls; grip an icon to use it.


### First person (optional)

- **Your own hands**: Lara's hands follow the controllers, with an optional
  full body (`first_person_body = full`) that bends her arms to reach them.
- **Holsters**: reach to a hip and grip to draw the pistols; grip again to put
  them away. Grip over a shoulder for the long gun.
- **Belt and pouches**: grab the grapple and fling it, lift the binoculars to
  your face, take a grenade or flare from the belt, and bring a medipack to
  your mouth.
- **Two-handed long guns**: the off hand on the foregrip steadies the aim.
- **Accurate shooting**: shots go where the gun points (no random spread in
  first person), with a VR crosshair at the impact point, recoil and haptics.
  Lock-on targeting follows your head and is widened for VR.
- **Hand-over-hand traversal**: grip ledges, vines, chains and swing bars and
  pull yourself along them; alternate hands quickly to shimmy fast; pull down
  with both hands to climb onto a ledge. A one-hand catch is secured by
  grabbing the hold (or automatically, with `auto_secure_catch = bars` or `all`).
- **Jumps go where you look** off chains and poles; on vines the stick works
  relative to your view.
- **Physical crouch**: crouch for real to crouch.
- **Holster setup in game**: Pause menu → *VR Settings* → *VR Holster Setup*
  lets you place the holsters, belt items and pouches where they suit your body.
- **Hand calibration in game**: Pause menu → *VR Settings* → *Hand Calibration*
  holds Lara's hands out in front of you; move each controller into its hand
  and squeeze the grip to lock it. Then aim your pistol at a target and pull
  the trigger to set your aim. A skips a step. Fixes hands and aim that do not
  line up with your controllers on any headset.
- **Left-handed mode**, akimbo or single pistols, and three aiming styles.

### Interface and comfort

- **HUD on its own layer**, drawn at a comfortable size with a gentle head
  follow; menus are world-locked panels you can look around.
- **World markers** (caution icons, grapple targets, lock-on) are placed at
  their real position in the world, not on a flat screen.
- **Prompts speak VR**: tutorial and context prompts name the VR controller
  button instead of the keyboard key, including for left-handed play.
- **Smooth or snap turning**, an optional comfort vignette, and movies and
  loading screens shown on a world-locked screen.

---

## Installation

### 1. Prepare the game

Run Tomb Raider: Legend once normally, reach the main menu, and quit. In the
game's **Setup** dialog turn **off**:

| Setting | Why |
|---|---|
| Next Generation Content | Its separate renderer gives a black scene in VR |
| Fullscreen Effects | Flat-screen post effects smear the two eyes into each other |

Depth of Field is switched off by the mod itself (`depth_of_field`).

### 2. Copy the files

Copy everything inside `Main - Copy these into the base game folder` into the
folder that contains `trl.exe`. Steam's default:

```
C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider Legend\
```

| File | What it is |
|---|---|
| `d3d9.dll` | The mod (a Direct3D 9 proxy the game loads) |
| `d3d9_dxvk.dll` | The DXVK fork that renders through Vulkan |
| `openvr_api.dll` | OpenVR, 32-bit |
| `trlvr_launcher.exe` | The launcher (see below) |
| `trlvr_actions.json`, `bindings_*.json` | The SteamVR action manifest and controller bindings |

> [!IMPORTANT]
> **GOG release:** GOG ships its own `D3D9.dll` (a compatibility wrapper) in the
> game folder. Rename it first, for example to `D3D9.dll.gog-dxwrapper`, then copy
> the mod in. Any other `d3d9.dll` belongs to another graphics mod and must be
> moved aside too.

### 3. Start through the launcher

**Steam:** right-click the game → Properties → Launch Options:

```
"C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider Legend\trlvr_launcher.exe" %command%
```

Adjust the path if your library is elsewhere, then launch from Steam as usual.

**GOG:** start SteamVR, then run `trlvr_launcher.exe` from the game folder.

#### Why a launcher?

`trl.exe` is a 32-bit program without the `LARGE_ADDRESS_AWARE` flag, so it is
limited to 2 GB of memory: not enough for headset-resolution, two-eye render
targets. The launcher writes a patched **copy**, `trl_vr.exe`, with the flag set
and starts that. The original `trl.exe` is never modified.

---

## Controls

A fresh install uses the **classic layout**: the VR controllers send the game's
own keyboard and mouse controls, so everything the game can do has a button.
Defaults for Oculus Touch-style controllers (Reverb G2, Quest, Rift):

| Control | Action |
|---|---|
| Left stick | Move |
| Left stick click / double click / hold | Pause / recentre the view / health pack |
| Right stick | Turn |
| Right stick click / double click / hold | Switch weapons / PDA / binoculars |
| Left trigger | Action, interact, pull the grapple |
| Right trigger | Fire; select in menus |
| Left grip | Throw the grapple |
| Right grip | Combat lock-on |
| A | Jump, surface; select in menus |
| B | Dive, crouch, roll; back in menus |
| X | Accurate aim |
| Y tap / hold | Grenade or flare / personal light |

Index, WMR and per-controller details are in
[`release/VR CONTROLLER BINDINGS.txt`](release/VR%20CONTROLLER%20BINDINGS.txt).
The keyboard keeps working alongside the controllers, and **F1** recentres.

### Immersive controls

With `immersive_controls = 1` the grips become your hands. In first person:
holsters at the hips and over the shoulders, gear on the belt, pouches on the
chest, the light on a left-hand grip at the left chest, and climbing with your
hands. The action trigger aims with a gun out and acts otherwise. X, the long
press on Y and the long press on the right stick give way to gestures.

### Developer keys

Mostly for diagnosing problems. Each writes what it did to `trlvr.log`.

| Key | Effect |
|---|---|
| F1 | Recentre |
| F2 / Shift+F2 | Object / terrain culling: headset frustum ↔ open (for comparison) |
| F3 | Time the next frame on the GPU (`gpu profile` lines) |
| F4 | Log every draw of the next frame |
| F5 | Reload hand offsets from `trlvr.ini` |
| F9 | Capture the interface textures |
| `[` / `]` | Nearer / farther convergence of first-person world markers |

---

## Configuration

Settings live in **`trlvr.ini`** beside the game, written on first run. Every
key is explained in the file itself. Missing settings are added on start-up;
values you chose are never replaced. Edit the file with the game closed.

**Third person with the classic layout is the default**, because it is the
safest place to start. To try first person:

```ini
[vr]
first_person = 1
first_person_tracked_hands = 1

[controls]
immersive_controls = 1
```

`first_person` only sets the view you start in. With immersive controls on,
**long-press the right stick** (or press `\` on the keyboard) to switch between
first and third person at any time in gameplay.

### The settings most worth knowing

| Setting | Default | What it does |
|---|---|---|
| `first_person` | `0` | First-person view through Lara's eyes |
| `first_person_tracked_hands` | `0` | Lara's hands follow your controllers (first person) |
| `immersive_controls` | `0` | Hand gestures: holsters, belt gear, pouches, hand climbing, gear cross |
| `first_person_body` | `hands` | `full` draws Lara's whole body below the view |
| `third_person_mode` | `classic` | `classic`, `shoulder` or `board`; `all` makes the right-stick long press cycle classic → shoulder → board → first person |
| `left_handed` | `0` | Main hand on the left; swaps the triggers' roles |
| `pistol_mode` | `akimbo` | `single`: each hip draws its own pistol, fired by that hand |
| `aim_mode` | `hand` | `hands` aims between both pistols; `hands_head` mixes in the headset |
| `two_handed_mode` | `auto` | Long-gun foregrip: `auto` (bring the hand close), `toggle`, `hold` |
| `turn_mode` | `smooth` | `snap` turns by `snap_turn_degrees` (30) |
| `comfort_vignette` | `off` | `jumps` or `full` darkens the edge of view while moving |
| `physical_crouch` | `1` | Crouch for real to crouch |
| `auto_secure_catch` | `off` | One-hand catches secure themselves: `bars` (swing bars only) or `all` (ledges and bars) |
| `catch_extra_time` | `4.5` | Extra seconds to secure a one-hand catch by hand (first person, about 6 s in all; `0` = game timing) |
| `first_person_pullup_anim` | `266` | Ledge pull-up animation used in first person (`-1` = the game's choice); replaces those in `first_person_pullup_replace` (`236`, the slow one) |
| `first_person_no_spread` | `1` | Shots go exactly where the gun points |
| `camera_collision` | `1` | Third-person view stops short of walls |
| `ui_scale` | `0.6` | Size of the HUD and menus |
| `hud_follow` | `0.85` | How closely the HUD follows your head (1 = locked) |
| `msaa` | `4` | Anti-aliasing cap when FSAA is on: `0`, `2`, `4` or `8` |
| `first_person_object_culling` | `frustum` | `open` draws every object in a visible room, at a large CPU cost |
| `terrain_culling` | `frustum` | `open` draws every terrain strip of a visible room |
| `depth_of_field` | `off` | `on` or `game` (keep the launcher's setting) |
| `world_scale` | `291` | Game units per metre. Raise to feel smaller, lower to feel larger |

### Performance

The game is old but the work is doubled for two eyes, and some of it runs on
the CPU. In rough order of effect:

1. **SteamVR's per-game resolution** (Settings → Video). The mod reads it at
   start-up, so restart the game after changing it.
2. **Keep the culling settings on `frustum`** (the default). `open` costs a lot
   of CPU in busy rooms.
3. **`msaa`**: `2` or `0` frees GPU time.
4. Turning off Water Effects, Reflections and Shadows in the Setup dialog.
5. If 90 fps is out of reach, a **fixed half-rate with Motion Smoothing** in
   SteamVR usually feels smoother than a frame rate that keeps jumping.

`trlvr.log` gets a `perf:` line every 5 seconds: frame rate, GPU and CPU time,
and the costliest objects. It says why an area is slow.

---

## Building from source

### Prerequisites

- **Visual Studio 2022** (or Build Tools) with the C++ x86 toolset; the scripts
  find it with `vswhere`.
- The **Vulkan SDK** installed under `C:\VulkanSDK` (headers and
  `glslangValidator`).
- **Python 3** with **meson** and **ninja**: `py -3 -m pip install meson ninja`.
- OpenVR is included in [`third_party/openvr`](third_party/openvr).

Everything is built **32-bit**: `trl.exe` is a 32-bit process.

### Build

```bat
build_dxvk.bat      :: the DXVK fork  -> dist\d3d9_dxvk.dll  (slow the first time)
build.bat           :: the mod and launcher -> dist\d3d9.dll, dist\trlvr_launcher.exe
mathtest.bat        :: tests for the VR maths
test.bat            :: loads the proxy in a test host
```

`build_dxvk.bat clean` reconfigures the DXVK build from scratch.

### Install or package

```bat
install.bat dxvk ["<game folder>"]   :: copies the build into the game folder (Steam default if omitted)
package.bat                          :: the drag-and-drop release in %USERPROFILE%\Desktop\TRLVRmod
```

The DXVK fork (`dxvk-trlvr/`) adds `Direct3DCreateVR9` (`src/d3d9/d3d9_vr.*`),
which hands the Vulkan image behind a Direct3D 9 surface to OpenVR, and a DXVK
profile for `trl_vr.exe`. The shipped `d3d9_dxvk.dll` is a **modified** DXVK
build; report problems with it here, not upstream.

### Layout

| Path | Contents |
|---|---|
| `src/proxy` | D3D9 and swap-chain proxy, MSAA cap, GPU profiler |
| `src/vr` | VR session and submit, cameras and first person, culling, input and gestures, UI, water |
| `src/common` | `trlvr.ini` settings and the log |
| `src/launcher` | `trlvr_launcher.exe` |
| `src/test` | Maths and proxy tests |
| `input` | SteamVR action manifest and controller bindings |
| `release` | Player documentation shipped in the package |
| `dxvk-trlvr` | DXVK fork |
| `third_party/openvr` | OpenVR header and runtime |

---

## Known limitations

- **Next Generation Content is not supported.** Its renderer gives a black
  scene. Keep it off.
- **Water looks flatter than on a monitor.** The game's water reflection and
  full-screen water effect only exist for one eye, so they are skipped.
- **Changing anti-aliasing in the in-game options crashes the game.** Change it
  in the Setup dialog before starting.
- **Cinematics play in third person** on a world-locked view.
- **Some wet-rock decals flicker by view angle.** The flat game does the same.
- **Performance dips in a few busy scenes**, for example animal attacks. The
  `perf:` lines in the log help pin these down.
- **No hand collision yet.** In first person your hands pass through walls and
  through Lara's body.

---

## Uninstalling

Clear the Steam launch option, then delete from the game folder:

```
d3d9.dll   d3d9_dxvk.dll   openvr_api.dll   trlvr_launcher.exe
trlvr_actions.json   bindings_*.json
trlvr.ini   trlvr.log*   trlvr_crash.dmp*   trl_vr.exe
trlvr_launcher.log   trl_vr_d3d9.log
```

On GOG, rename `D3D9.dll.gog-dxwrapper` back to `D3D9.dll`. The game's own
files were never changed. The game saves its settings when it quits, so
Depth of Field may show as off in the Setup dialog afterwards; turn it back on
there if you want it.

---

## Troubleshooting

**The game runs on the monitor but nothing appears in the headset.**
Check `trlvr.log` beside the game. It records the headset, every hook and the
controllers, and says plainly when something fails. Make sure the game was
started through `trlvr_launcher.exe` and that SteamVR was running.

**The view is offset or facing the wrong way.**
Double-click the left stick (or press F1) to recentre.

**Controller buttons do nothing, or the wrong thing.**
SteamVR may have kept an older or custom binding. Open SteamVR → Settings →
Controllers → Manage Controller Bindings and select the supplied Tomb Raider:
Legend VR binding.

**Performance is poor.**
See [Performance](#performance). The `perf:` lines in `trlvr.log` show whether
it is the GPU or the CPU and which objects cost the most.

**The game crashes.**
Send the files beside the game: `trlvr.log` and `trlvr_crash.dmp`, plus
`trl_vr_d3d9.log`. If you have started the game again since, the previous run
is kept as `trlvr.log.prev` and `trlvr_crash.dmp.prev`. The crash report names
the module and the SteamVR or DXVK call in progress, and the dump holds the
rest.

---

## Licence

This project's own code (`src/`, `input/`, the scripts and the documentation)
is released under the **Apache License 2.0**. See [LICENSE](LICENSE) and
[NOTICE](NOTICE).

Not covered by it, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):

- **DXVK** (zlib/libpng). The release's `d3d9_dxvk.dll` is a **modified** build.
- **OpenVR SDK** (BSD 3-Clause, © Valve). `openvr_api.dll` is redistributed
  unmodified.
- **TRAWindowed** (MIT, © chreden), which the D3D9 proxy started from.

Tomb Raider: Legend is © its owners. This mod is unofficial, not affiliated
with or endorsed by Crystal Dynamics, Eidos or their successors, contains no
game data, and needs a legitimate copy of the game.

---

> [!IMPORTANT]
> ## Thanks
>
> This mod exists because other people solved harder problems first and
> published their work.
>
> - **Philip Rebohle** and the **DXVK** contributors. DXVK's Direct3D 9 to
>   Vulkan translation is what makes a 2006 D3D9 game reachable by a modern VR
>   compositor at all.
> - **chreden**, for **TRAWindowed**, the D3D9 wrapper this mod's proxy grew from.
> - **Valve**, for **OpenVR** and SteamVR.
> - **Crystal Dynamics**, for making Tomb Raider: Legend.

Any mistakes in this mod are my own, not theirs.


## In Loving Memory
Sophie 2010-2026

<img width="1024" height="768" alt="image0" src="https://github.com/user-attachments/assets/b4979262-379b-4c29-a9ed-5284aac9dfd9" />
