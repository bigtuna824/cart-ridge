# Cart Ridge

An FPS for the 3DS where you reload by physically pulling the Game Card out
of the cart slot and putting a fresh one back in.

This works because homebrew never runs *from* the cart slot — it runs from
SD card (via the Homebrew Launcher) or from an installed CIA. That leaves
the physical Game Card slot free during play, so the game can watch it with
`FSUSER_CardSlotIsInserted()` and treat an eject/insert as the reload event.
You don't need a special "ammo cartridge" for this — any spare Game Card
works, since the game only checks whether *something* is in the slot, not
what it is.

**Important limitation:** this can only be tested on real hardware. Citra
does not emulate the cart slot, so `FSUSER_CardSlotIsInserted` will not
behave meaningfully there — you'll need your 3DS and a spare Game Card
sitting next to it for every test of the reload mechanic. Everything else
(movement, rendering, shooting) can be checked in Citra first.

## Current state (arcade wave-survival)

- A small raycast (Wolfenstein-3D style) engine rendering a 10x10 test map
  to the top screen, with texture-mapped walls (`gfx/wall.png`), a gun
  viewmodel with idle/firing/empty states, and muzzle flash.
- Menu -> Playing -> Game Over flow. At the menu, `SELECT` cycles the
  immersion level and `R` starts a run, **locking that level until you
  die**: BASE (any cart reloads), MEDIUM (must swap to a different cart,
  identified by Title ID), FULL (each distinct cart is its own persistent
  magazine). `START` quits from any screen.
- Wave-survival loop: enemies (placeholder red squares -- no enemy art yet)
  spawn in escalating waves and walk straight at the player, dealing damage
  and dying on contact if they're not shot first. `R` fires a forgiving
  hitscan cone at the nearest enemy roughly in front of you, blocked by
  walls like everything else. Health hits 0 -> Game Over, showing the wave
  reached and kill count.
- Circle Pad to move/strafe; look/turn via C-Stick (New3DS) or `Y`/`A`
  (works on any 3DS).
- Reload logic: pull the Game Card at any time and the gun goes dead
  ("NO CARTRIDGE"); reinsert to reload, per whichever immersion level is
  locked in for the run.
- Lower screen shows a cartridge-status icon, ammo, gun status, and
  wave/HP/kills.

Only a single tiny 10x10 test map, and enemies are unstyled colored squares
-- this proves the wave-survival loop feels good, but still needs real
level design and enemy art.

## Setup (do this once)

1. **Install devkitPro.** Download the Windows installer from
   https://devkitpro.org/wiki/Getting_Started and run it. When prompted for
   components, make sure **3DS development** is checked (this pulls in
   devkitARM, libctru, citro2d, and citro3d). This step needs your own
   clicks/admin approval, so I'm not attempting to automate it.

2. After install, open a **"devkitPro MSYS2"** shell (installed as a Start
   Menu shortcut) and confirm the toolchain is on PATH:

   ```bash
   arm-none-eabi-gcc --version
   ```

3. From that same MSYS2 shell (or any shell with `DEVKITPRO`/`DEVKITARM`
   set — the installer sets these as system environment variables), `cd`
   into this project folder and build:

   ```bash
   make
   ```

   This produces `cart-ridge.3dsx`.

4. **Test movement/rendering in Citra** (fast iteration, no cart-slot
   behavior): open `cart-ridge.3dsx` in Citra.

5. **Test the reload mechanic on real hardware:** copy `cart-ridge.3dsx` to
   `/3ds/` on your SD card, boot the Homebrew Launcher, launch it, and try
   pulling/reinserting a spare Game Card mid-game.

## Roadmap

1. ~~Raycaster + movement + shooting + cart-slot reload~~ (this POC)
2. ~~Wall textures instead of flat shaded columns~~
3. ~~Enemies / targets~~ -- waves, health, hitscan, menu/game-over flow are
   in; still needs real enemy art (currently flat red squares) and a bigger
   level than the one tiny 10x10 test map
4. Sound (gunshot, the actual click of ejecting a cart is a fun cue to add
   via a custom sound effect)
5. A custom icon (currently building with libctru's generic default icon via
   the SMDH auto-build in the Makefile) and a title screen
6. ~~Decide campaign vs. arcade~~ -- arcade/wave-survival, confirmed
7. Stereoscopic 3D (the physical 3D slider) -- render each eye to a separate
   `C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT/GFX_RIGHT)` target with a small
   camera offset driven by `osGet3DSliderState()`, and call `gfxSet3D(true)`.
   No new art needed, just a second render pass per frame.
8. Local multiplayer -- likely built on libctru's `uds` service (local
   wireless, no internet needed, same mechanism as Download Play). Haven't
   scoped this out yet; will need real research once we get here, since it
   touches session hosting/joining and probably changes how waves/enemies
   are shared between players.
9. Gun skin changes based on which 3DS model the player is running on --
   `CFGU_GetSystemModel()` (needs `cfguInit()`) returns old 3DS/3DS XL vs.
   New 3DS/XL vs. 2DS family. Needs a distinct gun sprite set per model
   (art work), swapped in at startup based on the detected model.

## Project layout

```
source/     C source files (main.c is the whole game right now)
include/    headers (empty for now)
data/       binary data compiled into the app (currently unused)
romfs/      assets shipped alongside the app, read at runtime (currently unused)
Makefile    devkitPro build rules
```
