# SimGolf on Linux

Play **Sid Meier's SimGolf** (2002) on Linux. This fix makes the game work properly under Wine, the program that runs Windows games on Linux. It also adds a few extras the original never had:
- volume controls;
- optional widescreen;
- a fix for sounds that kept repeating forever.

Without the fix, the golf course is drawn as garbage or black areas, or not at all.

This project contains **no game files**. You need your own copy of SimGolf.

The steps below are written for players, not programmers. They take about 15 minutes. Technical details are at the end.

## What you need

- **Your SimGolf CD**, or a disc image of it (an `.iso` file).
- **The SimGolf 1.03 patch.** This is a newer `golf.exe` from Firaxis/EA, or a program that installs it. Some editions of the disc include it in a `PATCH` folder.
- **A Linux computer** with an internet connection, to install Wine and download this fix.

## Using the terminal

Most steps use the **terminal**, a window where you type commands. Open it from your application menu: search for "Terminal" (it may be called Konsole or Console).

- **Run a command:** copy it from this page, paste it into the terminal with **Ctrl+Shift+V** (plain Ctrl+V doesn't work there), then press **Enter**.
- **Passwords:** commands that start with `sudo` ask for your password. Nothing appears while you type it; that's normal. Type it and press Enter.

## Step 1: Install Wine

Paste the command for your version of Linux. If you're not sure which you have, run `cat /etc/os-release` and look at the `NAME=` line.

**Arch, EndeavourOS, Manjaro:**
```sh
sudo pacman -S --needed wine gamescope xorg-xrandr
```

**Ubuntu, Linux Mint, Pop!_OS, Zorin, Debian:**
```sh
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install wine wine32:i386 x11-xserver-utils
sudo apt install gamescope
```
If the last command says it can't find `gamescope`, skip it. The game still works, in a window instead of full screen.

**Fedora:**
```sh
sudo dnf install wine gamescope xrandr
```

This fix was tested with Wine 11.19. Your Linux may ship an older Wine. It will probably work, but it hasn't been tested. To check your version, run `wine --version`.

## Step 2: Create a home for SimGolf in Wine

Wine keeps each game's pretend Windows in its own folder, called a *prefix*. This command creates one called `.wine-simgolf` in your home folder:
```sh
WINEPREFIX=$HOME/.wine-simgolf wineboot -i
```

It takes a few seconds. If a window asks to install **Wine Mono** or **Wine Gecko**, you can click **Cancel**: SimGolf doesn't need them.

The folder name starts with a dot, which makes it hidden. To see it in your file manager, press **Ctrl+H**.

## Step 3: Install SimGolf from the disc

1. **Open the disc.**
   - **CD:** insert it. Your desktop shows it in the file manager.
   - **Disc image:** right-click the `.iso` file and choose **Open With → Disk Image Mounter**, or **Mount**. The disc then appears in the file manager like a CD.
2. **Start the installer.** In the terminal, type the start of this command, ending with a space after `wine`. Don't press Enter yet:
   ```sh
   WINEPREFIX=$HOME/.wine-simgolf wine 
   ```
   Then drag the disc's **`Setup.exe`** (or `SETUP.EXE`) from the file manager into the terminal window. That types its location for you. Now press **Enter**.
3. **Follow the installer, keeping its suggested settings.** Leave the install folder as it is. If it offers to install **DirectX** or to **register** the game, decline both: Wine already provides DirectX.
4. **Set the prefix to Windows XP,** the system SimGolf was made for:
   ```sh
   WINEPREFIX=$HOME/.wine-simgolf winecfg -v winxp
   ```
5. **Don't start the game yet.** Patch it first (step 4).

## Step 4: Apply the 1.03 patch

The game must be at version 1.03, or this fix refuses to install.

**If your patch is a single `golf.exe` file:** copy it over the one in the game folder.

1. Open the game folder in your file manager. Press **Ctrl+H** if you don't see `.wine-simgolf`:
   `.wine-simgolf` → `drive_c` → `Program Files (x86)` → `EA Games` → `Firaxis Games` → `Sid Meiers SimGolf`
2. Rename the `golf.exe` that's there to `golf-1.00.exe`, to keep it as a backup.
3. Copy the 1.03 `golf.exe` into this folder.

**If your patch is a program** (for example `SimGolf_Patch_v103.exe`): run it the same way as the installer. Type `WINEPREFIX=$HOME/.wine-simgolf wine ` followed by a space, drag the patch file into the terminal, and press Enter.

## Step 5: Install this fix

1. Download `simgolf-linux-0.2.1.tar.gz` from this project's **Releases** page.
2. Extract it into your home folder: right-click it and choose **Extract Here**, then move the folder to your home folder if it isn't already there. You should end up with a folder called `simgolf-linux-0.2.1` in your home folder.
3. In the terminal, run:
   ```sh
   cd ~/simgolf-linux-0.2.1
   ./patch-terrain.sh
   ```
   It checks that it's looking at the right game files before changing anything, and keeps a backup. When it's done, its last lines start with `Installed`.

## Step 6: Play

```sh
~/simgolf-linux-0.2.1/run.sh
```

The game opens full screen, scaled up as large as it fits on your monitor with crisp pixels. To quit, use the game's own menu: control wheel → **System** (the wrench) → **Exit the Game**.

**To start SimGolf from your application menu** instead of the terminal, run this once:
```sh
cd ~/simgolf-linux-0.2.1
mkdir -p ~/.local/share/applications
printf "[Desktop Entry]\nType=Application\nName=SimGolf\nComment=Sid Meier's SimGolf\nExec=\"%s/run.sh\"\nIcon=applications-games\nTerminal=false\nCategories=Game;\n" "$PWD" > ~/.local/share/applications/simgolf.desktop
```
SimGolf then appears in your menu under Games. Keep the `simgolf-linux-0.2.1` folder where it is, because the menu entry starts the game from there.

**To undo everything** and put the game files back as they were:
```sh
~/simgolf-linux-0.2.1/patch-terrain.sh --revert
```

## Settings

The fix adds a `settings.ini` file to the game folder (`.wine-simgolf` → `drive_c` → `Program Files (x86)` → `EA Games` → `Firaxis Games` → `Sid Meiers SimGolf`). Installing the fix again never overwrites your settings.

- **Volumes:** set them in the game, under the control wheel's **System** menu (the wrench), **Sound Settings...**. Each click on a volume steps it down by 20% (from 0% it goes back to 100%). Changes apply at once and are saved to `settings.ini`.
- **Everything else:** open `settings.ini` with your text editor (right-click → Open With), change the value after the `=`, and save. Changes take effect the next time the game starts. Text after a `;` is a note for you; the game ignores it.

```ini
[Display]
Resolution=1280x720   ; at least 800x600
Art=native            ; native (original pixel art, 1:1) or large
LockMouse=0           ; 1: keep the mouse inside the game window

[Sound]
Master=100            ; volumes in percent, 0 to 100
Music=100
Voices=100
Effects=100
Ambience=100
Interface=100

[Game]
AutoSaves=6           ; autosaves to keep, 2 to 9
```

- **Resolution:** the game has no volume or screen settings of its own; `sghooks.dll` adds them by changing the game in memory at start-up, without touching its files.
  - The course art is drawn at its original pixel size, so a bigger screen shows more of the course.
  - The panels stay at the screen edges, and reports, menus, the title screen, the world map and the load/save screens stay 800x600, centred.
  - `run.sh` scales the result up to fill your monitor, so choose a size your monitor is a whole multiple of:

    | Monitor | Resolution |
    |---|---|
    | 2560x1440 | 1280x720 |
    | 3840x2160 | 1920x1080 or 1280x720 |
    | 1920x1080 | 1920x1080 |
    | 4:3 | 1024x768 or 800x600 |
- **Art:** `large` uses the game's own enlarged course art for 1024x768 and 1280x1024, chosen by screen height. It is less sharp, and objects line up with the ground slightly less exactly.
- **Sound:** sounds are sorted by their folder and name:
  - **Music:** the songs, and the short cues that start a golfer's storyline.
  - **Voices:** golfers' and celebrities' lines.
  - **Ambience:** course background loops.
  - **Interface:** clicks and menu sounds.
  - **Effects:** everything else.
- **LockMouse:** `1` keeps the mouse inside the game window (gamescope's `--force-grab-cursor`), so panning at a screen edge can't slide the pointer onto another monitor. Switch away with your desktop's keyboard shortcuts.
- **AutoSaves:** the game autosaves every couple of minutes. The original keeps only two autosaves, so you can go back about three minutes. With `AutoSaves=6` it keeps six (`&AutoSave1` to `&AutoSave6` in the load list), always overwriting the oldest.
- **Overrides:** the environment variables `SIMGOLF_RES` and `SIMGOLF_ART` override `[Display]`, for example `SIMGOLF_RES=1024x768 ./run.sh`.

## Troubleshooting

- **`wine: command not found`.** Wine isn't installed yet. See step 1.
- **`patch-terrain.sh` says golf.exe isn't the 1.03 version.** Apply the 1.03 patch (step 4), then run `./patch-terrain.sh` again. Nothing was changed.
- **`Permission denied` when running `./patch-terrain.sh` or `run.sh`.** Your unpacking program dropped the files' "can run" flag. Run `chmod +x ~/simgolf-linux-0.2.1/*.sh` and try again.
- **`game folder not found`.** The game isn't installed where the scripts look. If you installed it somewhere else, pass the game folder to `patch-terrain.sh`: type `./patch-terrain.sh ` followed by a space, drag the game folder into the terminal, and press Enter. For `run.sh`, start it as `SIMGOLF_DIR="/path/to/the/game folder" ~/simgolf-linux-0.2.1/run.sh`.
- **An empty desktop opens, but the game doesn't start.** `golf.exe` has to be started from its own folder. `run.sh` does this for you, so start the game with `run.sh`.
- **The game won't start after a crash or a run you interrupted.** An old copy of the game may still be running. Clear it with:
  `WINEPREFIX=~/.wine-simgolf wineserver -k`
  If processes still remain, run `pkill -9 -f 'winedevice|gamescope'`.
- **"Can't Find A Suitable PixelFormat" message boxes.** `Terrain.dll` isn't patched. Run `patch-terrain.sh` again.
- **Something still renders wrong.** Run with logging on, then open an issue on this project's page and attach the log:
  `TERRGL32_LOG='Z:\tmp\terrgl32.log' ./run.sh` (the log is written to `/tmp/terrgl32.log`).

- **The game shows small, with black bars, instead of scaled up.** `run.sh` tells gamescope your monitor's size, which it detects with `xrandr`. If it picks the wrong monitor, set the size yourself: `SIMGOLF_OUTPUT=2560x1440 ./run.sh`.

## Known issues

- **Only the English 1.03 version of SimGolf is supported.** `patch-terrain.sh` checks `golf.exe` and refuses other versions, because the fix changes the game at addresses specific to 1.03.
- **Wide resolutions** are new in 0.2.0. If a screen or panel shows up in the wrong place, please open an issue with a screenshot.

- **Looping sound** (fixed in 0.2.0). The end of a sound, such as a laugh, a "Woohoo!" or the faint ambient "radar" beeping, could keep looping, even after you returned to the main menu. Wine's `mmioAdvance` drops bytes that the caller hasn't read yet, while Windows keeps them. The game's `sound.dll` relies on the Windows behaviour when streaming longer sounds: with bytes missing, a stream never reaches its end and replays its last buffer forever. `sghooks.dll` fixes it at any resolution, in memory and without changing `sound.dll`. `SGHOOKS_NO_SOUNDFIX=1` turns the fix off.

## For the technically curious

Everything below explains how the fix works and how to build it. You don't need it to play.

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
