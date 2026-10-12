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
 *   SGHOOKS_DRAWLOG=1 also log every distinct draw onto the screen (with
 *                     SGHOOKS_LOG), for mapping the HUD
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

/* ---- draw hooks: HUD placement and the draw log ----
 * The HUD is drawn straight onto the screen at 800x600 positions, through
 * jgld.dll's sprite/surface draw methods and GDI TextOutA. We replace those in
 * jgld.dll's vtables and import table. For each draw onto the screen, the
 * golf.exe return addresses on the stack identify which HUD part is drawing;
 * a matching rule moves it to the right or bottom edge of the real screen.
 * With SGHOOKS_DRAWLOG=1, every distinct draw is also logged.
 * Addresses are jgld.dll's preferred base; it is rebased at runtime. */
#define JGLD_BASE        0x10000000
#define JGLD_IMAGE_SIZE  0x13a000
#define SPRITE_VTBL      0x1011d380
#define SURFACE_VTBL     0x1011d0b0
#define GOLF_TEXT_START  0x401000
#define GOLF_TEXT_END    0x4b942b

enum anchor { ANCHOR_RIGHT, ANCHOR_BOTTOM, ANCHOR_CENTER };

/* A HUD part: draws issued from these golf.exe return addresses move together.
 * Some call sites draw several parts (e.g. a loop over panel shadows); those
 * rules also filter on the draw's original 800x600 position. */
struct hud_rule
{
    const char *name;
    DWORD start, end;          /* return addresses in [start, end) */
    enum anchor anchor;
    BOOL filter;               /* only draws with x in [x0, x1) and y in [y0, y1) */
    int x0, x1, y0, y1;
};

static const struct hud_rule hud_rules[] =
{
    /* HUD routine (0x40f5c0): money / happiness / rating panels, sprites and text */
    {"stats panel", 0x4194b3, 0x419740, ANCHOR_RIGHT},
    /* HUD routine: translucent panel shadows, drawn for all panels from one call
     * site and redrawn from three more */
    {"stats panel shadow", 0x40f84d, 0x40f84e, ANCHOR_RIGHT, TRUE, 600, 800, 0, 200},
    {"stats panel shadow", 0x420f11, 0x420f12, ANCHOR_RIGHT, TRUE, 600, 800, 0, 200},
    {"stats panel shadow", 0x42114b, 0x42114c, ANCHOR_RIGHT, TRUE, 600, 800, 0, 200},
    {"stats panel shadow", 0x4212d8, 0x4212d9, ANCHOR_RIGHT, TRUE, 600, 800, 0, 200},
    /* dark bars behind each stat (626,23 / 669,74 / 706,125), drawn through
     * 0x470140 from these two call sites; 0x47c31f also redraws other things */
    {"stats bars", 0x407d4b, 0x407d4c, ANCHOR_RIGHT, TRUE, 600, 800, 0, 200},
    {"stats bars", 0x47c31f, 0x47c320, ANCHOR_RIGHT, TRUE, 600, 800, 0, 200},
    /* HUD routine's call to the control wheel (0x432ba0) */
    {"control wheel", 0x4197c9, 0x4197ca, ANCHOR_BOTTOM},
};

typedef int (__fastcall *getter_fn)(void *self, void *edx);
typedef int (__fastcall *draw4_fn)(void *self, void *edx, void *a, int b, int c, void *d);
typedef int (__fastcall *draw5_fn)(void *self, void *edx, void *a, void *b, int c, int d, void *e);
typedef int (__fastcall *rect3_fn)(void *self, void *edx, void *target, RECT *src, RECT *dst);
typedef BOOL (WINAPI *textout_fn)(HDC, int, int, LPCSTR, int);

static draw4_fn spr_draw_5c, spr_draw_44;
static draw5_fn spr_draw_54, spr_draw_40;
static rect3_fn surf_blit_40;
static textout_fn real_TextOutA;
static BOOL draw_log;
static int draw_lines;

/* Return addresses into golf.exe's .text found on the stack (each must follow
 * a call instruction), innermost first. */
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

static BOOL is_screen(void *target)
{
    void **vtbl;

    if (!target || !target_surface_vtbl) return FALSE;
    vtbl = *(void ***)target;
    if (vtbl != target_surface_vtbl) return FALSE;
    return ((getter_fn)vtbl[0xd8 / 4])(target, NULL) == (int)screen_w &&
           ((getter_fn)vtbl[0xdc / 4])(target, NULL) == (int)screen_h;
}

/* Size of a jgld surface, or 0x0 if target isn't one. */
static void surface_size(void *target, int *w, int *h)
{
    void **vtbl;

    *w = *h = 0;
    if (!target || !target_surface_vtbl) return;
    vtbl = *(void ***)target;
    if (vtbl != target_surface_vtbl) return;
    *w = ((getter_fn)vtbl[0xd8 / 4])(target, NULL);
    *h = ((getter_fn)vtbl[0xdc / 4])(target, NULL);
}

/* With the draw log on, also log draws into off-screen surfaces (never moved). */
static void log_offscreen(const char *what, void *obj, void *target, int x, int y)
{
    static DWORD seen[4096];
    static int nseen;
    DWORD callers[4] = {0}, key;
    int i, w, h;

    if (!draw_log || draw_lines >= 20000) return;
    surface_size(target, &w, &h);
    golf_callers(callers, 4);
    key = (DWORD)what * 31 + callers[0] * 7 + callers[1] * 3 + callers[2] + x * 131071 + y * 8191 + w;
    for (i = 0; i < nseen && seen[i] != key; i++);
    if (i < nseen) return;
    if (nseen < 4096) seen[nseen++] = key;
    draw_lines++;
    log_msg("offscreen %s obj %p at %d,%d into %p %dx%d callers %08lx %08lx %08lx %08lx",
            what, obj, x, y, target, w, h, callers[0], callers[1], callers[2], callers[3]);
}

static void anchor_offset(enum anchor a, int *dx, int *dy)
{
    *dx = *dy = 0;
    if (a == ANCHOR_RIGHT) *dx = screen_w - 800;
    else if (a == ANCHOR_BOTTOM) *dy = screen_h - 600;
    else { *dx = (screen_w - 800) / 2; *dy = (screen_h - 600) / 2; }
}

/* Find the HUD rule for the current draw (innermost matching caller wins),
 * add its offset to x/y, and log the draw if asked. */
static void place_draw(const char *what, void *obj, int *x, int *y, int w, int h, const char *text)
{
    static DWORD seen[4096];
    static int nseen;
    DWORD callers[8] = {0}, key;
    const struct hud_rule *rule = NULL;
    int n, i, j, dx = 0, dy = 0;

    n = golf_callers(callers, 8);
    if (screen_w != 800 || screen_h != 600)
        for (i = 0; i < n && !rule; i++)
            for (j = 0; j < (int)(sizeof(hud_rules) / sizeof(hud_rules[0])); j++)
            {
                const struct hud_rule *r = &hud_rules[j];
                if (callers[i] < r->start || callers[i] >= r->end) continue;
                if (r->filter && (*x < r->x0 || *x >= r->x1 || *y < r->y0 || *y >= r->y1)) continue;
                rule = r;
                break;
            }
    if (rule) anchor_offset(rule->anchor, &dx, &dy);

    if (draw_log && draw_lines < 20000)
    {
        key = (DWORD)what * 31 + callers[0] * 7 + callers[1] * 3 + callers[2] + *x * 131071 + *y * 8191;
        for (i = 0; i < nseen && seen[i] != key; i++);
        if (i == nseen)
        {
            if (nseen < 4096) seen[nseen++] = key;
            draw_lines++;
            log_msg("draw %s obj %p at %d,%d size %dx%d callers %08lx %08lx %08lx %08lx%s%s%s%s",
                    what, obj, *x, *y, w, h, callers[0], callers[1], callers[2], callers[3],
                    rule ? " moved by " : "", rule ? rule->name : "",
                    text ? " text " : "", text ? text : "");
        }
    }
    *x += dx;
    *y += dy;
}

static int __fastcall spr_draw_5c_hook(void *self, void *edx, void *target, int x, int y, void *clip)
{
    if (is_screen(target)) place_draw("sprite", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite", self, target, x, y);
    return spr_draw_5c(self, edx, target, x, y, clip);
}

static int __fastcall spr_draw_44_hook(void *self, void *edx, void *target, int x, int y, void *clip)
{
    if (is_screen(target)) place_draw("sprite-scaled", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-scaled", self, target, x, y);
    return spr_draw_44(self, edx, target, x, y, clip);
}

static int __fastcall spr_draw_54_hook(void *self, void *edx, void *a, void *target, int x, int y, void *clip)
{
    if (is_screen(target)) place_draw("sprite-54", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-54", self, target, x, y);
    return spr_draw_54(self, edx, a, target, x, y, clip);
}

static int __fastcall spr_draw_40_hook(void *self, void *edx, void *target, void *b, int x, int y, void *clip)
{
    if (is_screen(target)) place_draw("sprite-40", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-40", self, target, x, y);
    return spr_draw_40(self, edx, target, b, x, y, clip);
}

static int __fastcall surf_blit_40_hook(void *self, void *edx, void *target, RECT *src, RECT *dst)
{
    RECT moved;
    int x, y;

    if (!dst || !is_screen(target)) return surf_blit_40(self, edx, target, src, dst);
    x = dst->left;
    y = dst->top;
    place_draw("blit", self, &x, &y, dst->right - dst->left, dst->bottom - dst->top, NULL);
    moved = *dst;
    OffsetRect(&moved, x - dst->left, y - dst->top);
    return surf_blit_40(self, edx, target, src, &moved);
}

/* jgld.dll draws text with GDI onto the back buffer's DC. */
static BOOL WINAPI TextOutA_hook(HDC dc, int x, int y, LPCSTR str, int len)
{
    char buf[40] = "";

    if (draw_log && str) lstrcpynA(buf, str, len + 1 < (int)sizeof(buf) ? len + 1 : (int)sizeof(buf));
    place_draw("text", dc, &x, &y, 0, 0, draw_log ? buf : NULL);
    return real_TextOutA(dc, x, y, str, len);
}

static void *swap_ptr(void **slot, void *hook)
{
    void *orig = *slot;
    DWORD old;

    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return NULL;
    *slot = hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return orig;
}

/* Replace an imported function in a module's import address table. */
static void *hook_import(HMODULE mod, const char *dll, const char *func, void *hook)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)mod + ((IMAGE_DOS_HEADER *)mod)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)mod + dir->VirtualAddress);

    for (; imp->Name; imp++)
    {
        IMAGE_THUNK_DATA *names, *iat;
        if (_stricmp((char *)mod + imp->Name, dll)) continue;
        names = (IMAGE_THUNK_DATA *)((BYTE *)mod + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        iat = (IMAGE_THUNK_DATA *)((BYTE *)mod + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++)
        {
            IMAGE_IMPORT_BY_NAME *ibn;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)((BYTE *)mod + names->u1.AddressOfData);
            if (!strcmp((char *)ibn->Name, func)) return swap_ptr((void **)&iat->u1.Function, hook);
        }
    }
    return NULL;
}

static void hook_draws(const char *dir)
{
    char path[MAX_PATH + 16];
    HMODULE jgld;
    IMAGE_NT_HEADERS *nt;
    void **spr, **surf;

    snprintf(path, sizeof(path), "%s\\jgld.dll", dir);
    if (!(jgld = LoadLibraryA(path))) { log_msg("draw hooks: can't load %s", path); return; }
    nt = (IMAGE_NT_HEADERS *)((BYTE *)jgld + ((IMAGE_DOS_HEADER *)jgld)->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage != JGLD_IMAGE_SIZE)
    {
        log_msg("draw hooks: unexpected jgld.dll (size %#lx), not hooked", nt->OptionalHeader.SizeOfImage);
        return;
    }
    spr = (void **)((BYTE *)jgld + (SPRITE_VTBL - JGLD_BASE));
    surf = (void **)((BYTE *)jgld + (SURFACE_VTBL - JGLD_BASE));
    target_surface_vtbl = surf;
    spr_draw_5c = swap_ptr(&spr[0x5c / 4], spr_draw_5c_hook);
    spr_draw_44 = swap_ptr(&spr[0x44 / 4], spr_draw_44_hook);
    spr_draw_54 = swap_ptr(&spr[0x54 / 4], spr_draw_54_hook);
    spr_draw_40 = swap_ptr(&spr[0x40 / 4], spr_draw_40_hook);
    surf_blit_40 = swap_ptr(&surf[0x40 / 4], surf_blit_40_hook);
    real_TextOutA = hook_import(jgld, "GDI32.dll", "TextOutA", TextOutA_hook);
    log_msg("draw hooks: jgld.dll at %p, TextOutA %s", jgld, real_TextOutA ? "hooked" : "NOT hooked");
}

/* ---- HUD click handling ----
 * Each moved HUD part's hit test gets the mouse position moved back by the
 * same offset, so the game's own 800x600 button logic still works. */

/* Control wheel: int __cdecl hit_test(x, y) at 0x4326a0, nearest button or -1. */
typedef int (__cdecl *wheel_hit_fn)(int x, int y);
static wheel_hit_fn wheel_hit_orig;

static int __cdecl wheel_hit_hook(int x, int y)
{
    return wheel_hit_orig(x, y - ((int)screen_h - 600));
}

static void hook_hud_input(void)
{
    static const BYTE wheel_prologue[] = {0x51, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x14};  /* push ecx/ebx/ebp; mov ebp,[esp+0x14] */

    wheel_hit_orig = install_hook("control wheel hit test", 0x4326a0, wheel_prologue,
                                  sizeof(wheel_prologue), wheel_hit_hook);
}

/* Edge scrolling (main loop, after reading the mouse at 0x41e174) pans when
 *   x >= 799: cmp edx, 0x31f at 0x41e199 (81 fa 1f 03 00 00)
 *   y >= 599: cmp esi, 0x257 at 0x41e1a3 (81 fe 57 02 00 00)
 * i.e. the last column and row of an 800x600 screen. */
static void patch_edge_scroll(void)
{
    static const BYTE orig_x[] = {0x81, 0xfa, 0x1f, 0x03, 0x00, 0x00};
    static const BYTE orig_y[] = {0x81, 0xfe, 0x57, 0x02, 0x00, 0x00};
    BYTE new_x[sizeof(orig_x)], new_y[sizeof(orig_y)];
    DWORD last_x = screen_w - 1, last_y = screen_h - 1;

    memcpy(new_x, orig_x, sizeof(new_x));
    memcpy(new_y, orig_y, sizeof(new_y));
    memcpy(new_x + 2, &last_x, 4);
    memcpy(new_y + 2, &last_y, 4);
    patch_bytes("edge scroll right", 0x41e199, orig_x, new_x, sizeof(orig_x));
    patch_bytes("edge scroll bottom", 0x41e1a3, orig_y, new_y, sizeof(orig_y));
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
        patch_edge_scroll();
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
    draw_log = log_file && GetEnvironmentVariableA("SGHOOKS_DRAWLOG", path, sizeof(path));
    if (draw_log || screen_w != 800 || screen_h != 600)
    {
        char dir[MAX_PATH], *slash;
        GetModuleFileNameA(inst, dir, sizeof(dir));
        if ((slash = strrchr(dir, '\\'))) *slash = 0;
        hook_draws(dir);
    }
    if (screen_w != 800 || screen_h != 600) hook_hud_input();
    return TRUE;
}
