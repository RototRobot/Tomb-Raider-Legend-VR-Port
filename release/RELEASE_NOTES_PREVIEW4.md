# Tomb Raider: Legend VR — Preview 4

A preview build for testers. Third person is playable start to finish; first person most of the way. Bug reports with logs are very welcome (see **Reporting problems** below).

**Download:** `TRLVRmod-Preview4.zip`. Unzip it, copy everything inside `Main - Copy these into the base game folder` into the folder containing `trl.exe`, and follow `READ ME FIRST.txt`.

> [!IMPORTANT]
> **Upgrading from Preview 3 or earlier?** Run **Pause menu → VR Settings → VR Holster Setup** once. Holster positions are now measured from Lara's palm, not the controller, so older placements will sit slightly off.

---

## What's new

### VR Settings in the pause menu
During a level, the game's *Display* menu becomes **VR Settings**. Each entry changes a VR option on the spot and saves it to `trlvr.ini`. Press an entry to step through its choices:

| Entry | Choices |
|---|---|
| Theatre | Off · Cutscenes · Always |
| Third Person | Classic · Shoulder · Board · All |
| Hand Collision | Off · On · On + Body |
| Body | Hands · Full |
| Left Handed | Off · On |
| Hardcore Mode | Off · On |

*VR Holster Setup* and *Hand Calibration* are still there. Everything is translated into the game's five languages (English, French, German, Italian, Spanish).

### Comfort and camera
- **Theatre mode:** cutscenes, and optionally third-person play, are shown on a large window fixed in the room, still in 3D, instead of moving your view.
- **Third-person scale and zoom:** turn your off hand palm up to bring out two sliders that resize the world and move the camera in or out. They're saved per camera style.
- **Board diorama** (`board_diorama = 1`): the tabletop world sits in an open box you can grab and move.
- **Hardcore mode** (off by default): in first person, the view turns with Lara through flips, rolls and spins.

### First person
- **The pouches show the game's own medipack and grenade models** while you carry any. If you have none, you see an outline instead.
- **Roomscale:** walk around your room and Lara follows, stopping at walls. A real step plays her walk animation.
- **Finger tracking** (`finger_tracking = 1`): on Quest and Index controllers, Lara's fingers follow yours.
- **Full body:**
  - the torso gives a little as she moves;
  - with Hand Collision set to On + Body, your hands stop at her body instead of passing through.
- **The flashlight's glow and the grapple's rope** now come from the gear in your hands.

### Third person
- **Wall touch:** walking beside a wall with nothing drawn, Lara reaches out and runs her hand along it. She keeps it there for a moment when she stops.
- **Radial gear menu:** with the classic layout, hold the left stick and push it towards an item:
  - up: health pack;
  - right: binoculars;
  - down: switch weapon;
  - left: personal light.

  With immersive controls, set `gear_menu = radial` to use it instead of the 3D gear cross.

### Controllers
- **Quest (Oculus Touch) controllers get their own tuned defaults** for hand alignment, aim and holsters. Other controllers keep the Reverb G2 defaults. The right set is picked automatically.
- **The game's rumble** (explosions, screen shakes) now reaches both VR controllers (`shake_rumble`).

### Graphics
- **The distance is sharper:**
  - the mod now actually switches off Depth of Field and Fullscreen Effects at every start (in earlier previews this setting could fail to take effect);
  - it also switches on the game's anti-aliasing, with its strength capped by `msaa`.
- **New `trlvr.ini` settings:**
  - `fullscreen_effects = off`;
  - `fsaa = on`.

  Set either one, or `depth_of_field`, to `game` to keep what the Setup dialog says.

---

## Fixes
- **Sky:** the sky no longer slides when you push the right stick up or down, or enter precision aim. This affected levels whose sky sits close to the camera, such as the second level.
- **Vine climbing:**
  - hand-over-hand pulls on vines now climb reliably;
  - each grip climbs in one direction only, set by your first pull. Let go and grip again to change direction.
- **The pause menu** no longer drops the frame rate.
- **Tutorial and context prompts** show the right VR button name in every language.
- Many smaller fixes.

---

## Known limitations
- **Next Generation Content** is not supported (black scene). Keep it off in the Setup dialog.
- **Water looks flatter than on a monitor:** the game's water reflection and full-screen water effect only exist for one eye, so they are skipped.
- **Graphics options can only be changed in the Setup dialog** or in `trlvr.ini`. During a level, the in-game Display menu is the VR Settings menu instead.
- **Cinematics play in third person.** Theatre mode shows them on a screen in the room instead.
- **Windows Smart App Control** blocks unsigned mods like this one. See the README.

---

## Reporting problems
Describe what happened and roughly when, and attach these files from the game folder (whichever exist):

- `trlvr.log` and `trlvr.log.prev`
- `trlvr_crash.dmp` (written if the game crashes)
- `trl_vr_d3d9.log`

A screenshot from the SteamVR mirror helps with anything visual.

---

## Package
| File | SHA-256 |
|---|---|
| `TRLVRmod-Preview4.zip` | `0944581A4AADA31350AF01015F7B82F4F40F6B7ABFE55FA8D2B5AB11FC7258A1` |
| `d3d9.dll` | `D04A4EDA1FE7D1CE3F0E7261CD347705058ED1D7093114728A74C2DB211315BE` |

The package now includes a `Licences` folder with the mod's licence (Apache 2.0) and the licences of the bundled DXVK fork and OpenVR runtime.

*This mod was made with heavy AI (Claude) assistance. It is unofficial and not affiliated with Crystal Dynamics or the Tomb Raider rights holders. A legitimate copy of the game is required.*
