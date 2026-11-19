# Changelog

## 0.1.4 — 2026-11-19

The world map no longer flickers after a bonus game. Once a stage had ended
in the Scratch and Match bonus game, the map below the stage list showed the
island on one frame and a field of stray tiles on the next, which a
screenshot or a recording catches as a scrambled island. The renderer
rebuilt the game's video registers after each frame from the latest writes
alone, so a register set once (here, the map's BG mode and tile bases) could
keep a value from before the map, on every other frame. It now keeps the
game's own value. Your in-game saves and save states from 0.1.3 keep
loading.

## 0.1.3 — 2026-11-11

Current snesrecomp and recomp-ui, and RecompLauncher's recommendations. The
framework brings its access-exact sound timing (a per-clock S-DSP), sprite
priority, brightness-rounding, HDMA and mosaic fixes. A game that reads the
controller by hand gets fresh buttons on every read ($4016 strobe); a save
state loaded in another scene is no longer drawn with the previous scene's
registers; the sound chip runs from power on for a game that reads it first.
Save states now keep the CPU's place, so one loaded on the title or during
the story no longer resumes in the wrong loop. Your in-game saves and the
save states from 0.1.2 keep loading, with the music playing on (the old
sound state is converted). With [RecompLauncher](https://recomplauncher.com)
the game builds on the launcher's setup screen, Play goes straight into the
game, Big Picture opens full screen, and the renderer and shader are on the
game's page. Controllers are no longer read while the window is in the
background ([General] BackgroundInput = 1 restores that); the log keeps the
last runs. The update check on start stays; when RecompLauncher starts the
game it leaves updates to the launcher.

## 0.1.2 — 2026-11-05

Changing a setting during play no longer sends the game back to the start.
A few settings (renderer, audio output and rate, mods) need the game to
restart; it saves the moment it leaves to `saves\resume.sav` and comes back
to it, but it came back without the CPU's registers and resume point, so it
booted from power-on (or, with only half the state back, showed a black
picture over the stage's music). That one-shot file now holds the whole
machine and is deleted once loaded; your save states and in-game saves are
not touched, and every one made by 0.1.0 or 0.1.1 keeps loading.

## 0.1.1 — 2026-11-05

World 1-7 (Touch Fuzzy, Get Dizzy) is drawn right. Its stage uses BG mode 2,
where a table in BG3 sets the scroll of each column of the stage layer, and
the renderer read that table one row off: the stage was put together from the
wrong rows of its map. Once that was fixed, the stage shook while Yoshi walked,
one frame forward and one back: the game uploads the next frame's table at the
bottom of the screen, and the renderer drew the whole frame with it. Video
memory, palettes and sprites written while the picture is drawn now reach only
the lines below the write, as on the console. Saves and save states from 0.1.0
keep working.

## 0.1.0 — 2026-11-03

First release: a setup pack that recompiles the game from your own ROM. Boots
through the Nintendo logo, the opening story and the title screen into the file
select and the levels, at full speed with the Super FX graphics drawn; the
in-game save is kept in `build\saves\save.srm`. A fresh install starts with no
shader (the plain picture); scanline, CRT and upscaler presets (ScaleFX,
NNEDI3) are in Settings → Shader. Settings during play (Ctrl+L or Select+L3);
audio interpolation (I key: Gaussian, Cubic, Sinc); update check on start that
backs up your saves before installing; asks for the dedicated graphics card on
laptops with two; console-free Windows build.

Renderer fixes: Yoshi's sprite is drawn whole while he moves (the renderer had
mixed two of his animation frames, showing extra heads and stray body parts),
and mosaic effects are drawn.
