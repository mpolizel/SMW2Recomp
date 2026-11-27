/*
 * Yoshi's Island -- fan hacks and MSU-1 packs, applied in memory.
 *
 * Both are chosen in the launcher's Settings, EXTRAS card:
 *
 *   Fan hacks  The player picks the folder where the hacks' zips were
 *              unzipped; every hack below that is found there (by the
 *              SHA-256 of its .ips, any folder depth) becomes a checkbox:
 *                - No Crying, Improved SFX and Red Coins (Trisma), as its
 *                  three individual patches;
 *                - Life Saver (racknae);
 *                - Yoshi Color Fix 3.4;
 *                - SFC/PAL controller button colors.
 *   MSU-1      A pack made for Conn's MSU-1 patch: the player picks the
 *              folder holding smw2_msu_us_1.0.ips and the smw2_msu1-<N>.pcm
 *              tracks. The cartridge's music comes from the pack instead, as
 *              on an emulator or an FXPak.
 *
 * Only the USA 1.0 versions of these patches (the ROM this build needs)
 * are listed in kPatches, Header and Headerless alike; anything else in
 * the folder -- the patches for other revisions, regions or the SNES Mini
 * -- is left alone. The choices live in extras.ini beside the executable.
 *
 * When the game starts, after the ROM passed its own SHA-256 check, the host
 * hands it to Smw2ReplaceRom, which applies what is on to a copy in memory --
 * the ROM file is never changed -- in kPatches order, MSU-1 last. None of the
 * supported patches writes the same bytes as another (Life Saver and Color
 * Fix both rewrite the header checksum, which nothing reads). The combined
 * "No Crying, Improved SFX and Red Coins" file is not used: it overwrites
 * 619 bytes of Color Fix, while its three individual patches do not. This
 * works because all of this title's code runs in the interpreter
 * (recomp/bank00.cfg forces the vectors to LLE), so patched bytes are
 * executed as written. The assist tools' ROM codes check the original byte
 * before swapping it, so a hack that changed the same spot is left alone.
 *
 * Nothing on: the game is the plain build.
 *
 * Save states and the rewind filmstrip do not record the MSU-1 player, so
 * after one is loaded the track the pack's code last chose ($7E:0160) is
 * selected again from its start.
 */
#include "smw2_extras.h"

#include "common_rtl.h"
#include "snes/msu1.h"
#include "rom_patch.h"
#include "sha256.h"
#ifdef RECOMP_LAUNCHER
#include "recomp_launcher.h"
#endif

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kHeader = 0x200 };

/* The hacks, in the order they are applied (MSU-1 is separate). */
enum { kNoCry, kSfx, kRedCoins, kLifeSaver, kColorFix, kButtons, kHacks };

static const char *const kHackLabels[kHacks] = {
    "No crying (Baby Mario no longer cries)",
    "Improved sound effects (softer alarm, mouse, ice and chain sounds)",
    "Red coins look red",
    "Life Saver (lives are kept between resets)",
    "Yoshi Color Fix 3.4 (X cycles Yoshi's colors)",
    "Japanese/PAL controller button colors",
};
static const char *const kHackKeys[kHacks] = {
    "NoCrying", "ImprovedSfx", "RedCoins", "LifeSaver", "ColorFix", "PalButtons",
};

typedef struct KnownPatch {
    const char *sha256;  /* of the .ips file */
    int hack;            /* kHacks = the MSU-1 patch */
    bool header;         /* made for a ROM with a 512-byte copier header */
} KnownPatch;

static const KnownPatch kPatches[] = {
    { "33515f5ef602216c82269fd30cd1333f8cd6d36fb91c3819f823f17585fafa04", kNoCry, false },
    { "67c2f88d6535c255ca72f438c962cdb332e19641aba2e522bcac350e2e003659", kSfx, false },
    { "0bc2635e8da2fb190b2ea9812b18f970b5ca4e395d1067735f4a98a72c8dcf0f", kRedCoins, false },
    { "ca72cc63995dcc498094a784067e3df256f9a90e3a6b656927ed7a0c278a3788", kLifeSaver, false },
    { "734d3d43dbb8be6ee5d30462137809fd1776fe938912317e66637227475fbb9d", kLifeSaver, true },
    { "0943c7b10f9aa90b18141ce17967db0b2b2d0014d83e902dd7ab9bbbfa316a8a", kColorFix, false },
    { "98d027e3fbd2ae15ae2dbc777d874a93f06179c598f2767d14bddc050447816d", kColorFix, true },
    { "1625d9e62c4b29c8f8ac997154e3508a6d9e4a2acaccff6a048de8709b7fbf35", kButtons, false },
    { "22eccf1944225aefe8e48435b911127bac639c79ab37cb0d3274036220cc896e", kButtons, true },
    { "febefc2f58f1ccea78de7820f8c910efc598c7ca5f4381ed5c40ed6e1f077746", kHacks, false },
};
enum { kKnown = sizeof(kPatches) / sizeof(kPatches[0]) };
/* The combined No Crying file: recognized only to say what to use instead. */
static const char kNoCryCombined[] =
    "6e1c2928d60b31372de09c6c21979b578b74d026705804b549ccfd0847516683";

enum { kTrackWram = 0x0160, kLoopTable = 0x7D20 /* $00:FD20 */ };

/* ---- settings (extras.ini) ------------------------------------------- */

static struct {
    bool loaded;
    int hacks_on;
    char hacks_dir[1024];
    int hack_on[kHacks];
    int msu_on;
    char msu_dir[1024];
} s_set;

/* What the chosen folders hold, worked out when chosen or loaded. */
static struct {
    int hack[kHacks];              /* kPatches index found, or -1 */
    char hack_path[kHacks][1100];
    bool combined_seen;
    char hacks_status[512];
    int hacks_level;
    int msu;                       /* kPatches index of Conn's patch, or -1 */
    char msu_patch_path[1100];
    int msu_tracks;
    char msu_status[512];
    int msu_level;
} s_chk;

static bool s_msu_live;            /* MSU-1 patch applied this run */
static uint8_t s_loop[0x30];       /* the MSU patch's loop table, per track */

static void IniPath(char *out, size_t cap)
{
    const char *env = getenv("SMW2_EXTRAS_INI");   /* tests */
    if (env && *env) { snprintf(out, cap, "%s", env); return; }
    const char *base = SDL_GetBasePath();
    snprintf(out, cap, "%sextras.ini", base ? base : "");
}

static void Trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') ++i;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void SaveSettings(void)
{
    char path[1100];
    IniPath(path, sizeof(path));
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
        "; Yoshi's Island extras, set in the launcher (Settings, EXTRAS).\n"
        "[Hacks]\nEnabled = %d\nFolder = %s\n", s_set.hacks_on, s_set.hacks_dir);
    for (int h = 0; h < kHacks && n > 0 && (size_t)n < sizeof(buf); ++h)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s = %d\n", kHackKeys[h], s_set.hack_on[h]);
    if (n > 0 && (size_t)n < sizeof(buf))
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "[MSU1]\nEnabled = %d\nFolder = %s\n",
                      s_set.msu_on, s_set.msu_dir);
    if (n > 0 && (size_t)n < sizeof(buf))
        SDL_SaveFile(path, buf, (size_t)n);
}

static void CheckAll(void);

static void LoadSettings(void)
{
    if (s_set.loaded) return;
    s_set.loaded = true;
    char path[1100];
    IniPath(path, sizeof(path));
    size_t size = 0;
    char *data = (char *)SDL_LoadFile(path, &size);
    if (data) {
        char section[32] = "";
        for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
            Trim(line);
            if (!line[0] || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[') { snprintf(section, sizeof(section), "%s", line); continue; }
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = line, *val = eq + 1;
            Trim(key); Trim(val);
            if (!strcmp(section, "[Hacks]")) {
                if (!strcmp(key, "Enabled")) s_set.hacks_on = atoi(val) != 0;
                else if (!strcmp(key, "Folder")) snprintf(s_set.hacks_dir, sizeof(s_set.hacks_dir), "%s", val);
                else for (int h = 0; h < kHacks; ++h)
                    if (!strcmp(key, kHackKeys[h])) s_set.hack_on[h] = atoi(val) != 0;
            } else if (!strcmp(section, "[MSU1]")) {
                if (!strcmp(key, "Enabled")) s_set.msu_on = atoi(val) != 0;
                else if (!strcmp(key, "Folder")) snprintf(s_set.msu_dir, sizeof(s_set.msu_dir), "%s", val);
            }
        }
        SDL_free(data);
    }
    CheckAll();
}

/* ---- checking files --------------------------------------------------- */

/* kPatches index of the file at `path` (by SHA-256), -1 unknown, -2
 * unreadable; hex gets the digest when the file could be read. */
static int Identify(const char *path, char hex[65])
{
    hex[0] = '\0';
    size_t size = 0;
    void *data = SDL_LoadFile(path, &size);
    if (!data) return -2;
    uint8_t digest[32];
    sha256_compute((const uint8_t *)data, size, digest);
    SDL_free(data);
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    for (int k = 0; k < kKnown; ++k)
        if (strcmp(kPatches[k].sha256, hex) == 0) return k;
    return -1;
}

static bool EndsWithI(const char *s, const char *ext)
{
    size_t n = strlen(s), e = strlen(ext);
    return n >= e && SDL_strcasecmp(s + n - e, ext) == 0;
}

static void JoinPath(char *out, size_t cap, const char *dir, const char *name)
{
    const size_t dl = strlen(dir);
    const bool sep = dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/');
    snprintf(out, cap, "%s%s%s", dir, sep ? "" : "/", name);
}

typedef struct Scan { int depth; } Scan;

static SDL_EnumerationResult SDLCALL HackVisit(void *ud, const char *dir, const char *name)
{
    const Scan *scan = (const Scan *)ud;
    char path[1100];
    JoinPath(path, sizeof(path), dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (scan->depth < 4) {
            Scan sub = { scan->depth + 1 };
            SDL_EnumerateDirectory(path, HackVisit, &sub);
        }
    } else if (info.type == SDL_PATHTYPE_FILE && info.size < (4 << 20) && EndsWithI(name, ".ips")) {
        char hex[65];
        const int k = Identify(path, hex);
        if (k >= 0 && kPatches[k].hack < kHacks && s_chk.hack[kPatches[k].hack] < 0) {
            s_chk.hack[kPatches[k].hack] = k;
            snprintf(s_chk.hack_path[kPatches[k].hack], sizeof(s_chk.hack_path[0]), "%s", path);
        } else if (k == -1 && !strcmp(hex, kNoCryCombined)) {
            s_chk.combined_seen = true;
        }
    }
    return SDL_ENUM_CONTINUE;
}

static void CheckHacks(void)
{
    for (int h = 0; h < kHacks; ++h) { s_chk.hack[h] = -1; s_chk.hack_path[h][0] = '\0'; }
    s_chk.combined_seen = false;
    s_chk.hacks_level = 0;
    if (!s_set.hacks_dir[0]) {
        snprintf(s_chk.hacks_status, sizeof(s_chk.hacks_status),
                 "Choose the folder where you unzipped the hacks (USA 1.0 versions): No Crying, "
                 "Improved SFX and Red Coins; Life Saver; Yoshi Color Fix 3.4; SFC controller "
                 "palette.");
        return;
    }
    s_chk.hacks_level = 2;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(s_set.hacks_dir, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        snprintf(s_chk.hacks_status, sizeof(s_chk.hacks_status), "This folder cannot be opened.");
        return;
    }
    Scan scan = { 0 };
    SDL_EnumerateDirectory(s_set.hacks_dir, HackVisit, &scan);
    int found = 0;
    for (int h = 0; h < kHacks; ++h) found += s_chk.hack[h] >= 0;
    if (!found) {
        snprintf(s_chk.hacks_status, sizeof(s_chk.hacks_status),
                 "No supported hack here. Files are checked by their SHA-256; this game takes "
                 "the USA 1.0 (not Rev 1) versions of No Crying, Improved SFX and Red Coins "
                 "(its individual patches), Life Saver, Yoshi Color Fix 3.4 and the SFC "
                 "controller palette.");
        return;
    }
    s_chk.hacks_level = 1;
    snprintf(s_chk.hacks_status, sizeof(s_chk.hacks_status),
             "Recognized: %d hack patch(es), listed below.%s", found,
             s_chk.combined_seen && s_chk.hack[kNoCry] < 0
                 ? " The combined No Crying file is not used: take the three files in its "
                   "\"Individual Patches\" folder."
                 : "");
}

static bool IsAscii(const char *s)
{
    for (; *s; ++s) if ((unsigned char)*s >= 0x80) return false;
    return true;
}

static SDL_EnumerationResult SDLCALL MsuVisit(void *ud, const char *dir, const char *name)
{
    (void)ud;
    char path[1100];
    JoinPath(path, sizeof(path), dir, name);
    if (EndsWithI(name, ".pcm")) { ++s_chk.msu_tracks; return SDL_ENUM_CONTINUE; }
    if (EndsWithI(name, ".ips")) {
        char hex[65];
        const int k = Identify(path, hex);
        if (k >= 0 && kPatches[k].hack == kHacks && s_chk.msu < 0) {
            s_chk.msu = k;
            snprintf(s_chk.msu_patch_path, sizeof(s_chk.msu_patch_path), "%s", path);
        }
    }
    return SDL_ENUM_CONTINUE;
}

static void CheckMsu(void)
{
    s_chk.msu = -1;
    s_chk.msu_tracks = 0;
    s_chk.msu_patch_path[0] = '\0';
    s_chk.msu_level = 0;
    if (!s_set.msu_dir[0]) {
        snprintf(s_chk.msu_status, sizeof(s_chk.msu_status),
                 "Choose the folder with Conn's MSU-1 patch for USA 1.0 (smw2_msu_us_1.0.ips) "
                 "and the pack's smw2_msu1-<N>.pcm tracks.");
        return;
    }
    s_chk.msu_level = 2;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(s_set.msu_dir, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        snprintf(s_chk.msu_status, sizeof(s_chk.msu_status), "This folder cannot be opened.");
        return;
    }
    if (!IsAscii(s_set.msu_dir)) {
        snprintf(s_chk.msu_status, sizeof(s_chk.msu_status),
                 "The tracks cannot be played from a folder whose path has accented or "
                 "non-English letters. Move the pack to a folder such as C:\\Games\\yoshimsu "
                 "and choose it again.");
        return;
    }
    SDL_EnumerateDirectory(s_set.msu_dir, MsuVisit, NULL);
    if (s_chk.msu < 0 && s_chk.msu_tracks == 0)
        snprintf(s_chk.msu_status, sizeof(s_chk.msu_status),
                 "No MSU-1 pack here: the folder needs Conn's smw2_msu_us_1.0.ips and the "
                 "smw2_msu1-<N>.pcm tracks.");
    else if (s_chk.msu < 0)
        snprintf(s_chk.msu_status, sizeof(s_chk.msu_status),
                 "%d track(s) found, but not Conn's MSU-1 patch for USA 1.0 "
                 "(smw2_msu_us_1.0.ips, checked by its SHA-256; the 1.1 and PAL ones do not fit "
                 "this ROM). It comes in Conn's smw2_msu1_patch.zip: put it in this folder.",
                 s_chk.msu_tracks);
    else if (s_chk.msu_tracks == 0)
        snprintf(s_chk.msu_status, sizeof(s_chk.msu_status),
                 "Conn's MSU-1 patch found, but no smw2_msu1-<N>.pcm tracks.");
    else {
        s_chk.msu_level = 1;
        snprintf(s_chk.msu_status, sizeof(s_chk.msu_status),
                 "Recognized: Conn's MSU-1 patch (USA 1.0) and %d music track(s).",
                 s_chk.msu_tracks);
    }
}

static void CheckAll(void)
{
    CheckHacks();
    CheckMsu();
}

/* ---- the launcher's EXTRAS card --------------------------------------- */

#ifdef RECOMP_LAUNCHER
static int HacksGet(void) { LoadSettings(); return s_set.hacks_on; }
static void HacksSet(int on) { LoadSettings(); s_set.hacks_on = on != 0; SaveSettings(); }
static const char *HacksPath(void) { LoadSettings(); return s_set.hacks_dir; }
static void HacksPick(const char *path)
{
    LoadSettings();
    snprintf(s_set.hacks_dir, sizeof(s_set.hacks_dir), "%s", path);
    CheckHacks();
    s_set.hacks_on = s_chk.hacks_level == 1;
    for (int h = 0; h < kHacks; ++h) s_set.hack_on[h] = s_chk.hack[h] >= 0;
    SaveSettings();
}
static const char *HacksStatus(int *level) { LoadSettings(); *level = s_chk.hacks_level; return s_chk.hacks_status; }
static int s_sub_map[kHacks];
static int HacksSubCount(void)
{
    LoadSettings();
    int n = 0;
    for (int h = 0; h < kHacks; ++h) if (s_chk.hack[h] >= 0) s_sub_map[n++] = h;
    return n;
}
static const char *HacksSubLabel(int i) { return kHackLabels[s_sub_map[i]]; }
static int HacksSubGet(int i) { return s_set.hack_on[s_sub_map[i]]; }
static void HacksSubSet(int i, int on) { s_set.hack_on[s_sub_map[i]] = on != 0; SaveSettings(); }

static int MsuGet(void) { LoadSettings(); return s_set.msu_on; }
static void MsuSet(int on) { LoadSettings(); s_set.msu_on = on != 0; SaveSettings(); }
static const char *MsuPath(void) { LoadSettings(); return s_set.msu_dir; }
static void MsuPick(const char *path)
{
    LoadSettings();
    snprintf(s_set.msu_dir, sizeof(s_set.msu_dir), "%s", path);
    CheckMsu();
    s_set.msu_on = s_chk.msu_level == 1;
    SaveSettings();
}
static const char *MsuStatus(int *level) { LoadSettings(); *level = s_chk.msu_level; return s_chk.msu_status; }

static const RecompLauncherHostExtra kExtras[] = {
    {
        .label = "Fan hacks",
        .help = "Fan-made patches for the USA 1.0 game, from romhacking.net: No Crying, "
                "Improved SFX and Red Coins (hack 3945; use its three Individual Patches), Life "
                "Saver (6981), Yoshi Color Fix (9980) and the SFC controller palette (7740). "
                "Unzip them into one folder and choose it; each one found becomes a checkbox. "
                "Your ROM file is never changed: they are applied in memory when the game "
                "starts. A change made while playing applies the next time the game starts.",
        .pick_folder = 1,
        .pick_title = "Choose the folder with the unzipped hacks",
        .get_enabled = HacksGet, .set_enabled = HacksSet,
        .get_path = HacksPath, .set_path = HacksPick,
        .status = HacksStatus,
        .sub_count = HacksSubCount, .sub_label = HacksSubLabel,
        .sub_get = HacksSubGet, .sub_set = HacksSubSet,
    },
    {
        .label = "MSU-1 music",
        .help = "Plays an MSU-1 pack's CD-quality soundtrack instead of the cartridge's music. "
                "Choose the folder holding Conn's MSU-1 patch for USA 1.0 (smw2_msu_us_1.0.ips, "
                "in his smw2_msu1_patch.zip) and the pack's smw2_msu1-<N>.pcm tracks; packs are "
                "shared on the Zeldix forum. Sound effects still come from the console's sound "
                "chip. A change made while playing applies the next time the game starts.",
        .pick_folder = 1,
        .pick_title = "Choose the MSU-1 pack folder",
        .get_enabled = MsuGet, .set_enabled = MsuSet,
        .get_path = MsuPath, .set_path = MsuPick,
        .status = MsuStatus,
    },
};

void Smw2ExtrasConfigureLauncher(RecompLauncherCGameInfo *info)
{
    LoadSettings();
    info->host_extras_title = "EXTRAS";
    info->host_extras = kExtras;
    info->num_host_extras = (int)(sizeof(kExtras) / sizeof(kExtras[0]));
}
#endif

/* ---- building the image ----------------------------------------------- */

/* Apply the patch file at `path` to the headerless image in (*img, *size). */
static bool Apply(uint8_t **img, size_t *size, const char *path, const KnownPatch *k,
                  const char *name)
{
    size_t patch_size = 0;
    void *patch = SDL_LoadFile(path, &patch_size);
    if (!patch) {
        fprintf(stderr, "[extras] cannot read '%s': %s not applied\n", path, name);
        return false;
    }
    uint8_t *src = *img;
    size_t src_size = *size;
    uint8_t *with_header = NULL;
    if (k->header) {
        with_header = (uint8_t *)calloc(1, src_size + kHeader);
        if (!with_header) { SDL_free(patch); return false; }
        memcpy(with_header + kHeader, src, src_size);
        src = with_header;
        src_size += kHeader;
    }
    uint8_t *out = NULL;
    size_t out_size = 0;
    RomPatchStatus st = snes_rom_patch_apply(src, src_size, (const uint8_t *)patch, patch_size,
                                             (size_t)16 << 20, &out, &out_size);
    free(with_header);
    SDL_free(patch);
    if (st != kRomPatch_Ok || (k->header && out_size <= kHeader)) {
        fprintf(stderr, "[extras] %s could not be applied: %s\n", name,
                st != kRomPatch_Ok ? snes_rom_patch_status_text(st) : "empty result");
        free(out);
        return false;
    }
    if (k->header) {
        memmove(out, out + kHeader, out_size - kHeader);
        out_size -= kHeader;
    }
    free(*img);
    *img = out;
    *size = out_size;
    fprintf(stderr, "[extras] applied %s ('%s'), ROM now %u bytes\n", name, path,
            (unsigned)out_size);
    return true;
}

uint8_t *Smw2ReplaceRom(const uint8_t *rom, size_t size, size_t *out_size)
{
    LoadSettings();
    CheckAll();   /* files may have moved since they were chosen */
    int hacks = 0;
    if (s_set.hacks_on)
        for (int h = 0; h < kHacks; ++h) hacks += s_set.hack_on[h] && s_chk.hack[h] >= 0;
    const bool msu = s_set.msu_on && s_chk.msu_level == 1;
    if (s_set.hacks_on && !hacks)
        fprintf(stderr, "[extras] fan hacks are on but none is used: %s\n", s_chk.hacks_status);
    if (s_set.msu_on && !msu)
        fprintf(stderr, "[extras] MSU-1 is on but not used: %s\n", s_chk.msu_status);
    if (!rom || (!hacks && !msu))
        return NULL;

    /* Work on the bare ROM: a copier header is dropped. */
    const size_t head = (size & 0x7FFF) == kHeader ? kHeader : 0;
    size_t img_size = size - head;
    uint8_t *img = (uint8_t *)malloc(img_size);
    if (!img) return NULL;
    memcpy(img, rom + head, img_size);

    bool changed = false;
    if (s_set.hacks_on)
        for (int h = 0; h < kHacks; ++h)
            if (s_set.hack_on[h] && s_chk.hack[h] >= 0 &&
                Apply(&img, &img_size, s_chk.hack_path[h], &kPatches[s_chk.hack[h]], kHackLabels[h]))
                changed = true;
    if (msu && Apply(&img, &img_size, s_chk.msu_patch_path, &kPatches[s_chk.msu],
                     "Conn's MSU-1 patch (USA 1.0)")) {
        changed = true;
        s_msu_live = true;
        if (kLoopTable + sizeof(s_loop) <= img_size)
            memcpy(s_loop, img + kLoopTable, sizeof(s_loop));
        /* RtlRegisterGame (right after this) arms the chip from the
         * environment; a folder resolves to the pack's "<name>-<N>.pcm" base. */
#ifdef _WIN32
        _putenv_s("SNESRECOMP_MSU1", s_set.msu_dir);
#else
        setenv("SNESRECOMP_MSU1", s_set.msu_dir, 1);
#endif
        fprintf(stderr, "[MSU-1] pack on: %d track(s) in '%s'\n", s_chk.msu_tracks, s_set.msu_dir);
    }
    if (!changed) { free(img); return NULL; }
    *out_size = img_size;
    return img;
}

/* A save state or rewind frame was loaded: play what the game now expects. */
void Smw2ExtrasOnReset(void)
{
    if (!s_msu_live || !msu1_enabled())
        return;
    const uint8_t track = g_ram[kTrackWram];
    if (track == 0 || track >= sizeof(s_loop)) {
        msu1_write(0x2007, 0);
        return;
    }
    msu1_write(0x2004, track);
    msu1_write(0x2005, 0);
    msu1_write(0x2006, 0xFF);
    msu1_write(0x2007, s_loop[track] ? s_loop[track] : 3);
}
