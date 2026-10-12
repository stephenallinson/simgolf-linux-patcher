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
    return TRUE;
}
