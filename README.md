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
- Wave-survival loop: enemies (`gfx/enemy.png`, flipped horizontally every
  ~0.3s for a cheap walk animation, mirrored about its own center rather
  than an edge) spawn in escalating waves and walk straight at the player,
  dealing damage and dying on contact if they're not shot first. Every
  wave, walls get tinted a fresh random color, blended in purely by
  distance regardless of which way the wall faces. `R` fires a forgiving
  hitscan cone at the nearest enemy roughly in front of you, blocked by
  walls like everything else; kills spawn a brief enlarged muzzle-flash
  "poof" at the death location. Health hits 0 -> Game Over, showing the
  wave reached and kill count.
- Scoring: 300 pts per kill, 1000 pts for clearing a wave, and a 500 pt
  bonus for clearing a wave without taking any damage -- all multiplied by
  the current wave number, and then by the locked-in immersion level (BASE
  1x, MEDIUM 2x, FULL 3x), since the stricter levels are harder to play
  under.
- Minimap on the bottom screen (to the right of the existing HUD text): a
  full radar, not line-of-sight-limited -- enemies (red) and, in
  multiplayer, other players (their own colors) always show their real
  position regardless of walls in between, since the point is to always
  know where the threats are. Your own position and facing direction show
  as a green dot with a short line.
- Real title screen (`gfx/title.png`) instead of placeholder text.
- Sound via NDSP: gunshot on fire, distinct cues for pulling
  (`audio/reload1.wav`) vs. inserting (`audio/reload2.wav`) a cartridge,
  and a looping background music track (`audio/music.wav`).
- Local multiplayer: 4-player co-op wave-survival over local wireless
  (libctru's `uds` service) is implemented but **shelved for now** --
  only one physical 3DS is currently available to test with, so it's
  unverified and not exercised in normal play until a second console is
  on hand. One console hosts (creates the network and runs the full
  simulation -- waves, enemy AI, hit resolution, health), up to 3 more
  join as clients (send input, render a host-broadcast snapshot of the
  world). Each player's cart-reload state is entirely local and never
  networked -- your ammo is your own physical cartridge. Other players
  currently render as flat colored placeholder rectangles; no avatar art
  exists yet.
- Stereoscopic 3D: the top screen renders separately per eye. Each wall
  column and sprite gets a small screen-space horizontal shift based on
  its own distance (near = shifts toward the center/"pops out", far =
  shifts outward/recedes), zero right at a fixed convergence distance and
  hard-clamped to a few pixels max so nothing -- not even a wall right
  against the camera -- can ever demand more disparity than a comfortable
  amount. Scaled by the physical 3D slider (`osGet3DSliderState()`), so
  it's flat at slider-zero. Menus and the HUD render the same flat
  content to both eyes; only the actual gameplay view gets real
  parallax. (An earlier version shifted the raycast camera position
  directly instead -- that produced unbounded, reversed disparity and
  was genuinely nauseating to test; this pixel-shift-with-convergence
  approach replaced it.)
- Circle Pad to move/strafe; look/turn via C-Stick (New3DS) or `Y`/`A`
  (works on any 3DS).
- Reload logic: pull the Game Card at any time and the gun goes dead
  ("NO CARTRIDGE"); reinsert to reload, per whichever immersion level is
  locked in for the run.
- Lower screen shows a cartridge-status icon, ammo, gun status, and
  wave/HP/kills.

Only a single tiny 10x10 test map -- the wave-survival loop and its
presentation are in reasonable shape, but the level itself still needs
real design.

**Known gap:** multiplayer is implemented and self-reviewed but has never
been run -- it needs two or more physical 3DS consoles in local wireless
range to test at all, which isn't possible from this dev environment. See
the roadmap note below for what to watch for first.

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
3. ~~Enemies / targets~~ -- waves, health, hitscan, menu/game-over flow,
   real enemy art with a walk animation, and death effects are all in;
   still needs a bigger level than the one tiny 10x10 test map
4. ~~Sound~~ -- gunshot, reload cues, and looping background music, all via
   NDSP
5. ~~A custom icon and a title screen~~
6. ~~Decide campaign vs. arcade~~ -- arcade/wave-survival, confirmed
7. Local multiplayer -- 4-player host-authoritative co-op over `uds` local
   wireless is written and self-reviewed but **shelved for now**: it needs
   2+ physical consoles to test, and only one is currently available.
   Revisit once a second console is on hand. Still needs real avatar art
   (other players are flat colored rectangles right now) and mid-game
   joining isn't supported (lobby-only, pre-start).
8. ~~Stereoscopic 3D~~ -- each eye renders to its own
   `C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT/GFX_RIGHT)`. Disparity is a
   per-object screen-space pixel shift derived from that object's own
   distance, zero at a fixed convergence distance and clamped to a small
   max so nothing can ever demand excessive separation, scaled by
   `osGet3DSliderState()`. First version moved the raycast camera position
   directly and had the eyes swapped -- confirmed nauseating on hardware
   (reversed/unbounded disparity), replaced with this clamped,
   convergence-based approach. Still untested on hardware; the constants
   (`STEREO_CONVERGE_DIST`, `STEREO_MAX_SHIFT_PX`, `STEREO_STRENGTH_PX` in
   `source/main.c`) are a conservative starting guess and may need
   tuning once you can try it.
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
