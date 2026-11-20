/*
 * Super Mario World 2: Yoshi's Island — desktop host shim.
 *
 * Identity and hooks, nothing else. The host itself is the framework's
 * (snesrecomp/runner/src/desktop/host_main.h): the pre-boot launcher, ROM
 * resolution and digest checks, config.ini and keybinds.ini, the window and
 * the SDL / OpenGL presenters, audio, gamepads, the in-game save-state
 * browser and rewind filmstrip, the OSD, the pacing clock, crash handlers and
 * the post-mortem report, mod packages, Generate & rebuild, and the netplay
 * barrier when the project is built with it. A fix there reaches this
 * project on a submodule pull; nothing in this file needs to change for it.
 *
 * This file used to BE the host -- a ~700-line copy made at scaffold time --
 * so every project scaffolded before a fix kept the bug. Two ports of the
 * same game, a week apart, had different hosts.
 *
 * Nothing here ever ships a ROM. The player supplies one; the host makes
 * that easy (launcher, then positional argument, then a copy beside the
 * executable, then the rom.cfg cache, then a file picker) and checks what it
 * is handed against the digests in rom_identity.txt.
 *
 * Grow this file only with what is specific to THIS title: a custom
 * presenter (prepare_frame / draw_frame), an SPC player, a pacing rule
 * (keep_pacing_debt), a Mods provider. See SnesDesktopHostGame for the
 * complete list of hooks and what each is for.
 */

#include "host_main.h"
#include "game_rtl.h"
#include "snesrecomp_rom_identity.h"  /* generated from rom_identity.txt */

#ifndef __ANDROID__
#define SDL_MAIN_HANDLED 1
#endif
#include "desktop/sdl_compat.h"
#ifdef __ANDROID__
/* SDLActivity loads libmain.so and dlsyms its SDL_main. */
#define main SDL_main
#endif

#ifndef SNES_GAME_VERSION
#define SNES_GAME_VERSION "dev"
#endif

#include "config.h"
#include "smw2_update.h"
#include "common_rtl.h"
#include "widescreen.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/cart.h"
#include "snes/superfx.h"
#include "snes/ws_shadow.h"
#include "common_cpu_infra.h"
#include <string.h>
#include <stdlib.h>

#ifdef RECOMP_LAUNCHER
#include "recomp_launcher.h"
/* The host switches to OpenGL whenever a shader is selected; say so where the
 * player picks the renderer. The update check gets its checkbox here too. */
static void Smw2ConfigureLauncher(RecompLauncherCGameInfo *info)
{
    info->renderer_note = "Shaders need OpenGL: choosing one switches the renderer to OpenGL.";
#ifdef _WIN32
    info->host_toggle_label = "Check for updates on start";
    info->host_toggle_help =
        "Look for a newer release on GitHub when the game starts and offer to "
        "install it. Saves, save states and settings are kept.";
    info->host_toggle_get = Smw2UpdateGetEnabled;
    info->host_toggle_set = Smw2UpdateSetEnabled;
#endif
}
#endif

#if defined(_WIN32) && !defined(SMW2_CONSOLE)
#include <io.h>
#include <stdio.h>
#undef HIBYTE  /* the framework's types.h has its own; this file does not use it */
#undef LOBYTE
#include <windows.h>
#include <wchar.h>

/* The Windows build is a window app (no console, see CMakeLists.txt). Keep the
 * host's stdout/stderr diagnostics in <exe>.log beside the executable unless
 * the caller already redirected them. */
static void Smw2RedirectLogsToFile(void)
{
    /* A caller that gave us a pipe (RecompLauncher reads stdout for the
     * setup's progress lines) keeps it; only a missing stream goes to the log. */
    const int have_err = GetStdHandle(STD_ERROR_HANDLE) != NULL;
    const int have_out = GetStdHandle(STD_OUTPUT_HANDLE) != NULL;
    if (have_err)
        return;
    wchar_t base[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, base, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 8)
        return;
    wchar_t *dot = wcsrchr(base, L'.');
    if (dot) *dot = L'\0';
    /* Keep the last few runs: X.log is this run, X.1.log the one before. */
    wchar_t from[MAX_PATH], to[MAX_PATH];
    for (int i = 3; i >= 1; --i) {
        if (i == 1) swprintf(from, MAX_PATH, L"%ls.log", base);
        else swprintf(from, MAX_PATH, L"%ls.%d.log", base, i - 1);
        swprintf(to, MAX_PATH, L"%ls.%d.log", base, i);
        DeleteFileW(to);
        MoveFileW(from, to);
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls.log", base);
    FILE *log;
    if (_wfreopen_s(&log, path, L"w", stderr) != 0)
        return;
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!have_out && _dup2(_fileno(stderr), 1) == 0)
        setvbuf(stdout, NULL, _IONBF, 0);
}
#endif

/* GLSL presets (assets/shaders) render only on the OpenGL presenter, so make
 * it this title's default. A fresh install starts with no shader: the plain
 * picture costs the GPU almost nothing, while a full-screen CRT preset made
 * weaker laptops drop frames. Whatever the player picks in the launcher is
 * saved to config.ini and left alone. */
static void Smw2AfterConfig(void)
{
    if (g_config.renderer[0] == '\0') {
        strcpy(g_config.renderer, "opengl");
        g_config.output_method = kOutputMethod_OpenGL;
    }
    Smw2UpdateCheckOnStart(SNES_GAME_VERSION);
}

/* Widescreen prototype: with [Graphics] Widescreen = 1 the PPU draws a 16:9
 * field (398 columns, square pixels, as the MMX ports do) and the game's own
 * 256 columns sit in the middle. */
static void Smw2PrepareFrame(int dw, int dh, int *w, int *h)
{
    (void)dw; (void)dh;
    *w = g_config.widescreen ? 398 : 256;
    *h = 224;
}

/* Widescreen policy, per frame. In a stage (game mode $7E0118 = $0F) the
 * view anchors to the level: no columns left of the level's left edge
 * (camera X, $7E0039, is 0 there), the spare columns go to the right. Every
 * other screen (map, menus, bonus games, story) is the native 256 columns,
 * centred with black bars. */
/* The whole stage is in memory while it plays: a 16x8 grid of 256x256
 * screens, $70:0CAA giving each screen's slot ($80 = none), and each slot
 * 16x16 map16 block IDs at $7F8000 + slot*$200. A block's four 8x8 BG1
 * tilemap entries are in ROM at $0C33F2 + table[id >> 8] + (id & $FF)*8,
 * with the per-page table at $0C32A4 (file offsets, headerless). The
 * widescreen margins draw BG1 from that, so they show the stage as it is
 * instead of whatever the 64x32 VRAM map last held there. */
static uint16_t Smw2RomWord(uint32_t off)
{
    const Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || !cart->rom || off + 1 >= cart->romSize) return 0;
    return (uint16_t)(cart->rom[off] | cart->rom[off + 1] << 8);
}

static WsShadowSource Smw2Bg1World(void *context, uint32_t wtx, uint32_t wty,
                                   uint16_t *entry)
{
    (void)context;
    const Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || !cart->ram || cart->ramSize < 0x0CAA + 128) return kWsShadowMiss;
    const uint32_t cx = wtx >> 1, cy = wty >> 1;        /* 16x16 block */
    uint16_t id = 0;
    if (cx < 256 && cy < 128) {
        const uint8_t slot = cart->ram[0x0CAA + (cy >> 4) * 16 + (cx >> 4)];
        if (slot != 0x80) {
            const uint32_t a = 0x18000 + slot * 0x200 + ((cy & 15) * 16 + (cx & 15)) * 2;
            if (a + 1 < sizeof(g_ram)) id = (uint16_t)(g_ram[a] | g_ram[a + 1] << 8);
        }
    }
    const uint32_t off = 0x0C33F2 + Smw2RomWord(0x0C32A4 + (id >> 8) * 2) +
                         (id & 0xFF) * 8 + ((wty & 1) * 2 + (wtx & 1)) * 2;
    *entry = Smw2RomWord(off);
    return kWsShadowExact;
}

/* Sprite spawning. $03:9596 hands the Super FX a scan column at camera X +
 * table[$73] and a Y band at camera Y + table[$75], from the table at
 * $03:958E: +$120 / -$30 (X, moving right / left), +$110 / -$20 (Y). Level
 * sprites on that column come to life. With a wider view the column moves
 * out by the widest margin, so they are alive before they reach the screen.
 * All code here is interpreted and reads this table from ROM each time. */
static void Smw2PatchWord(Cart *cart, uint32_t off, uint16_t v)
{
    if (off + 2 < cart->romSize) { cart->rom[off] = (uint8_t)v; cart->rom[off + 1] = (uint8_t)(v >> 8); }
}

/* Sprite despawn. $03:A299/A2B0/A2C7 test, and $03:A2DE/A2F8 act on, a
 * sprite's camera-relative X ($7680,X) against three windows: dx+$100 < $300,
 * dx+$80 < $200, dx+$40 < $180. Outside, $03:A31E removes it (and frees it to
 * spawn again). Widescreen widens each window by the margin on both sides,
 * so a sprite spawned for the margin is not removed on the spot. Y is left
 * alone. */
static void Smw2SetSpawnColumns(bool wide)
{
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || !cart->rom || cart->romSize < 0x48C93) return;
    const uint16_t e = wide ? 0x90 : 0;
    uint16_t right = wide ? 0x01B0 : 0x0120;
    uint16_t left = wide ? (uint16_t)-0x00C0 : (uint16_t)-0x0030;
    const char *er = getenv("SNESRECOMP_WS_SPAWN_RIGHT"), *el = getenv("SNESRECOMP_WS_SPAWN_LEFT");
    if (wide && er) right = (uint16_t)strtol(er, NULL, 0);   /* experiments */
    if (wide && el) left = (uint16_t)-strtol(el, NULL, 0);
    Smw2PatchWord(cart, 0x1958E, right);                    /* $03:958E */
    Smw2PatchWord(cart, 0x19590, left);
    static const struct { uint16_t adc, cmp, base_adc, base_cmp; } kWin[] = {
        { 0xA29D, 0xA2A0, 0x0100, 0x0300 },
        { 0xA2B4, 0xA2B7, 0x0080, 0x0200 },
        { 0xA2CB, 0xA2CE, 0x0040, 0x0180 },
        { 0xA2E2, 0xA2E5, 0x0080, 0x0200 },
        { 0xA2FC, 0xA2FF, 0x0040, 0x0180 },
    };
    /* The Super FX sprite loop ($09:89C1) keeps a sprite while
     * -m <= dx <= m + $F0, m from the per-class table at $09:8C83 (X margins
     * $60, $90, $90 for classes 4, 8, 12); outside, $09:8A2F clears its slot.
     * That is the limit that removed margin-spawned sprites on the spot. */
    static const struct { uint32_t off; uint16_t base; } kFxMargin[] = {
        { 0x48C87, 0x0060 }, { 0x48C8B, 0x0090 }, { 0x48C8F, 0x0090 },
    };
    for (size_t i = 0; i < sizeof(kFxMargin) / sizeof(kFxMargin[0]); i++)
        Smw2PatchWord(cart, kFxMargin[i].off, kFxMargin[i].base + e);
    for (size_t i = 0; i < sizeof(kWin) / sizeof(kWin[0]); i++) {
        Smw2PatchWord(cart, 0x18000 + (kWin[i].adc - 0x8000) + 1, kWin[i].base_adc + e);
        Smw2PatchWord(cart, 0x18000 + (kWin[i].cmp - 0x8000) + 1, kWin[i].base_cmp + 2 * e);
    }
}

/* OAM build ($08:B1D8, Super FX): a sprite tile goes to OAM only while
 * -15 <= x < $100 (CMP R7 / IWT R8,#$0100 / CMP R8 at $08:B219, R7 = -15
 * is shared with the Y test), and X bit 8 is set only for a negative x.
 * With widescreen the X test becomes one unsigned compare in the same 12
 * bytes, x + K < W with K = 15 + left margin and W = 271 + both margins
 * (IWT R8,#K / WITH R8 ADD R0 / IWT R9,#W / WITH R8 SUB R9 / BCS; the
 * INC R6 after it runs in the delay slot on both paths, which is harmless:
 * R6 is reloaded from R1 for the next tile). Bit 8 becomes (x >> 8) & 1
 * (8 bytes at $08:B227: MOVE R11,R0 / FROM R0 TO R9 HIB / WITH R9 AND #1),
 * so x in either margin is written as the raw 9-bit X the renderer places
 * there. Margins follow the live view every frame. */
static void Smw2SetOamClip(bool wide, int left_margin, int right_margin)
{
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || !cart->rom || cart->romSize < 0x43230) return;
    static const uint8_t kOrigX[12] = { 0x3F, 0x67, 0x07, 0x58, 0x01, 0xF8, 0x00, 0x01, 0x3F, 0x68, 0x06, 0x50 };
    static const uint8_t kOrigHi[8] = { 0xA9, 0x00, 0x2B, 0xB0, 0x0A, 0x02, 0x01, 0xD9 };
    static const uint8_t kWideHi[8] = { 0x2B, 0xB0, 0xB0, 0x19, 0xC0, 0x29, 0x3E, 0x71 };
    uint8_t *x = cart->rom + 0x43219, *hi = cart->rom + 0x43227;
    if (!wide) { memcpy(x, kOrigX, 12); memcpy(hi, kOrigHi, 8); return; }
    const uint16_t k = (uint16_t)(15 + left_margin);
    const uint16_t w = (uint16_t)(256 + 15 + left_margin + right_margin);
    const uint8_t code[12] = { 0xF8, (uint8_t)k, (uint8_t)(k >> 8), 0x28, 0x50,
                               0xF9, (uint8_t)w, (uint8_t)(w >> 8), 0x28, 0x69, 0x0D, 0x50 };
    memcpy(x, code, 12);
    memcpy(hi, kWideHi, 8);
}

/* Offset-per-tile terrain (6-4's rising and falling pillars). The Super FX
 * routine at $08:9DCF (R8 = camera X, R9 = camera Y) builds the Mode 2 rows
 * for the 32 native columns: the vertical row ends up at $70:1F32..$1F70 as
 * camera Y + a per-column lift from the terrain objects in view. Nothing
 * outside those 32 columns is ever computed, so the margins drew the pillars
 * where the level map has them. When the routine starts, it is replayed
 * twice on a private copy of Game Pak RAM with the camera moved 256 left
 * and right; the two results are the 32 columns on each side, published to
 * the renderer as the margin row. The real core is never driven. */
enum { kOptFirst = -32, kOptCount = 96 };
static uint16_t s_opt_row[kOptCount];
static bool s_opt_used;
static int s_opt_shift;
static uint16_t s_opt_cam;

static void Smw2OptShiftCamera(SuperFx *fx, uint32_t pc24, void *ctx)
{
    (void)pc24; (void)ctx;
    superfx_set_reg(fx, 8, (uint16_t)(superfx_reg(fx, 8) + s_opt_shift));
}

static void Smw2OptRoutine(SuperFx *fx, uint32_t pc24, void *ctx)
{
    (void)ctx;
    static uint8_t *ram;
    static SuperFx result;
    if (!fx->ram || fx->ram_size < 0x2000) return;
    if (!ram) ram = (uint8_t *)malloc(fx->ram_size);
    if (!ram) return;
    const SuperFxReplayPcHook hook = { pc24, Smw2OptShiftCamera, NULL };
    s_opt_cam = superfx_reg(fx, 8);
    /* The row is built one frame ahead of the scroll it is drawn with, so
     * the renderer may shift it by a column and reach into the native part:
     * keep that part too (side 1), next to the left and right ones. */
    for (int side = 0; side < 3; side++) {
        s_opt_shift = (side - 1) * 256;
        memcpy(ram, fx->ram, fx->ram_size);
        if (!superfx_replay_snapshot_with_hooks(fx, ram, &result, &hook, 1)) return;
        for (int i = 0; i < 32; i++) {
            const int k = (side - 1) * 32 + i - kOptFirst;
            s_opt_row[k] = (uint16_t)(ram[0x1F32 + 2 * i] | ram[0x1F33 + 2 * i] << 8);
        }
    }
    s_opt_used = true;
}

static void Smw2InstallOptHooks(void)
{
    static bool done;
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (done || !cart || !cart->superfx) return;
    superfx_set_enhancement_mode(cart->superfx, kSuperFxEnhancement_PresentationReplay);
    superfx_set_pc_hook(cart->superfx, 0x089DCF, Smw2OptRoutine, NULL);
    done = true;
}

static void Smw2BeginFrame(unsigned number)
{
    (void)number;
    static int s_spawn_wide = -1;
    if (s_spawn_wide != (int)g_ws_active) {
        Smw2SetSpawnColumns(g_ws_active);
        s_spawn_wide = g_ws_active;
    }
    if (!g_ws_active)
        return;
    const int budget = g_ws_extra;
    PpuSetWidescreenViewAnchor(g_ppu, true);
    static int s_log = -1;
    if (s_log < 0) s_log = getenv("WS_LOG") != NULL;
    if (s_log) {
        static unsigned n;
        fprintf(stderr, "[wslog] %u mode=%02X tm=%02X ts=%02X tmw=%02X tsw=%02X wsel=%06X w1=%u-%u w2=%u-%u cgw=%02X cga=%02X\n",
                n++, g_ram[0x118], g_ppu->screenEnabled[0], g_ppu->screenEnabled[1],
                g_ppu->screenWindowed[0], g_ppu->screenWindowed[1], (unsigned)g_ppu->windowsel,
                g_ppu->window1left, g_ppu->window1right, g_ppu->window2left, g_ppu->window2right,
                g_ppu->cgwsel, g_ppu->cgadsub);
    }
    static bool s_was_level;
    const bool level = g_ram[0x118] == 0x0F;
    if (level) {
        const int cam_x = g_ram[0x39] | g_ram[0x3A] << 8;
        const int cam_y = g_ram[0x3B] | g_ram[0x3C] << 8;
        /* Level edges: the outermost screen columns that exist in the rows
         * the view spans ($70:0CAA, $80 = no screen). */
        int lvl_left = 0, lvl_right = 16 * 256;
        const Cart *lc = g_snes ? g_snes->cart : NULL;
        if (lc && lc->ram && lc->ramSize >= 0x0CAA + 128) {
            int r0 = cam_y >> 8, r1 = (cam_y + 223) >> 8, cmin = 16, cmax = -1;
            for (int r = r0; r <= r1 && r < 8; r++)
                for (int c = 0; c < 16; c++)
                    if (r >= 0 && lc->ram[0x0CAA + r * 16 + c] != 0x80) {
                        if (c < cmin) cmin = c;
                        if (c > cmax) cmax = c;
                    }
            if (cmax >= 0) { lvl_left = cmin * 256; lvl_right = (cmax + 1) * 256; }
        }
        int room_l = cam_x - lvl_left, room_r = lvl_right - (cam_x + 256);
        if (room_l < 0) room_l = 0;
        if (room_r < 0) room_r = 0;
        int left = room_l < budget ? room_l : budget;
        int right = room_r < 2 * budget - left ? room_r : 2 * budget - left;
        if (left < 2 * budget - right) left = room_l < 2 * budget - right ? room_l : 2 * budget - right;
        /* Iris and wipe transitions (stage start and end, death) mask the
         * layers with window 1, edges rewritten per line by HDMA; play has
         * TMW set but no layer window enabled (windowsel 0). The windows only
         * cover the native 256 columns, so black bars while a layer window
         * is on, or while the colour window clips to black. */
        const bool windowed = (g_ppu->screenWindowed[0] | g_ppu->screenWindowed[1]) &&
                              (g_ppu->windowsel & 0xAAAAAA) != 0;
        (void)windowed;   /* message boxes use layer windows too: not a transition */
        if (PPU_clipMode(g_ppu) != 0) left = right = 0;
        PpuSetExtraSideSpace(g_ppu, left, right, 0);
        Smw2SetOamClip(true, left, right);
        Smw2InstallOptHooks();
        if (s_log) fprintf(stderr, "[wsopt] used=%d bgmode=%02X row0=%04X row40=%04X row60=%04X\n",
                           (int)s_opt_used, g_ppu->bgmode, s_opt_row[0], s_opt_row[40], s_opt_row[60]);
        if (s_opt_used && (g_ppu->bgmode & 7) == 2)
            PpuSetWidescreenOptMarginV(g_ppu, kOptFirst, s_opt_row, kOptCount, s_opt_cam);
        else
            PpuSetWidescreenOptMarginV(g_ppu, 0, NULL, 0, 0);
        s_opt_used = false;
        /* In a stage BG3 is scenery (the foreground plants of 1-1), not a
         * status bar: let it fill the margins too. */
        PpuSetWidescreenBg3Widen(g_ppu, 1);
        /* BG1 scroll is the camera, aliased by the 512x256 map. */
        int32_t wx = cam_x, wy = cam_y;
        int32_t dx = (g_ppu->hScroll[0] - wx) & 511, dy = (g_ppu->vScroll[0] - wy) & 255;
        if (dx >= 256) dx -= 512;
        if (dy >= 128) dy -= 256;
        wx += dx; wy += dy;
        if (wx >= 0 && wy >= 0) {
            WsShadowSetWorld(0, (uint32_t)wx, (uint32_t)wy);
            WsShadowSetScroll(0, g_ppu->hScroll[0], g_ppu->vScroll[0]);
            WsShadowSetWorldProvider(0, Smw2Bg1World, NULL, false);
            WsShadowSetBlankTile(0, -1);
            WsShadowSetRespectGameWrites(0, 60);
        }
    } else {
        PpuSetExtraSideSpace(g_ppu, 0, 0, 0);
        Smw2SetOamClip(true, 0, 0);
        if (s_was_level) WsShadowReset();
    }
    s_was_level = level;
    WsShadowFrame(g_ppu);
}

/* Columns outside the live margins are not drawn this frame (pillarbox):
 * clear them, or they keep the last picture that reached them. */
static void Smw2EndFrame(const uint8_t *field, unsigned number)
{
    (void)number;
    if (!g_ws_active || !field) return;
    const int fw = 256 + 2 * g_ws_extra;
    const int l0 = g_ppu->extraFrameLeft - g_ppu->extraLeftCur;
    const int r0 = g_ppu->extraFrameLeft + 256 + g_ppu->extraRightCur;
    if (l0 <= 0 && r0 >= fw) return;
    uint8_t *px = (uint8_t *)field;
    for (int y = 0; y < 224; y++) {
        uint8_t *row = px + (size_t)y * fw * 4;
        if (l0 > 0) memset(row, 0, (size_t)l0 * 4);
        if (r0 < fw) memset(row + (size_t)r0 * 4, 0, (size_t)(fw - r0) * 4);
    }
}

static const SnesDesktopHostGame kGameHost = {
    .display_name        = "Super Mario World 2: Yoshi's Island",
    .window_title        = "Super Mario World 2: Yoshi's Island",
    .region              = SNESRECOMP_ROM_REGION,
    .rom_file            = SNESRECOMP_ROM_FILE,
    .expected_sha256_hex = SNESRECOMP_ROM_EXPECTED_SHA256,
    .expected_crc32_hex  = SNESRECOMP_ROM_EXPECTED_CRC32,
    .game_id             = SNESRECOMP_ROM_GAME_ID,
    .build_version       = SNES_GAME_VERSION,
    .game_info           = &kGameInfo,
    .num_players         = 1,
    .shader_supported    = 1,
    .after_config        = Smw2AfterConfig,
    .widescreen_supported = 1,
    .native_widescreen   = 1,
    .prepare_frame       = Smw2PrepareFrame,
    .begin_sim_frame     = Smw2BeginFrame,
    .end_sim_frame       = Smw2EndFrame,
#ifdef RECOMP_LAUNCHER
    .configure_launcher  = Smw2ConfigureLauncher,
#endif
    /* Battery-backed SRAM shows the launcher's SAVES panel. Leave NULL for a
     * title without one. The path is exe-relative. */
    .sram_path           = "saves/save.srm",
    /* Ctrl+L (or Select+L3) reopens the launcher mid-game; shader, scale,
     * volume and bindings apply on RESUME without a restart. */
    .in_game_launcher    = 1,
};

#ifdef _WIN32
/* Laptops with switchable graphics (NVIDIA Optimus, AMD PowerXpress) run an
 * unknown program on the integrated GPU, which cannot keep a full-screen
 * shader at 60 fps. The drivers read these exports from the executable and
 * pick the discrete GPU instead. */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

#ifndef __ANDROID__
#undef main   /* desktop: keep plain main() even if SDL_main.h remapped it */
#endif
int main(int argc, char **argv)
{
#if defined(_WIN32) && !defined(SMW2_CONSOLE)
    Smw2RedirectLogsToFile();
#endif
    return snesrecomp_desktop_main(&kGameHost, argc, argv);
}
