/*
 * sgsnd.dll: debugging proxy for SimGolf's sound.dll.
 *
 * sound.dll gets its DirectSound object from CoCreateInstance. With its
 * "ole32.dll" import renamed to "sgsnd.dll", calls come here: CoInitialize and
 * CoUninitialize are forwarded, and CoCreateInstance swaps in copied method
 * tables on DirectSound objects and their secondary buffers so we can record:
 *  - buffer creation (format, size, flags),
 *  - Play / Stop / SetCurrentPosition, and
 *  - every Unlock: the audio the game wrote, appended to buf<N>.raw.
 *
 * Does nothing beyond forwarding unless SGSND_DUMP is set to a Windows folder
 * path (e.g. 'Z:\tmp\simgolf-audio'); events go to events.log there.
 */
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <stdarg.h>

/* IDirectSound vtable slots */
#define DS_CREATESOUNDBUFFER 3
#define DS_SLOTS 12
/* IDirectSoundBuffer(8) vtable slots */
#define DSB_RELEASE 2
#define DSB_LOCK 11
#define DSB_PLAY 12
#define DSB_SETCURRENTPOSITION 13
#define DSB_STOP 18
#define DSB_UNLOCK 19
#define DSB_SLOTS 24

typedef HRESULT (WINAPI *PFN_CREATEBUFFER)(void *, const DSBUFFERDESC *, IDirectSoundBuffer **, IUnknown *);
typedef ULONG (WINAPI *PFN_RELEASE)(void *);
typedef HRESULT (WINAPI *PFN_LOCK)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD);
typedef HRESULT (WINAPI *PFN_PLAY)(void *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *PFN_SETPOS)(void *, DWORD);
typedef HRESULT (WINAPI *PFN_STOP)(void *);
typedef HRESULT (WINAPI *PFN_UNLOCK)(void *, void *, DWORD, void *, DWORD);

static void *ds_vtbl[DS_SLOTS], *dsb_vtbl[DSB_SLOTS];
static void **real_ds_vtbl, **real_dsb_vtbl;

#define MAX_BUFFERS 256
struct tracked
{
    void *iface;
    int id;
    FILE *raw;
    DWORD bytes_per_sec, total, last_mark;
};
static struct tracked buffers[MAX_BUFFERS];
static int next_id;

static CRITICAL_SECTION cs;
static char dump_dir[MAX_PATH];
static FILE *events;
static DWORD start_ms;

static void log_event(const char *fmt, ...)
{
    va_list args;

    if (!events) return;
    fprintf(events, "%8.3f %04lx ", (GetTickCount() - start_ms) / 1000.0, GetCurrentThreadId());
    va_start(args, fmt);
    vfprintf(events, fmt, args);
    va_end(args);
    fputc('\n', events);
    fflush(events);
}

static struct tracked *find_buffer(void *iface)
{
    int i;
    for (i = 0; i < MAX_BUFFERS; i++)
        if (buffers[i].iface == iface) return &buffers[i];
    return NULL;
}

/* ---- IDirectSoundBuffer hooks ---- */

static ULONG WINAPI dsb_Release(void *iface)
{
    ULONG ref = ((PFN_RELEASE)real_dsb_vtbl[DSB_RELEASE])(iface);
    struct tracked *b;

    if (ref) return ref;
    EnterCriticalSection(&cs);
    if ((b = find_buffer(iface)))
    {
        log_event("buf%d released, %lu bytes written in total", b->id, b->total);
        if (b->raw) fclose(b->raw);
        memset(b, 0, sizeof(*b));
    }
    LeaveCriticalSection(&cs);
    return 0;
}

static HRESULT WINAPI dsb_Lock(void *iface, DWORD offset, DWORD bytes, void **p1, DWORD *n1,
                               void **p2, DWORD *n2, DWORD flags)
{
    HRESULT hr = ((PFN_LOCK)real_dsb_vtbl[DSB_LOCK])(iface, offset, bytes, p1, n1, p2, n2, flags);
    struct tracked *b;

    EnterCriticalSection(&cs);
    /* the ring buffer is locked ~66 times a second; log only the big one-shot locks */
    if ((b = find_buffer(iface)) && bytes > 65536)
        log_event("buf%d Lock offset %lu bytes %lu flags %#lx -> %#lx", b->id, offset, bytes, flags, hr);
    LeaveCriticalSection(&cs);
    return hr;
}

static HRESULT WINAPI dsb_Unlock(void *iface, void *p1, DWORD n1, void *p2, DWORD n2)
{
    struct tracked *b;

    EnterCriticalSection(&cs);
    if ((b = find_buffer(iface)) && b->raw)
    {
        if (p1 && n1) fwrite(p1, 1, n1, b->raw);
        if (p2 && n2) fwrite(p2, 1, n2, b->raw);
        b->total += n1 + (p2 ? n2 : 0);
        if (GetTickCount() - b->last_mark >= 1000)
        {
            b->last_mark = GetTickCount();
            log_event("buf%d mark: raw offset %lu = %.3fs", b->id, b->total,
                      b->bytes_per_sec ? (double)b->total / b->bytes_per_sec : 0.0);
        }
    }
    LeaveCriticalSection(&cs);
    return ((PFN_UNLOCK)real_dsb_vtbl[DSB_UNLOCK])(iface, p1, n1, p2, n2);
}

static HRESULT WINAPI dsb_Play(void *iface, DWORD r1, DWORD prio, DWORD flags)
{
    struct tracked *b;

    EnterCriticalSection(&cs);
    if ((b = find_buffer(iface)))
        log_event("buf%d Play flags %#lx%s (raw offset %lu = %.3fs)", b->id, flags,
                  flags & DSBPLAY_LOOPING ? " LOOPING" : "", b->total,
                  b->bytes_per_sec ? (double)b->total / b->bytes_per_sec : 0.0);
    LeaveCriticalSection(&cs);
    return ((PFN_PLAY)real_dsb_vtbl[DSB_PLAY])(iface, r1, prio, flags);
}

static HRESULT WINAPI dsb_SetCurrentPosition(void *iface, DWORD pos)
{
    struct tracked *b;

    EnterCriticalSection(&cs);
    if ((b = find_buffer(iface))) log_event("buf%d SetCurrentPosition %lu", b->id, pos);
    LeaveCriticalSection(&cs);
    return ((PFN_SETPOS)real_dsb_vtbl[DSB_SETCURRENTPOSITION])(iface, pos);
}

static HRESULT WINAPI dsb_Stop(void *iface)
{
    struct tracked *b;

    EnterCriticalSection(&cs);
    if ((b = find_buffer(iface)))
        log_event("buf%d Stop (raw offset %lu = %.3fs)", b->id, b->total,
                  b->bytes_per_sec ? (double)b->total / b->bytes_per_sec : 0.0);
    LeaveCriticalSection(&cs);
    return ((PFN_STOP)real_dsb_vtbl[DSB_STOP])(iface);
}

/* ---- IDirectSound hooks ---- */

static void hook_buffer(IDirectSoundBuffer *dsb, const DSBUFFERDESC *desc)
{
    void ***obj = (void ***)dsb;
    struct tracked *b = NULL;
    char path[MAX_PATH + 32];
    int i;

    if (!real_dsb_vtbl)
    {
        real_dsb_vtbl = *obj;
        memcpy(dsb_vtbl, real_dsb_vtbl, sizeof(dsb_vtbl));
        dsb_vtbl[DSB_RELEASE] = dsb_Release;
        dsb_vtbl[DSB_LOCK] = dsb_Lock;
        dsb_vtbl[DSB_PLAY] = dsb_Play;
        dsb_vtbl[DSB_SETCURRENTPOSITION] = dsb_SetCurrentPosition;
        dsb_vtbl[DSB_STOP] = dsb_Stop;
        dsb_vtbl[DSB_UNLOCK] = dsb_Unlock;
    }
    else if (*obj != real_dsb_vtbl)
    {
        log_event("buffer %p has an unexpected vtable, not hooked", dsb);
        return;
    }

    for (i = 0; i < MAX_BUFFERS; i++)
        if (!buffers[i].iface) { b = &buffers[i]; break; }
    if (!b) return;
    b->iface = dsb;
    b->id = next_id++;
    b->bytes_per_sec = desc->lpwfxFormat ? desc->lpwfxFormat->nAvgBytesPerSec : 0;
    snprintf(path, sizeof(path), "%s\\buf%d.raw", dump_dir, b->id);
    b->raw = fopen(path, "wb");
    *obj = dsb_vtbl;

    if (desc->lpwfxFormat)
        log_event("buf%d created %p: %lu bytes, flags %#lx, %u ch %lu Hz %u bit", b->id, dsb,
                  desc->dwBufferBytes, desc->dwFlags, desc->lpwfxFormat->nChannels,
                  desc->lpwfxFormat->nSamplesPerSec, desc->lpwfxFormat->wBitsPerSample);
}

static HRESULT WINAPI ds_CreateSoundBuffer(void *iface, const DSBUFFERDESC *desc,
                                           IDirectSoundBuffer **out, IUnknown *outer)
{
    HRESULT hr = ((PFN_CREATEBUFFER)real_ds_vtbl[DS_CREATESOUNDBUFFER])(iface, desc, out, outer);

    EnterCriticalSection(&cs);
    if (SUCCEEDED(hr) && out && *out && !(desc->dwFlags & DSBCAPS_PRIMARYBUFFER))
        hook_buffer(*out, desc);
    else if (FAILED(hr))
        log_event("CreateSoundBuffer flags %#lx failed %#lx", desc ? desc->dwFlags : 0, hr);
    LeaveCriticalSection(&cs);
    return hr;
}

/* ---- exports ---- */

HRESULT WINAPI sg_CoInitialize(void *reserved)
{
    return CoInitialize(reserved);
}

void WINAPI sg_CoUninitialize(void)
{
    CoUninitialize();
}

HRESULT WINAPI sg_CoCreateInstance(REFCLSID clsid, IUnknown *outer, DWORD ctx, REFIID iid, void **out)
{
    HRESULT hr = CoCreateInstance(clsid, outer, ctx, iid, out);
    void ***obj;

    if (!dump_dir[0] || FAILED(hr) || !out || !*out) return hr;
    if (!IsEqualCLSID(clsid, &CLSID_DirectSound) && !IsEqualCLSID(clsid, &CLSID_DirectSound8)) return hr;

    EnterCriticalSection(&cs);
    obj = (void ***)*out;
    if (!real_ds_vtbl)
    {
        real_ds_vtbl = *obj;
        memcpy(ds_vtbl, real_ds_vtbl, sizeof(ds_vtbl));
        ds_vtbl[DS_CREATESOUNDBUFFER] = ds_CreateSoundBuffer;
    }
    if (*obj == real_ds_vtbl)
    {
        *obj = ds_vtbl;
        log_event("DirectSound object %p hooked", *out);
    }
    LeaveCriticalSection(&cs);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    char path[MAX_PATH + 32];

    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    InitializeCriticalSection(&cs);
    if (!GetEnvironmentVariableA("SGSND_DUMP", dump_dir, sizeof(dump_dir))) return TRUE;
    CreateDirectoryA(dump_dir, NULL);
    snprintf(path, sizeof(path), "%s\\events.log", dump_dir);
    if (!(events = fopen(path, "w"))) dump_dir[0] = 0;
    start_ms = GetTickCount();
    log_event("sgsnd loaded, dumping to %s (GetTickCount at start: %lu)", dump_dir, start_ms);
    return TRUE;
}
