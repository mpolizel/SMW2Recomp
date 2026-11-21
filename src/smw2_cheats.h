/*
 * Yoshi's Island assist tools / cheats: Pro Action Replay style codes for the
 * USA 1.0 ROM, chosen on the launcher's Assist Tools page and kept in
 * cheats.ini beside the executable. Every one is off by default.
 */
#pragma once

enum {
    kSmw2CheatLives,        /* lives never go down           ROM $04:F6F9 */
    kSmw2CheatStarsHit,     /* stars never go down   ROM $00:E310 (and its WRAM copy) */
    kSmw2CheatItems,        /* power-up items are not used   ROM $01:DB87 */
    kSmw2CheatStarsFull,    /* star countdown held at 30     RAM $7E03B6 */
    kSmw2CheatYoshiColor,   /* choice: 0 = stage's own, 1+n = color n RAM $7E0383 */
    kSmw2CheatStage,        /* choice: 0 = off, 1+n = stage n RAM $7E021A, held */
    kSmw2CheatCount
};

/* Labels, help texts and choices for the launcher rows, in enum order. */
typedef struct Smw2CheatInfo {
    const char *key;            /* cheats.ini key */
    const char *label;
    const char *help;
    const char *const *choices; /* NULL: on/off */
    int num_choices;
} Smw2CheatInfo;

const Smw2CheatInfo *Smw2CheatTable(void);
int  Smw2CheatGet(int index);
void Smw2CheatSet(int index, int value);
/* Once per frame, before the game runs: ROM patches follow the switches,
 * RAM codes are written. Does nothing in netplay. */
void Smw2CheatsFrame(void);
