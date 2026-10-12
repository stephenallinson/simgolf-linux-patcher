/* xdrive: send keys and mouse clicks to an X display through XTest, for
 * driving the game on a virtual display (Xvfb) during testing.
 *
 *   xdrive key <keysym>...       e.g. xdrive key Escape Return
 *   xdrive move <x> <y>
 *   xdrive click <x> <y> [button]
 *   xdrive drag <x0> <y0> <x1> <y1>
 *
 * The display comes from $DISPLAY. Build: tools/xdrive/build.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

static void pause_ms(int ms)
{
    usleep(ms * 1000);
}

static void move(Display *d, int x, int y)
{
    XTestFakeMotionEvent(d, -1, x, y, CurrentTime);
    XFlush(d);
    pause_ms(50);
}

static void button(Display *d, int b, Bool down)
{
    XTestFakeButtonEvent(d, b, down, CurrentTime);
    XFlush(d);
    pause_ms(80);
}

int main(int argc, char **argv)
{
    Display *d;
    int i;

    if (argc < 2) { fprintf(stderr, "usage: see source\n"); return 2; }
    if (!(d = XOpenDisplay(NULL))) { fprintf(stderr, "can't open display\n"); return 1; }
    if (!strcmp(argv[1], "key"))
    {
        for (i = 2; i < argc; i++)
        {
            KeySym sym = XStringToKeysym(argv[i]);
            KeyCode code = sym ? XKeysymToKeycode(d, sym) : 0;
            if (!code) { fprintf(stderr, "unknown key %s\n", argv[i]); continue; }
            XTestFakeKeyEvent(d, code, True, CurrentTime);
            XFlush(d);
            pause_ms(60);
            XTestFakeKeyEvent(d, code, False, CurrentTime);
            XFlush(d);
            pause_ms(150);
        }
    }
    else if (!strcmp(argv[1], "move") && argc >= 4)
        move(d, atoi(argv[2]), atoi(argv[3]));
    else if (!strcmp(argv[1], "click") && argc >= 4)
    {
        int b = argc >= 5 ? atoi(argv[4]) : 1;
        move(d, atoi(argv[2]), atoi(argv[3]));
        button(d, b, True);
        button(d, b, False);
    }
    else if (!strcmp(argv[1], "drag") && argc >= 6)
    {
        int x0 = atoi(argv[2]), y0 = atoi(argv[3]), x1 = atoi(argv[4]), y1 = atoi(argv[5]), s;
        move(d, x0, y0);
        button(d, 1, True);
        for (s = 1; s <= 10; s++) move(d, x0 + (x1 - x0) * s / 10, y0 + (y1 - y0) * s / 10);
        button(d, 1, False);
    }
    else { fprintf(stderr, "bad command\n"); return 2; }
    XCloseDisplay(d);
    return 0;
}
