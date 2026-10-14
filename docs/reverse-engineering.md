# SimGolf reverse-engineering notes

Working notes on how Sid Meier's SimGolf (patch 1.03) is put together, aimed at hooking and eventually replacing parts of it (first target: widescreen / higher resolution). Addresses are the modules' preferred (link-time) addresses unless stated otherwise.

## Runtime module map

Logged in-process by terrgl32 (`TERRGL32_LOG`) under Wine 11.19:

| Module | Loaded at | Preferred base | Notes |
|---|---|---|---|
| golf.exe | 0x00400000 | 0x00400000 | game logic and UI |
| jgld.dll | 0x01420000 | 0x10000000 | "Jackal" 2D engine (larger build); **rebased**, so subtract 0x01420000 and add 0x10000000 to map runtime addresses |
| TERRAIN.DLL | 0x10000000 | 0x10000000 | OpenGL course renderer |
| sound.dll | 0x01f90000 | 0x10000000 | rebased |
| BINKW32.DLL | 0x30000000 | 0x30000000 | RAD Bink video |
| DDRAW.DLL | (Wine) | | loaded at runtime; user not yet identified |

`jgl.dll` (the smaller engine build) and `drvmgt.dll` are not loaded.

## golf.exe

- Sections `stxt774`, `stxt371` and `newidata` come from a copy-protection wrapper; `.text` is not flagged executable, but the code is plain on disk. We do not touch the protection; hooks go in at runtime from our own DLLs.
- Its visible import table is the wrapper's. The original import descriptors survive in `.rdata` (file offset 0xbdc08): binkw32, Terrain, KERNEL32, USER32, WINMM. The IAT is at 0x4ba000.
- Exports four `Terrain` methods of its own: `tileAt`, `getElevation`, `getWall`, `getType` (0x4490d0 to 0x449150).
- Loads the engine dynamically: data contains `"jgld.dll"` (file offset 0xd1704) and `"get_graphsy_object_ptr"`.
- **Start-up: `FUN_0045baf0`** (the game's main routine):
  - sets the screen size globals **`DAT_00822c8c = 800`, `DAT_00822c90 = 600`** (instructions at 0x45baf6 and 0x45bb00; the 32-bit operands are at **0x45bafc** and **0x45bb06**);
  - starts the engine: `FUN_004855b0("jgld.dll", "Sid Meier's SimGolf", 0xffffffff)` → `FUN_004a00a0` (LoadLibraryA + GetProcAddress("get_graphsy_object_ptr"), object stored in **`DAT_0083ad50`**), then engine method 1 creates the window with **`&DAT_0083a7b8` as the screen surface/callback** (the engine calls its vtbl[0](w, h, bpp, 1) whenever the display size changes);
  - sets the display mode: engine method 22 (`+0x58`) with `(800, 600, 16)`;
  - creates the main window object and the screen-sized surface with `FUN_00404970(320) x FUN_00404970(240)`;
  - registers fonts (Arial Bold, Manual SSi Bold from `manu3_.ttf`, Klepto ITC from `klepto_.ttf`, Comic Sans MS Bold, Times New Roman Bold), builds a 64K-entry 16-bit color table, then enters the game.
- **Leftover support for other widths:** `FUN_0042fb90` special-cases screen widths 1024 (`0x400`) and 1280 (`0x500`), and `FUN_004628d0`, `FUN_00462a30`, `FUN_00462be0` branch on widths above 1000 and above 1199. The world-to-screen code uses `width / 2` and `height / 2`. So 1024x768 and 1280x1024 were probably supported during development.
- **Experiment:** `hooks/sghooks.dll` (loaded by terrgl32 at start-up) rewrites the two `mov` immediates when `SIMGOLF_RES=WxH` is set; `run.sh` sizes gamescope to match. Verified to patch in the real game; results of playing at 1024x768 are pending.
- **UI system:** `FUN_004806c0` / `FUN_0047a4c0` create windows `(x, y, w, h, title, flags, parent, ...)`; negative x/y mean "from the right/bottom edge" of the parent (or of the screen via engine `+0xa8`). In play, only the main window goes through it; the HUD is drawn directly.
- **Art:** `FUN_00473bf0(image, x, y, w, h, ...)` cuts a sprite out of a loaded sheet (source coordinates, not screen positions); `FUN_00492000` trims transparent (0xff) edges. Game image objects: +0x4 engine sprite or surface, +0x18/+0x1c size, +0x20/+0x24 trim offset.
- **Draw helpers → engine methods:** `FUN_00473df0` → sprite `+0x5c` (target, x, y, clip); `FUN_00473e60` → sprite `+0x44` (scaled); `FUN_00473f60` → sprite `+0x54` (5 args); `FUN_00475c90` → surface `+0x40` (target, src RECT, dst RECT). Engine classes: sprite (0x50 bytes, vtable 0x1011d380, created by engine `+0x80`), surface (0x4d4 bytes, vtable 0x1011d0b0, created by engine `+0x7c`).
- **HUD map (draw log at 1024x768, `tools/ghidra/drawmap.py`):** `FUN_0040f5c0` draws the in-game HUD: its own draws (x 0–724, y 0–575: logo, stats), control wheel via `FUN_00432ba0` (x 0–146, y 435–570), tool palette via `FUN_00434350` / `FUN_00433190` / `FUN_00435760` (y 474–563). `FUN_0040cc00` ← `FUN_0046d6e0` (198 draws, x 40–544, y 100–570) and `FUN_0043b610` ← `FUN_0043d2a0` look like menu screens. Full-screen copies go through `FUN_00475c60`.
- **Moving a panel needs drawing and click handling shifted together.**
- **Main loop `FUN_0040f5c0`** (12,663 decompiled lines; needs `DecompileOne.java` with a large payload): HUD drawing, input and edge scrolling.
  - Mouse position: `FUN_0047ab50(window, &x, &y)` (engine `+0x2c` cursor position). Click position globals: `DAT_00822d60` (x), `DAT_00822d64` (y).
  - Edge scrolling: pans at x ≤ 0, x ≥ 799 (`cmp edx, 0x31f` at 0x41e199), y ≤ 0, y ≥ 599 (`cmp esi, 0x257` at 0x41e1a3).
  - Bottom HUD click dispatch: y > 440 (`cmp ecx, 0x1b8` at 0x41e406) and x < 200 → control wheel `FUN_00432720(x, y)`; else y > 480 (0x41e459) → tool palette handler by mode `DAT_00567afc`: 0 `FUN_00433040`, 1 `FUN_00434140`, 2 `FUN_00435680`, 3/4 `FUN_00436060`, all `(x, y)`.
  - HUD drawing: stats panel at 0x4194c5–0x41973b; control wheel `FUN_00432ba0(x, y)` (hit test `FUN_004326a0(x, y)`, button table at 0x4c7930); tool palette draw per mode `FUN_00433190` / `FUN_00434350` / `FUN_00435760` / `FUN_004362f0`, all `(mouse x, mouse y)`.
  - Panel drop shadows: sprite method `+0x6c` (target, x, y, ?, clip), through `FUN_00474030` / `0x4740f0`; the stats panel's come from 0x4194e8, 0x4195dc, 0x4196a1 (via `FUN_00421b90`). The sprite list drawn from 0x40f84d and redrawn from 0x420f11, 0x42114b, 0x4212d8 is the **main menu's** buttons, and `FUN_00470140` from 0x407d4b / 0x47c31f is the **world map** ("Where will you build your golf course?"), not HUD pieces.
  - Report windows build a darkened backdrop of the screen; the darkening only covers 800x600 and leftover HUD pieces show in it (not yet handled).
- **Reports (next step):** the Course Report is drawn by `FUN_0044fb30` (called from the main loop and the wheel click handler). It first calls `FUN_00474440(&DAT_00519cd4, 0, 0, 0.25f, 0, 1)`, which copies and darkens the screen into the screen-sized buffer `DAT_00519cd4` through image method `+0x94` (jgld surface 0x1000ad30 / sprite 0x10018920). The 800x600-only darkening is probably in that method. The report is then drawn into the buffer at fixed positions. Plan: a centre rule over `FUN_0044fb30`'s code range (excluding the backdrop copy), fix the darkening size, and find the ✓ button's click test. Another report: `FUN_0043b610` ← `FUN_0043d2a0` (x 0–781, y 0–528). Speech bubble at the top: `FUN_0040d320`.
- **Text menus / message boxes:** `FUN_0046d6e0(centre_x, top_y, flags, ?, ?)`, `__cdecl`; hooked in sghooks (wheel menus follow the wheel, others centred).
- **Rectangle fill:** `FUN_00480b00`, `__thiscall (x, y, w, h, colour)`; hooked through the HUD rules.
- **Partial resolution independence:** `FUN_00404970(x) = (DAT_00822c8c * x + 160) / 320` scales a 320-wide base layout to the screen width (used for both axes, so it assumes 4:3). Only 21 call sites use it, and only 14 functions read the size globals; about 89 literal `800`s and 84 literal `600`s remain, plus other absolute UI coordinates.
- **Surfaces:** `FUN_00474dd0(w, h, depth, ...)` creates an off-screen surface through engine method 31 (`+0x7c`), which returns a surface object whose `+0x4` method allocates it. golf.exe creates 800x600 screen buffers (`FUN_0044bde0`, `FUN_0044b9c0`, `FUN_0044cff0`, `FUN_0044e770`, `FUN_0045f0f0`) and **1024x800** course buffers (`FUN_00442180`, `FUN_0043cd70`, `FUN_0043dbe0`), the 1024x800 16bpp DIB seen in the GDI traces. `FUN_00473bf0` / `FUN_00473c60(surface, x, y, w, h, ...)` fill or copy rectangles at absolute coordinates (UI layout).
- Engine method offsets golf.exe uses (`*DAT_0083ad50 + off`): 0x10, 0x20 (HWND getter), 0x24 (HINSTANCE), 0x2c, 0x30, 0x38, 0x58 (set mode), 0x74–0xc4, and up to 0x150. 0x9c and 0xb4 are the most frequent (roles not yet identified).
- Picks the Terrain orientation from the OS version: after `GetVersionExA`, a major version of 5 or more sets the byte at 0x820f28, which is passed as `Terrain::initSystem`'s bool (call at 0x449841).

## jgld.dll: the "Jackal" engine

- Source paths: `c:\jackdev\jglsprite.cpp` and `jglsprite_8_16c.cpp`. Window class `"JackalClass"`. Debug CRT, with libpng embedded.
- Only one named export: `get_graphsy_object_ptr` (0x10001780). It allocates a 0x14c-byte object and runs its constructor (0x100654b0); its vtable is at **0x1011d640, with 86 methods**. Pointers to the object are stored in globals 0x1012870c and 0x10128420.
- Method summary, from the Windows APIs each reaches:

| # | Address | Role (inferred) |
|---|---|---|
| 0 | 0x100655c0 | initialization (LoadLibrary/GetProcAddress, critical sections) |
| 1 | 0x100657e0 | create the window: class "JackalClass", style WS_POPUP, desktop-sized (`GetSystemMetrics`); store the HWND (+0x144) and window DC (+0x138); then call the game callback `(*param_3->vtbl[0])(width, height, 8, 1)` |
| 2 | 0x100656c0 | destroy the window, restore the display mode |
| 4–7 | | ShowWindow variants |
| 10–14 | | SetWindowPos, cursor position, InvalidateRect, GetWindowRect |
| 21 | 0x10066200 | GetSystemMetrics |
| 25 | 0x100662a0 | set the display mode: if the desktop is 640 wide, show the "must be 800x600" box; otherwise ChangeDisplaySettingsA to the selected mode from the list at 0x101286fc (0x94-byte DEVMODEs), store width/height/bpp (+0x12c/+0x130/+0x134), and call the game callback with them |
| 26–28 | | enumerate and change display modes |
| 29–32 | | create sub-objects (likely surfaces and sprites; not yet identified) |
| 41 | 0x10066470 | BitBlt with IntersectRect (screen update?) |
| 46–49 | | create/delete a compatible DC; create/delete a palette |
| 50–71 | | field getters |
| 72–84 | | message-loop variants (PeekMessage/WaitMessage) |

- Engine object fields: +0x12c width, +0x130 height, +0x134 bits per pixel, +0x138 window DC, +0x140 HINSTANCE, +0x144 HWND, +0x148 game callback object.
- The resolution is **not** fixed inside the engine: it's whatever mode the game selects, reported back through the callback. The 800x600 choice and the UI layout live in golf.exe.

## TERRAIN.DLL

See the main notes in CLAUDE.md / README: a debug build with named C++ exports. `initSystem(w, h, HDC, bool)` at 0x10009c80; the PIXELFORMATDESCRIPTOR is at 0x10063e10. It draws with OpenGL into jgl's 800x600 16bpp back buffer.

## sound.dll

- Exports `create_sound`, `delete_sound`, `init_sound_timer`, `release_sound`, `get_sound_version`, and `Dll_{Wave,Midi,Wave_In}_Device` create/delete.
- Creates DirectSound through `CoCreateInstance(CLSID_DirectSound)` (0x10011d00); its DSOUND ordinal 2 import is `DirectSoundEnumerateA`.
- Software mixer `FUN_100075e0`, driven by a `timeSetEvent` timer, writes into a 64 KB, 22050 Hz stereo looping DirectSound buffer; `FUN_10008070` converts the mixed doubles to 16-bit.
- WAV loader `FUN_100326a0` reads `fmt `, `data`, `smpl` (loop points) and `cue `. Files over 128 KB are streamed through a 128 KB in-memory ring (`FUN_10033300`, using mmioAdvance); smaller files load whole.
- Open bug: a sound's last ~0.3 s sometimes repeats forever (6825 or 6678 frames in two sessions). The stuck sound hasn't been identified.

## Tools

- Ghidra 12.1.2 headless: `/opt/ghidra/support/analyzeHeadless <proj> <name> -import <bin> -postScript DecompileAll.java <outdir>` dumps decompiled C (script in `tools/ghidra/`). For golf.exe, add `-preScript MarkTextExecutable.java`.
- `tools/trace-run.sh`, `tools/audio-trace.sh`, `tools/audio-dump.sh`: traced runs (see each script).

## Screens centred at higher resolutions (sghooks)

- **Title screen:** inside the main routine: first draw 0x40f7a0–0x40f9fc, loop 0x420e35–0x421610. Backdrop `interface\TitleBase.pcx` loaded into 0x568d10 and stretched 800x600 → screen through `FUN_00475d00(target, sx, sy, sw, sh, dx, dy, dw, dh)`; buttons hit-tested by distance (`FUN_0040acd0`) from the mouse (0x421050). Also `logo.pcx` and `interface//Pictbckgrnd1.pcx` screens, and `FUN_0043a400` called from it.
- **World map:** `FUN_0046f550` (0x46f550–0x470a00; `FUN_00470a00` jumps to it), `interface\WorldBase.pcx`; reads the mouse at 0x47016d / 0x4703de against fixed button positions.
- **Load / save screens:** `FUN_0043b610` / `FUN_0043a8c0`, reached through `FUN_0046ddd0`. They run the window system (`FUN_0043d2a0`, 0x4a5xxx); the file list's items are drawn by a callback at 0x40bac9 more than 8 return addresses deep. The list tests the click globals directly: `cmp [0x822d60], 0x140` (x > 320) then `mov eax, [0x822d64]`, row = (y − 116) / 16, at 0x43c7ab and 0x43b3b1.
- **Top 10 Designers:** `FUN_00473470` (copies an 800x600 backdrop to (0,0), then tiles).
- **Professional Accomplishments:** `FUN_0046e810` (a report range).
- **Panel drop shadows:** sprite method `+0x6c` (target, x, y, ?, clip), ret 0x14.
- **Main window object:** 0x519a60. `FUN_00480b00` fills through it onto the screen.
- **Scaling helper `FUN_00404970`:** window heights (0x45bbd4, 0x45be6d: scale(240)) and font sizes (0x45be22, 0x45be3c) are redirected to a height-based version for wide screens. Still width-based: mouse → 320-space conversions (0x46558x–0x46586x, 0x46e01b/36) and a box at 0x46e451–0x46e490 (h, w, y, x → `FUN_0040d0b0`), 0x46e719–0x46e73d, 0x404a92/9e.

## Status and next steps (2026-10-12)

**Working with sghooks** (`SIMGOLF_RES=WxH`, tested at 1024x768 and 1280x720):
- The HUD is anchored to the screen edges: stats panel and its drop shadows, control wheel, tool palette, mood bar.
- Centred: reports, with clicks and darkening covering the whole screen; the title screen and the screens it opens; the world map; the load/save screens, including file-list clicks; Top 10 Designers; Professional Accomplishments.
- Speech bubble along the top (`FUN_0040d320`): centred horizontally.
- Widescreen sizes:
  - window heights and fonts use height-based scaling;
  - the course size class (tier) is chosen from the screen height: <700 → 800x600, 700–899 → 1024x768, ≥900 → 1280x1024;
  - in golf.exe, the tile table (0x44a6e0) and the projection compares (0x42fbd8 / 0x42fbe0) follow the tier;
  - in Terrain.dll, the size compares follow the real size, and the view extent in `initSystem` and `setZoomLevel` is scaled by screen/tier.
- **At 1280x720 this matches native 1024x768**, checked by screenshot comparison after loading and after zooming.

**Fixed since:**
- **Pan drift:** the original 1024x768 tier's `initSystem` height extent (1303) didn't match the screen shape. It now uses 1357.65 (commit 673e3da), and the fix applies at native 1024x768 too.
- **Palette flag:** drawn through sprite `+0x78`, which is now hooked, along with `+0x4c` and `+0x50`.
- **Golfer list:** clipped at x 790 in its own coordinates.
- **Golfer close-up inset:** its limits now follow the screen size.
- **Release:** `make-release.sh` ships sghooks, `patch-terrain.sh` installs it, and the README documents `SIMGOLF_RES`.
- **Checked at 1280x720:** report darkening, centred reports and the Information menu.

- **Clicks right of the tool palette (x ≥ 800):** they now reach the course, through a stub at the palette boundary (0x41e459). Previously every click below y 480 went to the palette.

**Open:**
1. The golfer close-up inset at widescreen isn't verified on screen yet. It needs a moment in a golfer's shot.
3. The release version is still 0.1.0, set by `TERRGL32_VERSION`. Bump it before publishing.

**Testing without the monitor:** `tools/xdrive/load-save.sh WxH :77` runs the game on a private Xvfb display, skips the intro and loads the 8th save. Then use `DISPLAY=:77 tools/xdrive/xdrive click X Y` / `key Escape` and `DISPLAY=:77 import -window root shot.png`. Wheel buttons at 1280x720: + (32,662), − (32,708). At 1024x768 they're 48 px lower. Only one game can run per Wine prefix. Stop it with `tools/xdrive/load-save.sh stop`, which also restores the saved games. The game autosaves while it runs, so the script backs up `Saved Games` and `top10.sve` to `~/.cache/simgolf-xdrive-saves` first. Don't use `pkill -f golf.exe`: it matches the shell running it.
