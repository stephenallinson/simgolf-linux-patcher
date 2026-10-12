/*
 * sghooks.dll: runtime patches for golf.exe, loaded by terrgl32.dll at process
 * start (before golf.exe's own code runs). golf.exe on disk is never modified.
 *
 * Each patch checks the original bytes first and is skipped if they differ,
 * so a different game version is left alone.
 *
 * Environment:
 *   SIMGOLF_RES=WxH   screen size instead of 800x600 (experimental); top-level
 *                     UI windows laid out for 800x600 are moved to the matching
 *                     screen edge
 *   SGHOOKS_LOG=path  Windows path for a log file (also lists every UI window
 *                     the game creates)
 *   SGHOOKS_DRAWLOG=1 also log every distinct draw onto a screen-sized surface
 *                     (with SGHOOKS_LOG), for mapping the HUD
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *log_file;

static void log_msg(const char *fmt, ...)
{
    va_list args;

    if (!log_file) return;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
}

/* Replace len bytes at addr with repl if they currently equal orig. */
static BOOL patch_bytes(const char *what, DWORD addr, const void *orig, const void *repl, SIZE_T len)
{
    void *p = (void *)addr;
    DWORD old;

    if (memcmp(p, orig, len))
    {
        log_msg("%s: unexpected bytes at %#lx, skipped", what, addr);
        return FALSE;
    }
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old))
    {
        log_msg("%s: VirtualProtect failed (%lu)", what, GetLastError());
        return FALSE;
    }
    memcpy(p, repl, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    log_msg("%s: patched %lu bytes at %#lx", what, (unsigned long)len, addr);
    return TRUE;
}

/* Hook a function: overwrite its first `len` bytes (whole instructions that
 * can run anywhere, at least 5) with a jump to `detour`, and return a
 * trampoline that runs those bytes and continues in the original. */
static void *install_hook(const char *what, DWORD addr, const void *orig, SIZE_T len, void *detour)
{
    BYTE *tramp, jmp[5];
    DWORD old;

    if (len < 5 || memcmp((void *)addr, orig, len))
    {
        log_msg("%s: unexpected bytes at %#lx, not hooked", what, addr);
        return NULL;
    }
    if (!(tramp = VirtualAlloc(NULL, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) return NULL;
    memcpy(tramp, orig, len);
    tramp[len] = 0xe9;
    *(DWORD *)(tramp + len + 1) = addr + len - ((DWORD)tramp + len + 5);

    jmp[0] = 0xe9;
    *(DWORD *)(jmp + 1) = (DWORD)detour - (addr + 5);
    if (!VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old)) return NULL;
    memcpy((void *)addr, jmp, 5);
    memset((BYTE *)addr + 5, 0x90, len - 5);
    VirtualProtect((void *)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, len);
    log_msg("%s: hooked %#lx", what, addr);
    return tramp;
}

static unsigned int screen_w = 800, screen_h = 600;
static void **target_surface_vtbl;

/* Window setup, 0x47a4c0: int __thiscall(x, y, w, h, title, flags, parent, ?, ?).
 * Negative x/y already mean "from the right/bottom edge" of the parent, or of
 * the screen for top-level windows (parent == NULL). Written as __fastcall
 * with a dummy edx so the stack layout matches __thiscall. */
typedef int (__fastcall *window_setup_fn)(void *self, void *edx, int x, int y, int w, int h,
                                         const char *title, unsigned int flags, void *parent,
                                         void *a9, void *a10);
static window_setup_fn window_setup_orig;

/* Full-screen top-level windows (the game's main window): their children are
 * effectively positioned on the screen too. */
#define MAX_ROOTS 8
static void *roots[MAX_ROOTS];
static int nroots;

static BOOL is_root(void *w)
{
    int i;
    for (i = 0; i < nroots; i++) if (roots[i] == w) return TRUE;
    return FALSE;
}

/* Move a top-level window laid out for 800x600 to the matching edge of the
 * real screen: flush right/bottom stays flush, centred stays centred. */
static void anchor_window(int *x, int *y, int w, int h)
{
    int dx = screen_w - 800, dy = screen_h - 600;

    if (w < 800 && *x >= 0)
    {
        if (abs(*x + w / 2 - 400) <= 8) *x += dx / 2;
        else if (*x + w >= 800 - 24 && *x + w / 2 > 400) *x += dx;
    }
    if (h < 600 && *y >= 0)
    {
        if (abs(*y + h / 2 - 300) <= 8) *y += dy / 2;
        else if (*y + h >= 600 - 24 && *y + h / 2 > 300) *y += dy;
    }
}

static int __fastcall window_setup_hook(void *self, void *edx, int x, int y, int w, int h,
                                        const char *title, unsigned int flags, void *parent,
                                        void *a9, void *a10)
{
    int nx = x, ny = y;
    char name[48] = "";

    if (title && !IsBadStringPtrA(title, sizeof(name) - 1))
        lstrcpynA(name, title, sizeof(name));
    if (!parent && w == (int)screen_w && h == (int)screen_h && nroots < MAX_ROOTS && !is_root(self))
        roots[nroots++] = self;
    if ((!parent || is_root(parent)) && (screen_w != 800 || screen_h != 600))
        anchor_window(&nx, &ny, w, h);
    log_msg("window %p: %d,%d %dx%d -> %d,%d flags %#x parent %p caller %p \"%s\"",
            self, x, y, w, h, nx, ny, flags, parent, __builtin_return_address(0), name);
    return window_setup_orig(self, edx, nx, ny, w, h, title, flags, parent, a9, a10);
}

static void hook_windows(void)
{
    static const BYTE prologue[] = {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00};  /* mov eax, fs:[0] */

    window_setup_orig = install_hook("window setup", 0x47a4c0, prologue, sizeof(prologue),
                                     window_setup_hook);
}

/* ---- draw logger (SGHOOKS_DRAWLOG) ----
 * Replaces draw methods in jgld.dll's sprite and surface vtables and logs each
 * distinct draw onto a screen-sized surface, with the golf.exe return addresses
 * found on the stack. Preferred-base addresses; jgld.dll is rebased at runtime. */
#define JGLD_BASE        0x10000000
#define JGLD_IMAGE_SIZE  0x13a000
#define SPRITE_VTBL      0x1011d380
#define SURFACE_VTBL     0x1011d0b0
#define GOLF_TEXT_START  0x401000
#define GOLF_TEXT_END    0x4b942b

typedef int (__fastcall *getter_fn)(void *self, void *edx);
typedef int (__fastcall *draw4_fn)(void *self, void *edx, void *a, int b, int c, void *d);
typedef int (__fastcall *draw5_fn)(void *self, void *edx, void *a, void *b, int c, int d, void *e);
typedef int (__fastcall *rect3_fn)(void *self, void *edx, void *target, RECT *src, RECT *dst);

static draw4_fn spr_draw_5c, spr_draw_44;
static draw5_fn spr_draw_54, spr_draw_40;
static rect3_fn surf_blit_40;
static int draw_lines;

/* Return addresses into golf.exe's .text found on the stack (each must follow
 * a call instruction). */
static int golf_callers(DWORD *out, int max)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    DWORD *sp = __builtin_frame_address(0), *end = sp + 512;
    int n = 0;

    if ((void *)end > tib->StackBase) end = tib->StackBase;

    for (; sp < end && n < max; sp++)
    {
        DWORD a = *sp;
        const BYTE *c = (const BYTE *)a;
        if (a < GOLF_TEXT_START + 6 || a >= GOLF_TEXT_END) continue;
        if (c[-5] == 0xe8 || (c[-6] == 0xff && (c[-5] & 0x38) == 0x10) ||
            (c[-2] == 0xff && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xff && (c[-2] & 0x38) == 0x10))
            out[n++] = a;
    }
    return n;
}

static BOOL screen_sized(void *target)
{
    void **vtbl;
    int w, h;

    if (!target) return FALSE;
    vtbl = *(void ***)target;
    if (vtbl != (void **)target_surface_vtbl) return FALSE;
    w = ((getter_fn)vtbl[0xd8 / 4])(target, NULL);
    h = ((getter_fn)vtbl[0xdc / 4])(target, NULL);
    return w == (int)screen_w && h == (int)screen_h;
}

/* Log a draw once per (method, callers, position). */
static void log_draw(const char *what, void *obj, void *target, int x, int y, int w, int h)
{
    static DWORD seen[4096];
    static int nseen;
    DWORD callers[4] = {0}, key;
    int n, i;

    if (!screen_sized(target) || draw_lines > 20000) return;
    n = golf_callers(callers, 4);
    key = (DWORD)what * 31 + callers[0] * 7 + callers[1] * 3 + callers[2] + x * 131071 + y * 8191;
    for (i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 4096) seen[nseen++] = key;
    draw_lines++;
    log_msg("draw %s obj %p at %d,%d size %dx%d callers %08lx %08lx %08lx %08lx",
            what, obj, x, y, w, h, callers[0], callers[1], callers[2], callers[3]);
    (void)n;
}

static int __fastcall spr_draw_5c_hook(void *self, void *edx, void *target, int x, int y, void *clip)
{
    log_draw("sprite", self, target, x, y, 0, 0);
    return spr_draw_5c(self, edx, target, x, y, clip);
}

static int __fastcall spr_draw_44_hook(void *self, void *edx, void *target, int x, int y, void *clip)
{
    log_draw("sprite-scaled", self, target, x, y, 0, 0);
    return spr_draw_44(self, edx, target, x, y, clip);
}

static int __fastcall spr_draw_54_hook(void *self, void *edx, void *a, void *target, int x, int y, void *clip)
{
    log_draw("sprite-54", self, target, x, y, 0, 0);
    return spr_draw_54(self, edx, a, target, x, y, clip);
}

static int __fastcall spr_draw_40_hook(void *self, void *edx, void *target, void *b, int x, int y, void *clip)
{
    log_draw("sprite-40", self, target, x, y, 0, 0);
    return spr_draw_40(self, edx, target, b, x, y, clip);
}

static int __fastcall surf_blit_40_hook(void *self, void *edx, void *target, RECT *src, RECT *dst)
{
    if (dst) log_draw("blit", self, target, dst->left, dst->top, dst->right - dst->left, dst->bottom - dst->top);
    return surf_blit_40(self, edx, target, src, dst);
}

static void *swap_vtbl(void **slot, void *hook)
{
    void *orig = *slot;
    DWORD old;

    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return NULL;
    *slot = hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return orig;
}

static void hook_draws(const char *dir)
{
    char path[MAX_PATH + 16];
    HMODULE jgld;
    IMAGE_NT_HEADERS *nt;
    void **spr, **surf;

    snprintf(path, sizeof(path), "%s\\jgld.dll", dir);
    if (!(jgld = LoadLibraryA(path))) { log_msg("draw log: can't load %s", path); return; }
    nt = (IMAGE_NT_HEADERS *)((BYTE *)jgld + ((IMAGE_DOS_HEADER *)jgld)->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage != JGLD_IMAGE_SIZE)
    {
        log_msg("draw log: unexpected jgld.dll (size %#lx), not hooked", nt->OptionalHeader.SizeOfImage);
        return;
    }
    spr = (void **)((BYTE *)jgld + (SPRITE_VTBL - JGLD_BASE));
    surf = (void **)((BYTE *)jgld + (SURFACE_VTBL - JGLD_BASE));
    target_surface_vtbl = surf;
    spr_draw_5c = swap_vtbl(&spr[0x5c / 4], spr_draw_5c_hook);
    spr_draw_44 = swap_vtbl(&spr[0x44 / 4], spr_draw_44_hook);
    spr_draw_54 = swap_vtbl(&spr[0x54 / 4], spr_draw_54_hook);
    spr_draw_40 = swap_vtbl(&spr[0x40 / 4], spr_draw_40_hook);
    surf_blit_40 = swap_vtbl(&surf[0x40 / 4], surf_blit_40_hook);
    log_msg("draw log: hooked jgld.dll at %p", jgld);
}

/* The game's main routine (0x45baf0) starts with
 *   mov dword [0x822c8c], 800   ; c7 05 8c 2c 82 00 20 03 00 00
 *   mov dword [0x822c90], 600   ; c7 05 90 2c 82 00 58 02 00 00
 * Everything else reads these globals; nothing else writes them. */
static void patch_resolution(void)
{
    static const BYTE orig_w[] = {0xc7, 0x05, 0x8c, 0x2c, 0x82, 0x00, 0x20, 0x03, 0x00, 0x00};
    static const BYTE orig_h[] = {0xc7, 0x05, 0x90, 0x2c, 0x82, 0x00, 0x58, 0x02, 0x00, 0x00};
    BYTE new_w[sizeof(orig_w)], new_h[sizeof(orig_h)];
    char buf[32];
    unsigned int w, h;

    if (!GetEnvironmentVariableA("SIMGOLF_RES", buf, sizeof(buf))) return;
    if (sscanf(buf, "%ux%u", &w, &h) != 2 || w < 640 || h < 480 || w > 7680 || h > 4320)
    {
        log_msg("SIMGOLF_RES=%s not understood (want e.g. 1024x768)", buf);
        return;
    }
    memcpy(new_w, orig_w, sizeof(new_w));
    memcpy(new_h, orig_h, sizeof(new_h));
    memcpy(new_w + 6, &w, 4);
    memcpy(new_h + 6, &h, 4);
    if (patch_bytes("screen width", 0x45baf6, orig_w, new_w, sizeof(orig_w)) &&
        patch_bytes("screen height", 0x45bb00, orig_h, new_h, sizeof(orig_h)))
    {
        screen_w = w;
        screen_h = h;
    }
    log_msg("requested resolution %ux%u", w, h);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    char path[MAX_PATH];
    char exe[MAX_PATH], *name;

    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    if (GetEnvironmentVariableA("SGHOOKS_LOG", path, sizeof(path))) log_file = fopen(path, "w");

    /* only patch golf.exe, not e.g. the shim's test programs */
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    log_msg("sghooks loaded into %s", exe);
    name = strrchr(exe, '\\');
    if (_stricmp(name ? name + 1 : exe, "golf.exe")) return TRUE;
    patch_resolution();
    if (log_file || screen_w != 800 || screen_h != 600) hook_windows();
    if (log_file && GetEnvironmentVariableA("SGHOOKS_DRAWLOG", path, sizeof(path)))
    {
        char dir[MAX_PATH], *slash;
        GetModuleFileNameA(inst, dir, sizeof(dir));
        if ((slash = strrchr(dir, '\\'))) *slash = 0;
        hook_draws(dir);
    }
    return TRUE;
}
