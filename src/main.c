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
#include "snes/ws_shadow.h"
#include "common_cpu_infra.h"
#include <string.h>

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
static void Smw2SetSpawnColumns(bool wide)
{
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || !cart->rom || cart->romSize < 0x19596) return;
    const uint16_t right = wide ? 0x0170 : 0x0120;
    const uint16_t left = wide ? (uint16_t)-0x0060 : (uint16_t)-0x0030;
    uint8_t *t = cart->rom + 0x1958E;               /* $03:958E */
    t[0] = (uint8_t)right; t[1] = (uint8_t)(right >> 8);
    t[2] = (uint8_t)left;  t[3] = (uint8_t)(left >> 8);
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
    static bool s_was_level;
    const bool level = g_ram[0x118] == 0x0F;
    if (level) {
        const int cam_x = g_ram[0x39] | g_ram[0x3A] << 8;
        const int cam_y = g_ram[0x3B] | g_ram[0x3C] << 8;
        const int left = cam_x < budget ? cam_x : budget;
        PpuSetExtraSideSpace(g_ppu, left, 2 * budget - left, 0);
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
        if (s_was_level) WsShadowReset();
    }
    s_was_level = level;
    WsShadowFrame(g_ppu);
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
