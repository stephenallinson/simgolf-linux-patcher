# Changelog

## 0.2.1 (2026-10-17), beta

### Settings
- `AutoSaves` in `settings.ini` keeps up to 9 rotating autosaves instead of the game's 2 (6 for new installs), always overwriting the oldest. The game autosaves about every 100 seconds, so 6 slots go back about 10 minutes.

### Installing
- The README now walks through the whole install for players new to Linux and Wine: installing Wine, creating the prefix, installing from the disc, the 1.03 patch, this fix, and a menu entry for the game.
- `patch-terrain.sh` refuses a `golf.exe` other than 1.03 before changing anything, and `sghooks.dll` leaves any other version alone. Without the patch, the game would have crashed with no explanation.

## 0.2.0 (2026-10-15), beta

The first public release. It needs your own copy of SimGolf, patched to 1.03.

### Rendering
- The course renders correctly under Wine, including panning and zooming. `terrgl32.dll` draws `Terrain.dll`'s OpenGL into an offscreen buffer and writes back only the pixels it changed. `patch-terrain.sh` changes two bytes in `Terrain.dll` to load it.
- `run.sh` runs the game in gamescope, scaled up by the largest whole number that fits your monitor. It passes gamescope the monitor's size, so the picture fills the screen instead of sitting in black bars.

### Sound
- Fixed sounds that looped forever, even after returning to the main menu. Wine's `mmioAdvance` drops bytes the game hasn't read yet, so its streamed sounds never reached their end. `sghooks.dll` fixes this in memory, without changing `sound.dll`.
- Volume controls, which the game never had: Master, Music, Voices, Effects, Ambience and Interface. Set them in the game (control wheel → System → **Sound Settings...**) or in `settings.ini`. Changes apply at once.

### Higher resolutions
- Optional wide and larger resolutions, such as 1280x720 for 2x scaling on a 2560x1440 monitor. Set `Resolution` in `settings.ini`.
  - The course art keeps its original pixel size, so a bigger screen shows more of the course.
  - The panels sit at the screen edges.
  - Reports, menus, the title screen, the world map, the load/save screens, Top 10 Designers, the hire screen and the skill upgrade screen stay 800x600, centred, and their buttons work.
  - Playing as your golfer works across the whole screen: aiming, shooting, and the shot information.
  - Building previews, aiming and clicks reach the course to the right of the tool palette.
- `Art=large` uses the game's own enlarged course art for 1024x768 and 1280x1024 instead.

### Settings
- A `settings.ini` in the game folder, for `Resolution`, `Art`, `LockMouse` and the volumes. `patch-terrain.sh` installs it and never overwrites an existing one.
- `LockMouse=1` keeps the mouse inside the game window, so panning at a screen edge can't slide the pointer onto another monitor.

### Known issues
- The golfer close-up inset at wide resolutions hasn't been checked on screen yet.
