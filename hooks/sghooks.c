/*
 * sghooks.dll: runtime patches for golf.exe, loaded by terrgl32.dll at process
 * start (before golf.exe's own code runs). golf.exe on disk is never modified.
 *
 * Each patch checks the original bytes first and is skipped if they differ,
 * so a different game version is left alone.
 *
 * Environment:
 *   SIMGOLF_RES=WxH   screen size instead of 800x600 (experimental)
 *   SGHOOKS_LOG=path  Windows path for a log file
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
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
    patch_bytes("screen width", 0x45baf6, orig_w, new_w, sizeof(orig_w));
    patch_bytes("screen height", 0x45bb00, orig_h, new_h, sizeof(orig_h));
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
    return TRUE;
}
