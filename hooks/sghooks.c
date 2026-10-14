/*
 * sghooks.dll: runtime patches for golf.exe, loaded by terrgl32.dll at process
 * start (before golf.exe's own code runs). golf.exe on disk is never modified.
 *
 * Each patch checks the original bytes first and is skipped if they differ,
 * so a different game version is left alone.
 *
 * Environment:
 *   SIMGOLF_ART=large  use the game's enlarged course art (1024x768 / 1280x1024
 *                     tiers, by screen height) instead of the 800x600 art
 *   SIMGOLF_RES=WxH   screen size instead of 800x600 (experimental); top-level
 *                     UI windows laid out for 800x600 are moved to the matching
 *                     screen edge
 *   SGHOOKS_LOG=path  Windows path for a log file (also lists every UI window
 *                     the game creates)
 *   SGHOOKS_NO_MENUS=1 leave text menus and message boxes where they are
 *   SGHOOKS_DRAWLOG=1 also log every distinct draw onto the screen (with
 *                     SGHOOKS_LOG), for mapping the HUD
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *log_file;
static CRITICAL_SECTION log_lock;

static void log_msg(const char *fmt, ...)
{
    va_list args;

    if (!log_file) return;
    EnterCriticalSection(&log_lock);
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
    LeaveCriticalSection(&log_lock);
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

/* RIGHT / BOTTOM: follow that screen edge; CENTER: centred; TOP: centred
 * horizontally, at the top; NONE: never moved */
enum anchor { ANCHOR_RIGHT, ANCHOR_BOTTOM, ANCHOR_CENTER, ANCHOR_TOP, ANCHOR_NONE };

/* A HUD part: draws issued from these golf.exe return addresses move together.
 * A rule can also filter on the draw's original 800x600 position, for call
 * sites that draw several parts. */
struct hud_rule
{
    const char *name;
    DWORD start, end;          /* return addresses in [start, end) */
    enum anchor anchor;
    BOOL filter;               /* only draws with x in [x0, x1) and y in [y0, y1) */
    int x0, x1, y0, y1;
    int clip_x;                /* if set, draws at original x >= clip_x are skipped */
};

static const struct hud_rule hud_rules[] =
{
    /* text menus / message boxes (0x46d6e0) are moved by their own hook; this
     * stops the report rules below from moving them a second time */
    {"text menu", 0x46d6e0, 0x46dd0e, ANCHOR_NONE},
    /* report and info windows: each darkens the screen with black.pcx, then
     * draws itself at fixed 800x600 positions; centre them */
    {"report", 0x44cff0, 0x44e770, ANCHOR_CENTER},
    {"report", 0x44e770, 0x44f6b0, ANCHOR_CENTER},
    {"report", 0x44f6b0, 0x44faf0, ANCHOR_CENTER},
    {"report", 0x44fb30, 0x453260, ANCHOR_CENTER},
    {"report", 0x453330, 0x4546b0, ANCHOR_CENTER},
    {"report", 0x4546b0, 0x454c50, ANCHOR_CENTER},
    {"report", 0x454c50, 0x455a30, ANCHOR_CENTER},
    {"report", 0x455a30, 0x455ed0, ANCHOR_CENTER},
    {"report", 0x455ed0, 0x456b70, ANCHOR_CENTER},
    {"report", 0x459400, 0x459850, ANCHOR_CENTER},
    {"report", 0x459850, 0x45a090, ANCHOR_CENTER},
    {"report", 0x45a090, 0x45ae30, ANCHOR_CENTER},
    {"report", 0x46e810, 0x46f1d0, ANCHOR_CENTER},
    /* title screen, inside the main routine: first draw, then its loop (also the
     * logo and Pictbckgrnd1 screens, and screens it calls, e.g. 0x43a400) */
    {"title screen", 0x40f7a0, 0x40f9fc, ANCHOR_CENTER},
    {"title screen", 0x420e35, 0x421610, ANCHOR_CENTER},
    /* world map ("Where will you build your golf course?", load/save game) */
    {"world map", 0x46f550, 0x470a00, ANCHOR_CENTER},
    /* load game screen (0x43b610, with its save list) and its sibling 0x43a8c0,
     * opened through 0x46ddd0 from the title screen and elsewhere */
    {"load/save screen", 0x43a8c0, 0x43b610, ANCHOR_CENTER},
    {"load/save screen", 0x43b610, 0x43cb50, ANCHOR_CENTER},
    /* Top 10 Designers (from the wheel's Information menu) */
    {"top 10 designers", 0x473470, 0x473a70, ANCHOR_CENTER},
    /* speech bubble along the top (face and message), 0x40d320, called each
     * frame through 0x40d6a0 */
    {"speech bubble", 0x40d320, 0x40d6a0, ANCHOR_TOP},
    /* HUD routine (0x40f5c0): money / happiness / rating panels, sprites and
     * text, and their drop shadows (sprite +0x6c through 0x421b90) */
    {"stats panel", 0x4194b3, 0x419740, ANCHOR_RIGHT},
    /* HUD routine's call to the control wheel (0x432ba0) */
    {"control wheel", 0x4197c9, 0x4197ca, ANCHOR_BOTTOM},
    /* HUD routine's call to 0x4321d0, which also draws the wheel's base */
    {"control wheel", 0x419762, 0x419763, ANCHOR_BOTTOM},
    /* HUD routine: golfer mood bar along the bottom (faces and numbers) */
    {"mood bar", 0x418d40, 0x418eea, ANCHOR_BOTTOM},
    /* HUD routine's calls to the tool palette, one per palette mode:
     * 0x433190, 0x434350, 0x435760, 0x4362f0 (modes 3 and 4). The golfer list
     * (mode 3/4) draws every group (121 px apart: 673, 794, 915, ...) and
     * relies on the 800-wide screen to cut off those past its panel; skip
     * draws from x 790 on (only a 6 px sliver of the 794 group ever showed). */
    {"tool palette", 0x4197ea, 0x4197eb, ANCHOR_BOTTOM, FALSE, 0, 0, 0, 0, 790},
    {"tool palette", 0x419816, 0x419817, ANCHOR_BOTTOM, FALSE, 0, 0, 0, 0, 790},
    {"tool palette", 0x419842, 0x419843, ANCHOR_BOTTOM, FALSE, 0, 0, 0, 0, 790},
    {"tool palette", 0x4198b3, 0x4198b4, ANCHOR_BOTTOM, FALSE, 0, 0, 0, 0, 790},
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
#define DRAW_LOG_MAX 300000

/* Draw-log deduplication: a set of line keys (open addressing). Returns TRUE
 * the first time a key is seen; once the set is full, nothing more is new. */
#define SEEN_SIZE 65536
static BOOL first_seen(DWORD key)
{
    static DWORD seen[SEEN_SIZE];
    static int count;
    DWORD i;

    if (!key) key = 1;   /* 0 marks an empty slot */
    for (i = (key * 2654435761u) & (SEEN_SIZE - 1); seen[i]; i = (i + 1) & (SEEN_SIZE - 1))
        if (seen[i] == key) return FALSE;
    if (count >= SEEN_SIZE * 3 / 4) return FALSE;
    seen[i] = key;
    count++;
    return TRUE;
}

/* Return addresses into golf.exe's .text found on the stack (each must follow
 * a call instruction), innermost first. The main routine (0x40f5c0) is the
 * outermost frame that matters, and its big frame holds stale addresses, so
 * the scan stops at the first return address into it. */
#define MAX_CALLERS 16   /* window-system callbacks (e.g. list boxes) sit deep */
#define MAIN_START 0x40f5c0
#define MAIN_END   0x421b60
static int golf_callers(DWORD *out, int max)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    DWORD *sp = __builtin_frame_address(0), *end = sp + 1024;
    int n = 0, i;

    if ((void *)end > tib->StackBase) end = tib->StackBase;
    for (; sp < end && n < max; sp++)
    {
        DWORD a = *sp;
        const BYTE *c = (const BYTE *)a;
        if (a < GOLF_TEXT_START + 6 || a >= GOLF_TEXT_END) continue;
        if (!(c[-5] == 0xe8 || (c[-6] == 0xff && (c[-5] & 0x38) == 0x10) ||
              (c[-2] == 0xff && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xff && (c[-2] & 0x38) == 0x10)))
            continue;
        /* stale copies of a return address can sit in many stack slots; keep one */
        for (i = 0; i < n && out[i] != a; i++);
        if (i == n) out[n++] = a;
        if (a >= MAIN_START && a < MAIN_END) break;
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
    DWORD callers[4] = {0}, key;
    int w, h;

    if (!draw_log || draw_lines >= DRAW_LOG_MAX) return;
    surface_size(target, &w, &h);
    golf_callers(callers, 4);
    key = (DWORD)what * 31 + callers[0] * 7 + callers[1] * 3 + callers[2] + x * 131071 + y * 8191 + w;
    if (!first_seen(key)) return;
    draw_lines++;
    log_msg("offscreen %s obj %p at %d,%d into %p %dx%d callers %08lx %08lx %08lx %08lx",
            what, obj, x, y, target, w, h, callers[0], callers[1], callers[2], callers[3]);
}

static void anchor_offset(enum anchor a, int *dx, int *dy)
{
    *dx = *dy = 0;
    if (a == ANCHOR_RIGHT) *dx = screen_w - 800;
    else if (a == ANCHOR_BOTTOM) *dy = screen_h - 600;
    else if (a == ANCHOR_CENTER) { *dx = (screen_w - 800) / 2; *dy = (screen_h - 600) / 2; }
    else if (a == ANCHOR_TOP) *dx = (screen_w - 800) / 2;
}

/* The HUD rule for the innermost caller that matches one, or NULL. */
static const struct hud_rule *find_rule(const DWORD *callers, int n, int x, int y)
{
    int i, j;

    if (screen_w == 800 && screen_h == 600) return NULL;
    for (i = 0; i < n; i++)
        for (j = 0; j < (int)(sizeof(hud_rules) / sizeof(hud_rules[0])); j++)
        {
            const struct hud_rule *r = &hud_rules[j];
            if (callers[i] < r->start || callers[i] >= r->end) continue;
            if (r->filter && (x < r->x0 || x >= r->x1 || y < r->y0 || y >= r->y1)) continue;
            return r;
        }
    return NULL;
}

/* Find the HUD rule for the current draw, add its offset to x/y, and log the
 * draw if asked. Returns the rule. */
/* Set by place_draw when the draw's rule clips it; the hook then skips it. */
static BOOL draw_clipped;

static const struct hud_rule *place_draw(const char *what, void *obj, int *x, int *y, int w, int h, const char *text)
{
    DWORD callers[MAX_CALLERS] = {0}, key;
    const struct hud_rule *rule;
    int n, i, dx = 0, dy = 0;

    n = golf_callers(callers, MAX_CALLERS);
    rule = find_rule(callers, n, *x, *y);
    draw_clipped = rule && rule->clip_x && *x >= rule->clip_x;
    /* full-screen copies (backdrops) never move */
    if (rule && !(w == (int)screen_w && h == (int)screen_h)) anchor_offset(rule->anchor, &dx, &dy);

    if (draw_log && draw_lines < DRAW_LOG_MAX)
    {
        key = (DWORD)what * 31 + callers[0] * 7 + callers[1] * 3 + callers[2] + *x * 131071 + *y * 8191;
        if (first_seen(key))
        {
            draw_lines++;
            char list[MAX_CALLERS * 9 + 1] = "";
            for (i = 0; i < n; i++) sprintf(list + i * 9, " %08lx", callers[i]);
            log_msg("draw %s obj %p at %d,%d size %dx%d callers%s%s%s%s%s",
                    what, obj, *x, *y, w, h, list,
                    rule ? " moved by " : "", rule ? rule->name : "",
                    text ? " text " : "", text ? text : "");
        }
    }
    *x += dx;
    *y += dy;
    return rule;
}

static int __fastcall spr_draw_5c_hook(void *self, void *edx, void *target, int x, int y, void *clip)
{
    if (is_screen(target)) place_draw("sprite", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite", self, target, x, y);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return spr_draw_5c(self, edx, target, x, y, clip);
}

static int __fastcall spr_draw_44_hook(void *self, void *edx, void *target, int x, int y, void *clip)
{
    if (is_screen(target)) place_draw("sprite-scaled", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-scaled", self, target, x, y);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return spr_draw_44(self, edx, target, x, y, clip);
}

static int __fastcall spr_draw_54_hook(void *self, void *edx, void *a, void *target, int x, int y, void *clip)
{
    if (is_screen(target)) place_draw("sprite-54", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-54", self, target, x, y);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return spr_draw_54(self, edx, a, target, x, y, clip);
}

static int __fastcall spr_draw_40_hook(void *self, void *edx, void *target, void *b, int x, int y, void *clip)
{
    if (is_screen(target)) place_draw("sprite-40", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-40", self, target, x, y);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return spr_draw_40(self, edx, target, b, x, y, clip);
}

/* Sprite +0x6c: drop shadow (target, x, y, ?, clip), through golf.exe's
 * 0x474030 / 0x4740f0 (the panels' shadows, and golfers'). */
typedef int (__fastcall *shadow_fn)(void *self, void *edx, void *target, int x, int y, int a, void *clip);
static shadow_fn spr_shadow_6c;

static int __fastcall spr_shadow_6c_hook(void *self, void *edx, void *target, int x, int y, int a, void *clip)
{
    if (is_screen(target)) place_draw("shadow", self, &x, &y, 0, 0, NULL);
    else log_offscreen("shadow", self, target, x, y);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return spr_shadow_6c(self, edx, target, x, y, a, clip);
}

static void fill_black(int x, int y, int w, int h);

/* Sprite +0x78 (target, x, y, ?, clip), ret 0x14, through 0x4741b0; +0x4c
 * (target, x, y, ?, clip, ?), ret 0x18, through 0x474260; +0x50 (a, b, c, x, y,
 * clip), ret 0x18, through 0x473fc0, with the target among a / b / c. */
typedef int (__fastcall *draw78_fn)(void *self, void *edx, void *target, int x, int y, int a, void *clip);
typedef int (__fastcall *draw4c_fn)(void *self, void *edx, void *target, int x, int y, int a, void *clip, int b);
typedef int (__fastcall *draw50_fn)(void *self, void *edx, void *a, void *b, void *c, int x, int y, void *clip);
static draw78_fn spr_draw_78;
static draw4c_fn spr_draw_4c;
static draw50_fn spr_draw_50;

static int __fastcall spr_draw_78_hook(void *self, void *edx, void *target, int x, int y, int a, void *clip)
{
    if (is_screen(target)) place_draw("sprite-78", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-78", self, target, x, y);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return spr_draw_78(self, edx, target, x, y, a, clip);
}

static int __fastcall spr_draw_4c_hook(void *self, void *edx, void *target, int x, int y, int a, void *clip, int b)
{
    if (is_screen(target)) place_draw("sprite-4c", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-4c", self, target, x, y);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return spr_draw_4c(self, edx, target, x, y, a, clip, b);
}

static int __fastcall spr_draw_50_hook(void *self, void *edx, void *a, void *b, void *c, int x, int y, void *clip)
{
    if (is_screen(a) || is_screen(b) || is_screen(c)) place_draw("sprite-50", self, &x, &y, 0, 0, NULL);
    else log_offscreen("sprite-50", self, a, x, y);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return spr_draw_50(self, edx, a, b, c, x, y, clip);
}

static int __fastcall surf_blit_40_hook(void *self, void *edx, void *target, RECT *src, RECT *dst)
{
    const struct hud_rule *rule;
    RECT moved;
    int x, y, w, h;

    if (!dst || !is_screen(target)) return surf_blit_40(self, edx, target, src, dst);
    x = dst->left;
    y = dst->top;
    w = dst->right - dst->left;
    h = dst->bottom - dst->top;
    rule = place_draw("blit", self, &x, &y, w, h, NULL);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    moved = *dst;
    OffsetRect(&moved, x - dst->left, y - dst->top);
    /* A centred screen's 800x600 backdrop, copied to (0,0) or stretched over
     * the whole screen: draw it 1:1 in the middle, with black around it. */
    if (rule && rule->anchor == ANCHOR_CENTER && src &&
        src->left == 0 && src->top == 0 && src->right == 800 && src->bottom == 600 &&
        dst->left == 0 && dst->top == 0 &&
        ((w == 800 && h == 600) || (w == (int)screen_w && h == (int)screen_h)))
    {
        int cx = ((int)screen_w - 800) / 2, cy = ((int)screen_h - 600) / 2;

        SetRect(&moved, cx, cy, cx + 800, cy + 600);
        fill_black(0, 0, screen_w, cy);
        fill_black(0, cy + 600, screen_w, screen_h - cy - 600);
        fill_black(0, cy, cx, 600);
        fill_black(cx + 800, cy, screen_w - cx - 800, 600);
    }
    return surf_blit_40(self, edx, target, src, &moved);
}

/* jgld.dll draws text with GDI onto the back buffer's DC. */
static BOOL WINAPI TextOutA_hook(HDC dc, int x, int y, LPCSTR str, int len)
{
    char buf[40] = "";

    if (draw_log && str) lstrcpynA(buf, str, len + 1 < (int)sizeof(buf) ? len + 1 : (int)sizeof(buf));
    place_draw("text", dc, &x, &y, 0, 0, draw_log ? buf : NULL);
    if (draw_clipped) { draw_clipped = FALSE; return TRUE; }
    return real_TextOutA(dc, x, y, str, len);
}

/* Rectangle fill: int __thiscall fill(x, y, w, h, colour) at 0x480b00 (the
 * mood bar's number boxes, among others). Draws through it follow the same
 * HUD rules. */
typedef int (__fastcall *fill_fn)(void *self, void *edx, int x, int y, int w, int h, unsigned int colour);
static fill_fn fill_orig;

static int __fastcall fill_hook(void *self, void *edx, int x, int y, int w, int h, unsigned int colour)
{
    place_draw("fill", self, &x, &y, w, h, NULL);
    if (draw_clipped) { draw_clipped = FALSE; return 0; }
    return fill_orig(self, edx, x, y, w, h, colour);
}

/* Fill part of the screen with black, through the main window object
 * (0x519a60), as the game's own fills do. */
#define MAIN_WINDOW 0x519a60
static void fill_black(int x, int y, int w, int h)
{
    if (fill_orig && w > 0 && h > 0) fill_orig((void *)MAIN_WINDOW, NULL, x, y, w, h, 0);
}

/* Sprite +0x94: blended draw (target, x, y, factor, clip, flag), ret 0x18.
 * Report windows darken the screen by drawing interface\infoscreens\black.pcx,
 * an 800x600 image held by the game image at 0x821040, at (0,0) with factor
 * 0.25. On a bigger screen, tile it so the whole screen is darkened. */
#define DIM_IMAGE 0x821040
typedef int (__fastcall *draw6_fn)(void *self, void *edx, void *target, int x, int y, DWORD factor, void *clip, int flag);
static draw6_fn spr_blend_94;

static int __fastcall spr_blend_94_hook(void *self, void *edx, void *target, int x, int y, DWORD factor, void *clip, int flag)
{
    int ret = spr_blend_94(self, edx, target, x, y, factor, clip, flag);

    if (x == 0 && y == 0 && self == *(void **)(DIM_IMAGE + 4) && is_screen(target))
    {
        if (screen_w > 800) spr_blend_94(self, edx, target, 800, 0, factor, clip, flag);
        if (screen_h > 600) spr_blend_94(self, edx, target, 0, 600, factor, clip, flag);
        if (screen_w > 800 && screen_h > 600) spr_blend_94(self, edx, target, 800, 600, factor, clip, flag);
    }
    return ret;
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
        iat = (IMAGE_THUNK_DATA *)((BYTE *)mod + imp->FirstThunk);
        if (!imp->OriginalFirstThunk)
        {
            /* no name table (golf.exe): the slots hold bound addresses, so
             * find the one holding the function's address */
            HMODULE target = GetModuleHandleA(dll);
            DWORD addr = target ? (DWORD)GetProcAddress(target, func) : 0;
            if (!addr) return NULL;
            for (; iat->u1.Function; iat++)
                if (iat->u1.Function == addr) return swap_ptr((void **)&iat->u1.Function, hook);
            return NULL;
        }
        names = (IMAGE_THUNK_DATA *)((BYTE *)mod + imp->OriginalFirstThunk);
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
    spr_shadow_6c = swap_ptr(&spr[0x6c / 4], spr_shadow_6c_hook);
    spr_draw_78 = swap_ptr(&spr[0x78 / 4], spr_draw_78_hook);
    spr_draw_4c = swap_ptr(&spr[0x4c / 4], spr_draw_4c_hook);
    spr_draw_50 = swap_ptr(&spr[0x50 / 4], spr_draw_50_hook);
    spr_blend_94 = swap_ptr(&spr[0x94 / 4], spr_blend_94_hook);
    surf_blit_40 = swap_ptr(&surf[0x40 / 4], surf_blit_40_hook);
    real_TextOutA = hook_import(jgld, "GDI32.dll", "TextOutA", TextOutA_hook);
    {
        static const BYTE fill_prologue[] = {0x8b, 0x44, 0x24, 0x14, 0x8b, 0x54, 0x24, 0x10};  /* mov eax,[esp+0x14]; mov edx,[esp+0x10] */
        fill_orig = install_hook("rectangle fill", 0x480b00, fill_prologue, sizeof(fill_prologue), fill_hook);
    }
    log_msg("draw hooks: jgld.dll at %p, TextOutA %s", jgld, real_TextOutA ? "hooked" : "NOT hooked");
}

/* ---- HUD click handling ----
 * The main loop routes clicks in the bottom HUD by the click position
 * (globals 0x822d60 x / 0x822d64 y), checked against 800x600 boundaries:
 *   y > 440 (0x41e406), x < 200 -> control wheel click handler 0x432720
 *   y > 480 (0x41e459)          -> tool palette click handler for the mode
 * Those boundaries move down with the bottom HUD, and every handler and hover
 * check gets the mouse position moved back by the same offset, so the game's
 * own 800x600 button logic still works. */

/* Control wheel: int __cdecl hit_test(x, y) at 0x4326a0, nearest button or -1
 * (called by both its draw function, for hover, and its click handler). */
typedef int (__cdecl *mouse_fn)(int x, int y);
static mouse_fn wheel_hit_orig;

static int __cdecl wheel_hit_hook(int x, int y)
{
    return wheel_hit_orig(x, y - ((int)screen_h - 600));
}

/* Tool palette: draw functions (mouse position for hover) and click handlers,
 * all int __cdecl f(x, y), each starting with a 5-byte mov eax, [abs]. */
#define PALETTE_FUNCS 8
static const DWORD palette_addr[PALETTE_FUNCS] =
{
    0x433190, 0x434350, 0x435760, 0x4362f0,   /* draw, per palette mode */
    0x433040, 0x434140, 0x435680, 0x436060,   /* click, per palette mode */
};
static mouse_fn palette_orig[PALETTE_FUNCS];

#define PALETTE_HOOK(i) \
    static int __cdecl palette_hook_##i(int x, int y) \
    { return palette_orig[i](x, y - ((int)screen_h - 600)); }
PALETTE_HOOK(0) PALETTE_HOOK(1) PALETTE_HOOK(2) PALETTE_HOOK(3)
PALETTE_HOOK(4) PALETTE_HOOK(5) PALETTE_HOOK(6) PALETTE_HOOK(7)
static void *const palette_hook[PALETTE_FUNCS] =
{
    palette_hook_0, palette_hook_1, palette_hook_2, palette_hook_3,
    palette_hook_4, palette_hook_5, palette_hook_6, palette_hook_7,
};

/* Text menus and message boxes: uint __cdecl menu(center_x, top_y, flags, ?, ?)
 * at 0x46d6e0 sizes, draws and runs a box (with its own mouse handling) from
 * its position arguments, so moving those moves drawing and clicks together.
 * The control wheel's pop-up menus (Information etc.) follow the wheel down;
 * everything else is centred on the screen. */
typedef unsigned int (__cdecl *menu_fn)(int x, int y, unsigned int flags, int a4, unsigned int a5);
static menu_fn menu_orig;

static unsigned int __cdecl menu_hook(int x, int y, unsigned int flags, int a4, unsigned int a5)
{
    DWORD caller = (DWORD)__builtin_return_address(0);

    if (caller == 0x432820 || caller == 0x43292a || caller == 0x4329e2)
        y += (int)screen_h - 600;   /* from the wheel's click handler */
    else
    {
        x += ((int)screen_w - 800) / 2;
        y += ((int)screen_h - 600) / 2;
    }
    return menu_orig(x, y, flags, a4, a5);
}

/* Mouse position: void __thiscall get_mouse(int *x, int *y) at 0x47ab50.
 * Report windows, the title screen and the world map run their own loops,
 * reading the mouse here and comparing it with fixed 800x600 positions (e.g.
 * the close button); when the caller's rule (as for draws) centres it, move
 * the position back by the centring offset. */
typedef void (__fastcall *get_mouse_fn)(void *self, void *edx, int *x, int *y);
static get_mouse_fn get_mouse_orig;

static void __fastcall get_mouse_hook(void *self, void *edx, int *x, int *y)
{
    const struct hud_rule *rule;
    DWORD callers[MAX_CALLERS];
    int n;

    get_mouse_orig(self, edx, x, y);
    if (!x || !y) return;
    n = golf_callers(callers, MAX_CALLERS);
    rule = find_rule(callers, n, *x, *y);
    if (rule && rule->anchor == ANCHOR_CENTER)
    {
        *x -= ((int)screen_w - 800) / 2;
        *y -= ((int)screen_h - 600) / 2;
    }
}

/* Move a cmp reg, imm32 boundary by delta (checks the instruction first). */
static void move_boundary(const char *what, DWORD addr, const BYTE op[2], DWORD orig, int delta)
{
    BYTE old_ins[6], new_ins[6];
    DWORD moved = orig + delta;

    memcpy(old_ins, op, 2);
    memcpy(old_ins + 2, &orig, 4);
    memcpy(new_ins, op, 2);
    memcpy(new_ins + 2, &moved, 4);
    patch_bytes(what, addr, old_ins, new_ins, sizeof(old_ins));
}

/* The load and save screens' file lists (0x43c7ab in 0x43b610, 0x43b3b1 in
 * 0x43a8c0) test the click globals directly:
 *   cmp dword [0x822d60], 0x140 ; jle   (x > 320)
 *   mov eax, [0x822d64]          (y; then y >= 116, row = (y - 116) / 16)
 * Raise the x boundary by the centring offset, and replace the y load with a
 * call to a stub that loads y minus the offset. */
static void patch_file_lists(void)
{
    static const DWORD sites[] = {0x43c7ab, 0x43b3b1};
    static const BYTE cmp_x[] = {0x81, 0x3d, 0x60, 0x2d, 0x82, 0x00, 0x40, 0x01, 0x00, 0x00};
    static const BYTE mov_y[] = {0xa1, 0x64, 0x2d, 0x82, 0x00};
    int dx = ((int)screen_w - 800) / 2, dy = ((int)screen_h - 600) / 2, i;
    BYTE *stub;

    if (!dx && !dy) return;
    if (!(stub = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
    {
        log_msg("file lists: VirtualAlloc failed");
        return;
    }
    memcpy(stub, mov_y, sizeof(mov_y));       /* mov eax, [0x822d64] */
    stub[5] = 0x2d;                            /* sub eax, dy */
    memcpy(stub + 6, &dy, 4);
    stub[10] = 0xc3;                           /* ret */
    for (i = 0; i < 2; i++)
    {
        BYTE new_x[sizeof(cmp_x)], call_y[5];
        DWORD x_limit = 0x140 + dx, rel = (DWORD)stub - (sites[i] + 0x10 + 5);
        char what[48];

        memcpy(new_x, cmp_x, sizeof(new_x));
        memcpy(new_x + 6, &x_limit, 4);
        call_y[0] = 0xe8;
        memcpy(call_y + 1, &rel, 4);
        snprintf(what, sizeof(what), "file list x at %#lx", sites[i]);
        patch_bytes(what, sites[i], cmp_x, new_x, sizeof(cmp_x));
        snprintf(what, sizeof(what), "file list y at %#lx", sites[i] + 0x10);
        patch_bytes(what, sites[i] + 0x10, mov_y, call_y, sizeof(mov_y));
    }
}

/* The palette boundary (0x41e459: cmp ecx, 0x1e0; jle not-palette) sends
 * every click below y 480 to the palette, which spans that whole strip on an
 * 800-wide screen only. Replace the cmp with a call to a stub that also sends
 * clicks at x >= 800 (eax holds the click x here, from 0x41e412) to the
 * course: it sets the flags as cmp would, or forces "less or equal". */
static void patch_palette_boundary(int dy)
{
    static const BYTE cmp_y[] = {0x81, 0xf9, 0xe0, 0x01, 0x00, 0x00};
    BYTE *stub, call_ins[6];
    DWORD limit = 0x1e0 + dy, rel;
    int n = 0;

    if (!(stub = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
    {
        log_msg("palette boundary: VirtualAlloc failed");
        return;
    }
    stub[n++] = 0x3d; memcpy(stub + n, (DWORD[]){800}, 4); n += 4;      /* cmp eax, 800 */
    stub[n++] = 0x7d; stub[n++] = 7;                                   /* jge force */
    stub[n++] = 0x81; stub[n++] = 0xf9; memcpy(stub + n, &limit, 4); n += 4;  /* cmp ecx, limit */
    stub[n++] = 0xc3;                                                  /* ret */
    stub[n++] = 0x39; stub[n++] = 0xc9;                                /* force: cmp ecx, ecx */
    stub[n++] = 0xc3;                                                  /* ret */
    rel = (DWORD)stub - (0x41e459 + 5);
    call_ins[0] = 0xe8;
    memcpy(call_ins + 1, &rel, 4);
    call_ins[5] = 0x90;
    patch_bytes("bottom HUD click boundary (palette)", 0x41e459, cmp_y, call_ins, sizeof(cmp_y));
}

static void hook_hud_input(void)
{
    static const BYTE wheel_prologue[] = {0x51, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x14};  /* push ecx/ebx/ebp; mov ebp,[esp+0x14] */
    static const BYTE cmp_ecx[2] = {0x81, 0xf9};
    int dy = (int)screen_h - 600, i;

    wheel_hit_orig = install_hook("control wheel hit test", 0x4326a0, wheel_prologue,
                                  sizeof(wheel_prologue), wheel_hit_hook);
    for (i = 0; i < PALETTE_FUNCS; i++)
    {
        BYTE prologue[5];
        char what[48];

        /* the prologue's operand is an absolute address, so it can be copied as-is;
         * only the mov eax, [abs] opcode is checked */
        memcpy(prologue, (void *)palette_addr[i], 5);
        if (prologue[0] != 0xa1) { log_msg("palette function %#lx: unexpected prologue, not hooked", palette_addr[i]); continue; }
        snprintf(what, sizeof(what), "tool palette function %#lx", palette_addr[i]);
        palette_orig[i] = install_hook(what, palette_addr[i], prologue, 5, palette_hook[i]);
    }
    if (!GetEnvironmentVariableA("SGHOOKS_NO_MENUS", NULL, 0))
    {
        static const BYTE menu_prologue[] = {0x83, 0xec, 0x48, 0x8b, 0x44, 0x24, 0x54};  /* sub esp,0x48; mov eax,[esp+0x54] */
        menu_orig = install_hook("text menus and message boxes", 0x46d6e0, menu_prologue,
                                 sizeof(menu_prologue), menu_hook);
    }
    {
        static const BYTE mouse_prologue[] = {0x83, 0xec, 0x10, 0x53, 0x56};  /* sub esp,0x10; push ebx; push esi */
        get_mouse_orig = install_hook("mouse position", 0x47ab50, mouse_prologue, sizeof(mouse_prologue), get_mouse_hook);
    }
    move_boundary("bottom HUD click boundary (wheel)", 0x41e406, cmp_ecx, 0x1b8, dy);
    patch_palette_boundary(dy);
    patch_file_lists();
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

/* The game scales a 320x240 layout with int __cdecl scale(v) at 0x404970,
 * (screen_w * v + 160) / 320, for both axes, which assumes 4:3. Vertical
 * uses get (screen_h * v + 120) / 240 instead (the same at 4:3):
 *   0x45bbd4, 0x45be6d  main window heights, scale(240)
 *   0x45be22, 0x45be3c  font sizes, so text keeps its size relative to the
 *                       screen height on wide screens */
#define SCALE_FN 0x404970
static int __cdecl scale_y(int v)
{
    return ((int)screen_h * v + 120) / 240;
}

static void redirect_call(const char *what, DWORD site, DWORD from, void *to)
{
    BYTE old_ins[5] = {0xe8}, new_ins[5] = {0xe8};
    DWORD old_rel = from - (site + 5), new_rel = (DWORD)to - (site + 5);

    memcpy(old_ins + 1, &old_rel, 4);
    memcpy(new_ins + 1, &new_rel, 4);
    patch_bytes(what, site, old_ins, new_ins, sizeof(old_ins));
}

/* Replace the 32-bit immediate at the end of an instruction (opcode bytes
 * checked as they are, then the old value). */
static void patch_imm32(const char *what, DWORD addr, int oplen, DWORD from, DWORD to)
{
    BYTE old_ins[8], new_ins[8];

    memcpy(old_ins, (void *)addr, oplen);
    memcpy(new_ins, (void *)addr, oplen);
    memcpy(old_ins + oplen, &from, 4);
    memcpy(new_ins + oplen, &to, 4);
    patch_bytes(what, addr, old_ins, new_ins, oplen + 4);
}

/* The golfer close-up inset (a 160x140 magnified box beside a golfer,
 * 0x4168c0) is only shown for golfers inside x 80..720, y 80..520 and kept
 * inside 40..760 / 40..560: 800x600 bounds. Use the screen's. */
static void patch_closeup_bounds(void)
{
    patch_imm32("close-up x limit", 0x416919, 1, 720, screen_w - 80);   /* cmp eax, imm */
    patch_imm32("close-up y limit", 0x416924, 2, 520, screen_h - 80);   /* cmp ecx, imm */
    patch_imm32("close-up x clamp", 0x416943, 1, 760, screen_w - 40);   /* push imm */
    patch_imm32("close-up y clamp", 0x416953, 1, 560, screen_h - 40);
    patch_imm32("close-up x clamp (2)", 0x41696d, 1, 720, screen_w - 80);
    patch_imm32("close-up y clamp (2)", 0x41697d, 1, 520, screen_h - 80);
}

/* The tool palette's object previews (benches, bridges, ...) are queued and
 * drawn through 0x4628d0 from 0x432480, sized by the tier (above), so on a
 * bigger tier they overlap in the palette. Run that call with the 800x600
 * tier, as the palette was laid out for. */
/* the course tier (see patch_course_tier) */
static DWORD tier_w = 800, tier_h = 600;

typedef void (__cdecl *queue_object_fn)(int, int, int, int, int, int, int, int);
static void __cdecl palette_preview(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8)
{
    DWORD saved = tier_w;

    tier_w = 800;
    ((queue_object_fn)0x4628d0)(a1, a2, a3, a4, a5, a6, a7, a8);
    tier_w = saved;
}

static void patch_vertical_scale(void)
{
    redirect_call("main window height", 0x45bbd4, SCALE_FN, scale_y);
    redirect_call("main window height (second)", 0x45be6d, SCALE_FN, scale_y);
    redirect_call("font size", 0x45be22, SCALE_FN, scale_y);
    redirect_call("font size (second)", 0x45be3c, SCALE_FN, scale_y);
}

/* The game has course tile sizes and a projection scale for three screen
 * sizes only: 800x600, 1024x768 and 1280x1024.
 *   0x44a6e0  tile sizes: mov eax, [0x820f30] (width), then
 *             cmp dword [0x820f34], 600 / 768 / 1024 (height) at 0x44a6f3,
 *             0x44a72a, 0x44a75c; any other size gets uninitialised values
 *   0x42fbd8  projection: cmp edi, 1024 / cmp edi, 1280 (0x42fbe0) on the
 *             screen width (edi is used again afterwards to centre objects,
 *             so the compares change, not the load)
 * Both follow a tier picked from the screen height instead, so they agree and
 * wide screens get a supported one. */

static const BYTE tile_w_load_orig[] = {0xa1, 0x8c, 0x2c, 0x82, 0x00};   /* mov eax, [0x822c8c] */

static void patch_course_tier(void)
{
    static const BYTE tile_w[] = {0xa1, 0x30, 0x0f, 0x82, 0x00};
    static const DWORD cmp_sites[] = {0x44a6f3, 0x44a72a, 0x44a75c};
    static const DWORD cmp_imm[] = {600, 768, 1024};
    BYTE new_tile[5], old_cmp[10], new_cmp[10];
    DWORD pw = (DWORD)&tier_w, ph = (DWORD)&tier_h;
    int i;

    {
        /* Default: the 800x600 tier at any size, so the pixel-art tiles and
         * objects are drawn 1:1 and line up exactly; a bigger screen shows
         * more of the course. SIMGOLF_ART=large picks the game's enlarged
         * 1024x768 / 1280x1024 art by screen height instead (objects and
         * ground are then scaled slightly differently, 16/13 vs 1.25 x 1.3). */
        char art[16] = "";

        GetEnvironmentVariableA("SIMGOLF_ART", art, sizeof(art));
        if (!_stricmp(art, "large"))
        {
            if (screen_h >= 900) { tier_w = 1280; tier_h = 1024; }
            else if (screen_h >= 700) { tier_w = 1024; tier_h = 768; }
        }
    }
    log_msg("course tier %lux%lu", tier_w, tier_h);
    {
        /* object sizes: 0x4628d0, 0x462a43, 0x462c09 load the screen width
         * and pick a divisor (16, 13 above 1000, 10 from 1200); eax is
         * overwritten right after, so the load can read the tier instead */
        static const DWORD obj_sites[] = {0x4628d0, 0x462a43, 0x462c09};
        BYTE new_ld[5] = {0xa1};
        int k;

        memcpy(new_ld + 1, &pw, 4);
        for (k = 0; k < 3; k++)
            patch_bytes("course object size (width)", obj_sites[k], tile_w_load_orig, new_ld, 5);
    }
    redirect_call("palette object previews", 0x432480, 0x4628d0, palette_preview);
    memcpy(new_tile, tile_w, 1);
    memcpy(new_tile + 1, &pw, 4);
    patch_bytes("course tile sizes (width)", 0x44a6e0, tile_w, new_tile, sizeof(tile_w));
    for (i = 0; i < 3; i++)
    {
        char what[48];
        old_cmp[0] = new_cmp[0] = 0x81;
        old_cmp[1] = new_cmp[1] = 0x3d;
        memcpy(old_cmp + 2, "\x34\x0f\x82\x00", 4);
        memcpy(new_cmp + 2, &ph, 4);
        memcpy(old_cmp + 6, &cmp_imm[i], 4);
        memcpy(new_cmp + 6, &cmp_imm[i], 4);
        snprintf(what, sizeof(what), "course tile sizes (height %lu)", cmp_imm[i]);
        patch_bytes(what, cmp_sites[i], old_cmp, new_cmp, sizeof(old_cmp));
    }
    {
        /* the tier's compare matches the real width; a native width that
         * isn't the tier's must match neither */
        DWORD w1024 = tier_w == 1024 ? screen_w : screen_w == 1024 ? 0xffffffff : 1024;
        DWORD w1280 = tier_w == 1280 ? screen_w : screen_w == 1280 ? 0xffffffff : 1280;
        BYTE old_ins[6] = {0x81, 0xff}, new_ins[6] = {0x81, 0xff};
        DWORD v = 1024;

        memcpy(old_ins + 2, &v, 4);
        memcpy(new_ins + 2, &w1024, 4);
        if (w1024 != 1024) patch_bytes("course projection (1024)", 0x42fbd8, old_ins, new_ins, 6);
        v = 1280;
        memcpy(old_ins + 2, &v, 4);
        memcpy(new_ins + 2, &w1280, 4);
        if (w1280 != 1280) patch_bytes("course projection (1280)", 0x42fbe0, old_ins, new_ins, 6);
    }
}

/* Terrain.dll has the same three sizes. Four places compare its width and
 * height (this+0x20 / +0x24, or initSystem's arguments) with exactly 800x600,
 * 1024x768 and 1280x1024 to pick tile sizes and, in initSystem, the extent of
 * its glOrtho view (in world units); other sizes get nothing. For the tier in
 * use, make its compares match the real screen size, and scale its extent by
 * screen / tier size. Each site is a 7-byte instruction with its 32-bit value
 * at offset 3 (addresses at Terrain.dll's preferred base 0x10000000). */
#define TERRAIN_BASE 0x10000000
static const DWORD terrain_cmp_w[3][4] =
{
    {0x100068d2, 0x10009003, 0x10009dec, 0x10009f45},   /* 800 */
    {0x10006908, 0x1000902b, 0x10009e10, 0x10009f67},   /* 1024 */
    {0x1000693e, 0x10009053, 0x10009e34, 0x10009f89},   /* 1280 */
};
static const DWORD terrain_cmp_h[3][4] =
{
    {0x100068de, 0x1000900f, 0x10009df8, 0x10009f4e},   /* 600 */
    {0x10006914, 0x10009037, 0x10009e1c, 0x10009f70},   /* 768 */
    {0x1000694a, 0x1000905f, 0x10009e40, 0x10009f92},   /* 1024 */
};
static const DWORD terrain_extent[3][2] =   /* mov [ebp+8], w / mov [ebp+0xc], h */
{
    {0x10009f57, 0x10009f5e}, {0x10009f79, 0x10009f80}, {0x10009f9b, 0x10009fa2},
};
/* setZoomLevel (0x10008fc0) has its own extents, as floats:
 * mov [ebp-8], w / mov [ebp-0xc], h */
static const DWORD terrain_zoom_extent[3][2] =
{
    {0x10009018, 0x1000901f}, {0x10009040, 0x10009047}, {0x10009068, 0x1000906f},
};
static const DWORD terrain_zoom_value[3][2] =   /* 1767.77 x 1325.83, 1810.19 x 1357.65, 1740.57 x 1392.46 */
{
    {0x44dcf88b, 0x44a5ba68}, {0x44e24630, 0x44a9b4a4}, {0x44d99242, 0x44ae0e9b},
};

static void patch_terrain_value(HMODULE terrain, const char *what, DWORD va, DWORD from, DWORD to)
{
    BYTE *p = (BYTE *)terrain + (va - TERRAIN_BASE);
    BYTE old_ins[7], new_ins[7];

    memcpy(old_ins, p, 3);   /* opcode and operand bytes, checked through the value */
    memcpy(old_ins + 3, &from, 4);
    memcpy(new_ins, p, 3);
    memcpy(new_ins + 3, &to, 4);
    patch_bytes(what, (DWORD)p, old_ins, new_ins, sizeof(old_ins));
}

/* In the 1024x768 tier, initSystem's extent is 1810 x 1303: not the screen's
 * shape (setZoomLevel's is 1810.19 x 1357.65, and the other tiers' match
 * theirs). The ground is drawn about 4% too tall around the screen centre, so
 * strips drawn at the top and bottom edges while panning land 10-20 px off
 * and the scroll carries that inwards; a redraw puts them back. Use 1357.65
 * for that tier, at its native size too. */
#define TIER1024_HEIGHT_X100 135765

static void patch_terrain_tier(void)
{
    static const DWORD tw[3] = {800, 1024, 1280}, th[3] = {600, 768, 1024};
    static const DWORD ext_w[3] = {0x6e7, 0x712, 0x6cc}, ext_h[3] = {0x52d, 0x517, 0x570};
    HMODULE terrain = GetModuleHandleA("Terrain.dll");
    BOOL native;
    DWORD new_h;
    int t, i;

    t = tier_w == 1280 ? 2 : tier_w == 1024 ? 1 : 0;
    native = screen_w == tw[t] && screen_h == th[t];
    if (native && t != 1) return;
    if (!terrain) { log_msg("Terrain.dll not loaded, its sizes not patched"); return; }
    new_h = t == 1 ? (DWORD)(((ULONGLONG)TIER1024_HEIGHT_X100 * screen_h + 768 * 50) / (768 * 100))
                   : (DWORD)(((ULONGLONG)ext_h[t] * screen_h + th[t] / 2) / th[t]);
    patch_terrain_value(terrain, "Terrain view extent (height)", terrain_extent[t][1], ext_h[t], new_h);
    if (native) return;
    for (i = 0; i < 4; i++)
    {
        patch_terrain_value(terrain, "Terrain size compare (width)", terrain_cmp_w[t][i], tw[t], screen_w);
        patch_terrain_value(terrain, "Terrain size compare (height)", terrain_cmp_h[t][i], th[t], screen_h);
    }
    patch_terrain_value(terrain, "Terrain view extent (width)", terrain_extent[t][0], ext_w[t],
                        (DWORD)(((ULONGLONG)ext_w[t] * screen_w + tw[t] / 2) / tw[t]));
    {
        union { DWORD d; float f; } w, h;

        w.d = terrain_zoom_value[t][0];
        h.d = terrain_zoom_value[t][1];
        w.f = w.f * screen_w / tw[t];
        h.f = h.f * screen_h / th[t];
        patch_terrain_value(terrain, "Terrain zoom extent (width)", terrain_zoom_extent[t][0],
                            terrain_zoom_value[t][0], w.d);
        patch_terrain_value(terrain, "Terrain zoom extent (height)", terrain_zoom_extent[t][1],
                            terrain_zoom_value[t][1], h.d);
    }
}

/* Terrain::stripRender(Tile *camera, int direction, float angle) redraws a
 * fixed band of tiles around the camera after a pan (about 18 tiles along
 * the edge), sized for the tier's own view. When the screen shows more than
 * that, the band misses the outer tiles (black notches at the edges). Then
 * golf.exe's import of it (0x4ba144) is pointed at a full
 * Terrain::render(Tile *camera, float angle) (import 0x4ba15c) instead. */
#define IMP_STRIP_RENDER 0x4ba144
#define IMP_RENDER       0x4ba15c
typedef char (__fastcall *render_fn)(void *terrain, void *edx, void *camera, float angle);
static render_fn terrain_render;

static void __fastcall strip_render_full(void *terrain, void *edx, void *camera, int direction, float angle)
{
    terrain_render(terrain, edx, camera, angle);
}

static void patch_strip_render(void)
{
    HMODULE terrain = GetModuleHandleA("Terrain.dll");
    IMAGE_NT_HEADERS *nt;
    BYTE *r, *st;

    if (screen_w <= tier_w && screen_h <= tier_h) return;
    if (!terrain) { log_msg("Terrain::stripRender: Terrain.dll not loaded"); return; }
    nt = (IMAGE_NT_HEADERS *)((BYTE *)terrain + ((IMAGE_DOS_HEADER *)terrain)->e_lfanew);
    r = *(BYTE **)IMP_RENDER;
    st = *(BYTE **)IMP_STRIP_RENDER;
    if (r < (BYTE *)terrain || r >= (BYTE *)terrain + nt->OptionalHeader.SizeOfImage ||
        st < (BYTE *)terrain || st >= (BYTE *)terrain + nt->OptionalHeader.SizeOfImage)
    {
        log_msg("Terrain::stripRender: imports not bound to Terrain.dll yet (%p, %p), not patched", r, st);
        return;
    }
    terrain_render = (render_fn)r;
    {
        /* render itself only visits tiles within 16 (<< zoom level) of the
         * camera, enough for the tier's own view; allow 32 (tiles off screen
         * are still culled, and the range is clamped to the 50x50 map) */
        static const BYTE range_old[] = {0xba, 0x10, 0x00, 0x00, 0x00};   /* mov edx, 0x10 */
        static const BYTE range_new[] = {0xba, 0x20, 0x00, 0x00, 0x00};
        patch_bytes("Terrain::render tile range", (DWORD)((BYTE *)terrain + (0x100059bd - TERRAIN_BASE)),
                    range_old, range_new, sizeof(range_old));
    }
    if (swap_ptr((void **)IMP_STRIP_RENDER, strip_render_full))
        log_msg("Terrain::stripRender: full redraws instead (screen larger than the tier's view)");
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
        patch_vertical_scale();
        patch_course_tier();
        patch_terrain_tier();
        patch_strip_render();
        patch_closeup_bounds();
    }
    log_msg("requested resolution %ux%u", w, h);
}

/* ---- sound.dll stream monitor (SGHOOKS_SNDLOG=1, debugging) ----
 * sound.dll's mixer (timer thread) asks for a streamed sound's ring buffer to
 * be refilled by setting flag 0x400 in the stream (+0x58) and posting message
 * 0x7eb (wParam = stream) to its sound thread, whose handler (0x100288a0,
 * reached through the thunk at 0x10001780) refills and clears the flag. A
 * request that never completes leaves the stream replaying its ring forever.
 * Log the requests and handling, and report streams stuck with 0x400 set. */
#define SND_SIZE         0xbe000
#define SND_PREF_BASE    0x10000000
#define SND_HANDLER_THUNK 0x10001780
#define SND_HANDLER      0x100288a0
#define SND_FLAGS        0x58
#define SND_PENDING      0x1370
#define SND_MAX_STREAMS  64

static BYTE *snd_base;
static struct { BYTE *stream; DWORD posted, handled; BOOL stuck; } snd_streams[SND_MAX_STREAMS];
static CRITICAL_SECTION snd_lock;
typedef LRESULT (__cdecl *snd_handler_fn)(HWND, UINT, void *, int *);
static snd_handler_fn snd_handler;
typedef BOOL (WINAPI *post_thread_fn)(DWORD, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *post_msg_fn)(HWND, UINT, WPARAM, LPARAM);
static post_thread_fn real_PostThreadMessageA;
static post_msg_fn real_PostMessageA;

static int snd_slot(BYTE *stream)
{
    int i, free_slot = -1;

    for (i = 0; i < SND_MAX_STREAMS; i++)
    {
        if (snd_streams[i].stream == stream) return i;
        if (!snd_streams[i].stream && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) snd_streams[free_slot].stream = stream;
    return free_slot;
}

static void snd_note(const char *what, BYTE *stream)
{
    DWORD now = GetTickCount();
    int i;

    EnterCriticalSection(&snd_lock);
    if ((i = snd_slot(stream)) >= 0)
    {
        if (what[0] == 'p') snd_streams[i].posted = now;
        else snd_streams[i].handled = now;
        if (snd_streams[i].stuck && what[0] == 'h') snd_streams[i].stuck = FALSE;
    }
    LeaveCriticalSection(&snd_lock);
    log_msg("%lu snd %s stream %p flags %#lx pending %ld thread %lu", now, what, stream,
            *(DWORD *)(stream + SND_FLAGS), *(LONG *)(stream + SND_PENDING), GetCurrentThreadId());
}

static BOOL WINAPI snd_PostThreadMessageA(DWORD thread, UINT msg, WPARAM wp, LPARAM lp)
{
    BOOL ret = real_PostThreadMessageA(thread, msg, wp, lp);
    if (msg == 0x7eb && wp) snd_note(ret ? "post" : "post FAILED", (BYTE *)wp);
    else if (msg >= 0x7e8 && msg <= 0x7f0) log_msg("%lu snd thread msg %#x wp %#lx lp %#lx -> %d", GetTickCount(), msg, (DWORD)wp, (DWORD)lp, ret);
    return ret;
}

static BOOL WINAPI snd_PostMessageA(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    BOOL ret = real_PostMessageA(hwnd, msg, wp, lp);
    if (msg == 0x7eb && wp) snd_note(ret ? "post(wnd)" : "post(wnd) FAILED", (BYTE *)wp);
    else if (msg >= 0x7e8 && msg <= 0x7f0) log_msg("%lu snd window msg %#x wp %#lx lp %#lx -> %d", GetTickCount(), msg, (DWORD)wp, (DWORD)lp, ret);
    return ret;
}

static LRESULT __cdecl snd_handler_hook(HWND hwnd, UINT msg, void *wp, int *lp)
{
    LRESULT ret;

    if (msg == 0x7eb && wp) snd_note("handle", wp);
    ret = snd_handler(hwnd, msg, wp, lp);
    if (msg == 0x7eb && wp) snd_note("handled", wp);
    return ret;
}

static DWORD WINAPI snd_monitor(void *arg)
{
    HMODULE snd = NULL;
    int i;

    for (;;)
    {
        Sleep(100);
        if (!snd)
        {
            IMAGE_NT_HEADERS *nt;
            BYTE *thunk;
            DWORD rel, old;

            if (!(snd = GetModuleHandleA("sound.dll"))) continue;
            nt = (IMAGE_NT_HEADERS *)((BYTE *)snd + ((IMAGE_DOS_HEADER *)snd)->e_lfanew);
            if (nt->OptionalHeader.SizeOfImage != SND_SIZE)
            {
                log_msg("snd monitor: unexpected sound.dll (size %#lx)", nt->OptionalHeader.SizeOfImage);
                return 0;
            }
            snd_base = (BYTE *)snd;
            real_PostThreadMessageA = hook_import(snd, "USER32.dll", "PostThreadMessageA", snd_PostThreadMessageA);
            real_PostMessageA = hook_import(snd, "USER32.dll", "PostMessageA", snd_PostMessageA);
            thunk = snd_base + (SND_HANDLER_THUNK - SND_PREF_BASE);
            snd_handler = (snd_handler_fn)(snd_base + (SND_HANDLER - SND_PREF_BASE));
            /* the thread-message path (unused by golf.exe, which uses the
             * window path, logged by the fix's wrapper) */
            if (thunk[0] == 0xe9 && thunk + 5 + *(LONG *)(thunk + 1) == (BYTE *)snd_handler &&
                VirtualProtect(thunk, 5, PAGE_EXECUTE_READWRITE, &old))
            {
                rel = (DWORD)snd_handler_hook - (DWORD)(thunk + 5);
                memcpy(thunk + 1, &rel, 4);
                VirtualProtect(thunk, 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), thunk, 5);
            }
            else snd_handler = NULL;
            log_msg("snd monitor: sound.dll at %p, PostThreadMessageA %s, PostMessageA %s, handler %s", snd_base,
                    real_PostThreadMessageA ? "hooked" : "not hooked", real_PostMessageA ? "hooked" : "not hooked",
                    snd_handler ? "hooked" : "not hooked");
            continue;
        }
        EnterCriticalSection(&snd_lock);
        for (i = 0; i < SND_MAX_STREAMS; i++)
        {
            BYTE *st = snd_streams[i].stream;
            DWORD now = GetTickCount(), flags;

            if (!st || snd_streams[i].stuck || IsBadReadPtr(st + SND_FLAGS, 4)) continue;
            flags = *(DWORD *)(st + SND_FLAGS);
            if ((flags & 0x400) && now - snd_streams[i].posted > 1000)
            {
                snd_streams[i].stuck = TRUE;
                log_msg("%lu snd STUCK stream %p flags %#lx pending %ld posted %lu ms ago, last handled %lu ms ago",
                        now, st, flags, *(LONG *)(st + SND_PENDING), now - snd_streams[i].posted,
                        snd_streams[i].handled ? now - snd_streams[i].handled : 0);
            }
        }
        LeaveCriticalSection(&snd_lock);
    }
    return 0;
}

/* ---- sound.dll fix: streamed sounds looping forever ----
 * A streamed sound's ring buffer is refilled by sound.dll's window procedure
 * (0x10028b70, reached through the thunk at 0x100025ef and set as the window
 * class procedure) on the game's main thread, when the mixer posts it message
 * 0x7eb. The mixer runs on a timer thread holding the critical section at
 * 0x100b4990; the handler takes no lock, yet both read-modify-write the
 * stream's flags word (+0x58). When an update is lost, e.g. the handler's
 * clearing of the "refill pending" bit 0x400, no refill is ever requested
 * again, the "finished" bit is never set, and the reader cycles the ring's
 * last contents forever. Run the handler for 0x7eb under the mixer's lock. */
#define SND_WNDPROC_THUNK 0x100025ef
#define SND_WNDPROC       0x10028b70
#define SND_MIXER_LOCK    0x100b4990
typedef LRESULT (WINAPI *snd_wndproc_fn)(HWND, UINT, WPARAM, LPARAM);   /* a window procedure: stdcall */
static snd_wndproc_fn snd_wndproc;
static CRITICAL_SECTION *snd_mixer_lock;
static BOOL snd_fixed;

static void snd_note(const char *what, BYTE *stream);
static BOOL snd_log;

static LRESULT WINAPI snd_wndproc_hook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT ret;

    if (msg != 0x7eb) return snd_wndproc(hwnd, msg, wp, lp);
    EnterCriticalSection(snd_mixer_lock);
    if (snd_log && wp) snd_note("handle", (BYTE *)wp);
    ret = snd_wndproc(hwnd, msg, wp, lp);
    if (snd_log && wp) snd_note("handled", (BYTE *)wp);
    LeaveCriticalSection(snd_mixer_lock);
    return ret;
}

static void fix_sound_dll(void)
{
    HMODULE snd;
    IMAGE_NT_HEADERS *nt;
    BYTE *thunk, *target;
    DWORD rel, old;

    if (snd_fixed || !(snd = GetModuleHandleA("sound.dll"))) return;
    snd_fixed = TRUE;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)snd + ((IMAGE_DOS_HEADER *)snd)->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage != 0xbe000)
    {
        log_msg("sound fix: unexpected sound.dll (size %#lx), not patched", nt->OptionalHeader.SizeOfImage);
        return;
    }
    thunk = (BYTE *)snd + (SND_WNDPROC_THUNK - 0x10000000);
    target = (BYTE *)snd + (SND_WNDPROC - 0x10000000);
    if (thunk[0] != 0xe9 || thunk + 5 + *(LONG *)(thunk + 1) != target)
    {
        log_msg("sound fix: unexpected code at the window procedure thunk, not patched");
        return;
    }
    snd_wndproc = (snd_wndproc_fn)target;
    snd_mixer_lock = (CRITICAL_SECTION *)((BYTE *)snd + (SND_MIXER_LOCK - 0x10000000));
    if (!VirtualProtect(thunk, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    rel = (DWORD)snd_wndproc_hook - (DWORD)(thunk + 5);
    memcpy(thunk + 1, &rel, 4);
    VirtualProtect(thunk, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, 5);
    log_msg("sound fix: stream refills run under the mixer lock (sound.dll at %p)", snd);
}

typedef HMODULE (WINAPI *load_library_fn)(LPCSTR);
static load_library_fn real_LoadLibraryA_golf, real_LoadLibraryA_jgld;

static HMODULE WINAPI LoadLibraryA_jgld(LPCSTR name);

static HMODULE WINAPI LoadLibraryA_golf(LPCSTR name)
{
    HMODULE ret = real_LoadLibraryA_golf(name), jgld;

    /* jgld.dll may be loaded only now (e.g. at 800x600) */
    if (!real_LoadLibraryA_jgld && (jgld = GetModuleHandleA("jgld.dll")))
        real_LoadLibraryA_jgld = hook_import(jgld, "KERNEL32.dll", "LoadLibraryA", LoadLibraryA_jgld);
    fix_sound_dll();
    return ret;
}

static HMODULE WINAPI LoadLibraryA_jgld(LPCSTR name)
{
    HMODULE ret = real_LoadLibraryA_jgld(name);
    fix_sound_dll();
    return ret;
}

static void hook_sound_load(void)
{
    HMODULE jgld;

    if (GetEnvironmentVariableA("SGHOOKS_NO_SOUNDFIX", NULL, 0)) return;
    real_LoadLibraryA_golf = hook_import(GetModuleHandleA(NULL), "KERNEL32.DLL", "LoadLibraryA", LoadLibraryA_golf);
    if ((jgld = GetModuleHandleA("jgld.dll")))
        real_LoadLibraryA_jgld = hook_import(jgld, "KERNEL32.dll", "LoadLibraryA", LoadLibraryA_jgld);
    log_msg("sound fix: watching LoadLibraryA (golf.exe %s, jgld.dll %s)",
            real_LoadLibraryA_golf ? "hooked" : "not hooked", real_LoadLibraryA_jgld ? "hooked" : "not hooked");
    fix_sound_dll();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    char path[MAX_PATH];
    char exe[MAX_PATH], *name;

    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&log_lock);
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
    hook_sound_load();
    if (log_file && GetEnvironmentVariableA("SGHOOKS_SNDLOG", path, sizeof(path)))
    {
        InitializeCriticalSection(&snd_lock);
        snd_log = TRUE;
        CloseHandle(CreateThread(NULL, 0, snd_monitor, NULL, 0, NULL));
    }
    return TRUE;
}
