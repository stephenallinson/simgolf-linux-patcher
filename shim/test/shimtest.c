/*
 * Tests terrgl32.dll the way Terrain.dll uses it: GL into a 16bpp top-down
 * DIB in a memory DC, mixed with GDI drawing, through the shim's exports.
 * Usage: wine shimtest.exe <path to terrgl32.dll>
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

#define W 800
#define H 600

typedef HGLRC (WINAPI *PFN_CREATE)(HDC);
typedef BOOL (WINAPI *PFN_MAKECURRENT)(HDC, HGLRC);
typedef BOOL (WINAPI *PFN_DELETE)(HGLRC);
typedef void (WINAPI *PFN_BEGIN)(GLenum);
typedef void (WINAPI *PFN_FLUSH)(void);

static PFN_CREATE create;
static PFN_MAKECURRENT make_current;
static PFN_DELETE delete_context;
static PFN_BEGIN begin;
static PFN_FLUSH flush;
static int failures;

static void check(BOOL ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

/* Fill pixel rect [x0,x1) x [y0,y1) in GL window coordinates. */
static void quad(int x0, int y0, int x1, int y1, float r, float g, float b)
{
    glColor3f(r, g, b);
    begin(GL_QUADS);
    glVertex2i(x0, y0); glVertex2i(x1, y0); glVertex2i(x1, y1); glVertex2i(x0, y1);
    glEnd();
}

static int count_diffs(const WORD *a, const WORD *b)
{
    int i, n = 0;
    for (i = 0; i < W * H; i++) n += a[i] != b[i];
    return n;
}

int main(int argc, char **argv)
{
    static WORD snap[W * H];
    PIXELFORMATDESCRIPTOR pfd = {.nSize = sizeof(pfd), .nVersion = 1, .dwFlags = 0x28, .cColorBits = 32};
    BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = W, .biHeight = -H,
                                   .biPlanes = 1, .biBitCount = 16, .biCompression = BI_RGB}};
    HMODULE shim = LoadLibraryA(argc > 1 ? argv[1] : "terrgl32.dll");
    LARGE_INTEGER freq, t0, t1;
    HBITMAP bmp;
    HGLRC rc;
    WORD *bits;
    HDC dc;
    int i, x, y, ok;

    if (!shim) { printf("can't load shim\n"); return 1; }
    create = (PFN_CREATE)GetProcAddress(shim, "wglCreateContext");
    make_current = (PFN_MAKECURRENT)GetProcAddress(shim, "wglMakeCurrent");
    delete_context = (PFN_DELETE)GetProcAddress(shim, "wglDeleteContext");
    begin = (PFN_BEGIN)GetProcAddress(shim, "glBegin");
    flush = (PFN_FLUSH)GetProcAddress(shim, "glFlush");

    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    SelectObject(dc, bmp);
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    rc = create(dc);
    check(rc != NULL, "context created for memory DC");
    check(make_current(dc, rc), "context made current");

    /* y-up ortho in pixels, as Terrain.dll uses on NT5+ */
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    /* GDI background pattern */
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) bits[y * W + x] = (WORD)((x * 7 + y * 13) & 0x7fff);
    memcpy(snap, bits, sizeof(snap));

    /* 1: red quad on GL rows 0..9, cols 0..19 */
    quad(0, 0, 20, 10, 1, 0, 0);
    flush();
    check(bits[0] == 0x7c00 && bits[9 * W + 19] == 0x7c00, "GL row 0 is the DIB's first scanline, colour exact (555 red)");
    check(bits[10 * W] == snap[10 * W] && bits[20] == snap[20], "pixels just outside the quad untouched");
    check(count_diffs(bits, snap) == 200, "exactly the 200 drawn pixels changed");

    /* 2: GDI scroll, then flush with nothing drawn */
    BitBlt(dc, 0, 0, W - 64, H, dc, 64, 0, SRCCOPY);
    GdiFlush();
    memcpy(snap, bits, sizeof(snap));
    flush();
    check(count_diffs(bits, snap) == 0, "flush with nothing drawn leaves GDI scroll intact");

    /* 3: draw, GDI writes elsewhere before the flush */
    quad(W - 64, 100, W, 200, 0, 1, 0);
    BitBlt(dc, 0, 300, 50, 50, NULL, 0, 0, WHITENESS);
    GdiFlush();
    memcpy(snap, bits, sizeof(snap));
    flush();
    check(count_diffs(bits, snap) == 64 * 100, "only the 64x100 green strip changed after flush");
    check(bits[300 * W] == snap[300 * W] && (bits[300 * W] & 0x7fff) == 0x7fff, "GDI white written mid-frame survives");
    check(bits[150 * W + W - 1] == 0x03e0, "green strip colour exact (555 green)");

    /* 4: blending against DIB content uploaded after GDI change */
    for (y = 400; y < 410; y++) for (x = 0; x < 10; x++) bits[y * W + x] = 0x001f;  /* blue, direct write */
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 0, 0, 0.5f);
    begin(GL_QUADS); glVertex2i(0, 400); glVertex2i(10, 400); glVertex2i(10, 410); glVertex2i(0, 410); glEnd();
    glDisable(GL_BLEND);
    flush();
    ok = ((bits[405 * W + 5] >> 10) & 0x1f) >= 15 && (bits[405 * W + 5] & 0x1f) >= 15;
    check(ok, "blend sees directly-written DIB pixels (red over blue -> purple)");

    /* 6: game restores the DIB to its pre-draw state, then draws elsewhere:
     * GL's earlier drawing must not come back */
    memcpy(snap, bits, sizeof(snap));
    quad(100, 100, 300, 300, 0, 0, 1);
    flush();
    memcpy(bits, snap, sizeof(snap));  /* jgl puts the back buffer back */
    quad(500, 500, 510, 510, 1, 1, 1);
    flush();
    check(count_diffs(bits, snap) == 100, "after the game restores the DIB, only the new 10x10 draw appears");

    /* 5: speed: 300 tiny draw+flush cycles like drawLine */
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (i = 0; i < 300; i++) { quad(i % W, 500, i % W + 2, 502, 1, 1, 0); flush(); }
    QueryPerformanceCounter(&t1);
    printf("info: 300 small draw+flush cycles took %.1f ms (%.2f ms each)\n",
           (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart,
           (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart / 300);

    make_current(NULL, NULL);
    delete_context(rc);
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
