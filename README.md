# Cart Ridge

arcade fps designed around the cartridge slot. depending on your immersion mode, each cart has one of three reload behaviors

base - reloading happens when a cart is removed and inserted
medium - reloading happens when a unique cart is inserted (need two carts)
full - each cart is treated like a magazine with its own ammo count

## Setup

1. **Install devkitPro.** Download the Windows installer from
   https://devkitpro.org/wiki/Getting_Started and run it. When prompted for
   components, make sure **3DS development** is checked (this pulls in
   devkitARM, libctru, citro2d, and citro3d). 
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

   This produces `cart-ridge.3dsx`. copy `cart-ridge.3dsx` to
   `/3ds/` on your SD card, boot the Homebrew Launcher, launch it, and try
   pulling/reinserting a spare Game Card mid-game.

## Roadmap

7. Stereoscopic 3D
8. Local multiplayer
9. Gun skin changes based on which 3DS model the player is running on

## Project layout

```
source/     C source files (main.c is the whole game right now)
include/    headers (empty for now)
data/       binary data compiled into the app (currently unused)
romfs/      assets shipped alongside the app, read at runtime (currently unused)
Makefile    devkitPro build rules
```
