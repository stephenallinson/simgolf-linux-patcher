/*
 * terrgl32.dll: OpenGL shim for SimGolf's Terrain.dll.
 *
 * Terrain.dll renders with OpenGL into jgl.dll's 800x600 16bpp back buffer, a
 * top-down DIB section selected into a memory DC. On Windows the generic GL
 * implementation draws straight into the DIB's memory: GL row 0 is the DIB's
 * first scanline (the game picks a y-up glOrtho on NT5+ to suit), only drawn
 * fragments are written, and GDI drawing in between is preserved.
 *
 * Wine's memory DC support (flush_memory_dc() in dlls/win32u/opengl.c)
 * assumes 32bpp DIBs and copies the whole GL buffer over the DIB on every
 * glFlush/glReadPixels/glDrawPixels/glViewport/glClear, so it can't do this.
 * Instead, contexts created for memory DCs are made on a hidden window and
 * render into our own framebuffer object, which Wine never ties to the DIB:
 *  - before the first glBegin after a flush, DIB rows that changed since GL
 *    last saw them are uploaded into the FBO;
 *  - on glFlush, the FBO is read back and only the pixels GL changed are
 *    written to the DIB, in the DIB's format.
 *
 * All other imports are forwarded to opengl32 (see terrgl32.def).
 *
 * Debug log: set TERRGL32_LOG to a file path, or to "debug" to send it to
 * OutputDebugString (WINEDEBUG=+debugstr).
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif
#define GL_UNSIGNED_SHORT_5_6_5         0x8363
#define GL_UNSIGNED_SHORT_1_5_5_5_REV   0x8366
#define GL_FRAMEBUFFER                  0x8D40
#define GL_RENDERBUFFER                 0x8D41
#define GL_RGBA8                        0x8058
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5

typedef void (WINAPI *PFN_GENS)(GLsizei, GLuint *);
typedef void (WINAPI *PFN_BIND)(GLenum, GLuint);
typedef void (WINAPI *PFN_STORAGE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (WINAPI *PFN_ATTACH)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (WINAPI *PFN_STATUS)(GLenum);
typedef void (WINAPI *PFN_WINDOWPOS)(GLint, GLint);

#define TERRGL32_VERSION "0.1.0"

static PFN_GENS p_glGenFramebuffers, p_glGenRenderbuffers;
static PFN_GENS p_glDeleteFramebuffers, p_glDeleteRenderbuffers;
static PFN_BIND p_glBindFramebuffer, p_glBindRenderbuffer;
static PFN_STORAGE p_glRenderbufferStorage;
static PFN_ATTACH p_glFramebufferRenderbuffer;
static PFN_STATUS p_glCheckFramebufferStatus;
static PFN_WINDOWPOS p_glWindowPos2i;

/* A GL context we created for a memory DC. */
struct shim_context
{
    HGLRC rc;
    HDC memdc;
    GLuint fbo, color;
    int width, height;
    struct shim_context *next;
};

static struct shim_context *contexts, *current;
static HWND hidden_hwnd;
static HDC hidden_dc;

/* The DIB selected into the current memory DC. */
struct dib
{
    char *bits;
    int width, height, stride, bpp;
    GLenum format, type;
};

static char *scratch;   /* FBO readback, DIB layout */
static char *uploaded;  /* what the FBO holds, in the DIB's format and layout */
static size_t buf_size;
static BOOL need_upload = TRUE, uploaded_valid;
static int begins_since_flush, flush_count;

/* ---- logging ---- */

#define LOG_MAX_LINES 50000
static FILE *log_file;
static BOOL log_on, log_ods;
static int log_lines, log_inited;

static void log_msg(const char *fmt, ...)
{
    char buf[512];
    int len;
    va_list args;

    if (!log_inited)
    {
        log_inited = 1;
        if (GetEnvironmentVariableA("TERRGL32_LOG", buf, sizeof(buf)))
        {
            if (!strcmp(buf, "debug")) log_ods = TRUE;
            else log_file = fopen(buf, "w");
            log_on = log_ods || log_file;
        }
    }
    if (!log_on || log_lines >= LOG_MAX_LINES) return;
    len = snprintf(buf, sizeof(buf), "terrgl32 %8lu ", GetTickCount());
    va_start(args, fmt);
    vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
    va_end(args);
    if (log_ods) OutputDebugStringA(buf);
    else fprintf(log_file, "%s\n", buf + 9);
    if (++log_lines == LOG_MAX_LINES)
    {
        if (log_ods) OutputDebugStringA("terrgl32 log line limit reached");
        else fputs("log line limit reached\n", log_file);
    }
    if (log_file) fflush(log_file);
}

/* ---- DIB access ---- */

static BOOL get_target_dib(struct dib *dib)
{
    HBITMAP bmp;
    DIBSECTION ds;
    size_t size;

    if (!current) return FALSE;
    if (!(bmp = GetCurrentObject(current->memdc, OBJ_BITMAP))) return FALSE;
    if (GetObjectW(bmp, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits) return FALSE;

    dib->bits = ds.dsBm.bmBits;
    dib->width = ds.dsBm.bmWidth;
    dib->height = ds.dsBm.bmHeight;
    dib->stride = ds.dsBm.bmWidthBytes;
    dib->bpp = ds.dsBmih.biBitCount;
    switch (dib->bpp)
    {
    case 16:
        if (ds.dsBmih.biCompression == BI_BITFIELDS && ds.dsBitfields[1] == 0x07e0)
        {
            dib->format = GL_RGB;
            dib->type = GL_UNSIGNED_SHORT_5_6_5;
        }
        else
        {
            dib->format = GL_BGRA;  /* BI_RGB 16bpp is X1R5G5B5 */
            dib->type = GL_UNSIGNED_SHORT_1_5_5_5_REV;
        }
        break;
    case 24:
        dib->format = GL_BGR;
        dib->type = GL_UNSIGNED_BYTE;
        break;
    case 32:
        dib->format = GL_BGRA;
        dib->type = GL_UNSIGNED_BYTE;
        break;
    default:
        return FALSE;
    }

    size = (size_t)dib->stride * dib->height;
    if (buf_size < size)
    {
        free(scratch);
        free(uploaded);
        buf_size = size;
        scratch = malloc(size);
        uploaded = malloc(size);
        uploaded_valid = FALSE;
        if (!scratch || !uploaded) { buf_size = 0; return FALSE; }
    }
    return TRUE;
}

/* ---- hidden window and FBO ---- */

static BOOL create_hidden_window(void)
{
    static const PIXELFORMATDESCRIPTOR pfd =
    {
        .nSize = sizeof(pfd), .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,
        .iPixelType = PFD_TYPE_RGBA, .cColorBits = 32, .cAlphaBits = 8,
    };
    WNDCLASSA cls = {.lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(NULL),
                     .lpszClassName = "terrgl32_hidden"};
    int format;

    if (hidden_dc) return TRUE;
    RegisterClassA(&cls);
    hidden_hwnd = CreateWindowExA(0, cls.lpszClassName, "", WS_POPUP, 0, 0, 1, 1,
                                  NULL, NULL, cls.hInstance, NULL);
    if (!hidden_hwnd) return FALSE;
    hidden_dc = GetDC(hidden_hwnd);
    format = ChoosePixelFormat(hidden_dc, &pfd);
    if (!format || !SetPixelFormat(hidden_dc, format, &pfd))
    {
        log_msg("hidden window pixel format failed (format %d)", format);
        return FALSE;
    }
    return TRUE;
}

static void load_gl_extensions(void)
{
    if (p_glBindFramebuffer) return;
    p_glGenFramebuffers = (PFN_GENS)wglGetProcAddress("glGenFramebuffers");
    p_glDeleteFramebuffers = (PFN_GENS)wglGetProcAddress("glDeleteFramebuffers");
    p_glBindFramebuffer = (PFN_BIND)wglGetProcAddress("glBindFramebuffer");
    p_glGenRenderbuffers = (PFN_GENS)wglGetProcAddress("glGenRenderbuffers");
    p_glDeleteRenderbuffers = (PFN_GENS)wglGetProcAddress("glDeleteRenderbuffers");
    p_glBindRenderbuffer = (PFN_BIND)wglGetProcAddress("glBindRenderbuffer");
    p_glRenderbufferStorage = (PFN_STORAGE)wglGetProcAddress("glRenderbufferStorage");
    p_glFramebufferRenderbuffer = (PFN_ATTACH)wglGetProcAddress("glFramebufferRenderbuffer");
    p_glCheckFramebufferStatus = (PFN_STATUS)wglGetProcAddress("glCheckFramebufferStatus");
    p_glWindowPos2i = (PFN_WINDOWPOS)wglGetProcAddress("glWindowPos2i");
    log_msg("terrgl32 " TERRGL32_VERSION "; GL %s / %s; FBO %s, glWindowPos2i %s", glGetString(GL_VERSION), glGetString(GL_RENDERER),
            p_glBindFramebuffer ? "yes" : "NO", p_glWindowPos2i ? "yes" : "NO");
}

/* Make sure the current context has an FBO matching the DIB, and bind it. */
static BOOL ensure_fbo(const struct dib *dib)
{
    GLenum status;

    if (current->fbo && current->width == dib->width && current->height == dib->height) return TRUE;
    load_gl_extensions();
    if (!p_glBindFramebuffer) return FALSE;

    if (!current->fbo)
    {
        p_glGenFramebuffers(1, &current->fbo);
        p_glGenRenderbuffers(1, &current->color);
    }
    p_glBindRenderbuffer(GL_RENDERBUFFER, current->color);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, dib->width, dib->height);
    p_glBindFramebuffer(GL_FRAMEBUFFER, current->fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, current->color);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    current->width = dib->width;
    current->height = dib->height;
    uploaded_valid = FALSE;
    log_msg("FBO %u for %dx%d %dbpp DIB, status %#x", current->fbo, dib->width, dib->height, dib->bpp, status);
    return status == GL_FRAMEBUFFER_COMPLETE;
}

/* ---- DIB <-> GL ---- */

static BOOL pixel_changed(const char *a, const char *b, int bpp)
{
    switch (bpp)
    {
    case 16: return ((*(const WORD *)a ^ *(const WORD *)b) & 0x7fff) != 0;  /* ignore 5-5-5's top bit */
    case 24:
    case 32: return memcmp(a, b, 3) != 0;  /* GL's alpha isn't part of the image */
    }
    return FALSE;
}

static BOOL rows_equal(const char *a, const char *b, const struct dib *dib)
{
    int x, bytes = dib->bpp / 8;

    if (!memcmp(a, b, (size_t)dib->width * bytes)) return TRUE;
    for (x = 0; x < dib->width; x++)
        if (pixel_changed(a + x * bytes, b + x * bytes, dib->bpp)) return FALSE;
    return TRUE;
}

/* Upload DIB rows that differ from what the FBO holds. GL row y is DIB row y. */
static void upload_dib(void)
{
    struct dib dib;
    int y, y0 = -1, y1 = -1;

    if (!get_target_dib(&dib) || !ensure_fbo(&dib)) return;
    GdiFlush();
    for (y = 0; y < dib.height; y++)
    {
        size_t off = (size_t)y * dib.stride;
        if (uploaded_valid && rows_equal(dib.bits + off, uploaded + off, &dib)) continue;
        if (y0 < 0) y0 = y;
        y1 = y;
    }
    if (y0 < 0) return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_TEXTURE_1D);
    glDisable(GL_TEXTURE_2D);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelZoom(1.0f, 1.0f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  /* DIB rows are DWORD aligned */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
    if (p_glWindowPos2i)
        p_glWindowPos2i(0, y0);
    else
    {
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glViewport(0, 0, dib.width, dib.height);
        glRasterPos2f(-1.0f, -1.0f + 2.0f * y0 / dib.height);
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
    }
    glDrawPixels(dib.width, y1 - y0 + 1, dib.format, dib.type, dib.bits + (size_t)y0 * dib.stride);
    glPopClientAttrib();
    glPopAttrib();  /* restores matrix mode and viewport too */

    memcpy(uploaded + (size_t)y0 * dib.stride, dib.bits + (size_t)y0 * dib.stride,
           (size_t)(y1 - y0 + 1) * dib.stride);
    if (!uploaded_valid)
    {
        /* first upload covers all rows, so the whole copy is now valid */
        uploaded_valid = TRUE;
    }
    log_msg("upload rows %d..%d, glGetError %#x", y0, y1, glGetError());
}

/* Read the FBO back and write the pixels GL changed into the DIB. */
static void readback_dib(void)
{
    struct dib dib;
    int y, x, bytes, x0, x1 = -1, y0 = -1, y1 = -1;

    if (!get_target_dib(&dib) || !uploaded_valid) return;
    bytes = dib.bpp / 8;
    x0 = dib.width;

    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
    glReadPixels(0, 0, dib.width, dib.height, dib.format, dib.type, scratch);
    glPopClientAttrib();

    GdiFlush();
    for (y = 0; y < dib.height; y++)
    {
        size_t off = (size_t)y * dib.stride;
        const char *gl = scratch + off, *old = uploaded + off;
        char *dst = dib.bits + off;
        int first = -1, last = -1;

        for (x = 0; x < dib.width; x++)
        {
            if (!pixel_changed(gl + x * bytes, old + x * bytes, dib.bpp)) continue;
            if (dib.bpp == 16) *(WORD *)(dst + x * 2) = *(const WORD *)(gl + x * 2) & 0x7fff;
            else memcpy(dst + x * bytes, gl + x * bytes, 3);
            if (first < 0) first = x;
            last = x;
        }
        if (first < 0) continue;
        if (first < x0) x0 = first;
        if (last > x1) x1 = last;
        if (y0 < 0) y0 = y;
        y1 = y;
    }
    /* The FBO now holds the readback, not what was uploaded: GL's drawing must
     * not be mistaken for DIB content if the game later restores the DIB. */
    memcpy(uploaded, scratch, (size_t)dib.stride * dib.height);
    if (y0 >= 0) log_msg("  GL changed DIB x %d..%d y %d..%d", x0, x1, y0, y1);
    else log_msg("  GL changed nothing");
}

/* ---- exports ---- */

void WINAPI shim_glBegin(GLenum mode)
{
    if (need_upload && current)
    {
        upload_dib();
        need_upload = FALSE;
    }
    begins_since_flush++;
    glBegin(mode);
}

void WINAPI shim_glFlush(void)
{
    glFlush();
    if (current)
    {
        log_msg("flush #%d caller %p begins %d", flush_count, __builtin_return_address(0), begins_since_flush);
        if (begins_since_flush) readback_dib();
    }
    need_upload = TRUE;
    flush_count++;
    begins_since_flush = 0;
}

/* Log every loaded module with its address range (for reverse engineering). */
static void log_modules(void)
{
    HMODULE mods[256];
    DWORD needed, i;
    MODULEINFO info;
    char name[MAX_PATH];

    if (!log_on) return;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    for (i = 0; i < needed / sizeof(HMODULE) && i < 256; i++)
    {
        if (!GetModuleFileNameA(mods[i], name, sizeof(name))) continue;
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &info, sizeof(info))) continue;
        log_msg("module %p-%p %s", info.lpBaseOfDll, (char *)info.lpBaseOfDll + info.SizeOfImage, name);
    }
}

HGLRC WINAPI shim_wglCreateContext(HDC hdc)
{
    struct shim_context *ctx;
    HGLRC rc;

    if (GetObjectType(hdc) != OBJ_MEMDC) return wglCreateContext(hdc);
    if (!create_hidden_window()) return NULL;
    if (!(rc = wglCreateContext(hidden_dc)))
    {
        log_msg("wglCreateContext on hidden window failed, error %lu", GetLastError());
        return NULL;
    }
    ctx = calloc(1, sizeof(*ctx));
    ctx->rc = rc;
    ctx->memdc = hdc;
    ctx->next = contexts;
    contexts = ctx;
    log_msg("wglCreateContext memdc %p -> rc %p on hidden window", hdc, rc);
    log_modules();
    return rc;
}

static struct shim_context *find_context(HGLRC rc)
{
    struct shim_context *ctx;

    for (ctx = contexts; ctx; ctx = ctx->next)
        if (ctx->rc == rc) return ctx;
    return NULL;
}

BOOL WINAPI shim_wglMakeCurrent(HDC dc, HGLRC rc)
{
    struct shim_context *ctx = rc ? find_context(rc) : NULL;
    struct dib dib;
    BOOL first, ret;

    need_upload = TRUE;
    if (!ctx)
    {
        current = NULL;
        ret = wglMakeCurrent(dc, rc);
        log_msg("wglMakeCurrent dc %p rc %p -> %d", dc, rc, ret);
        return ret;
    }

    ctx->memdc = dc;
    if (!(ret = wglMakeCurrent(hidden_dc, rc)))
    {
        log_msg("wglMakeCurrent on hidden window failed, error %lu", GetLastError());
        current = NULL;
        return FALSE;
    }
    first = !ctx->fbo;
    current = ctx;
    uploaded_valid = FALSE;
    if (get_target_dib(&dib) && ensure_fbo(&dib))
    {
        p_glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        if (first) glViewport(0, 0, dib.width, dib.height);  /* Windows' default for a DIB */
    }
    log_msg("wglMakeCurrent memdc %p rc %p -> %d", dc, rc, ret);
    return ret;
}

BOOL WINAPI shim_wglDeleteContext(HGLRC rc)
{
    struct shim_context **p, *ctx;

    for (p = &contexts; (ctx = *p); p = &ctx->next)
    {
        if (ctx->rc != rc) continue;
        if (ctx == current) current = NULL;
        *p = ctx->next;
        free(ctx);  /* the FBO goes away with the context */
        break;
    }
    return wglDeleteContext(rc);
}

void WINAPI shim_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    log_msg("glViewport %d,%d %dx%d", x, y, w, h);
    glViewport(x, y, w, h);
}
