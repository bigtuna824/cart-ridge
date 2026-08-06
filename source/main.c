// Cart Ridge -- arcade wave-survival
//
// A raycasting FPS where the "reload" is you physically pulling the Game
// Card out of the cart slot and putting a fresh one back in. The game
// itself runs from SD/CIA (never from the cart slot), so the slot is free
// for the player to use as a prop during play.

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sprites.h"
#include "walltex.h"

#define SCREEN_W       400
#define SCREEN_H       240
#define MAP_SIZE       10
#define FOV            1.2f   // radians, ~69 degrees
#define PI             3.14159265359f
#define MOVE_SPEED     2.2f   // map tiles per second
#define TURN_SPEED     2.6f   // radians per second
#define MAX_DEPTH      20.0f
#define MAG_SIZE       8
#define MAX_TRACKED_CARTS 8

#define MAX_ENEMIES         16
#define PLAYER_MAX_HEALTH   5
#define ENEMY_SPEED         1.0f   // tiles per second
#define ENEMY_MELEE_RANGE   0.5f
#define ENEMY_SPAWN_MIN_DIST 3.0f  // don't spawn closer than this to the player
#define ENEMY_AIM_TOLERANCE 0.15f  // radians -- forgiving hitscan cone

// 1 = wall, 0 = open floor. Kept tiny on purpose -- this is just enough
// to prove out the raycaster and the reload mechanic, not a real level.
static const int map[MAP_SIZE][MAP_SIZE] = {
	{1,1,1,1,1,1,1,1,1,1},
	{1,0,0,0,0,0,0,0,0,1},
	{1,0,1,1,0,1,1,0,0,1},
	{1,0,1,0,0,0,1,0,0,1},
	{1,0,1,0,0,0,1,0,0,1}, // (4,4) left open -- player spawns here
	{1,0,0,0,1,0,0,0,0,1},
	{1,0,1,0,1,0,1,1,0,1},
	{1,0,1,0,0,0,0,0,0,1},
	{1,0,0,0,0,1,0,0,0,1},
	{1,1,1,1,1,1,1,1,1,1},
};

typedef enum {
	STATE_READY,       // cartridge in, gun works
	STATE_RELOADING,   // cartridge pulled, gun dead until a new one goes in
	STATE_CHECKING     // a cart just went back in -- briefly waiting for the
	                   // system to actually mount it before reading its ID
} GunState;

// Immersion levels, chosen at the menu and locked for the run:
//   BASE   -- one cartridge. Pull it and put any cart back in to reload.
//   MEDIUM -- two+ carts. Re-inserting the SAME cart you pulled does nothing;
//             you must swap in a genuinely different one to reload.
//   FULL   -- each distinct cartridge is its own magazine with its own
//             persistent ammo count, tracked by the game's Title ID (the
//             closest thing to a "cartridge ID" libctru exposes -- there's
//             no true per-physical-cart hardware serial available, so two
//             copies of the identical game are indistinguishable).
typedef enum { IMM_BASE, IMM_MEDIUM, IMM_FULL, IMM_COUNT } Immersion;

typedef enum { SCREEN_MENU, SCREEN_PLAYING, SCREEN_GAMEOVER } GameScreen;

typedef struct { float x, y; bool alive; } Enemy;

static const char* immersion_name(Immersion imm) {
	switch (imm) {
		case IMM_BASE:   return "BASE";
		case IMM_MEDIUM: return "MEDIUM";
		case IMM_FULL:   return "FULL";
		default:         return "?";
	}
}

typedef struct { u64 titleId; int ammo; } CartMag;
static CartMag mags[MAX_TRACKED_CARTS];
static int magCount = 0;

// Finds the magazine for a given cartridge (by Title ID), creating a
// freshly-full one the first time a cartridge is seen. If more than
// MAX_TRACKED_CARTS distinct carts get used in one session, later ones
// just share the first tracked slot rather than growing unbounded.
static int find_or_create_mag(u64 titleId) {
	for (int i = 0; i < magCount; i++) {
		if (mags[i].titleId == titleId) return i;
	}
	if (magCount < MAX_TRACKED_CARTS) {
		mags[magCount].titleId = titleId;
		mags[magCount].ammo = MAG_SIZE;
		return magCount++;
	}
	return 0;
}

// Title ID of whatever's in the Game Card slot right now, or 0 if nothing
// readable is there. This is the closest thing to a "cartridge ID" exposed
// by libctru -- it identifies which game, not which physical copy.
// outResult/outTitlesRead are for the on-screen debug readout -- they let
// us tell apart "AM call failed", "AM succeeded but found nothing", and
// "AM succeeded and this is the ID it found" instead of guessing.
static u64 read_cart_title_id(Result* outResult, u32* outTitlesRead) {
	u32 titlesRead = 0;
	u64 titleId = 0;
	Result r = AM_GetTitleList(&titlesRead, MEDIATYPE_GAME_CARD, 1, &titleId);
	if (outResult) *outResult = r;
	if (outTitlesRead) *outTitlesRead = titlesRead;
	if (R_FAILED(r) || titlesRead == 0) return 0;
	return titleId;
}

static int wall_is_solid(int x, int y) {
	if (x < 0 || y < 0 || x >= MAP_SIZE || y >= MAP_SIZE) return 1;
	return map[y][x] != 0;
}

// Cheap line-of-sight check by stepping along the segment and testing each
// sample point against the map. Used both to decide whether an enemy is
// visible (and thus drawable/shootable) and, doubling as the same check,
// to stop the player from shooting or being shot through walls.
static bool has_line_of_sight(float x0, float y0, float x1, float y1) {
	float dx = x1 - x0, dy = y1 - y0;
	float dist = sqrtf(dx * dx + dy * dy);
	if (dist < 0.0001f) return true;
	int steps = (int)(dist / 0.1f) + 1;
	for (int i = 1; i < steps; i++) {
		float t = (float)i / (float)steps;
		if (wall_is_solid((int)(x0 + dx * t), (int)(y0 + dy * t))) return false;
	}
	return true;
}

static float normalize_angle(float a) {
	while (a > PI) a -= 2.0f * PI;
	while (a < -PI) a += 2.0f * PI;
	return a;
}

// Finds a free enemy slot and places it on a random open tile that isn't
// too close to the player. Silently does nothing if no slot/spot is found.
static void spawn_enemy(Enemy* enemies, float px, float py) {
	int slot = -1;
	for (int i = 0; i < MAX_ENEMIES; i++) {
		if (!enemies[i].alive) { slot = i; break; }
	}
	if (slot < 0) return;

	for (int tries = 0; tries < 50; tries++) {
		int ex = rand() % MAP_SIZE;
		int ey = rand() % MAP_SIZE;
		if (wall_is_solid(ex, ey)) continue;
		float ddx = (ex + 0.5f) - px, ddy = (ey + 0.5f) - py;
		if (ddx * ddx + ddy * ddy < ENEMY_SPAWN_MIN_DIST * ENEMY_SPAWN_MIN_DIST) continue;
		enemies[slot].x = ex + 0.5f;
		enemies[slot].y = ey + 0.5f;
		enemies[slot].alive = true;
		return;
	}
}

static int count_alive_enemies(Enemy* enemies) {
	int n = 0;
	for (int i = 0; i < MAX_ENEMIES; i++) if (enemies[i].alive) n++;
	return n;
}

// width of each rendered strip in pixels -- one C2D_DrawImageAt call is
// issued per strip, and citro3d's command buffer can't take 400 individual
// draw calls in one frame (that's what caused earlier crashes), so we
// cast fewer, wider rays instead of one per screen column.
#define RENDER_STRIDE 4

static void draw_frame(float px, float py, float pa, C2D_Image wallImg) {
	for (int col = 0; col < SCREEN_W; col += RENDER_STRIDE) {
		float rayAngle = (pa - FOV / 2.0f) + ((float)col / SCREEN_W) * FOV;
		float rayDirX = cosf(rayAngle);
		float rayDirY = sinf(rayAngle);

		int mapX = (int)px;
		int mapY = (int)py;

		float deltaDistX = (rayDirX == 0) ? 1e30f : fabsf(1.0f / rayDirX);
		float deltaDistY = (rayDirY == 0) ? 1e30f : fabsf(1.0f / rayDirY);

		int stepX, stepY;
		float sideDistX, sideDistY;

		if (rayDirX < 0) { stepX = -1; sideDistX = (px - mapX) * deltaDistX; }
		else              { stepX = 1;  sideDistX = (mapX + 1.0f - px) * deltaDistX; }
		if (rayDirY < 0) { stepY = -1; sideDistY = (py - mapY) * deltaDistY; }
		else              { stepY = 1;  sideDistY = (mapY + 1.0f - py) * deltaDistY; }

		int hit = 0, side = 0;
		float dist = 0.0f;

		while (!hit && dist < MAX_DEPTH) {
			if (sideDistX < sideDistY) {
				sideDistX += deltaDistX;
				mapX += stepX;
				side = 0;
			} else {
				sideDistY += deltaDistY;
				mapY += stepY;
				side = 1;
			}
			if (wall_is_solid(mapX, mapY)) hit = 1;
		}

		dist = side == 0 ? (sideDistX - deltaDistX) : (sideDistY - deltaDistY);
		if (dist < 0.05f) dist = 0.05f;
		// correct fisheye distortion
		dist *= cosf(rayAngle - pa);

		int lineHeight = (int)(SCREEN_H / dist);
		int drawStart = -lineHeight / 2 + SCREEN_H / 2;
		int drawEnd = lineHeight / 2 + SCREEN_H / 2;
		if (drawStart < 0) drawStart = 0;
		if (drawEnd >= SCREEN_H) drawEnd = SCREEN_H - 1;

		// exact spot the ray hit along the wall face, used to pick which
		// vertical slice of the texture to sample for this strip
		float wallHit = (side == 0) ? (py + dist * rayDirY) : (px + dist * rayDirX);
		wallHit -= floorf(wallHit);

		Tex3DS_SubTexture strip = *wallImg.subtex;
		strip.width = 1;
		strip.left = wallHit;
		strip.right = wallHit + (1.0f / (float)wallImg.subtex->width);
		C2D_Image colImg = { wallImg.tex, &strip };

		// distance + side shading, done as a tint-toward-black so the
		// texture detail survives instead of flattening to a solid color
		float darken = dist / MAX_DEPTH;
		if (darken > 0.85f) darken = 0.85f;
		if (side == 1) darken += (1.0f - darken) * 0.3f;
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, C2D_Color32(0, 0, 0, 255), darken);

		C2D_DrawImageAt(colImg, (float)col, (float)drawStart, 0.5f, &tint,
			(float)RENDER_STRIDE, (float)(drawEnd - drawStart) / (float)strip.height);
	}
}

// Placeholder enemy rendering (flat red squares) -- no enemy art exists
// yet. Angle-to-screen-X uses the same linear mapping draw_frame() uses
// for wall columns (not a tangent-correct projection), so enemies line up
// with the walls instead of drifting relative to them.
static void draw_enemies(Enemy* enemies, float px, float py, float pa) {
	for (int i = 0; i < MAX_ENEMIES; i++) {
		if (!enemies[i].alive) continue;

		float dx = enemies[i].x - px, dy = enemies[i].y - py;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist < 0.2f || dist > MAX_DEPTH) continue;

		float relAngle = normalize_angle(atan2f(dy, dx) - pa);
		if (fabsf(relAngle) > FOV / 2.0f + 0.3f) continue; // cheap off-screen cull
		if (!has_line_of_sight(px, py, enemies[i].x, enemies[i].y)) continue;

		float screenX = SCREEN_W * (relAngle + FOV / 2.0f) / FOV;
		float size = (SCREEN_H / dist) * 0.5f;

		float shade = 1.0f - dist / MAX_DEPTH;
		if (shade < 0.2f) shade = 0.2f;
		u32 color = C2D_Color32((u8)(220 * shade), (u8)(40 * shade), (u8)(40 * shade), 255);

		C2D_DrawRectSolid(screenX - size / 2.0f, SCREEN_H / 2.0f - size / 2.0f, 0.52f,
			size, size, color);
	}
}

int main(int argc, char **argv) {
	gfxInitDefault();
	// hid and fs are already brought up by libctru's default __appInit, so
	// hidScanInput() and FSUSER_* calls work with no extra setup here. The
	// C-stick and title lookups are separate services that do need their
	// own init.
	irrstInit();
	amInit();
	romfsInit();
	srand((unsigned int)svcGetSystemTick());

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();

	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

	C2D_SpriteSheet uiSheet = C2D_SpriteSheetLoad("romfs:/gfx/sprites.t3x");
	C2D_SpriteSheet wallSheet = C2D_SpriteSheetLoad("romfs:/gfx/walltex.t3x");

	C2D_Image imgGunIdle      = C2D_SpriteSheetGetImage(uiSheet, sprites_gunidle_idx);
	C2D_Image imgGunShooting  = C2D_SpriteSheetGetImage(uiSheet, sprites_gunshooting_idx);
	C2D_Image imgGunEmpty     = C2D_SpriteSheetGetImage(uiSheet, sprites_gunempty_idx);
	C2D_Image imgCrosshair    = C2D_SpriteSheetGetImage(uiSheet, sprites_crosshair_idx);
	(void)imgCrosshair; // drawing is disabled below until it's aligned to the barrel
	C2D_Image imgMuzzleflash  = C2D_SpriteSheetGetImage(uiSheet, sprites_muzzleflash_idx);
	C2D_Image imgHudLoaded    = C2D_SpriteSheetGetImage(uiSheet, sprites_loadedgunonhud_idx);
	C2D_Image imgHudUnloaded  = C2D_SpriteSheetGetImage(uiSheet, sprites_unloadedgunonhud_idx);
	C2D_Image imgWall         = C2D_SpriteSheetGetImage(wallSheet, walltex_idx);

	C2D_TextBuf textBuf = C2D_TextBufNew(1024);

	GameScreen screen = SCREEN_MENU;
	Immersion immersion = IMM_BASE;

	float px = 4.5f, py = 4.5f, pa = 0.0f;
	int ammo = 0;
	GunState gunState = STATE_RELOADING;
	bool prevCardInserted = false;

	bool hasLoadedCart = false;
	u64 loadedCartId = 0;
	int loadedMagIdx = -1;
	float checkTimer = 0.0f;

	// debug readout for medium/full mode -- shown on the bottom screen so a
	// bad cart-ID read is visible instead of just "reload silently doesn't work"
	Result lastAmResult = 0;
	u32 lastTitlesRead = 0;
	u64 lastReadTitleId = 0;

	float muzzleFlashTimer = 0.0f;

	Enemy enemies[MAX_ENEMIES];
	memset(enemies, 0, sizeof(enemies));
	int health = PLAYER_MAX_HEALTH;
	int wave = 1;
	int kills = 0;
	int enemiesQuotaThisWave = 0;
	int enemiesSpawnedThisWave = 0;
	float spawnTimer = 0.0f;
	bool waveBreak = false;
	float waveBreakTimer = 0.0f;
	int finalWave = 0;

	u64 lastTime = svcGetSystemTick();

	while (aptMainLoop()) {
		hidScanInput();
		irrstScanInput();

		u64 now = svcGetSystemTick();
		float dt = (float)(now - lastTime) / (float)SYSCLOCK_ARM11;
		if (dt > 0.1f) dt = 0.1f; // clamp on hiccups
		lastTime = now;

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START) break;

		if (screen == SCREEN_MENU) {
			if (kDown & KEY_SELECT) {
				immersion = (Immersion)((immersion + 1) % IMM_COUNT);
			}
			if (kDown & KEY_R) {
				// start a run -- immersion is now locked until death
				px = 4.5f; py = 4.5f; pa = 0.0f;
				health = PLAYER_MAX_HEALTH;
				wave = 1;
				kills = 0;
				enemiesQuotaThisWave = 2 + wave;
				enemiesSpawnedThisWave = 0;
				spawnTimer = 1.0f;
				waveBreak = false;
				memset(enemies, 0, sizeof(enemies));
				magCount = 0; // full mode starts each run with fresh magazines
				hasLoadedCart = false;
				loadedCartId = 0;
				loadedMagIdx = -1;
				ammo = 0;
				gunState = STATE_RELOADING;
				FSUSER_CardSlotIsInserted(&prevCardInserted);
				muzzleFlashTimer = 0.0f;
				screen = SCREEN_PLAYING;
			}
		} else if (screen == SCREEN_PLAYING) {
			// Circle Pad: move forward/back and strafe left/right.
			circlePosition cpos;
			hidCircleRead(&cpos);
			float moveInput = cpos.dy / 156.0f;
			float strafeInput = cpos.dx / 156.0f;
			if (fabsf(moveInput) < 0.15f) moveInput = 0.0f;
			if (fabsf(strafeInput) < 0.15f) strafeInput = 0.0f;

			// C-Stick: look/turn (New3DS only -- reads as centered/zero on
			// original 3DS, so it's harmless to leave active there).
			circlePosition cstick;
			hidCstickRead(&cstick);
			float turnInput = cstick.dx / 156.0f;
			if (fabsf(turnInput) < 0.15f) turnInput = 0.0f;

			// Face buttons: look/turn fallback for original 3DS, which has
			// no C-Stick. Y = left, A = right (matches their position in
			// the diamond). Additive with the C-Stick so both work on any
			// hardware without needing a mode switch.
			u32 kHeld = hidKeysHeld();
			if (kHeld & KEY_Y) turnInput -= 1.0f;
			if (kHeld & KEY_A) turnInput += 1.0f;
			if (turnInput > 1.0f) turnInput = 1.0f;
			if (turnInput < -1.0f) turnInput = -1.0f;

			pa += turnInput * TURN_SPEED * dt;

			const float HALF_PI = 1.57079632679f;
			float nx = px + cosf(pa) * moveInput * MOVE_SPEED * dt
			              + cosf(pa + HALF_PI) * strafeInput * MOVE_SPEED * dt;
			float ny = py + sinf(pa) * moveInput * MOVE_SPEED * dt
			              + sinf(pa + HALF_PI) * strafeInput * MOVE_SPEED * dt;
			if (!wall_is_solid((int)nx, (int)py)) px = nx;
			if (!wall_is_solid((int)px, (int)ny)) py = ny;

			// --- cartridge slot as the reload mechanism ---
			bool cardInserted = false;
			FSUSER_CardSlotIsInserted(&cardInserted);

			if ((gunState == STATE_READY || gunState == STATE_CHECKING) && prevCardInserted && !cardInserted) {
				// cart pulled (even mid-check) -- gun goes dead no matter how
				// much ammo is left, or how far along a pending ID check was
				gunState = STATE_RELOADING;
			} else if (gunState == STATE_RELOADING && !prevCardInserted && cardInserted) {
				if (immersion == IMM_BASE) {
					// base mode never needs to identify the cart, so it can
					// reload the instant it's back in -- no need for the
					// settle window that medium/full require below
					ammo = MAG_SIZE;
					hasLoadedCart = true;
					gunState = STATE_READY;
				} else {
					// a cart just went back in, but AM_GetTitleList tends to
					// come back empty if queried on the very same frame the
					// slot reports "inserted" -- the system hasn't mounted it
					// yet. Wait briefly before actually reading its ID.
					gunState = STATE_CHECKING;
					checkTimer = 0.35f;
				}
			} else if (gunState == STATE_CHECKING) {
				checkTimer -= dt;
				if (checkTimer <= 0.0f) {
					u64 newId = read_cart_title_id(&lastAmResult, &lastTitlesRead);
					lastReadTitleId = newId;
					bool doReload = true;
					if (hasLoadedCart && newId == loadedCartId) {
						doReload = false; // same cart re-inserted -- swap to a different one
					}
					if (doReload) {
						if (immersion == IMM_FULL) {
							loadedMagIdx = find_or_create_mag(newId);
							ammo = mags[loadedMagIdx].ammo;
						} else {
							ammo = MAG_SIZE;
						}
						loadedCartId = newId;
						hasLoadedCart = true;
						gunState = STATE_READY;
					} else {
						gunState = STATE_RELOADING;
					}
				}
			}
			prevCardInserted = cardInserted;

			if ((kDown & KEY_R) && gunState == STATE_READY && ammo > 0) {
				ammo--;
				if (immersion == IMM_FULL && loadedMagIdx >= 0) mags[loadedMagIdx].ammo = ammo;
				muzzleFlashTimer = 0.08f;

				// hit the closest alive enemy roughly in front of the
				// player, if there's a clear line of sight to it
				int bestIdx = -1;
				float bestDist = MAX_DEPTH;
				for (int i = 0; i < MAX_ENEMIES; i++) {
					if (!enemies[i].alive) continue;
					float dx = enemies[i].x - px, dy = enemies[i].y - py;
					float dist = sqrtf(dx * dx + dy * dy);
					if (dist >= bestDist) continue;
					float relAngle = normalize_angle(atan2f(dy, dx) - pa);
					if (fabsf(relAngle) > ENEMY_AIM_TOLERANCE) continue;
					if (!has_line_of_sight(px, py, enemies[i].x, enemies[i].y)) continue;
					bestDist = dist;
					bestIdx = i;
				}
				if (bestIdx >= 0) {
					enemies[bestIdx].alive = false;
					kills++;
				}
			}
			if (muzzleFlashTimer > 0.0f) muzzleFlashTimer -= dt;

			// --- waves ---
			if (waveBreak) {
				waveBreakTimer -= dt;
				if (waveBreakTimer <= 0.0f) {
					wave++;
					enemiesQuotaThisWave = 2 + wave;
					enemiesSpawnedThisWave = 0;
					waveBreak = false;
				}
			} else if (enemiesSpawnedThisWave < enemiesQuotaThisWave) {
				spawnTimer -= dt;
				if (spawnTimer <= 0.0f) {
					spawn_enemy(enemies, px, py);
					enemiesSpawnedThisWave++;
					spawnTimer = 1.2f;
				}
			} else if (count_alive_enemies(enemies) == 0) {
				waveBreak = true;
				waveBreakTimer = 2.0f;
			}

			// --- enemy movement + melee ---
			for (int i = 0; i < MAX_ENEMIES; i++) {
				if (!enemies[i].alive) continue;
				float dx = px - enemies[i].x, dy = py - enemies[i].y;
				float dist = sqrtf(dx * dx + dy * dy);
				if (dist < ENEMY_MELEE_RANGE) {
					enemies[i].alive = false;
					health--;
					if (health <= 0) {
						finalWave = wave;
						screen = SCREEN_GAMEOVER;
					}
					continue;
				}
				float mvx = dx / dist * ENEMY_SPEED * dt;
				float mvy = dy / dist * ENEMY_SPEED * dt;
				float newX = enemies[i].x + mvx;
				float newY = enemies[i].y + mvy;
				if (!wall_is_solid((int)newX, (int)enemies[i].y)) enemies[i].x = newX;
				if (!wall_is_solid((int)enemies[i].x, (int)newY)) enemies[i].y = newY;
			}
		} else if (screen == SCREEN_GAMEOVER) {
			if (kDown & KEY_R) {
				screen = SCREEN_MENU;
			}
		}

		// --- render ---
		// C3D_FrameBegin binds the low-level GPU command buffer -- without
		// it, every GPUCMD write below (including C2D_TargetClear's own
		// clear command) goes through a null pointer.
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		if (screen == SCREEN_MENU) {
			C2D_TargetClear(top, C2D_Color32(10, 10, 20, 255));
			C2D_SceneBegin(top);
			C2D_TextBufClear(textBuf);

			C2D_Text titleText;
			C2D_TextParse(&titleText, textBuf, "CART RIDGE");
			C2D_TextOptimize(&titleText);
			C2D_DrawText(&titleText, C2D_WithColor, 90.0f, 80.0f, 0.5f, 1.2f, 1.2f,
				C2D_Color32(255, 255, 255, 255));

			C2D_Text subText;
			C2D_TextParse(&subText, textBuf,
				"Pull the cartridge to reload. Survive the waves.");
			C2D_TextOptimize(&subText);
			C2D_DrawText(&subText, C2D_WithColor, 40.0f, 140.0f, 0.5f, 0.5f, 0.5f,
				C2D_Color32(180, 180, 180, 255));

			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);

			char line[64];
			C2D_Text modeText;
			snprintf(line, sizeof(line), "Immersion: %s", immersion_name(immersion));
			C2D_TextParse(&modeText, textBuf, line);
			C2D_TextOptimize(&modeText);
			C2D_DrawText(&modeText, C2D_WithColor, 10.0f, 60.0f, 0.5f, 0.6f, 0.6f,
				C2D_Color32(255, 255, 255, 255));

			C2D_Text promptText;
			C2D_TextParse(&promptText, textBuf,
				"SELECT: change immersion level\n"
				"R: start run (locks the level until you die)\n"
				"START: quit");
			C2D_TextOptimize(&promptText);
			C2D_DrawText(&promptText, C2D_WithColor, 10.0f, 100.0f, 0.5f, 0.45f, 0.45f,
				C2D_Color32(200, 200, 200, 255));
		} else if (screen == SCREEN_PLAYING) {
			bool cardInserted = prevCardInserted; // set above this frame

			// top screen: the raycast view + enemies + viewmodel
			C2D_TargetClear(top, C2D_Color32(10, 10, 15, 255));
			C2D_SceneBegin(top);
			draw_frame(px, py, pa, imgWall);
			draw_enemies(enemies, px, py, pa);

			C2D_Image gunImg = (gunState != STATE_READY) ? imgGunEmpty
				: (muzzleFlashTimer > 0.0f) ? imgGunShooting : imgGunIdle;
			// lower-right, mostly cropped off the bottom edge -- per mockup.
			// Tune these two if it still needs nudging: negative = left/up,
			// positive = right/down.
			const float gunNudgeX = -50.0f;
			const float gunNudgeY = 102.0f;
			float gunX = 3.0f * (SCREEN_W / 2.0f - gunImg.subtex->width / 2.0f) + gunNudgeX;
			float gunY = (SCREEN_H - gunImg.subtex->height) / 2.0f + gunNudgeY;
			// scaling grows the image from its top-left corner, which would
			// otherwise fling most of it past the right/bottom edges given
			// how close to those edges it's already anchored -- grow it
			// around that same anchor point instead so it just gets
			// chunkier in place
			const float gunScale = 2.0f;
			float gunDrawX = gunX - gunImg.subtex->width * (gunScale - 1.0f) / 2.0f;
			float gunDrawY = gunY - gunImg.subtex->height * (gunScale - 1.0f) / 2.0f;
			C2D_DrawImageAt(gunImg, gunDrawX, gunDrawY, 0.6f, NULL, gunScale, gunScale);

			if (muzzleFlashTimer > 0.0f) {
				// rough muzzle position -- upper-right of the viewmodel,
				// where the barrel sits in the source art; offsets scale
				// with the gun so it tracks the bigger sprite
				float mfX = gunDrawX + gunImg.subtex->width * gunScale - 30.0f * gunScale;
				float mfY = gunDrawY - 5.0f * gunScale;
				C2D_DrawImageAt(imgMuzzleflash, mfX, mfY, 0.55f, NULL, 1.0f, 1.0f);
			}

			// crosshair hidden for now -- it's not lined up with the barrel yet.
			// float chX = SCREEN_W / 2.0f - imgCrosshair.subtex->width / 2.0f;
			// float chY = SCREEN_H / 2.0f - imgCrosshair.subtex->height / 2.0f;
			// C2D_DrawImageAt(imgCrosshair, chX, chY, 0.5f, NULL, 1.0f, 1.0f);

			// bottom screen: cartridge-status icon, ammo, wave/health, controls
			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);

			C2D_Image hudImg = cardInserted ? imgHudLoaded : imgHudUnloaded;
			C2D_DrawImageAt(hudImg, 10.0f, 10.0f, 0.5f, NULL, 1.0f, 1.0f);

			C2D_TextBufClear(textBuf);

			char line[64];
			C2D_Text ammoText;
			snprintf(line, sizeof(line), "Ammo: %d / %d", ammo, MAG_SIZE);
			C2D_TextParse(&ammoText, textBuf, line);
			C2D_TextOptimize(&ammoText);
			C2D_DrawText(&ammoText, C2D_WithColor, 10.0f, 85.0f, 0.5f, 0.6f, 0.6f,
				C2D_Color32(255, 255, 255, 255));

			C2D_Text statusText;
			const char* statusStr = (gunState == STATE_READY) ? "READY"
				: (gunState == STATE_CHECKING) ? "CHECKING CARTRIDGE..." : "NO CARTRIDGE";
			u32 statusColor = (gunState == STATE_READY) ? C2D_Color32(120, 255, 120, 255)
				: (gunState == STATE_CHECKING) ? C2D_Color32(255, 220, 120, 255)
				: C2D_Color32(255, 120, 120, 255);
			C2D_TextParse(&statusText, textBuf, statusStr);
			C2D_TextOptimize(&statusText);
			C2D_DrawText(&statusText, C2D_WithColor, 10.0f, 110.0f, 0.5f, 0.6f, 0.6f, statusColor);

			C2D_Text statsText;
			snprintf(line, sizeof(line), "Wave %d   HP %d/%d   Kills %d", wave, health,
				PLAYER_MAX_HEALTH, kills);
			C2D_TextParse(&statsText, textBuf, line);
			C2D_TextOptimize(&statsText);
			C2D_DrawText(&statsText, C2D_WithColor, 10.0f, 140.0f, 0.5f, 0.5f, 0.5f,
				C2D_Color32(255, 220, 150, 255));

			C2D_Text debugText;
			if (immersion != IMM_BASE) {
				// temporary readout while we chase the medium/full mode bug --
				// shows exactly what the last cartridge ID read found, so a
				// bad read is visible instead of just "reload doesn't work"
				snprintf(line, sizeof(line), "id:%08lx%08lx loaded:%08lx%08lx n:%lu r:%08lx",
					(unsigned long)(lastReadTitleId >> 32), (unsigned long)(lastReadTitleId & 0xFFFFFFFF),
					(unsigned long)(loadedCartId >> 32), (unsigned long)(loadedCartId & 0xFFFFFFFF),
					(unsigned long)lastTitlesRead, (unsigned long)lastAmResult);
				C2D_TextParse(&debugText, textBuf, line);
				C2D_TextOptimize(&debugText);
				C2D_DrawText(&debugText, C2D_WithColor, 10.0f, 160.0f, 0.5f, 0.35f, 0.35f,
					C2D_Color32(255, 255, 100, 255));
			}

			C2D_Text helpText;
			C2D_TextParse(&helpText, textBuf,
				"Circle Pad: move/strafe   C-Stick/Y+A: look   R: fire\n"
				"Pull the Game Card to reload, reinsert to chamber.\n"
				"START: quit");
			C2D_TextOptimize(&helpText);
			C2D_DrawText(&helpText, C2D_WithColor, 10.0f, 180.0f, 0.5f, 0.4f, 0.4f,
				C2D_Color32(180, 180, 180, 255));
		} else { // SCREEN_GAMEOVER
			C2D_TargetClear(top, C2D_Color32(20, 10, 10, 255));
			C2D_SceneBegin(top);
			C2D_TextBufClear(textBuf);

			C2D_Text overText;
			C2D_TextParse(&overText, textBuf, "GAME OVER");
			C2D_TextOptimize(&overText);
			C2D_DrawText(&overText, C2D_WithColor, 100.0f, 80.0f, 0.5f, 1.2f, 1.2f,
				C2D_Color32(255, 120, 120, 255));

			char line[64];
			C2D_Text waveText;
			snprintf(line, sizeof(line), "You reached wave %d", finalWave);
			C2D_TextParse(&waveText, textBuf, line);
			C2D_TextOptimize(&waveText);
			C2D_DrawText(&waveText, C2D_WithColor, 90.0f, 140.0f, 0.5f, 0.55f, 0.55f,
				C2D_Color32(220, 220, 220, 255));

			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);

			C2D_Text killsText;
			snprintf(line, sizeof(line), "Kills: %d   Mode: %s", kills, immersion_name(immersion));
			C2D_TextParse(&killsText, textBuf, line);
			C2D_TextOptimize(&killsText);
			C2D_DrawText(&killsText, C2D_WithColor, 10.0f, 60.0f, 0.5f, 0.55f, 0.55f,
				C2D_Color32(255, 255, 255, 255));

			C2D_Text retryText;
			C2D_TextParse(&retryText, textBuf, "R: return to menu\nSTART: quit");
			C2D_TextOptimize(&retryText);
			C2D_DrawText(&retryText, C2D_WithColor, 10.0f, 100.0f, 0.5f, 0.45f, 0.45f,
				C2D_Color32(200, 200, 200, 255));
		}

		C3D_FrameEnd(0);
	}

	C2D_TextBufDelete(textBuf);
	C2D_SpriteSheetFree(wallSheet);
	C2D_SpriteSheetFree(uiSheet);
	C2D_Fini();
	C3D_Fini();
	romfsExit();
	amExit();
	gfxExit();
	return 0;
}
