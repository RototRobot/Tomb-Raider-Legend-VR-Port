# Tomb Raider: Legend VR — Preview 5

A preview build for testers. These notes cover everything since Preview 4, including the Preview 4 hotfix. Bug reports with logs are very welcome (see **Reporting problems** below).

**Download:** `TRLVRmod-Preview5.zip`. Unzip it, copy everything inside `Main - Copy these into the base game folder` into the folder containing `trl.exe`, and follow `READ ME FIRST.txt`.

> [!IMPORTANT]
> **New defaults.** A fresh `trlvr.ini` now starts with:
> - immersive controls on;
> - tracked hands on in first person;
> - the radial gear menu;
> - the right-stick long press stepping through classic, shoulder and board third person, then first person.
>
> An existing `trlvr.ini` keeps your settings. To get the new defaults, rename it and start the game once.

---

## What's new

### Controls
- **Left stick: click opens the radial gear menu, hold pauses** (swapped from before). Push the stick towards an item to pick it:
  - up: health pack;
  - right: binoculars;
  - down: switch weapon;
  - left: personal light.

  This works in first person too. In menus, a click still goes back.
- **Guns come first.** Reaching for a holster with gear in your hand puts the gear away and draws the gun.
- **Gear is held, not toggled.** Binoculars, grapple, grenades and medipacks stay in your hand while you hold the grip. Let go and they go back on the belt or in their pouch, unless you threw them.
- **Binocular view** only takes zoom (left stick) and the triggers. Press either grip, anywhere, to leave it.
- **Immersive first person** no longer uses the grenade button (Y) or the weapon-switch click: the holsters and pouches do those.
- **Fingertip reach:** the gear cross, the scale and zoom sliders and the theatre and diorama handles answer to your index fingertip as well as your palm.
- **`smooth_turn_speed`** sets how fast smooth turning is, in degrees per second (default 120).
- **The long-gun shoulder zone** sits higher, so reaching for the medipack or the chest light no longer draws the rifle.

### Motorbike (first person)
- **Grip a handlebar to hold it**, then grip again to let go. Lara's hand stays on the bar. `bike_handlebars` sets what holding does:

  | Value | What the handlebars do |
  |---|---|
  | `1` (default) | Hold for show; the stick rides the bike |
  | `2` | Your hands steer: push the left hand forward to turn right, the right hand forward to turn left. The stick is the throttle |
  | `3` | As 2, plus a twist throttle on whichever hand is on a bar: rotate back to speed up, forward to slow and brake |

  `bike_handlebar_markers = 1` shows where to grip.
- **Shooting on the bike:**
  - your pistol is in your main hand, so the other hand can stay on the bar;
  - the muzzle flash comes from the gun you are holding;
  - the long gun no longer disappears.

### Guns
- **Pistols always sit in Lara's grip**, including on the bike and after loading any checkpoint. No first draw is needed to "learn" the grip any more.
- **The shotgun is lined up out of the box:** the off hand sits on the pump and the barrel follows the crosshair.
- **Left-handed play mirrors every gun's tuning automatically:** foregrip, aim and the gun's angle in the hand.
- **Built-in foregrip defaults** for every long gun, including ones not tuned yet.
- **For tuners:** `gun_tuning_debug = 1` lets you adjust a long gun's angle in the hand with the numpad: 8/2 pitch, 4/6 yaw, 9/3 roll, 5 resets. Foregrip and aim values in `trlvr.ini` apply live, without restarting.

### Theatre window and board diorama
- **Resize handles at both right-hand corners.** Drag one to set the width and height separately, for any aspect ratio. On the diorama, the top handle moves the top of the box and the bottom handle moves the bottom. The new size is saved when you let go.
- **Handles fade in only as a hand comes near.**
- **New setting:** `cutscene_screen_height` (0 keeps the 16:9 shape).

### First person without immersive controls
- **Lara's own animated hands, or her full body, and her guns** are drawn.
- **A gamepad's right stick turns the view.**

### Other
- **`first_person_eye_height`** can now go lower, if your view sits too high on her body. It's about 3 units per cm.
- **The board-mode god hand** (pick-up, palm, flick) only works at board scale or smaller, not in shoulder mode.

---

## Fixes
- A warped copy of the crosshair, outlines and handles appeared in the other eye when they were close to your face.
- Diorama: hands vanished in the dark before reaching the handle, and a faded handle showed as a black bar.

---

## Known limitations
- **Next Generation Content** is not supported (black scene). Keep it off in the Setup dialog.
- **Water looks flatter than on a monitor:** the game's water reflection and full-screen water effect are skipped.
- **Graphics options can only be changed in the Setup dialog** or in `trlvr.ini`.
- **Cinematics play in third person.** Theatre mode shows them on a screen in the room instead.
- **In first person without immersive controls, a mouse does not turn the view** (a gamepad does).
- **Windows Smart App Control** blocks unsigned mods like this one. See the README.

---

## Reporting problems
Describe what happened and roughly when, and attach these files from the game folder (whichever exist):

- `trlvr.log` and `trlvr.log.prev`
- `trlvr_crash.dmp` (written if the game crashes)
- `trl_vr_d3d9.log`

A screenshot from the SteamVR mirror helps with anything visual.

## Package
| File | SHA-256 |
|---|---|
| `TRLVRmod-Preview5.zip` | `DCB3603DED3ED222A01DA55BBD48EDE2B888B9E5F5409274F019DBF740379F9F` |
| `d3d9.dll` | `CDAE5465161074836B1298C42662881946F850429387014CF4F5E918E1AF538F` |

---

*This mod was made with heavy AI (Claude) assistance. It is unofficial and not affiliated with Crystal Dynamics or the Tomb Raider rights holders. A legitimate copy of the game is required.*
