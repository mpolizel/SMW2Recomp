/*
 * Yoshi's Island assist tools / cheats. See smw2_cheats.h.
 *
 * ROM codes swap one instruction of the USA 1.0 ROM (each is checked against
 * the original byte first, so another dump is left alone); RAM codes write
 * WRAM once per frame before the game runs, like a Pro Action Replay does at
 * each NMI. Turning a ROM code off puts the original byte back.
 */
/* <windows.h> before any project header: the framework's types.h defines
 * FORCEINLINE as plain `inline` outside MSVC, and MinGW's winnt.h would then
 * emit its inline helpers (NtCurrentTeb...) as external definitions here and
 * in main.c: a duplicate-symbol link error with clang (issue #5). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "smw2_cheats.h"

#include "common_rtl.h"
#include "snes/snes.h"
#include "snes/cart.h"
#include "common_cpu_infra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SNESRECOMP_NET)
int snes_netplay_active(void);
#endif

static const char *const kYoshiColors[] = {
    "Stage's own", "Green", "Pink", "Yellow", "Light blue", "Purple",
    "Brown", "Red", "Dark blue",
};

/* 1-1 .. 6-8 and each world's extra stage; level id = world * 12 + n. */
static const char *const kStages[] = {
    "Off",
    "1-1", "1-2", "1-3", "1-4", "1-5", "1-6", "1-7", "1-8", "Extra 1",
    "2-1", "2-2", "2-3", "2-4", "2-5", "2-6", "2-7", "2-8", "Extra 2",
    "3-1", "3-2", "3-3", "3-4", "3-5", "3-6", "3-7", "3-8", "Extra 3",
    "4-1", "4-2", "4-3", "4-4", "4-5", "4-6", "4-7", "4-8", "Extra 4",
    "5-1", "5-2", "5-3", "5-4", "5-5", "5-6", "5-7", "5-8", "Extra 5",
    "6-1", "6-2", "6-3", "6-4", "6-5", "6-6", "6-7", "6-8", "Extra 6",
};

static const Smw2CheatInfo kCheats[kSmw2CheatCount] = {
    { "InfiniteLives", "Infinite lives",
      "Losing a life never lowers the life counter.", NULL, 0 },
    { "KeepStarsWhenHit", "Stars never go down",
      "The star countdown does not run while baby Mario floats away, and "
      "stars above 10 stay.", NULL, 0 },
    { "KeepItems", "Items are not used up",
      "Using an item from the pause menu keeps it.", NULL, 0 },
    { "FullStars", "Always 30 stars",
      "The star countdown stays at 30.", NULL, 0 },
    { "YoshiColor", "Yoshi's color",
      "Paint Yoshi in any color, in every stage.",
      kYoshiColors, (int)(sizeof(kYoshiColors) / sizeof(kYoshiColors[0])) },
    { "StageSelect", "Stage select",
      "Any stage you enter from the world map starts this stage instead "
      "(turn it off to play normally). Finishing it counts for that stage in "
      "your save.",
      kStages, (int)(sizeof(kStages) / sizeof(kStages[0])) },
};

static int s_value[kSmw2CheatCount];
static bool s_loaded;

const Smw2CheatInfo *Smw2CheatTable(void) { return kCheats; }

/* ---- cheats.ini beside the executable ---------------------------------- */

static bool CheatsPath(char *out, size_t cap)
{
#ifdef _WIN32
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    char *slash = strrchr(exe, '\\');
    if (!slash) return false;
    *slash = 0;
    return snprintf(out, cap, "%s\\cheats.ini", exe) < (int)cap;
#else
    return snprintf(out, cap, "cheats.ini") < (int)cap;
#endif
}

static void Load(void)
{
    s_loaded = true;
    char path[1024];
    if (!CheatsPath(path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        char *e = k + strlen(k);
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        for (int i = 0; i < kSmw2CheatCount; i++) {
            if (strcmp(k, kCheats[i].key) != 0) continue;
            int v = atoi(eq + 1);
            int max = kCheats[i].num_choices ? kCheats[i].num_choices - 1 : 1;
            s_value[i] = v < 0 ? 0 : v > max ? max : v;
        }
    }
    fclose(f);
}

static void Save(void)
{
    char path[1024];
    if (!CheatsPath(path, sizeof(path))) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "; Assist tools / cheats, set on the launcher's Assist Tools page.\n[Cheats]\n");
    for (int i = 0; i < kSmw2CheatCount; i++)
        fprintf(f, "%s = %d\n", kCheats[i].key, s_value[i]);
    fclose(f);
}

int Smw2CheatGet(int index)
{
    if (!s_loaded) Load();
    return index >= 0 && index < kSmw2CheatCount ? s_value[index] : 0;
}

void Smw2CheatSet(int index, int value)
{
    if (!s_loaded) Load();
    if (index < 0 || index >= kSmw2CheatCount) return;
    int max = kCheats[index].num_choices ? kCheats[index].num_choices - 1 : 1;
    s_value[index] = value < 0 ? 0 : value > max ? max : value;
    Save();
}

/* ---- applying them ------------------------------------------------------ */

typedef struct RomCode { int cheat; uint32_t snes; uint8_t orig, value; } RomCode;
static const RomCode kRomCodes[] = {
    { kSmw2CheatLives,    0x04F6F9, 0xCE, 0xAD },  /* DEC $0379 -> LDA $0379 */
    { kSmw2CheatStarsHit, 0x00E310, 0xCE, 0xAD },  /* DEC $03B6 -> LDA $03B6 (runs from WRAM) */
    { kSmw2CheatItems,    0x01DB87, 0x6D, 0x60 },  /* ADC $0CF5 -> RTS */
};

static uint32_t RomOffset(uint32_t snes)
{
    /* Banks $00-$3F, $8000-$FFFF: 32 KiB per bank. */
    return ((snes >> 16) & 0x3F) * 0x8000 + ((snes & 0xFFFF) - 0x8000);
}

void Smw2CheatsFrame(void)
{
    if (!s_loaded) Load();
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || !cart->rom) return;
    bool allowed = true;
#if defined(SNESRECOMP_NET)
    /* Both players must run the same game. */
    if (snes_netplay_active()) allowed = false;
#endif
    for (size_t i = 0; i < sizeof(kRomCodes) / sizeof(kRomCodes[0]); i++) {
        const RomCode *c = &kRomCodes[i];
        const uint32_t off = RomOffset(c->snes);
        if (off >= cart->romSize) continue;
        const bool on = allowed && s_value[c->cheat] != 0;
        uint8_t *b = &cart->rom[off];
        if (on && *b == c->orig) *b = c->value;
        else if (!on && *b == c->value) *b = c->orig;
        /* The game copies bank $00 $C000-$FFFF to WRAM $7E:C000 and runs it
         * from there, so a code in that range is also swapped in the copy. */
        if ((c->snes >> 16) == 0x00 && (c->snes & 0xFFFF) >= 0xC000) {
            uint8_t *w = &g_ram[c->snes & 0xFFFF];
            if (on && *w == c->orig) *w = c->value;
            else if (!on && *w == c->value) *w = c->orig;
        }
    }
    if (!allowed) return;
    if (s_value[kSmw2CheatStarsFull]) { g_ram[0x3B6] = 0x2C; g_ram[0x3B7] = 0x01; }
    if (s_value[kSmw2CheatYoshiColor]) g_ram[0x383] = (uint8_t)(s_value[kSmw2CheatYoshiColor] - 1);
    /* Like the Pro Action Replay code: the current stage number is held,
     * so the world map enters this stage whichever one is picked. */
    if (s_value[kSmw2CheatStage]) {
        const int n = s_value[kSmw2CheatStage] - 1;
        g_ram[0x21A] = (uint8_t)((n / 9) * 12 + n % 9);
    }
}
