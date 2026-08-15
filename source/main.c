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
#include "title.h"
#include "hud.h"

#define SCREEN_W       400
#define SCREEN_H       240
#define MAP_SIZE       10
#define FOV            1.2f   // radians, ~69 degrees
// Stereo 3D disparity is applied as a small per-object screen-space pixel
// shift (NOT a raycast camera move -- shifting the camera position caused
// unbounded, sign-reversed disparity for walls close to the player, which
// is exactly what makes a 3D effect nauseating instead of just subtle).
// STEREO_CONVERGE_DIST is the depth (in tiles) that lines up with zero
// disparity -- nearer objects pop out, farther objects recede. Everything
// closer than ~0.3 tiles is treated as if it were 0.3 tiles away for this
// calculation, and the final shift is hard-clamped to
// STEREO_MAX_SHIFT_PX regardless of distance, so nothing can ever demand
// more disparity than that even for a wall right against the camera.
#define STEREO_CONVERGE_DIST 2.5f
#define STEREO_MAX_SHIFT_PX  6.0f
// Wall strips are rendered independently (RENDER_STRIDE pixels wide) and
// each now gets its own depth-based shift, so where depth changes from one
// strip to the next -- a receding wall, a corner -- adjacent strips can
// end up shifted by different amounts and leave a thin gap between them.
// Drawing each strip a bit wider than it needs to be (and centered on its
// shifted position) makes strips overlap slightly instead, hiding that gap.
// Keep this small: it only needs to cover the sub-pixel-to-a-couple-pixel
// shift difference between adjacent strips on an ordinary continuous
// surface. At a genuine depth discontinuity (an actual corner) the shift
// difference is much bigger, and overlap can't hide that without smearing
// one strip's texture past the corner -- which reads as seeing a sliver of
// the other eye's view, since that's exactly where the two eyes differ
// most. A wider value trades more hidden gaps for worse corner smearing.
#define STEREO_STRIP_OVERLAP_PX 1.5f
#define STEREO_STRENGTH_PX   35.0f
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
#define ENEMY_FLIP_PERIOD   0.3f   // seconds per flip half-cycle (walk animation)

#define MAX_DEATH_EFFECTS     8
#define DEATH_EFFECT_DURATION 0.4f

// Gun viewmodel animation. idle/idle2 alternate continuously at this rate;
// fire1/fire2/reset play once (in that order) over FIRE_FRAME_COUNT frames
// each time the player shoots.
#define GUN_ANIM_FPS         12.0f
#define GUN_FRAME_PERIOD     (1.0f / GUN_ANIM_FPS)
#define GUN_FIRE_FRAME_COUNT 3
#define GUN_FIRE_DURATION    (GUN_FIRE_FRAME_COUNT * GUN_FRAME_PERIOD)

// How far (in screen px, at the gun's draw scale) the viewmodel drops when
// out of ammo/reloading vs. its normal raised position, and how quickly it
// eases between the two -- higher LOWER_SPEED snaps faster, lower is a
// slower settle.
#define GUN_LOWER_AMOUNT 90.0f
#define GUN_LOWER_SPEED  8.0f

// Classic DOOM-style weapon sway while moving: a horizontal side-to-side
// sway plus a vertical bob at twice the frequency (so it dips on every
// half-stride, not just once per full left-right cycle). Both scale with
// how much the player is actually moving, so the sway settles back to
// nothing when standing still rather than freezing mid-swing.
#define GUN_SWAY_SPEED     6.0f
#define GUN_SWAY_AMOUNT_X  6.0f
#define GUN_SWAY_AMOUNT_Y  5.0f

// NDSP channels -- one dedicated channel per sound so overlapping triggers
// (e.g. firing again before the last shot's sound finished) just restart
// that channel instead of needing a general-purpose mixer/voice pool.
#define CH_MUSIC   0
#define CH_GUNSHOT 1
#define CH_RELOAD1 2
#define CH_RELOAD2 3

// --- local multiplayer (UDS) -----------------------------------------
// Call patterns verified against devkitPro's official example
// (3ds-examples/network/uds/source/uds.c) since this service was
// completely new territory for the project and hardware-testable only
// with 2+ physical consoles running at once.
#define MP_WLANCOMMID    0x43415254u  // arbitrary app-unique ID ("CART")
#define MP_PASSPHRASE    "cart ridge multiplayer v1"
#define MP_DATA_CHANNEL  1
#define MP_MAX_PLAYERS   4
#define MP_TICK_INTERVAL 0.05f  // 20Hz network send rate, decoupled from render rate

typedef enum { MP_OFF, MP_HOST, MP_CLIENT } MpRole;
// Co-op (wave-survival, same as solo but with company) vs. versus (free-
// for-all deathmatch: no enemies, players damage each other, first to
// VERSUS_KILL_TARGET kills wins). Chosen by the host before starting;
// clients just inherit whatever the host picked, via PKT_HOST_START.
typedef enum { MP_MODE_COOP, MP_MODE_VERSUS } MpMode;
#define VERSUS_KILL_TARGET 5
#define VERSUS_DAMAGE      1
enum { PKT_CLIENT_INPUT = 1, PKT_HOST_STATE = 2, PKT_HOST_START = 3 };

// Sent by each client to the host, at MP_TICK_INTERVAL.
typedef struct {
	u8 type; // PKT_CLIENT_INPUT
	float px, py, pa;
	u8 firing; // edge-triggered: true for exactly one send per shot fired
} MpInputPacket;

// Sent by the host to everyone, at MP_TICK_INTERVAL -- the full
// authoritative world snapshot. Struct is memcpy'd raw over the wire,
// which is safe here since every player runs the same binary (same
// compiler, same ABI, same endianness).
typedef struct {
	u8 type; // PKT_HOST_STATE
	u8 wave;
	u8 gameOver;
	u8 finalWave;
	u32 waveTintColor;
	u8 versusWinnerIdx; // 0xFF = no winner yet
	struct {
		float x, y, pa;
		u8 health;
		u8 alive;
		u8 connected;
		u8 kills;
		// Bumped by the host every time this player respawns (versus
		// mode). Position is otherwise self-authoritative -- each player
		// reports their own x/y -- so this is the one case where the host
		// needs to force a client's position instead of just trusting
		// what they report. The client applies x/y whenever this differs
		// from the last value it saw, which (unlike a one-shot flag) is
		// robust to a dropped packet: it'll just catch up on any later
		// packet that still carries the new number, not only the first.
		u8 respawnSeq;
	} players[MP_MAX_PLAYERS];
	struct { float x, y; u8 alive; } enemies[MAX_ENEMIES];
} MpStatePacket;

typedef struct { u8 type; u8 mode; } MpStartPacket; // PKT_HOST_START

// Other players as seen for rendering -- populated from received network
// data, not simulated locally (that's the host's job).
typedef struct { float x, y; bool alive; bool connected; } RemotePlayer;

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

// Immersion levels, chosen at the menu and locked for the run. Each
// player's own immersion level and reload state stays fully local to
// their own console (and their own cart slot) even in multiplayer --
// only never transmitted.
//   BASE   -- one cartridge. Pull it and put any cart back in to reload.
//   MEDIUM -- two+ carts. Re-inserting the SAME cart you pulled does nothing;
//             you must swap in a genuinely different one to reload.
//   FULL   -- each distinct cartridge is its own magazine with its own
//             persistent ammo count, tracked by the game's Title ID (the
//             closest thing to a "cartridge ID" libctru exposes -- there's
//             no true per-physical-cart hardware serial available, so two
//             copies of the identical game are indistinguishable).
typedef enum { IMM_BASE, IMM_MEDIUM, IMM_FULL, IMM_COUNT } Immersion;

typedef enum {
	SCREEN_MENU, SCREEN_MP_MENU, SCREEN_MP_LOBBY, SCREEN_PLAYING, SCREEN_GAMEOVER
} GameScreen;

typedef struct { float x, y; bool alive; } Enemy;

// A turquoise blood-splatter particle burst on enemy death. Each droplet's
// direction/travel-distance/size is randomized once at spawn (not
// recomputed per frame -- that would make the splatter visibly jitter
// instead of smoothly radiating outward) and stored in world units, same
// convention as everything else this engine billboards.
#define BLOOD_PARTICLES 6
typedef struct {
	float x, y;
	float timer;
	bool active;
	float particleAngle[BLOOD_PARTICLES];
	float particleDist[BLOOD_PARTICLES];
	float particleSize[BLOOD_PARTICLES];
} DeathEffect;

static const char* immersion_name(Immersion imm) {
	switch (imm) {
		case IMM_BASE:   return "BASE";
		case IMM_MEDIUM: return "MEDIUM";
		case IMM_FULL:   return "FULL";
		default:         return "?";
	}
}

// Stricter immersion levels are harder to play under (a real cart swap vs.
// just any cart reloading), so they're worth more: BASE 1x, MEDIUM 2x, FULL
// 3x. Relies on the enum's declaration order (IMM_BASE=0, IMM_MEDIUM=1,
// IMM_FULL=2) matching that ordering.
static int immersion_score_multiplier(Immersion imm) {
	return (int)imm + 1;
}

typedef struct { u64 titleId; int ammo; } CartMag;
static CartMag mags[MAX_TRACKED_CARTS];
static int magCount = 0;

// --- audio -----------------------------------------------------------
// A WAV's raw PCM samples, loaded into linear memory (required for the DSP
// to read them directly) plus the format info needed to configure an NDSP
// channel to play them back correctly.
typedef struct {
	void* data;
	u32   nsamples;   // frames, not bytes
	u16   channels;
	u32   sampleRate;
} Sound;

static Sound sounds[4];          // indexed by CH_* constants
static ndspWaveBuf waveBufs[4];  // must outlive playback -- can't be a local
static bool audioReady = false;

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

// Parses a WAV file's fmt/data chunks (tolerating whatever chunks come
// before "data", which is all a typical encoder/converter needs) and
// copies the raw PCM into a freshly linearAlloc'd buffer ready for NDSP.
// 3DS homebrew apps run with a heap in the tens-of-MB range, nowhere near
// enough for, say, an uncompressed full-length music track (a several
// hundred MB WAV would blow the budget outright). Rather than let a
// too-large file take its chances with malloc, reject it up front so
// loading fails predictably instead of however OOM happens to behave.
// 16MB is roughly 80s of 16-bit stereo 48kHz audio -- plenty for a
// looping BGM clip.
#define MAX_WAV_FILE_SIZE (16 * 1024 * 1024)

static bool load_wav(const char* path, Sound* out) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fileSize <= 12 || fileSize > MAX_WAV_FILE_SIZE) { fclose(f); return false; }

	u8* raw = (u8*)malloc(fileSize);
	if (!raw) { fclose(f); return false; }
	size_t readBytes = fread(raw, 1, fileSize, f);
	fclose(f);
	if ((long)readBytes != fileSize) { free(raw); return false; }

	u16 fmtChannels = 2;
	u32 fmtRate = 48000;
	u16 fmtBits = 16;
	long dataOffset = -1;
	u32 dataSize = 0;

	for (long i = 12; i + 8 <= fileSize; ) {
		u32 chunkSize = raw[i+4] | (raw[i+5] << 8) | (raw[i+6] << 16) | ((u32)raw[i+7] << 24);
		if (memcmp(raw + i, "fmt ", 4) == 0 && i + 8 + 16 <= fileSize) {
			fmtChannels = raw[i+8+2] | (raw[i+8+3] << 8);
			fmtRate = raw[i+8+4] | (raw[i+8+5] << 8) | (raw[i+8+6] << 16) | ((u32)raw[i+8+7] << 24);
			fmtBits = raw[i+8+14] | (raw[i+8+15] << 8);
		} else if (memcmp(raw + i, "data", 4) == 0) {
			dataSize = chunkSize;
			dataOffset = i + 8;
			break;
		}
		i += 8 + chunkSize + (chunkSize & 1); // chunks are word-aligned
	}
	if (dataOffset < 0 || fmtChannels == 0 || fmtBits == 0) { free(raw); return false; }
	if (dataOffset + (long)dataSize > fileSize) dataSize = (u32)(fileSize - dataOffset);

	out->data = linearAlloc(dataSize);
	if (!out->data) { free(raw); return false; }
	memcpy(out->data, raw + dataOffset, dataSize);
	free(raw);
	DSP_FlushDataCache(out->data, dataSize);

	out->channels = fmtChannels;
	out->sampleRate = fmtRate;
	out->nsamples = dataSize / ((fmtBits / 8) * fmtChannels);
	return true;
}

static void setup_channel(int ch, const Sound* s) {
	if (!audioReady || !s->data) return;
	ndspChnReset(ch);
	ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	ndspChnSetRate(ch, (float)s->sampleRate);
	ndspChnSetFormat(ch, s->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
}

static void play_sound(int ch, const Sound* s, bool loop) {
	if (!audioReady || !s->data) return;
	ndspChnWaveBufClear(ch);
	memset(&waveBufs[ch], 0, sizeof(ndspWaveBuf));
	waveBufs[ch].data_vaddr = s->data;
	waveBufs[ch].nsamples = s->nsamples;
	waveBufs[ch].looping = loop;
	ndspChnWaveBufAdd(ch, &waveBufs[ch]);
}

static u32 random_wave_color(void) {
	return C2D_Color32((u8)(rand() % 256), (u8)(rand() % 256), (u8)(rand() % 256), 255);
}

// --- multiplayer networking -------------------------------------------

static bool mp_host_create(udsBindContext* ctx) {
	udsNetworkStruct network;
	udsGenerateDefaultNetworkStruct(&network, MP_WLANCOMMID, 0, MP_MAX_PLAYERS);
	Result r = udsCreateNetwork(&network, MP_PASSPHRASE, sizeof(MP_PASSPHRASE), ctx,
		MP_DATA_CHANNEL, UDS_DEFAULT_RECVBUFSIZE);
	return R_SUCCEEDED(r);
}

// One scan attempt, connecting to the first matching network found. This
// blocks for real time (a beacon scan takes a moment) -- fine behind a
// deliberate "Join" button press, not something to call mid-gameplay.
static bool mp_client_scan_and_connect(udsBindContext* ctx) {
	size_t tmpbufSize = 0x4000;
	void* tmpbuf = malloc(tmpbufSize);
	if (!tmpbuf) return false;
	memset(tmpbuf, 0, tmpbufSize);

	udsNetworkScanInfo* networks = NULL;
	size_t totalNetworks = 0;
	Result r = udsScanBeacons(tmpbuf, tmpbufSize, &networks, &totalNetworks, MP_WLANCOMMID, 0, NULL, false);
	free(tmpbuf);
	if (R_FAILED(r) || totalNetworks == 0) return false;

	r = udsConnectNetwork(&networks[0].network, MP_PASSPHRASE, sizeof(MP_PASSPHRASE), ctx,
		UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, MP_DATA_CHANNEL, UDS_DEFAULT_RECVBUFSIZE);
	free(networks);
	return R_SUCCEEDED(r);
}

static u32 player_color(int idx) {
	switch (idx) {
		case 0:  return C2D_Color32(220, 120, 0, 255);   // host -- orange
		case 1:  return C2D_Color32(60, 130, 240, 255);  // blue
		case 2:  return C2D_Color32(60, 210, 104, 255);  // green
		default: return C2D_Color32(224, 210, 60, 255);  // yellow
	}
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

static void spawn_death_effect(DeathEffect* effects, float x, float y) {
	for (int i = 0; i < MAX_DEATH_EFFECTS; i++) {
		if (!effects[i].active) {
			effects[i].x = x;
			effects[i].y = y;
			effects[i].timer = DEATH_EFFECT_DURATION;
			effects[i].active = true;
			for (int p = 0; p < BLOOD_PARTICLES; p++) {
				effects[i].particleAngle[p] = ((float)(rand() % 360)) * (PI / 180.0f);
				effects[i].particleDist[p] = 0.05f + ((float)(rand() % 100) / 100.0f) * 0.10f;
				effects[i].particleSize[p] = 0.02f + ((float)(rand() % 100) / 100.0f) * 0.03f;
			}
			return;
		}
	}
	// all slots busy -- purely cosmetic, fine to just drop it
}

// Finds the closest alive enemy within a forgiving aim cone of (fx,fy,fa)
// that has line of sight, kills it, and spawns a death effect. Shared by
// the local player's own shot and (in multiplayer) the host resolving a
// remote client's reported shot.
#define SCORE_PER_KILL       300
#define SCORE_PER_WAVE_CLEAR 1000
#define SCORE_NO_DAMAGE_WAVE 500

static void try_hitscan(Enemy* enemies, DeathEffect* effects, float fx, float fy, float fa,
		int wave, int scoreMult, int* kills, int* score) {
	int bestIdx = -1;
	float bestDist = MAX_DEPTH;
	for (int i = 0; i < MAX_ENEMIES; i++) {
		if (!enemies[i].alive) continue;
		float dx = enemies[i].x - fx, dy = enemies[i].y - fy;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist >= bestDist) continue;
		float relAngle = normalize_angle(atan2f(dy, dx) - fa);
		if (fabsf(relAngle) > ENEMY_AIM_TOLERANCE) continue;
		if (!has_line_of_sight(fx, fy, enemies[i].x, enemies[i].y)) continue;
		bestDist = dist;
		bestIdx = i;
	}
	if (bestIdx >= 0) {
		enemies[bestIdx].alive = false;
		if (kills) (*kills)++;
		if (score) *score += SCORE_PER_KILL * wave * scoreMult;
		spawn_death_effect(effects, enemies[bestIdx].x, enemies[bestIdx].y);
	}
}

// Versus PvP hitscan -- same forgiving aim-cone-plus-line-of-sight search
// as try_hitscan above, but against the OTHER players instead of enemies.
// Player slot 0 is always the host; slots 1-3 are whichever clients are
// connected. shooterIdx is excluded from its own search (can't hit
// yourself). Returns the hit player's slot, or -1 for a miss.
static int find_versus_hit(const float allX[MP_MAX_PLAYERS], const float allY[MP_MAX_PLAYERS],
		const bool allAlive[MP_MAX_PLAYERS], int shooterIdx, float fx, float fy, float fa) {
	int bestIdx = -1;
	float bestDist = MAX_DEPTH;
	for (int i = 0; i < MP_MAX_PLAYERS; i++) {
		if (i == shooterIdx || !allAlive[i]) continue;
		float dx = allX[i] - fx, dy = allY[i] - fy;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist >= bestDist) continue;
		float relAngle = normalize_angle(atan2f(dy, dx) - fa);
		if (fabsf(relAngle) > ENEMY_AIM_TOLERANCE) continue;
		if (!has_line_of_sight(fx, fy, allX[i], allY[i])) continue;
		bestDist = dist;
		bestIdx = i;
	}
	return bestIdx;
}

// Picks a random open (non-wall) tile for a versus-mode respawn. Doesn't
// bother avoiding other players like spawn_enemy avoids the player --
// deathmatch respawns landing near someone isn't unusual or unfair the
// way an enemy spawning on top of you would be.
static void find_respawn_point(float* outX, float* outY) {
	for (int tries = 0; tries < 50; tries++) {
		int x = rand() % MAP_SIZE;
		int y = rand() % MAP_SIZE;
		if (wall_is_solid(x, y)) continue;
		*outX = x + 0.5f;
		*outY = y + 0.5f;
		return;
	}
	*outX = 4.5f; *outY = 4.5f; // shouldn't happen on this map, but stay safe
}

// Applies one versus-mode hit: damages the victim (player slot 0 is
// always the host, using hostHealth/hostPx/hostPy directly since those
// aren't array-indexed like the rest; slots 1-3 are clients), and if that
// drops them to 0 HP, respawns them at a random open tile, credits the
// shooter with a kill, checks the win condition, and -- for a client
// victim -- bumps their respawnSeq so the state broadcast tells them to
// teleport (their position is normally self-reported, so the host can't
// just move them: see MpStatePacket's respawnSeq field).
static void versus_apply_hit(int shooterIdx, int hitIdx, int* hostHealth, float* hostPx, float* hostPy,
		int mpClientHealth[MP_MAX_PLAYERS], RemotePlayer remotePlayers[MP_MAX_PLAYERS],
		int mpKills[MP_MAX_PLAYERS], u8 mpRespawnSeq[MP_MAX_PLAYERS],
		DeathEffect* deathEffects, int* mpVersusWinnerIdx) {
	int* victimHealth = (hitIdx == 0) ? hostHealth : &mpClientHealth[hitIdx];
	*victimHealth -= VERSUS_DAMAGE;
	if (*victimHealth > 0) return;

	float hitX = (hitIdx == 0) ? *hostPx : remotePlayers[hitIdx].x;
	float hitY = (hitIdx == 0) ? *hostPy : remotePlayers[hitIdx].y;
	spawn_death_effect(deathEffects, hitX, hitY);
	mpKills[shooterIdx]++;

	float rx, ry;
	find_respawn_point(&rx, &ry);
	*victimHealth = PLAYER_MAX_HEALTH;
	if (hitIdx == 0) {
		*hostPx = rx; *hostPy = ry;
	} else {
		remotePlayers[hitIdx].x = rx;
		remotePlayers[hitIdx].y = ry;
		remotePlayers[hitIdx].alive = true;
		mpRespawnSeq[hitIdx]++;
	}

	if (mpKills[shooterIdx] >= VERSUS_KILL_TARGET) *mpVersusWinnerIdx = shooterIdx;
}

// Horizontal screen-space disparity for a point at the given distance.
// Positive means "shift toward this eye's side of center for a near
// object" -- the caller multiplies by +1 for the left eye and -1 for the
// right eye (or vice versa consistently) to get actual per-eye shifts.
// Zero at STEREO_CONVERGE_DIST, grows for nearer/farther points, always
// clamped so a single object can never demand more than
// STEREO_MAX_SHIFT_PX of separation.
static float stereo_shift_px(float dist, float slider3d) {
	if (slider3d <= 0.0f) return 0.0f;
	float d = dist < 0.3f ? 0.3f : dist;
	float depthTerm = (1.0f / d) - (1.0f / STEREO_CONVERGE_DIST);
	float shiftPx = STEREO_STRENGTH_PX * depthTerm * slider3d;
	if (shiftPx > STEREO_MAX_SHIFT_PX) shiftPx = STEREO_MAX_SHIFT_PX;
	if (shiftPx < -STEREO_MAX_SHIFT_PX) shiftPx = -STEREO_MAX_SHIFT_PX;
	return shiftPx;
}

// width of each rendered strip in pixels -- one C2D_DrawImageAt call is
// issued per strip, and citro3d's command buffer can't take 400 individual
// draw calls in one frame (that's what caused earlier crashes), so we
// cast fewer, wider rays instead of one per screen column.
#define RENDER_STRIDE 4

// A genuine per-pixel floor cast (sampling the texture separately for
// every screen pixel below the horizon) isn't practical here -- citro2d
// draws whole images, not individual texels, and the draw-call budget is
// already tight just from the wall strips above (see RENDER_STRIDE). This
// approximates a floor instead: a handful of horizontal bands, each the
// wall texture stretched across the full screen width, darkened more the
// closer a band is to the horizon (farther away). Not perspective-correct
// per column, but cheap (FLOOR_BANDS draw calls per eye) and still reads
// as a textured, shaded floor rather than a flat color.
#define FLOOR_BANDS 8

static void draw_floor(C2D_Image wallImg, float eyeSign, float slider3d) {
	float horizon = SCREEN_H / 2.0f;
	float bandHeight = (SCREEN_H - horizon) / (float)FLOOR_BANDS;
	for (int i = 0; i < FLOOR_BANDS; i++) {
		float y0 = horizon + i * bandHeight;
		float midY = y0 + bandHeight * 0.5f;
		float dist = (SCREEN_H / 2.0f) / (midY - SCREEN_H / 2.0f);
		if (dist > MAX_DEPTH) dist = MAX_DEPTH;

		// always noticeably darker than a wall at the same distance would
		// be (floor should read as clearly "underfoot"), plus it fades
		// further toward the horizon
		float darken = 0.55f + 0.35f * (dist / MAX_DEPTH);
		if (darken > 0.9f) darken = 0.9f;
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, C2D_Color32(0, 0, 0, 255), darken);

		float scaleX = (float)SCREEN_W / (float)wallImg.subtex->width;
		float scaleY = bandHeight / (float)wallImg.subtex->height;
		float drawX = eyeSign * stereo_shift_px(dist, slider3d);
		C2D_DrawImageAt(wallImg, drawX, y0, 0.4f, &tint, scaleX, scaleY);
	}
}

static void draw_frame(float px, float py, float pa, C2D_Image wallImg, u32 waveTintColor,
		float eyeSign, float slider3d) {
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

		// distance + side shading, done as a tint-toward-a-color so the
		// texture detail survives instead of flattening to a solid color.
		// The tint target is this wave's random color instead of plain
		// black, so far walls fade toward a wave-specific mood color
		// while near walls still show mostly-true wall texture. Blend
		// amount depends only on distance (with a floor so the tint is
		// visible even up close) -- it used to also jump for side==1
		// walls, which made N/S walls show the tint strongly while E/W
		// walls barely showed it at all ("only certain directions"). The
		// side-based lighting cue is now a darker TARGET color instead,
		// which doesn't suppress tint visibility.
		float darken = dist / MAX_DEPTH;
		if (darken > 0.85f) darken = 0.85f;
		if (darken < 0.3f) darken = 0.3f;
		u32 tintTarget = waveTintColor;
		if (side == 1) {
			u8 r = (u8)((waveTintColor & 0xFF) * 0.7f);
			u8 g = (u8)(((waveTintColor >> 8) & 0xFF) * 0.7f);
			u8 b = (u8)(((waveTintColor >> 16) & 0xFF) * 0.7f);
			tintTarget = C2D_Color32(r, g, b, 255);
		}
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, tintTarget, darken);

		float stereoX = (float)col + eyeSign * stereo_shift_px(dist, slider3d);
		float stripWidth = (float)RENDER_STRIDE + STEREO_STRIP_OVERLAP_PX;
		C2D_DrawImageAt(colImg, stereoX - STEREO_STRIP_OVERLAP_PX / 2.0f, (float)drawStart, 0.5f, &tint,
			stripWidth, (float)(drawEnd - drawStart) / (float)strip.height);
	}
}

// Angle-to-screen-X uses the same linear mapping draw_frame() uses for
// wall columns (not a tangent-correct projection), so billboarded sprites
// line up with the walls instead of drifting relative to them.
static float billboard_screen_x(float relAngle) {
	return SCREEN_W * (relAngle + FOV / 2.0f) / FOV;
}

static void draw_enemies(Enemy* enemies, float px, float py, float pa, C2D_Image img, bool flipped,
		float eyeSign, float slider3d) {
	float aspect = (float)img.subtex->width / (float)img.subtex->height;
	for (int i = 0; i < MAX_ENEMIES; i++) {
		if (!enemies[i].alive) continue;

		float dx = enemies[i].x - px, dy = enemies[i].y - py;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist < 0.2f || dist > MAX_DEPTH) continue;

		float relAngle = normalize_angle(atan2f(dy, dx) - pa);
		if (fabsf(relAngle) > FOV / 2.0f + 0.3f) continue; // cheap off-screen cull
		if (!has_line_of_sight(px, py, enemies[i].x, enemies[i].y)) continue;

		float screenX = billboard_screen_x(relAngle);
		float height = (SCREEN_H / dist) * 0.5f;
		float scale = height / (float)img.subtex->height;
		float halfWidth = (height * aspect) / 2.0f;

		float shade = 1.0f - dist / MAX_DEPTH;
		if (shade < 0.2f) shade = 0.2f;
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, C2D_Color32(0, 0, 0, 255), 1.0f - shade);

		// citro2d's flip (negative scaleX) is a pure UV swap inside a quad
		// whose geometry is always sized from fabs(scale) -- the quad's
		// screen position never changes based on sign. So the draw
		// position is the same either way; only the scale sign flips.
		// This also means it flips around the sprite's own center, not
		// an edge, which is what we want.
		float drawX = screenX - halfWidth + eyeSign * stereo_shift_px(dist, slider3d);
		float drawScaleX = flipped ? -scale : scale;
		C2D_DrawImageAt(img, drawX, SCREEN_H / 2.0f - height / 2.0f, 0.52f, &tint,
			drawScaleX, scale);
	}
}

// Turquoise blood-splatter droplets on enemy death, radiating outward from
// the death point and fading out over DEATH_EFFECT_DURATION. No source
// art needed -- just a handful of small solid circles per burst.
#define BLOOD_COLOR_R 48
#define BLOOD_COLOR_G 213
#define BLOOD_COLOR_B 200

static void draw_death_effects(DeathEffect* effects, float px, float py, float pa,
		float eyeSign, float slider3d) {
	for (int i = 0; i < MAX_DEATH_EFFECTS; i++) {
		if (!effects[i].active) continue;

		float dx = effects[i].x - px, dy = effects[i].y - py;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist < 0.1f || dist > MAX_DEPTH) continue;
		float relAngle = normalize_angle(atan2f(dy, dx) - pa);
		if (fabsf(relAngle) > FOV / 2.0f + 0.3f) continue;
		if (!has_line_of_sight(px, py, effects[i].x, effects[i].y)) continue;

		float screenX = billboard_screen_x(relAngle) + eyeSign * stereo_shift_px(dist, slider3d);
		float screenY = SCREEN_H / 2.0f;
		float pxPerUnit = SCREEN_H / dist; // same world-unit-to-pixel convention as walls/enemies

		float progress = 1.0f - (effects[i].timer / DEATH_EFFECT_DURATION); // 0 at spawn, 1 at end
		u8 alpha = (u8)(255.0f * (1.0f - progress));

		for (int p = 0; p < BLOOD_PARTICLES; p++) {
			float travel = effects[i].particleDist[p] * progress;
			float dropX = screenX + cosf(effects[i].particleAngle[p]) * travel * pxPerUnit;
			float dropY = screenY + sinf(effects[i].particleAngle[p]) * travel * pxPerUnit;
			float radius = effects[i].particleSize[p] * pxPerUnit;
			if (radius < 0.5f) continue; // too small on screen to bother drawing
			C2D_DrawCircleSolid(dropX, dropY, 0.53f, radius,
				C2D_Color32(BLOOD_COLOR_R, BLOOD_COLOR_G, BLOOD_COLOR_B, alpha));
		}
	}
}

// Placeholder rendering for other players (no avatar art yet) -- flat
// colored rectangles, one color per player slot, billboarded the same way
// as enemies. myIdx is skipped since a player never renders themselves.
static void draw_remote_players(RemotePlayer* players, int myIdx, float px, float py, float pa,
		float eyeSign, float slider3d) {
	for (int i = 0; i < MP_MAX_PLAYERS; i++) {
		if (i == myIdx || !players[i].connected || !players[i].alive) continue;

		float dx = players[i].x - px, dy = players[i].y - py;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist < 0.2f || dist > MAX_DEPTH) continue;

		float relAngle = normalize_angle(atan2f(dy, dx) - pa);
		if (fabsf(relAngle) > FOV / 2.0f + 0.3f) continue;
		if (!has_line_of_sight(px, py, players[i].x, players[i].y)) continue;

		float screenX = billboard_screen_x(relAngle);
		float height = (SCREEN_H / dist) * 0.6f; // a bit taller than enemies -- distinct silhouette
		float width = height * 0.6f;

		float shade = 1.0f - dist / MAX_DEPTH;
		if (shade < 0.3f) shade = 0.3f;
		u32 base = player_color(i);
		u8 r = (u8)((base & 0xFF) * shade);
		u8 g = (u8)(((base >> 8) & 0xFF) * shade);
		u8 b = (u8)(((base >> 16) & 0xFF) * shade);

		float drawX = screenX - width / 2.0f + eyeSign * stereo_shift_px(dist, slider3d);
		C2D_DrawRectSolid(drawX, SCREEN_H / 2.0f - height / 2.0f, 0.515f,
			width, height, C2D_Color32(r, g, b, 255));
	}
}

int main(int argc, char **argv) {
	gfxInitDefault();
	// hid and fs are already brought up by libctru's default __appInit, so
	// hidScanInput() and FSUSER_* calls work with no extra setup here. The
	// C-stick, title lookups, audio, and local multiplayer are separate
	// services that do need their own init.
	irrstInit();
	amInit();
	romfsInit();
	audioReady = R_SUCCEEDED(ndspInit());
	bool mpAvailable = R_SUCCEEDED(udsInit(0x3000, NULL));
	srand((unsigned int)svcGetSystemTick());

	if (audioReady) {
		// music is optional -- silently skipped if audio/music.wav doesn't
		// exist yet, everything else still works
		load_wav("romfs:/audio/music.wav", &sounds[CH_MUSIC]);
		load_wav("romfs:/audio/gunshot.wav", &sounds[CH_GUNSHOT]);
		load_wav("romfs:/audio/reload1.wav", &sounds[CH_RELOAD1]);
		load_wav("romfs:/audio/reload2.wav", &sounds[CH_RELOAD2]);
		setup_channel(CH_MUSIC, &sounds[CH_MUSIC]);
		setup_channel(CH_GUNSHOT, &sounds[CH_GUNSHOT]);
		setup_channel(CH_RELOAD1, &sounds[CH_RELOAD1]);
		setup_channel(CH_RELOAD2, &sounds[CH_RELOAD2]);
		play_sound(CH_MUSIC, &sounds[CH_MUSIC], true);
	}

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();

	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *topRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	// Always render both eye targets every frame (menus/HUD draw identical,
	// zero-parallax content to each; gameplay offsets the raycast camera
	// per eye). gfxSet3D just permits the 3D slider to take effect -- the
	// hardware itself blends down to a mono image when the slider is at 0,
	// so there's no need to branch on slider position here.
	gfxSet3D(true);

	C2D_SpriteSheet uiSheet = C2D_SpriteSheetLoad("romfs:/gfx/sprites.t3x");
	C2D_SpriteSheet wallSheet = C2D_SpriteSheetLoad("romfs:/gfx/walltex.t3x");
	// title.png is a large, full-screen-ish image that doesn't belong
	// crammed into the small-sprite atlas above -- before being resized
	// down, it alone was big enough to push the shared atlas past
	// tex3ds's max page size ("No atlas solution found"). Own standalone
	// texture instead, same treatment as the wall.
	C2D_SpriteSheet titleSheet = C2D_SpriteSheetLoad("romfs:/gfx/title.t3x");

	// idle/idle2 alternate as a 2-frame idle animation; fire1/fire2/reset
	// play once as a 3-frame sequence on each shot; empty is a single
	// static pose. All swapped in for the old single-pose sprites.
	C2D_Image imgGunIdle[2] = {
		C2D_SpriteSheetGetImage(uiSheet, sprites_idle_idx),
		C2D_SpriteSheetGetImage(uiSheet, sprites_idle2_idx),
	};
	C2D_Image imgGunFire[3] = {
		C2D_SpriteSheetGetImage(uiSheet, sprites_fire1_idx),
		C2D_SpriteSheetGetImage(uiSheet, sprites_fire2_idx),
		C2D_SpriteSheetGetImage(uiSheet, sprites_reset_idx),
	};
	C2D_Image imgGunEmpty     = C2D_SpriteSheetGetImage(uiSheet, sprites_empty_idx);
	C2D_Image imgCrosshair    = C2D_SpriteSheetGetImage(uiSheet, sprites_crosshair_idx);
	(void)imgCrosshair; // drawing is disabled below until it's aligned to the barrel
	C2D_Image imgHudLoaded    = C2D_SpriteSheetGetImage(uiSheet, sprites_loadedgunonhud_idx);
	C2D_Image imgHudUnloaded  = C2D_SpriteSheetGetImage(uiSheet, sprites_unloadedgunonhud_idx);
	C2D_Image imgEnemy        = C2D_SpriteSheetGetImage(uiSheet, sprites_enemy_idx);
	C2D_Image imgTitle        = C2D_SpriteSheetGetImage(titleSheet, title_idx);
	C2D_Image imgWall         = C2D_SpriteSheetGetImage(wallSheet, walltex_idx);
	// The scope/vignette HUD overlay (gfx/hud.png, gfx/hud.t3s) is on hold
	// -- not currently loaded or drawn -- until a less obtrusive graphic
	// replaces it. The asset and its standalone-texture pipeline are still
	// in place, so wiring it back in later is just re-adding the load +
	// draw calls.

	C2D_TextBuf textBuf = C2D_TextBufNew(1024);

	GameScreen screen = SCREEN_MENU;
	Immersion immersion = IMM_BASE;
	bool paused = false;

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

	// Counts UP from 0 while a fire animation is playing (0 when idle),
	// used to pick which of fire1/fire2/reset to show. Also still gates
	// the old rapid-refire lockout behavior via GunState/ammo checks
	// elsewhere -- unrelated to this timer directly.
	float fireAnimTimer = -1.0f;
	float animClock = 0.0f;
	float gunLowerOffset = 0.0f; // eased toward GUN_LOWER_AMOUNT when not STATE_READY
	float gunSwayPhase = 0.0f;
	float gunSwayX = 0.0f, gunSwayY = 0.0f; // computed in the update phase, read back in the (separate) render phase below

	Enemy enemies[MAX_ENEMIES];
	memset(enemies, 0, sizeof(enemies));
	DeathEffect deathEffects[MAX_DEATH_EFFECTS];
	memset(deathEffects, 0, sizeof(deathEffects));
	int health = PLAYER_MAX_HEALTH;
	int wave = 1;
	int kills = 0;
	int score = 0;
	// Tracks whether the player has taken damage during the current wave,
	// for the no-damage wave-clear bonus -- reset when a new wave starts.
	bool waveDamageTaken = false;
	int enemiesQuotaThisWave = 0;
	int enemiesSpawnedThisWave = 0;
	float spawnTimer = 0.0f;
	bool waveBreak = false;
	float waveBreakTimer = 0.0f;
	int finalWave = 0;
	u32 waveTintColor = C2D_Color32(0, 0, 0, 255);

	// --- multiplayer state ---
	MpRole mpRole = MP_OFF;
	udsBindContext mpBindCtx;
	bool mpBound = false;
	u16 mpMyNodeID = UDS_HOST_NETWORKNODEID;
	float mpSendTimer = 0.0f;
	bool mpPendingFire = false;
	RemotePlayer remotePlayers[MP_MAX_PLAYERS];
	memset(remotePlayers, 0, sizeof(remotePlayers));
	int mpClientHealth[MP_MAX_PLAYERS]; // host's authoritative HP tracking for each client slot
	memset(mpClientHealth, 0, sizeof(mpClientHealth));
	char mpErrorMsg[64] = "";
	MpMode mpMode = MP_MODE_COOP; // chosen at SCREEN_MP_MENU, host-only choice
	int mpKills[MP_MAX_PLAYERS];        // versus: host-authoritative per-player kill counts
	memset(mpKills, 0, sizeof(mpKills));
	u8 mpRespawnSeq[MP_MAX_PLAYERS];    // versus, host-side: bumped whenever that slot respawns
	memset(mpRespawnSeq, 0, sizeof(mpRespawnSeq));
	u8 mpMyLastRespawnSeq = 0;          // versus, client-side: last respawnSeq we've already applied
	int mpVersusWinnerIdx = -1;         // versus: who won, for the game-over screen (-1 = n/a)

	u64 lastTime = svcGetSystemTick();

	while (aptMainLoop()) {
		hidScanInput();
		irrstScanInput();

		u64 now = svcGetSystemTick();
		float dt = (float)(now - lastTime) / (float)SYSCLOCK_ARM11;
		if (dt > 0.1f) dt = 0.1f; // clamp on hiccups
		lastTime = now;

		u32 kDown = hidKeysDown();
		// START quits from every screen except SCREEN_PLAYING, where it
		// pauses instead (handled in that branch below) -- quitting mid-run
		// by accident is a much worse mistake than quitting from a menu.
		if ((kDown & KEY_START) && screen != SCREEN_PLAYING) break;

		if (screen == SCREEN_MENU) {
			if (kDown & KEY_SELECT) {
				immersion = (Immersion)((immersion + 1) % IMM_COUNT);
			}
			if (mpAvailable && (kDown & KEY_X)) {
				mpErrorMsg[0] = '\0';
				screen = SCREEN_MP_MENU;
			}
			if (kDown & KEY_R) {
				// start a single-player run -- immersion is now locked until death
				px = 4.5f; py = 4.5f; pa = 0.0f;
				health = PLAYER_MAX_HEALTH;
				wave = 1;
				kills = 0;
				score = 0;
				waveDamageTaken = false;
				enemiesQuotaThisWave = 2 + wave;
				enemiesSpawnedThisWave = 0;
				spawnTimer = 1.0f;
				waveBreak = false;
				waveTintColor = random_wave_color();
				memset(enemies, 0, sizeof(enemies));
				memset(deathEffects, 0, sizeof(deathEffects));
				magCount = 0; // full mode starts each run with fresh magazines
				hasLoadedCart = false;
				loadedCartId = 0;
				loadedMagIdx = -1;
				ammo = 0;
				gunState = STATE_RELOADING;
				FSUSER_CardSlotIsInserted(&prevCardInserted);
				fireAnimTimer = -1.0f;
				animClock = 0.0f;
				paused = false;
				gunLowerOffset = 0.0f;
				gunSwayPhase = 0.0f;
				mpRole = MP_OFF;
				mpMode = MP_MODE_COOP;
				screen = SCREEN_PLAYING;
			}
		} else if (screen == SCREEN_MP_MENU) {
			if (kDown & KEY_Y) {
				screen = SCREEN_MENU;
			}
			// Mode only matters to whoever hosts -- a client just inherits
			// it from PKT_HOST_START -- but it's harmless to let anyone
			// toggle it here before they know which role they'll take.
			if (kDown & KEY_SELECT) {
				mpMode = (mpMode == MP_MODE_COOP) ? MP_MODE_VERSUS : MP_MODE_COOP;
			}
			if (kDown & KEY_A) {
				if (mp_host_create(&mpBindCtx)) {
					mpRole = MP_HOST;
					mpBound = true;
					mpMyNodeID = UDS_HOST_NETWORKNODEID;
					memset(remotePlayers, 0, sizeof(remotePlayers));
					memset(mpClientHealth, 0, sizeof(mpClientHealth));
					memset(mpKills, 0, sizeof(mpKills));
					memset(mpRespawnSeq, 0, sizeof(mpRespawnSeq));
					mpVersusWinnerIdx = -1;
					mpErrorMsg[0] = '\0';
					screen = SCREEN_MP_LOBBY;
				} else {
					snprintf(mpErrorMsg, sizeof(mpErrorMsg), "Failed to create network. Try again.");
				}
			}
			if (kDown & KEY_B) {
				if (mp_client_scan_and_connect(&mpBindCtx)) {
					mpRole = MP_CLIENT;
					mpBound = true;
					udsConnectionStatus constatus;
					if (R_SUCCEEDED(udsGetConnectionStatus(&constatus))) {
						mpMyNodeID = constatus.cur_NetworkNodeID;
					}
					memset(remotePlayers, 0, sizeof(remotePlayers));
					mpErrorMsg[0] = '\0';
					screen = SCREEN_MP_LOBBY;
				} else {
					snprintf(mpErrorMsg, sizeof(mpErrorMsg), "No host found nearby. Try again.");
				}
			}
		} else if (screen == SCREEN_MP_LOBBY) {
			if (kDown & KEY_Y) {
				if (mpRole == MP_HOST) udsDestroyNetwork();
				else if (mpRole == MP_CLIENT) udsDisconnectNetwork();
				if (mpBound) { udsUnbind(&mpBindCtx); mpBound = false; }
				mpRole = MP_OFF;
				screen = SCREEN_MP_MENU;
			} else if (mpRole == MP_HOST) {
				if (kDown & KEY_R) {
					// redundant sends -- UDS isn't guaranteed-delivery, and
					// this is the one signal every client absolutely must
					// receive to leave the lobby
					MpStartPacket startPkt = { PKT_HOST_START, (u8)mpMode };
					for (int i = 0; i < 5; i++) {
						udsSendTo(UDS_BROADCAST_NETWORKNODEID, MP_DATA_CHANNEL, UDS_SENDFLAG_Default,
							&startPkt, sizeof(startPkt));
					}
					if (mpMode == MP_MODE_VERSUS) {
						find_respawn_point(&px, &py);
					} else {
						px = 4.5f; py = 4.5f;
					}
					pa = 0.0f;
					health = PLAYER_MAX_HEALTH;
					wave = 1;
					kills = 0;
					score = 0;
					waveDamageTaken = false;
					enemiesQuotaThisWave = 2 + wave;
					enemiesSpawnedThisWave = 0;
					spawnTimer = 1.0f;
					waveBreak = false;
					waveTintColor = random_wave_color();
					memset(enemies, 0, sizeof(enemies));
					memset(deathEffects, 0, sizeof(deathEffects));
					memset(mpKills, 0, sizeof(mpKills));
					memset(mpRespawnSeq, 0, sizeof(mpRespawnSeq));
					mpMyLastRespawnSeq = 0;
					mpVersusWinnerIdx = -1;
					// node_bitmask: bit i is set for NetworkNodeID (i+1), which is
					// player slot i in our arrays -- this is the actual source of
					// truth for who's connected right now, since clients don't
					// send anything of their own while sitting in the lobby
					udsConnectionStatus constatus;
					memset(&constatus, 0, sizeof(constatus));
					udsGetConnectionStatus(&constatus);
					for (int i = 1; i < MP_MAX_PLAYERS; i++) {
						bool connected = (constatus.node_bitmask & (1 << i)) != 0;
						remotePlayers[i].connected = connected;
						remotePlayers[i].alive = connected;
						mpClientHealth[i] = connected ? PLAYER_MAX_HEALTH : 0;
						if (mpMode == MP_MODE_VERSUS && connected) {
							float rx, ry;
							find_respawn_point(&rx, &ry);
							remotePlayers[i].x = rx;
							remotePlayers[i].y = ry;
						}
					}
					magCount = 0;
					hasLoadedCart = false;
					loadedCartId = 0;
					loadedMagIdx = -1;
					ammo = 0;
					gunState = STATE_RELOADING;
					FSUSER_CardSlotIsInserted(&prevCardInserted);
					fireAnimTimer = -1.0f;
					animClock = 0.0f;
					paused = false;
					gunLowerOffset = 0.0f;
					gunSwayPhase = 0.0f;
					mpSendTimer = 0.0f;
					screen = SCREEN_PLAYING;
				}
			} else if (mpRole == MP_CLIENT) {
				// drain incoming packets looking for the host's start signal
				u8 buf[UDS_DATAFRAME_MAXSIZE];
				size_t actualSize = 0;
				u16 srcNodeID = 0;
				for (int guard = 0; guard < 16; guard++) {
					Result r = udsPullPacket(&mpBindCtx, buf, sizeof(buf), &actualSize, &srcNodeID);
					if (R_FAILED(r) || actualSize == 0) break;
					if (actualSize >= sizeof(MpStartPacket) && buf[0] == PKT_HOST_START) {
						MpStartPacket startPkt;
						memcpy(&startPkt, buf, sizeof(startPkt));
						mpMode = (MpMode)startPkt.mode;
						if (mpMode == MP_MODE_VERSUS) {
							find_respawn_point(&px, &py);
						} else {
							px = 4.5f; py = 4.5f;
						}
						pa = 0.0f;
						health = PLAYER_MAX_HEALTH;
						wave = 1;
						kills = 0;
						score = 0;
						waveDamageTaken = false;
						memset(enemies, 0, sizeof(enemies));
						memset(deathEffects, 0, sizeof(deathEffects));
						memset(remotePlayers, 0, sizeof(remotePlayers));
						mpMyLastRespawnSeq = 0;
						mpVersusWinnerIdx = -1;
						magCount = 0;
						hasLoadedCart = false;
						loadedCartId = 0;
						loadedMagIdx = -1;
						ammo = 0;
						gunState = STATE_RELOADING;
						FSUSER_CardSlotIsInserted(&prevCardInserted);
						fireAnimTimer = -1.0f;
						animClock = 0.0f;
						paused = false;
						gunLowerOffset = 0.0f;
						gunSwayPhase = 0.0f;
						mpSendTimer = 0.0f;
						screen = SCREEN_PLAYING;
						break;
					}
				}
			}
		} else if (screen == SCREEN_PLAYING) {
			if (kDown & KEY_START) paused = !paused;

			if (paused) {
				// Y quits out to the menu from here instead of START, since
				// START now means "resume" -- everything else (movement,
				// firing, wave/enemy sim, cart-slot reload, networking) is
				// simply skipped below while paused.
				if (kDown & KEY_Y) {
					if (mpRole == MP_HOST) udsDestroyNetwork();
					else if (mpRole == MP_CLIENT) udsDisconnectNetwork();
					if (mpBound) { udsUnbind(&mpBindCtx); mpBound = false; }
					mpRole = MP_OFF;
					mpMode = MP_MODE_COOP;
					mpVersusWinnerIdx = -1;
					paused = false;
					screen = SCREEN_MENU;
				}
			} else {

			animClock += dt;

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

			// Movement and the cartridge-reload state machine below are
			// fully local regardless of multiplayer role -- each player's
			// own position and their own cart slot are their own business.
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
				play_sound(CH_RELOAD1, &sounds[CH_RELOAD1], false);
			} else if (gunState == STATE_RELOADING && !prevCardInserted && cardInserted) {
				if (immersion == IMM_BASE) {
					// base mode never needs to identify the cart, so it can
					// reload the instant it's back in -- no need for the
					// settle window that medium/full require below
					ammo = MAG_SIZE;
					hasLoadedCart = true;
					gunState = STATE_READY;
					play_sound(CH_RELOAD2, &sounds[CH_RELOAD2], false);
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
						play_sound(CH_RELOAD2, &sounds[CH_RELOAD2], false);
					} else {
						gunState = STATE_RELOADING;
					}
				}
			}
			prevCardInserted = cardInserted;

			if ((kDown & KEY_R) && gunState == STATE_READY && ammo > 0) {
				ammo--;
				if (immersion == IMM_FULL && loadedMagIdx >= 0) mags[loadedMagIdx].ammo = ammo;
				fireAnimTimer = 0.0f;
				play_sound(CH_GUNSHOT, &sounds[CH_GUNSHOT], false);

				if (mpRole == MP_CLIENT) {
					// the host owns the enemy/player state in multiplayer --
					// report the shot instead of resolving it locally, or
					// things would desync
					mpPendingFire = true;
				} else if (mpRole == MP_HOST && mpMode == MP_MODE_VERSUS) {
					float allX[MP_MAX_PLAYERS], allY[MP_MAX_PLAYERS];
					bool allAlive[MP_MAX_PLAYERS];
					allX[0] = px; allY[0] = py; allAlive[0] = health > 0;
					for (int p = 1; p < MP_MAX_PLAYERS; p++) {
						allX[p] = remotePlayers[p].x; allY[p] = remotePlayers[p].y;
						allAlive[p] = remotePlayers[p].connected && mpClientHealth[p] > 0;
					}
					int hitIdx = find_versus_hit(allX, allY, allAlive, 0, px, py, pa);
					if (hitIdx >= 0) {
						versus_apply_hit(0, hitIdx, &health, &px, &py, mpClientHealth, remotePlayers,
							mpKills, mpRespawnSeq, deathEffects, &mpVersusWinnerIdx);
					}
				} else {
					// single-player or co-op host: resolve the shot against
					// the local (and, if hosting, authoritative) enemy list
					try_hitscan(enemies, deathEffects, px, py, pa, wave,
						immersion_score_multiplier(immersion), &kills, &score);
				}
			}
			if (fireAnimTimer >= 0.0f) {
				fireAnimTimer += dt;
				if (fireAnimTimer >= GUN_FIRE_DURATION) fireAnimTimer = -1.0f;
			}

			// Weapon lower/raise: eases toward a lowered position whenever
			// the gun isn't ready to fire, and back up to normal otherwise.
			float gunLowerTarget = (gunState == STATE_READY) ? 0.0f : GUN_LOWER_AMOUNT;
			gunLowerOffset += (gunLowerTarget - gunLowerOffset) * fminf(1.0f, GUN_LOWER_SPEED * dt);

			// DOOM-style weapon sway: phase advances only while actually
			// moving (scaled by how much), so it settles back to a neutral
			// pose instead of freezing mid-swing when you stop.
			float moveMagnitude = fminf(1.0f, sqrtf(moveInput * moveInput + strafeInput * strafeInput));
			gunSwayPhase += moveMagnitude * GUN_SWAY_SPEED * dt;
			gunSwayX = sinf(gunSwayPhase) * GUN_SWAY_AMOUNT_X * moveMagnitude;
			gunSwayY = fabsf(sinf(gunSwayPhase * 2.0f)) * GUN_SWAY_AMOUNT_Y * moveMagnitude;

			for (int i = 0; i < MAX_DEATH_EFFECTS; i++) {
				if (!deathEffects[i].active) continue;
				deathEffects[i].timer -= dt;
				if (deathEffects[i].timer <= 0.0f) deathEffects[i].active = false;
			}

			if (mpRole != MP_CLIENT && mpMode == MP_MODE_COOP) {
				// single-player and co-op host both run the real
				// simulation -- waves, enemy AI, and melee against the
				// local player. Host additionally does the same against
				// connected remote players further below. None of this
				// applies to versus -- no enemies, no waves, just players
				// hitting each other (handled where shots are fired,
				// above).
				if (waveBreak) {
					waveBreakTimer -= dt;
					if (waveBreakTimer <= 0.0f) {
						wave++;
						enemiesQuotaThisWave = 2 + wave;
						enemiesSpawnedThisWave = 0;
						waveBreak = false;
						waveTintColor = random_wave_color();
						waveDamageTaken = false;
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
					int scoreMult = immersion_score_multiplier(immersion);
					score += SCORE_PER_WAVE_CLEAR * wave * scoreMult;
					if (!waveDamageTaken) score += SCORE_NO_DAMAGE_WAVE * wave * scoreMult;
				}

				for (int i = 0; i < MAX_ENEMIES; i++) {
					if (!enemies[i].alive) continue;
					float dx = px - enemies[i].x, dy = py - enemies[i].y;
					float dist = sqrtf(dx * dx + dy * dy);
					if (dist < ENEMY_MELEE_RANGE) {
						enemies[i].alive = false;
						health--;
						waveDamageTaken = true;
						spawn_death_effect(deathEffects, enemies[i].x, enemies[i].y);
						if (health <= 0 && mpRole == MP_OFF) {
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

				if (mpRole == MP_HOST) {
					// melee against connected remote players
					for (int i = 0; i < MAX_ENEMIES; i++) {
						if (!enemies[i].alive) continue;
						for (int p = 1; p < MP_MAX_PLAYERS; p++) {
							if (!remotePlayers[p].connected || mpClientHealth[p] <= 0) continue;
							float dx = remotePlayers[p].x - enemies[i].x, dy = remotePlayers[p].y - enemies[i].y;
							if (sqrtf(dx * dx + dy * dy) < ENEMY_MELEE_RANGE) {
								enemies[i].alive = false;
								mpClientHealth[p]--;
								remotePlayers[p].alive = mpClientHealth[p] > 0;
								spawn_death_effect(deathEffects, enemies[i].x, enemies[i].y);
								break;
							}
						}
					}
				}
			}

			// --- multiplayer networking tick ---
			if (mpRole == MP_HOST) {
				u8 buf[UDS_DATAFRAME_MAXSIZE];
				size_t actualSize = 0;
				u16 srcNodeID = 0;
				for (int guard = 0; guard < 16; guard++) {
					Result r = udsPullPacket(&mpBindCtx, buf, sizeof(buf), &actualSize, &srcNodeID);
					if (R_FAILED(r) || actualSize == 0) break;
					if (actualSize >= sizeof(MpInputPacket) && buf[0] == PKT_CLIENT_INPUT) {
						MpInputPacket pkt;
						memcpy(&pkt, buf, sizeof(pkt));
						int idx = (int)srcNodeID - 1;
						// connected/health were already seeded from the real UDS
						// connection state when the host started the match (see
						// SCREEN_MP_LOBBY) -- health only ever decreases from
						// here via melee, never auto-heals on a packet arriving,
						// or a client who died would get silently resurrected
						// the next time their input packet showed up
						if (idx >= 1 && idx < MP_MAX_PLAYERS && remotePlayers[idx].connected) {
							remotePlayers[idx].x = pkt.px;
							remotePlayers[idx].y = pkt.py;
							if (pkt.firing && mpClientHealth[idx] > 0) {
								if (mpMode == MP_MODE_VERSUS) {
									float allX[MP_MAX_PLAYERS], allY[MP_MAX_PLAYERS];
									bool allAlive[MP_MAX_PLAYERS];
									allX[0] = px; allY[0] = py; allAlive[0] = health > 0;
									for (int p = 1; p < MP_MAX_PLAYERS; p++) {
										allX[p] = remotePlayers[p].x; allY[p] = remotePlayers[p].y;
										allAlive[p] = remotePlayers[p].connected && mpClientHealth[p] > 0;
									}
									int hitIdx = find_versus_hit(allX, allY, allAlive, idx, pkt.px, pkt.py, pkt.pa);
									if (hitIdx >= 0) {
										// this is the one legitimate case where a client's
										// health goes back up from here -- a versus respawn,
										// not a desync
										versus_apply_hit(idx, hitIdx, &health, &px, &py, mpClientHealth,
											remotePlayers, mpKills, mpRespawnSeq, deathEffects, &mpVersusWinnerIdx);
									}
								} else {
									try_hitscan(enemies, deathEffects, pkt.px, pkt.py, pkt.pa, wave,
										immersion_score_multiplier(immersion), &kills, &score);
								}
							}
						}
					}
				}

				mpSendTimer -= dt;
				if (mpSendTimer <= 0.0f) {
					mpSendTimer = MP_TICK_INTERVAL;

					bool allDead = false;
					if (mpMode == MP_MODE_COOP) {
						allDead = (health <= 0);
						for (int p = 1; p < MP_MAX_PLAYERS; p++) {
							if (remotePlayers[p].connected && mpClientHealth[p] > 0) allDead = false;
						}
					}
					bool versusWon = (mpMode == MP_MODE_VERSUS && mpVersusWinnerIdx >= 0);

					MpStatePacket statePkt;
					memset(&statePkt, 0, sizeof(statePkt));
					statePkt.type = PKT_HOST_STATE;
					statePkt.wave = (u8)wave;
					statePkt.waveTintColor = waveTintColor;
					statePkt.gameOver = (allDead || versusWon) ? 1 : 0;
					statePkt.finalWave = (u8)wave;
					statePkt.versusWinnerIdx = (mpVersusWinnerIdx >= 0) ? (u8)mpVersusWinnerIdx : 0xFF;
					statePkt.players[0].x = px;
					statePkt.players[0].y = py;
					statePkt.players[0].pa = pa;
					statePkt.players[0].health = (u8)(health > 0 ? health : 0);
					statePkt.players[0].alive = health > 0 ? 1 : 0;
					statePkt.players[0].connected = 1;
					statePkt.players[0].kills = (u8)mpKills[0];
					statePkt.players[0].respawnSeq = mpRespawnSeq[0];
					for (int p = 1; p < MP_MAX_PLAYERS; p++) {
						statePkt.players[p].x = remotePlayers[p].x;
						statePkt.players[p].y = remotePlayers[p].y;
						statePkt.players[p].health = (u8)(mpClientHealth[p] > 0 ? mpClientHealth[p] : 0);
						statePkt.players[p].alive = mpClientHealth[p] > 0 ? 1 : 0;
						statePkt.players[p].connected = remotePlayers[p].connected ? 1 : 0;
						statePkt.players[p].kills = (u8)mpKills[p];
						statePkt.players[p].respawnSeq = mpRespawnSeq[p];
					}
					for (int i = 0; i < MAX_ENEMIES; i++) {
						statePkt.enemies[i].x = enemies[i].x;
						statePkt.enemies[i].y = enemies[i].y;
						statePkt.enemies[i].alive = enemies[i].alive ? 1 : 0;
					}
					udsSendTo(UDS_BROADCAST_NETWORKNODEID, MP_DATA_CHANNEL, UDS_SENDFLAG_Default,
						&statePkt, sizeof(statePkt));

					if (allDead || versusWon) {
						finalWave = wave;
						screen = SCREEN_GAMEOVER;
					}
				}
			} else if (mpRole == MP_CLIENT) {
				mpSendTimer -= dt;
				if (mpSendTimer <= 0.0f) {
					mpSendTimer = MP_TICK_INTERVAL;
					MpInputPacket inputPkt = { PKT_CLIENT_INPUT, px, py, pa, mpPendingFire ? (u8)1 : (u8)0 };
					udsSendTo(UDS_BROADCAST_NETWORKNODEID, MP_DATA_CHANNEL, UDS_SENDFLAG_Default,
						&inputPkt, sizeof(inputPkt));
					mpPendingFire = false;
				}

				u8 buf[UDS_DATAFRAME_MAXSIZE];
				size_t actualSize = 0;
				u16 srcNodeID = 0;
				for (int guard = 0; guard < 16; guard++) {
					Result r = udsPullPacket(&mpBindCtx, buf, sizeof(buf), &actualSize, &srcNodeID);
					if (R_FAILED(r) || actualSize == 0) break;
					if (actualSize >= sizeof(MpStatePacket) && buf[0] == PKT_HOST_STATE) {
						MpStatePacket statePkt;
						memcpy(&statePkt, buf, sizeof(statePkt));

						wave = statePkt.wave;
						waveTintColor = statePkt.waveTintColor;
						for (int i = 0; i < MAX_ENEMIES; i++) {
							enemies[i].x = statePkt.enemies[i].x;
							enemies[i].y = statePkt.enemies[i].y;
							enemies[i].alive = statePkt.enemies[i].alive != 0;
						}
						int myIdx = mpMyNodeID - 1;
						if (myIdx >= 0 && myIdx < MP_MAX_PLAYERS) {
							health = statePkt.players[myIdx].health;
							// Position is normally self-reported (we're the ones who
							// told the host where we are), but a versus respawn is
							// the one case the host has to override it -- checking
							// for a changed sequence number (rather than a one-shot
							// flag) means we'll still catch it on a later packet
							// even if the specific tick it changed on got dropped.
							if (statePkt.players[myIdx].respawnSeq != mpMyLastRespawnSeq) {
								px = statePkt.players[myIdx].x;
								py = statePkt.players[myIdx].y;
								mpMyLastRespawnSeq = statePkt.players[myIdx].respawnSeq;
							}
						}
						for (int p = 0; p < MP_MAX_PLAYERS; p++) {
							remotePlayers[p].x = statePkt.players[p].x;
							remotePlayers[p].y = statePkt.players[p].y;
							remotePlayers[p].alive = statePkt.players[p].alive != 0;
							remotePlayers[p].connected = statePkt.players[p].connected != 0;
							mpKills[p] = statePkt.players[p].kills;
						}
						mpVersusWinnerIdx = (statePkt.versusWinnerIdx == 0xFF) ? -1 : (int)statePkt.versusWinnerIdx;
						if (statePkt.gameOver) {
							finalWave = statePkt.finalWave;
							screen = SCREEN_GAMEOVER;
						}
					}
				}
			}
			} // end of "if (!paused)"
		} else if (screen == SCREEN_GAMEOVER) {
			if (kDown & KEY_R) {
				if (mpRole == MP_HOST) udsDestroyNetwork();
				else if (mpRole == MP_CLIENT) udsDisconnectNetwork();
				if (mpBound) { udsUnbind(&mpBindCtx); mpBound = false; }
				mpRole = MP_OFF;
				mpMode = MP_MODE_COOP;
				mpVersusWinnerIdx = -1;
				screen = SCREEN_MENU;
			}
		}

		// --- render ---
		// C3D_FrameBegin binds the low-level GPU command buffer -- without
		// it, every GPUCMD write below (including C2D_TargetClear's own
		// clear command) goes through a null pointer.
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		// osGet3DSliderState() ranges 0.0 (off) .. 1.0 (max depth). The raycast
		// itself runs identically for both eyes (same px/py/pa -- there's no
		// camera move here); each eye's screen-space disparity per object is
		// computed from that object's own distance via stereo_shift_px().
		float slider3d = osGet3DSliderState();

		if (screen == SCREEN_MENU) {
			// top screen: the title art (already includes the "CART RIDGE"
			// lettering, so no separately-drawn text over it)
			C2D_TargetClear(top, C2D_Color32(10, 10, 20, 255));
			C2D_SceneBegin(top);
			float titleScale = SCREEN_H / (float)imgTitle.subtex->height;
			float titleDrawW = imgTitle.subtex->width * titleScale;
			C2D_DrawImageAt(imgTitle, (SCREEN_W - titleDrawW) / 2.0f, 0.0f, 0.5f, NULL,
				titleScale, titleScale);

			// right eye: identical, zero-parallax -- this is a flat title
			// card, not a 3D scene, so both eyes just see the same image
			C2D_TargetClear(topRight, C2D_Color32(10, 10, 20, 255));
			C2D_SceneBegin(topRight);
			C2D_DrawImageAt(imgTitle, (SCREEN_W - titleDrawW) / 2.0f, 0.0f, 0.5f, NULL,
				titleScale, titleScale);

			// bottom screen: tagline, immersion selector, controls
			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);
			C2D_TextBufClear(textBuf);

			C2D_Text subText;
			C2D_TextParse(&subText, textBuf,
				"Pull the cartridge to reload. Survive the waves.");
			C2D_TextOptimize(&subText);
			C2D_DrawText(&subText, C2D_WithColor, 10.0f, 15.0f, 0.5f, 0.45f, 0.45f,
				C2D_Color32(180, 180, 180, 255));

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
				"R: start solo run (locks the level until you die)\n");
			C2D_TextOptimize(&promptText);
			C2D_DrawText(&promptText, C2D_WithColor, 10.0f, 100.0f, 0.5f, 0.45f, 0.45f,
				C2D_Color32(200, 200, 200, 255));

			C2D_Text prompt2Text;
			C2D_TextParse(&prompt2Text, textBuf,
				mpAvailable ? "X: local multiplayer\nSTART: quit" : "START: quit");
			C2D_TextOptimize(&prompt2Text);
			C2D_DrawText(&prompt2Text, C2D_WithColor, 10.0f, 150.0f, 0.5f, 0.45f, 0.45f,
				C2D_Color32(200, 200, 200, 255));
		} else if (screen == SCREEN_MP_MENU) {
			C2D_TargetClear(top, C2D_Color32(10, 10, 20, 255));
			C2D_SceneBegin(top);
			C2D_TextBufClear(textBuf);

			C2D_Text titleText;
			C2D_TextParse(&titleText, textBuf, "LOCAL MULTIPLAYER");
			C2D_TextOptimize(&titleText);
			C2D_DrawText(&titleText, C2D_WithColor, 60.0f, 90.0f, 0.5f, 0.9f, 0.9f,
				C2D_Color32(255, 255, 255, 255));

			C2D_TargetClear(topRight, C2D_Color32(10, 10, 20, 255));
			C2D_SceneBegin(topRight);
			C2D_DrawText(&titleText, C2D_WithColor, 60.0f, 90.0f, 0.5f, 0.9f, 0.9f,
				C2D_Color32(255, 255, 255, 255));

			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);

			char mpMenuLine[128];
			snprintf(mpMenuLine, sizeof(mpMenuLine),
				"A: Host a game (up to 4 players)\n"
				"B: Join a nearby game\n"
				"SELECT: mode -- %s\n"
				"Y: back",
				mpMode == MP_MODE_VERSUS ? "VERSUS" : "CO-OP");
			C2D_Text menuText;
			C2D_TextParse(&menuText, textBuf, mpMenuLine);
			C2D_TextOptimize(&menuText);
			C2D_DrawText(&menuText, C2D_WithColor, 10.0f, 30.0f, 0.5f, 0.5f, 0.5f,
				C2D_Color32(220, 220, 220, 255));

			if (mpErrorMsg[0]) {
				C2D_Text errText;
				C2D_TextParse(&errText, textBuf, mpErrorMsg);
				C2D_TextOptimize(&errText);
				C2D_DrawText(&errText, C2D_WithColor, 10.0f, 120.0f, 0.5f, 0.45f, 0.45f,
					C2D_Color32(255, 140, 140, 255));
			}
		} else if (screen == SCREEN_MP_LOBBY) {
			udsConnectionStatus constatus;
			memset(&constatus, 0, sizeof(constatus));
			udsGetConnectionStatus(&constatus);

			C2D_TargetClear(top, C2D_Color32(10, 10, 20, 255));
			C2D_SceneBegin(top);
			C2D_TextBufClear(textBuf);

			C2D_Text titleText;
			C2D_TextParse(&titleText, textBuf, mpRole == MP_HOST ? "HOSTING" : "CONNECTED");
			C2D_TextOptimize(&titleText);
			C2D_DrawText(&titleText, C2D_WithColor, 100.0f, 60.0f, 0.5f, 1.0f, 1.0f,
				C2D_Color32(120, 255, 160, 255));

			char line[64];
			C2D_Text countText;
			snprintf(line, sizeof(line), "%d / %d players -- %s", constatus.total_nodes, MP_MAX_PLAYERS,
				mpMode == MP_MODE_VERSUS ? "VERSUS" : "CO-OP");
			C2D_TextParse(&countText, textBuf, line);
			C2D_TextOptimize(&countText);
			C2D_DrawText(&countText, C2D_WithColor, 60.0f, 120.0f, 0.5f, 0.6f, 0.6f,
				C2D_Color32(255, 255, 255, 255));

			C2D_TargetClear(topRight, C2D_Color32(10, 10, 20, 255));
			C2D_SceneBegin(topRight);
			C2D_DrawText(&titleText, C2D_WithColor, 100.0f, 60.0f, 0.5f, 1.0f, 1.0f,
				C2D_Color32(120, 255, 160, 255));
			C2D_DrawText(&countText, C2D_WithColor, 60.0f, 120.0f, 0.5f, 0.6f, 0.6f,
				C2D_Color32(255, 255, 255, 255));

			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);

			C2D_Text promptText;
			C2D_TextParse(&promptText, textBuf, mpRole == MP_HOST
				? "SELECT: change your immersion level\nR: start the game\nY: cancel hosting"
				: "SELECT: change your immersion level\nWaiting for the host to start...\nY: disconnect");
			C2D_TextOptimize(&promptText);
			C2D_DrawText(&promptText, C2D_WithColor, 10.0f, 30.0f, 0.5f, 0.45f, 0.45f,
				C2D_Color32(220, 220, 220, 255));
		} else if (screen == SCREEN_PLAYING) {
			bool cardInserted = prevCardInserted; // set above this frame
			bool enemiesFlipped = fmodf(animClock, ENEMY_FLIP_PERIOD * 2.0f) >= ENEMY_FLIP_PERIOD;
			int myIdx = (mpRole == MP_OFF) ? -1 : (int)mpMyNodeID - 1;

			// top screen: the raycast view + enemies + other players + viewmodel,
			// rendered once per eye with the camera shifted perpendicular to
			// facing direction for stereo parallax. The gun viewmodel is
			// drawn identically to both eyes (no shift) -- it's rendered as
			// a screen-space overlay rather than a world object, and giving
			// it its own parallax would need a separate near-plane
			// convergence to look right instead of just uncomfortable.
			C2D_Image gunImg;
			if (gunState != STATE_READY) {
				gunImg = imgGunEmpty;
			} else if (fireAnimTimer >= 0.0f) {
				int frame = (int)(fireAnimTimer * GUN_ANIM_FPS);
				if (frame >= GUN_FIRE_FRAME_COUNT) frame = GUN_FIRE_FRAME_COUNT - 1;
				gunImg = imgGunFire[frame];
			} else {
				bool idle2 = fmodf(animClock, GUN_FRAME_PERIOD * 2.0f) >= GUN_FRAME_PERIOD;
				gunImg = imgGunIdle[idle2 ? 1 : 0];
			}
			// The new sprites are already framed as a full viewmodel pose
			// within their own canvas (unlike the old, tightly-cropped
			// ones), so no scaling is needed -- just anchor the sprite's
			// own bottom-right corner to the screen's. A small rightward
			// nudge pushes the sprite's own right edge just past the
			// visible screen area, since it was otherwise landing exactly
			// on-screen and showing as a hard edge.
			const float gunScale = 1.0f;
			const float gunOffsetX = 10.0f;
			const float gunOffsetY = 10.0f;
			float gunDrawX = SCREEN_W - gunImg.subtex->width * gunScale + gunOffsetX + gunSwayX;
			float gunDrawY = SCREEN_H - gunImg.subtex->height * gunScale + gunOffsetY + gunLowerOffset + gunSwayY;

			// eyeSign: left eye (physical GFX_LEFT target) gets +1, right eye
			// gets -1. Near objects (closer than STEREO_CONVERGE_DIST) then
			// shift right in the left-eye image and left in the right-eye
			// image -- "crossed" disparity, which is the correct/comfortable
			// direction for near depth (matches how your eyes naturally
			// converge on something close in real life). Getting this
			// backwards is what produced the reversed, nauseating effect.
			C2D_Text pausedText, pausedHintText;
			if (paused) {
				C2D_TextBufClear(textBuf);
				C2D_TextParse(&pausedText, textBuf, "PAUSED");
				C2D_TextOptimize(&pausedText);
				C2D_TextParse(&pausedHintText, textBuf, "START: resume   Y: quit to menu");
				C2D_TextOptimize(&pausedHintText);
			}
			for (int eye = 0; eye < 2; eye++) {
				C3D_RenderTarget *eyeTarget = (eye == 0) ? top : topRight;
				float eyeSign = (eye == 0) ? 1.0f : -1.0f;

				C2D_TargetClear(eyeTarget, C2D_Color32(10, 10, 15, 255));
				C2D_SceneBegin(eyeTarget);
				draw_floor(imgWall, eyeSign, slider3d);
				draw_frame(px, py, pa, imgWall, waveTintColor, eyeSign, slider3d);
				draw_enemies(enemies, px, py, pa, imgEnemy, enemiesFlipped, eyeSign, slider3d);
				draw_death_effects(deathEffects, px, py, pa, eyeSign, slider3d);
				if (mpRole != MP_OFF) draw_remote_players(remotePlayers, myIdx, px, py, pa, eyeSign, slider3d);

				// No separate muzzle-flash overlay -- the fire1/fire2/reset
				// frames already show it as part of the gun sprite itself.
				C2D_DrawImageAt(gunImg, gunDrawX, gunDrawY, 0.6f, NULL, gunScale, gunScale);

				if (paused) {
					C2D_DrawRectSolid(0.0f, 0.0f, 0.7f, (float)SCREEN_W, (float)SCREEN_H, C2D_Color32(0, 0, 0, 140));
					C2D_DrawText(&pausedText, C2D_WithColor, 140.0f, 90.0f, 0.71f, 1.5f, 1.5f,
						C2D_Color32(255, 255, 255, 255));
					C2D_DrawText(&pausedHintText, C2D_WithColor, 55.0f, 140.0f, 0.71f, 0.5f, 0.5f,
						C2D_Color32(200, 200, 200, 255));
				}
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
			if (mpRole != MP_OFF && mpMode == MP_MODE_VERSUS) {
				int myKills = (myIdx >= 0 && myIdx < MP_MAX_PLAYERS) ? mpKills[myIdx] : 0;
				snprintf(line, sizeof(line), "HP %d/%d   Kills %d/%d   %s", health, PLAYER_MAX_HEALTH,
					myKills, VERSUS_KILL_TARGET, mpRole == MP_HOST ? "HOST" : "CLIENT");
			} else if (mpRole == MP_OFF) {
				snprintf(line, sizeof(line), "Wave %d   HP %d/%d   Score %d", wave, health,
					PLAYER_MAX_HEALTH, score);
			} else {
				snprintf(line, sizeof(line), "Wave %d   HP %d/%d   %s", wave, health,
					PLAYER_MAX_HEALTH, mpRole == MP_HOST ? "HOST" : "CLIENT");
			}
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
				"START: pause");
			C2D_TextOptimize(&helpText);
			C2D_DrawText(&helpText, C2D_WithColor, 10.0f, 180.0f, 0.5f, 0.4f, 0.4f,
				C2D_Color32(180, 180, 180, 255));
		} else { // SCREEN_GAMEOVER
			bool versusOver = (mpRole != MP_OFF && mpMode == MP_MODE_VERSUS);
			int myIdx = (mpRole == MP_OFF) ? -1 : (int)mpMyNodeID - 1;

			C2D_TargetClear(top, C2D_Color32(20, 10, 10, 255));
			C2D_SceneBegin(top);
			C2D_TextBufClear(textBuf);

			char line[64];
			C2D_Text overText;
			C2D_TextParse(&overText, textBuf, versusOver
				? ((mpVersusWinnerIdx == myIdx) ? "YOU WIN!" : "GAME OVER")
				: "GAME OVER");
			C2D_TextOptimize(&overText);
			C2D_DrawText(&overText, C2D_WithColor, 100.0f, 80.0f, 0.5f, 1.2f, 1.2f,
				C2D_Color32(255, 120, 120, 255));

			C2D_Text waveText;
			if (versusOver) {
				if (mpVersusWinnerIdx == 0) {
					snprintf(line, sizeof(line), "Host wins with %d kills", VERSUS_KILL_TARGET);
				} else if (mpVersusWinnerIdx > 0) {
					snprintf(line, sizeof(line), "Player %d wins with %d kills",
						mpVersusWinnerIdx + 1, VERSUS_KILL_TARGET);
				} else {
					snprintf(line, sizeof(line), "Match ended");
				}
			} else {
				snprintf(line, sizeof(line), "You reached wave %d", finalWave);
			}
			C2D_TextParse(&waveText, textBuf, line);
			C2D_TextOptimize(&waveText);
			C2D_DrawText(&waveText, C2D_WithColor, 90.0f, 140.0f, 0.5f, 0.55f, 0.55f,
				C2D_Color32(220, 220, 220, 255));

			C2D_TargetClear(topRight, C2D_Color32(20, 10, 10, 255));
			C2D_SceneBegin(topRight);
			C2D_DrawText(&overText, C2D_WithColor, 100.0f, 80.0f, 0.5f, 1.2f, 1.2f,
				C2D_Color32(255, 120, 120, 255));
			C2D_DrawText(&waveText, C2D_WithColor, 90.0f, 140.0f, 0.5f, 0.55f, 0.55f,
				C2D_Color32(220, 220, 220, 255));

			C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
			C2D_SceneBegin(bottom);

			if (versusOver) {
				C2D_Text scoreboardText;
				char board[160];
				int n = 0;
				n += snprintf(board + n, sizeof(board) - n, "Kills\n");
				n += snprintf(board + n, sizeof(board) - n, "Host: %d\n", mpKills[0]);
				for (int p = 1; p < MP_MAX_PLAYERS; p++) {
					if (!remotePlayers[p].connected && p != myIdx) continue;
					n += snprintf(board + n, sizeof(board) - n, "Player %d: %d\n", p + 1, mpKills[p]);
				}
				C2D_TextParse(&scoreboardText, textBuf, board);
				C2D_TextOptimize(&scoreboardText);
				C2D_DrawText(&scoreboardText, C2D_WithColor, 10.0f, 20.0f, 0.5f, 0.55f, 0.55f,
					C2D_Color32(255, 255, 255, 255));
			} else {
				C2D_Text scoreText;
				snprintf(line, sizeof(line), "Score: %d", score);
				C2D_TextParse(&scoreText, textBuf, line);
				C2D_TextOptimize(&scoreText);
				C2D_DrawText(&scoreText, C2D_WithColor, 10.0f, 20.0f, 0.5f, 0.7f, 0.7f,
					C2D_Color32(255, 220, 120, 255));

				C2D_Text killsText;
				snprintf(line, sizeof(line), "Kills: %d   Mode: %s", kills, immersion_name(immersion));
				C2D_TextParse(&killsText, textBuf, line);
				C2D_TextOptimize(&killsText);
				C2D_DrawText(&killsText, C2D_WithColor, 10.0f, 65.0f, 0.5f, 0.55f, 0.55f,
					C2D_Color32(255, 255, 255, 255));
			}

			C2D_Text retryText;
			C2D_TextParse(&retryText, textBuf, "R: return to menu\nSTART: quit");
			C2D_TextOptimize(&retryText);
			C2D_DrawText(&retryText, C2D_WithColor, 10.0f, 105.0f, 0.5f, 0.45f, 0.45f,
				C2D_Color32(200, 200, 200, 255));
		}

		C3D_FrameEnd(0);
	}

	if (mpRole == MP_HOST) udsDestroyNetwork();
	else if (mpRole == MP_CLIENT) udsDisconnectNetwork();
	if (mpBound) udsUnbind(&mpBindCtx);
	if (mpAvailable) udsExit();

	for (int i = 0; i < 4; i++) {
		if (sounds[i].data) linearFree(sounds[i].data);
	}
	if (audioReady) ndspExit();

	C2D_TextBufDelete(textBuf);
	C2D_SpriteSheetFree(wallSheet);
	C2D_SpriteSheetFree(uiSheet);
	C2D_SpriteSheetFree(titleSheet);
	C2D_Fini();
	C3D_Fini();
	romfsExit();
	amExit();
	gfxExit();
	return 0;
}
