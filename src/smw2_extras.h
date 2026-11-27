#ifndef SMW2_EXTRAS_H
#define SMW2_EXTRAS_H
/* Fan hacks and MSU-1 packs, chosen in the launcher and applied in memory:
 * see smw2_extras.c. */
#include <stddef.h>
#include <stdint.h>

/* SnesDesktopHostGame.replace_rom: the verified ROM with the fan hacks and
 * the MSU-1 pack chosen in the launcher applied, or NULL when none is on. */
uint8_t *Smw2ReplaceRom(const uint8_t *rom, size_t size, size_t *out_size);
/* SnesDesktopHostGame.on_reset: after a save state or rewind frame is
 * loaded, select the MSU-1 track the game now expects. */
void Smw2ExtrasOnReset(void);

#ifdef RECOMP_LAUNCHER
struct RecompLauncherCGameInfo;
/* The launcher's EXTRAS card: fan hacks and MSU-1 rows. */
void Smw2ExtrasConfigureLauncher(struct RecompLauncherCGameInfo *info);
#endif

#endif
