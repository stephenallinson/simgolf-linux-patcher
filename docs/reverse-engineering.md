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
