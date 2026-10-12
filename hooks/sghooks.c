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
     * 0x433190, 0x434350, 0x435760, 0x4362f0 (modes 3 and 4) */
    {"tool palette", 0x4197ea, 0x4197eb, ANCHOR_BOTTOM},
    {"tool palette", 0x419816, 0x419817, ANCHOR_BOTTOM},
    {"tool palette", 0x419842, 0x419843, ANCHOR_BOTTOM},
    {"tool palette", 0x4198b3, 0x4198b4, ANCHOR_BOTTOM},
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
static const struct hud_rule *place_draw(const char *what, void *obj, int *x, int *y, int w, int h, const char *text)
{
    static DWORD seen[4096];
    static int nseen;
    DWORD callers[MAX_CALLERS] = {0}, key;
    const struct hud_rule *rule;
    int n, i, dx = 0, dy = 0;

    n = golf_callers(callers, MAX_CALLERS);
    rule = find_rule(callers, n, *x, *y);
    /* full-screen copies (backdrops) never move */
    if (rule && !(w == (int)screen_w && h == (int)screen_h)) anchor_offset(rule->anchor, &dx, &dy);

    if (draw_log && draw_lines < 20000)
    {
        key = (DWORD)what * 31 + callers[0] * 7 + callers[1] * 3 + callers[2] + *x * 131071 + *y * 8191;
        for (i = 0; i < nseen && seen[i] != key; i++);
        if (i == nseen)
        {
            if (nseen < 4096) seen[nseen++] = key;
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

/* Sprite +0x6c: drop shadow (target, x, y, ?, clip), through golf.exe's
 * 0x474030 / 0x4740f0 (the panels' shadows, and golfers'). */
typedef int (__fastcall *shadow_fn)(void *self, void *edx, void *target, int x, int y, int a, void *clip);
static shadow_fn spr_shadow_6c;

static int __fastcall spr_shadow_6c_hook(void *self, void *edx, void *target, int x, int y, int a, void *clip)
{
    if (is_screen(target)) place_draw("shadow", self, &x, &y, 0, 0, NULL);
    else log_offscreen("shadow", self, target, x, y);
    return spr_shadow_6c(self, edx, target, x, y, a, clip);
}

static void fill_black(int x, int y, int w, int h);

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
    spr_shadow_6c = swap_ptr(&spr[0x6c / 4], spr_shadow_6c_hook);
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
    move_boundary("bottom HUD click boundary (palette)", 0x41e459, cmp_ecx, 0x1e0, dy);
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
 *   0x42fbcc  projection: mov edi, [0x822c8c] (width), then 1024 / 1280
 * Both read a tier picked from the screen height instead, so they agree and
 * wide screens get a supported one. */
static DWORD tier_w = 800, tier_h = 600;

static void patch_course_tier(void)
{
    static const BYTE tile_w[] = {0xa1, 0x30, 0x0f, 0x82, 0x00};
    static const BYTE proj_w[] = {0x8b, 0x3d, 0x8c, 0x2c, 0x82, 0x00};
    static const DWORD cmp_sites[] = {0x44a6f3, 0x44a72a, 0x44a75c};
    static const DWORD cmp_imm[] = {600, 768, 1024};
    BYTE new_tile[5], new_proj[6], old_cmp[10], new_cmp[10];
    DWORD pw = (DWORD)&tier_w, ph = (DWORD)&tier_h;
    int i;

    if (screen_h >= 900) { tier_w = 1280; tier_h = 1024; }
    else if (screen_h >= 700) { tier_w = 1024; tier_h = 768; }
    log_msg("course tier %lux%lu", tier_w, tier_h);
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
    memcpy(new_proj, proj_w, 2);
    memcpy(new_proj + 2, &pw, 4);
    patch_bytes("course projection", 0x42fbcc, proj_w, new_proj, sizeof(proj_w));
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
