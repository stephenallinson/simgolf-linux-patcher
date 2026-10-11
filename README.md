# SimGolf on Linux

A fix that makes **Sid Meier's SimGolf** (2002, Firaxis/EA) playable under Wine on Linux. Without it, the course terrain renders as garbage, as black areas, or not at all. The course is built from tiles drawn with OpenGL, and those draws break under Wine.

This project contains **no game files**. You need your own installed copy of SimGolf, patched to 1.03. The fix changes two bytes in your own `Terrain.dll` and adds a small DLL of its own (`terrgl32.dll`).

Tested on Wine 11.19 (Arch), Mesa 26.2 / RADV, AMD Radeon RX 9070 XT.

## Install

1. Install SimGolf into a Wine prefix from your own disc, then apply the official 1.03 patch. The examples below use `~/.wine-simgolf`.
2. Download and extract the latest release, then run:

   ```sh
   WINEPREFIX=~/.wine-simgolf ./patch-terrain.sh
   ```

   If the game isn't in the standard folder (`C:\Program Files (x86)\EA Games\Firaxis Games\Sid Meiers SimGolf`), pass the folder as an argument:

   ```sh
   ./patch-terrain.sh "/path/to/prefix/drive_c/.../Sid Meiers SimGolf"
   ```

   The script checks that it's looking at the right file before changing anything. It saves a backup as `Terrain.dll.orig`, then installs `terrgl32.dll` next to it.

3. Play:

   ```sh
   WINEPREFIX=~/.wine-simgolf ./run.sh
   ```

   This runs the game at its native 800x600 inside [gamescope](https://github.com/ValveSoftware/gamescope), enlarged by the largest whole-number factor that fits your screen, so pixels stay crisp and square (for example 2x on a 1440p monitor). For other scaling, set gamescope options in `GAMESCOPE_ARGS`: `GAMESCOPE_ARGS="-F pixel" ./run.sh` or `"-F fsr"` fill the screen height. Without gamescope, `run.sh` uses a plain Wine virtual desktop; `./run.sh --no-gamescope` forces that.

To undo everything: `./patch-terrain.sh --revert`.

## Troubleshooting

- **An empty desktop opens, but the game doesn't start.** `golf.exe` has to be started from its own folder. `run.sh` does this for you. If you launch it some other way, `cd` into the game folder first.
- **The game won't start after a crash or a run you interrupted.** An old copy of the game may still be running. Clear it with:
  `WINEPREFIX=~/.wine-simgolf wineserver -k`
  If processes still remain, run `pkill -9 -f 'winedevice|gamescope'`.
- **"Can't Find A Suitable PixelFormat" message boxes.** `Terrain.dll` isn't patched. Run `patch-terrain.sh` again.
- **Something still renders wrong.** Run with logging on, then open an issue and attach the log:
  `TERRGL32_LOG='Z:\tmp\terrgl32.log' ./run.sh` (the log is written to `/tmp/terrgl32.log`).

## Known issues

- **Looping sound.** The "bang" when you place a building can keep looping, even after you return to the main menu. The game plays its sounds through DirectSound. This is under investigation.

## How it works

The game composes each frame in a 16-bit, 800x600 DIB section held in a memory DC. `Terrain.dll` draws the course into that same bitmap with OpenGL. Windows' built-in software OpenGL wrote straight into the bitmap's memory. It changed only the pixels it drew, and the game relies on that: it scrolls the bitmap with GDI, redraws only the newly exposed strips, and never clears the screen.

Wine's support for OpenGL on memory DCs doesn't match this:
- It has no 16-bit pixel formats for bitmaps, so `ChoosePixelFormat` fails. That causes the "Can't Find A Suitable PixelFormat" errors.
- It assumes 32-bit bitmaps when copying the image back.
- It copies the entire GL buffer over the bitmap on many GL calls, which wipes whatever the game drew in between.

The fix has two parts:

- **The `Terrain.dll` patch.** It asks for a 32-bit pixel format, and it loads `terrgl32.dll` in place of `opengl32.dll`.
- **`terrgl32.dll`.** It passes every GL call through to Wine's `opengl32`, with these exceptions:
  - A context for a memory DC is created on a hidden window instead, and renders into the DLL's own offscreen buffer (an FBO). Wine's memory-DC path is never used.
  - Before drawing, it uploads any bitmap rows that changed since GL last saw them.
  - On `glFlush`, it writes back **only the pixels GL actually changed**, in the bitmap's 5-5-5 format. GL row 0 maps to the bitmap's first scanline, which is the orientation the game selects on Windows 2000/XP and later.

## Building from source

You need `mingw-w64-gcc` (the `i686-w64-mingw32-gcc` compiler).

```sh
shim/build.sh             # builds shim/terrgl32.dll
./make-release.sh --test  # builds, runs the shim's tests under Wine, and packages dist/
```

`shim/test/shimtest.c` drives the shim the same way `Terrain.dll` does. It covers orientation, exact colours, that only drawn pixels change, that GDI and direct pixel writes made in between survive, and blending.

`tools/trace-run.sh` records a Wine relay trace of the game's GDI calls, interleaved with the shim's log. It's useful for debugging rendering problems.

## Legal

SimGolf is © Firaxis Games / Electronic Arts. This project isn't affiliated with or endorsed by either. It includes no game code or data. The patch script modifies only your own installed copy.
