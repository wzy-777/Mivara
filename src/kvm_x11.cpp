/**
 * KVM_X11.CPP – Linux/X11 backend for the stitch target (被控端).
 *
 * The Windows backend injects with SendInput; here the equivalent is XTest
 * (libXtst) on the session's X display. Input arrives as Windows set-1 scan
 * codes (the wire format shared with the Windows controller), which map to
 * Linux evdev key codes almost 1:1 for the unextended block; the E0-prefixed
 * keys need the small table below. X key codes are evdev + 8, so the target's
 * own keyboard layout still applies (a physical keyboard behaves the same).
 *
 * libXtst is loaded with dlopen so the build does not require libxtst-dev;
 * the runtime package (libxtst6 / libXtst) is present on any X11 desktop.
 * Native Wayland sessions have no global injection path; the project's Linux
 * capture is X11-only too, so that boundary is intentional.
 *
 * This file is the target (被控端) backend only; the Linux controller lives in
 * kvm_x11_ctl.cpp (XInput2 raw events + grabs).
 */
#include "kvm_core.h"
#include "kvm_target.h"

#ifdef __linux__

#include <X11/XF86keysym.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>

#include <cstring>
#include <iostream>

namespace {

Display *g_dpy = nullptr;

void ghostMoveX11(int x, int y);
void ghostPumpX11();
void ghostEndX11();

typedef int (*XTestFakeMotionEventFn)(Display *, int, int, int, unsigned long);
typedef int (*XTestFakeButtonEventFn)(Display *, unsigned int, Bool,
                                      unsigned long);
typedef int (*XTestFakeKeyEventFn)(Display *, unsigned int, Bool,
                                   unsigned long);
typedef Bool (*XTestQueryExtensionFn)(Display *, int *, int *, int *, int *);

XTestFakeMotionEventFn g_fakeMotion = nullptr;
XTestFakeButtonEventFn  g_fakeButton = nullptr;
XTestFakeKeyEventFn     g_fakeKey = nullptr;

Display *display()
{
    if (!g_dpy) g_dpy = XOpenDisplay(nullptr);
    return g_dpy;
}

bool initX11()
{
    /* SDL's X11 backend normally calls this already; doing it here as well
     * covers a standalone receiver where our listener/session threads are the
     * first Xlib users. It is idempotent and must precede XOpenDisplay. */
    XInitThreads();
    Display *d = XOpenDisplay(nullptr);
    if (!d) return false;

    void *lib = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libXtst.so", RTLD_NOW | RTLD_LOCAL);
    bool ok = false;
    if (lib) {
        g_fakeMotion = (XTestFakeMotionEventFn)dlsym(lib,
                                                     "XTestFakeMotionEvent");
        g_fakeButton = (XTestFakeButtonEventFn)dlsym(lib,
                                                    "XTestFakeButtonEvent");
        g_fakeKey = (XTestFakeKeyEventFn)dlsym(lib, "XTestFakeKeyEvent");
        auto query =
            (XTestQueryExtensionFn)dlsym(lib, "XTestQueryExtension");
        int ev = 0, err = 0, major = 0, minor = 0;
        ok = g_fakeMotion && g_fakeButton && g_fakeKey && query &&
             query(d, &ev, &err, &major, &minor);
    }
    XCloseDisplay(d);
    if (!ok) {
        std::cout << "[kvm] libXtst/XTest unavailable; stitch target disabled\n"
                  << std::flush;
        return false;
    }
    return true;
}

void metricsX11(int &x, int &y, uint32_t &w, uint32_t &h)
{
    x = 0;
    y = 0;
    Display *d = display();
    if (!d) {
        w = h = 1;
        return;
    }
    const int scr = DefaultScreen(d);
    w = (uint32_t)DisplayWidth(d, scr);
    h = (uint32_t)DisplayHeight(d, scr);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
}

/* Linux input event codes are the AT set-1 codes for the unextended block, so
 * the mapping is identity there. The E0-prefixed keys are the exception. */
int evdevForExtended(unsigned scancode)
{
    switch (scancode) {
    case 0x10: return 165;   /* KEY_PREVIOUSSONG */
    case 0x19: return 163;   /* KEY_NEXTSONG     */
    case 0x1C: return 96;    /* KEY_KPENTER      */
    case 0x1D: return 97;    /* KEY_RIGHTCTRL    */
    case 0x20: return 113;   /* KEY_MUTE         */
    case 0x22: return 164;   /* KEY_PLAYPAUSE    */
    case 0x24: return 166;   /* KEY_STOPCD       */
    case 0x2E: return 114;   /* KEY_VOLUMEDOWN   */
    case 0x30: return 115;   /* KEY_VOLUMEUP     */
    case 0x35: return 98;    /* KEY_KPSLASH      */
    case 0x37: return 99;    /* KEY_SYSRQ        */
    case 0x38: return 100;   /* KEY_RIGHTALT     */
    case 0x47: return 102;   /* KEY_HOME         */
    case 0x48: return 103;   /* KEY_UP           */
    case 0x49: return 104;   /* KEY_PAGEUP       */
    case 0x4B: return 105;   /* KEY_LEFT         */
    case 0x4D: return 106;   /* KEY_RIGHT        */
    case 0x4F: return 107;   /* KEY_END          */
    case 0x50: return 108;   /* KEY_DOWN         */
    case 0x51: return 109;   /* KEY_PAGEDOWN     */
    case 0x52: return 110;   /* KEY_INSERT       */
    case 0x53: return 111;   /* KEY_DELETE       */
    case 0x5B: return 125;   /* KEY_LEFTMETA     */
    case 0x5C: return 126;   /* KEY_RIGHTMETA    */
    case 0x5D: return 127;   /* KEY_COMPOSE      */
    default: return 0;
    }
}

/* Keys that arrive without a scan code (media keys from some sources) or that
 * must ignore the scan code (Pause). Values are Windows virtual-key codes. */
KeySym keysymForVk(uint32_t vk)
{
    switch (vk) {
    case 0x08: return XK_BackSpace;
    case 0x09: return XK_Tab;
    case 0x0D: return XK_Return;
    case 0x10: return XK_Shift_L;
    case 0x11: return XK_Control_L;
    case 0x12: return XK_Alt_L;
    case 0x13: return XK_Pause;
    case 0x14: return XK_Caps_Lock;
    case 0x1B: return XK_Escape;
    case 0x20: return XK_space;
    case 0x21: return XK_Page_Up;
    case 0x22: return XK_Page_Down;
    case 0x23: return XK_End;
    case 0x24: return XK_Home;
    case 0x25: return XK_Left;
    case 0x26: return XK_Up;
    case 0x27: return XK_Right;
    case 0x28: return XK_Down;
    case 0x2C: return XK_Print;
    case 0x2D: return XK_Insert;
    case 0x2E: return XK_Delete;
    case 0x5B: return XK_Super_L;
    case 0x5C: return XK_Super_R;
    case 0x5D: return XK_Menu;
    case 0x6A: return XK_KP_Multiply;
    case 0x6B: return XK_KP_Add;
    case 0x6D: return XK_KP_Subtract;
    case 0x6E: return XK_KP_Decimal;
    case 0x6F: return XK_KP_Divide;
    case 0x90: return XK_Num_Lock;
    case 0x91: return XK_Scroll_Lock;
    case 0xA0: return XK_Shift_L;
    case 0xA1: return XK_Shift_R;
    case 0xA2: return XK_Control_L;
    case 0xA3: return XK_Control_R;
    case 0xA4: return XK_Alt_L;
    case 0xA5: return XK_Alt_R;
    case 0xAD: return XF86XK_AudioMute;
    case 0xAE: return XF86XK_AudioLowerVolume;
    case 0xAF: return XF86XK_AudioRaiseVolume;
    case 0xB0: return XF86XK_AudioNext;
    case 0xB1: return XF86XK_AudioPrev;
    case 0xB2: return XF86XK_AudioStop;
    case 0xB3: return XF86XK_AudioPlay;
    default:
        if (vk >= 0x70 && vk <= 0x87)  /* F1..F24 */
            return XK_F1 + (vk - 0x70);
        return NoSymbol;
    }
}

int keycodeFor(uint32_t vk, uint32_t scan, bool ext)
{
    Display *d = display();
    if (!d) return 0;

    int code = 0;
    const unsigned sc = scan & 0xFFu;
    if (ext) {
        if (sc == 0x2A || sc == 0x36) return 0; /* fake shifts */
        code = evdevForExtended(sc);
    } else if (sc) {
        /* The scan CODE is layout-independent; the target's X keymap turns
         * the keycode into the layout's symbol, exactly like its own keys. */
        code = (int)sc;
    }
    if (!code) {
        const KeySym ks = keysymForVk(vk);
        if (ks == NoSymbol) return 0;
        return (int)XKeysymToKeycode(d, ks);
    }

    const int keycode = code + 8;   /* X keycode = evdev code + 8 */
    int min_kc = 8, max_kc = 255;
    XDisplayKeycodes(d, &min_kc, &max_kc);
    return (keycode >= min_kc && keycode <= max_kc) ? keycode : 0;
}

struct X11Held {
    bool key[256] = {};
    bool btn[16] = {};
    int  wheel_v = 0;
    int  wheel_h = 0;
};
X11Held g_held;

int xButton(uint32_t btn)
{
    switch (btn) {
    case 1: return 1;   /* left       */
    case 2: return 3;   /* right      */
    case 3: return 2;   /* middle     */
    case 4: return 8;   /* back       */
    case 5: return 9;   /* forward    */
    default: return 0;
    }
}

void injectMoveX11(int x, int y, uint32_t w, uint32_t h, int sx, int sy)
{
    (void)sx;
    (void)sy;
    Display *d = display();
    if (!d || !g_fakeMotion) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= w) x = (int)w - 1;
    if ((uint32_t)y >= h) y = (int)h - 1;
    g_fakeMotion(d, DefaultScreen(d), x, y, CurrentTime);
    ghostMoveX11(x + sx, y + sy);
    ghostPumpX11();
    XFlush(d);
}

void injectButtonX11(uint32_t btn, bool down)
{
    Display *d = display();
    if (!d || !g_fakeButton) return;
    const int b = xButton(btn);
    if (!b) return;
    g_fakeButton(d, (unsigned)b, down ? True : False, CurrentTime);
    if (b < 16) g_held.btn[b] = down;
    XFlush(d);
}

void injectWheelX11(int delta, bool horizontal)
{
    Display *d = display();
    if (!d || !g_fakeButton || delta == 0) return;
    int &acc = horizontal ? g_held.wheel_h : g_held.wheel_v;
    acc += delta;
    int clicks = acc / 120;
    acc -= clicks * 120;
    if (!clicks) return;
    const unsigned button = horizontal ? (clicks > 0 ? 7u : 6u)
                                       : (clicks > 0 ? 4u : 5u);
    const int n = clicks > 0 ? clicks : -clicks;
    for (int i = 0; i < n; i++) {
        g_fakeButton(d, button, True, CurrentTime);
        g_fakeButton(d, button, False, CurrentTime);
    }
    XFlush(d);
}

void injectKeyX11(bool down, uint32_t vk, uint32_t scan, bool ext)
{
    Display *d = display();
    if (!d || !g_fakeKey) return;
    const int keycode = keycodeFor(vk, scan, ext);
    if (!keycode) return;
    g_fakeKey(d, (unsigned)keycode, down ? True : False, CurrentTime);
    if (keycode > 0 && keycode < 256) g_held.key[keycode] = down;
    XFlush(d);
}

/* ── drag ghost (visual only) ─────────────────────────────────────────────
 * While a file drag crosses the shared edge the source's drag image stays on
 * the controller's desktop. Draw a small opaque card next to the injected
 * cursor on this screen. It is deliberately simple Xlib drawing (no compositor
 * / ARGB requirement); text is best-effort and a long or non-ASCII name still
 * gets a visible card. */
struct GhostStateX11 {
    Window win = 0;
    GC gc = 0;
    XFontStruct *font = nullptr;
    int x = 0, y = 0, w = 0, h = 0;
    int count = 0;
    int frames_left = 0;
    bool visible = false;
    std::string name;
};
GhostStateX11 g_ghost_x11;

std::string ghostLabel()
{
    std::string label = g_ghost_x11.name;
    if (label.empty()) label = "Files";
    if (g_ghost_x11.count > 1)
        label += "  +" + std::to_string(g_ghost_x11.count - 1);
    return label;
}

bool ghostCreateX11()
{
    if (g_ghost_x11.win) return true;
    Display *d = display();
    if (!d) return false;
    const int scr = DefaultScreen(d);
    XSetWindowAttributes a = {};
    a.override_redirect = True;
    a.background_pixel = BlackPixel(d, scr);
    a.border_pixel = WhitePixel(d, scr);
    a.event_mask = ExposureMask;
    g_ghost_x11.win = XCreateWindow(
        d, DefaultRootWindow(d), 0, 0, 240, 48, 1, CopyFromParent,
        InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &a);
    if (!g_ghost_x11.win) return false;
    g_ghost_x11.gc = XCreateGC(d, g_ghost_x11.win, 0, nullptr);
    g_ghost_x11.font = XLoadQueryFont(d, "9x15");
    if (!g_ghost_x11.font) g_ghost_x11.font = XLoadQueryFont(d, "fixed");
    if (g_ghost_x11.font)
        XSetFont(d, g_ghost_x11.gc, g_ghost_x11.font->fid);
    return true;
}

void ghostPaintX11()
{
    Display *d = display();
    if (!d || !g_ghost_x11.win) return;
    const int scr = DefaultScreen(d);
    XClearWindow(d, g_ghost_x11.win);
    XSetForeground(d, g_ghost_x11.gc, WhitePixel(d, scr));
    /* A tiny page glyph. */
    const int ix = 10, iy = 10;
    XDrawRectangle(d, g_ghost_x11.win, g_ghost_x11.gc, ix, iy, 18, 26);
    XDrawLine(d, g_ghost_x11.win, g_ghost_x11.gc, ix + 18, iy + 7,
              ix + 11, iy);
    XDrawLine(d, g_ghost_x11.win, g_ghost_x11.gc, ix + 11, iy,
              ix + 11, iy + 7);
    XDrawLine(d, g_ghost_x11.win, g_ghost_x11.gc, ix + 11, iy + 7,
              ix + 18, iy + 7);
    std::string label = ghostLabel();
    size_t cut = std::min<size_t>(label.size(), 96);
    while (cut > 0 && ((unsigned char)label[cut] & 0xC0) == 0x80) cut--;
    label.resize(cut);
    XDrawString(d, g_ghost_x11.win, g_ghost_x11.gc, 40, 31,
                label.c_str(), (int)label.size());
}

void ghostPumpX11()
{
    Display *d = display();
    if (!d || !g_ghost_x11.win) return;
    XEvent ev;
    while (XCheckWindowEvent(d, g_ghost_x11.win, ExposureMask, &ev)) {
        if (ev.type == Expose && ev.xexpose.count == 0) ghostPaintX11();
    }
}

void ghostPlaceX11()
{
    Display *d = display();
    if (!d || !g_ghost_x11.win) return;
    XMoveWindow(d, g_ghost_x11.win, g_ghost_x11.x + 16,
                g_ghost_x11.y + 16);
    XRaiseWindow(d, g_ghost_x11.win);
    XFlush(d);
}

void ghostShowX11()
{
    if (!ghostCreateX11()) return;
    Display *d = display();
    std::string label = ghostLabel();
    int tw = 240;
    if (g_ghost_x11.font) {
        tw = XTextWidth(g_ghost_x11.font, label.c_str(),
                        (int)std::min<size_t>(label.size(), 96)) + 56;
        if (tw < 160) tw = 160;
        if (tw > 380) tw = 380;
    }
    g_ghost_x11.w = tw;
    g_ghost_x11.h = 48;
    XResizeWindow(d, g_ghost_x11.win, (unsigned)tw, 48);
    ghostPlaceX11();
    XMapRaised(d, g_ghost_x11.win);
    XSync(d, False);
    ghostPaintX11();
    XFlush(d);
    g_ghost_x11.visible = true;
}

void ghostHideX11()
{
    Display *d = display();
    if (!d || !g_ghost_x11.win) return;
    XUnmapWindow(d, g_ghost_x11.win);
    XFlush(d);
    g_ghost_x11.visible = false;
}

void ghostMoveX11(int x, int y)
{
    g_ghost_x11.x = x;
    g_ghost_x11.y = y;
    if (g_ghost_x11.visible) ghostPlaceX11();
}

void ghostEndX11()
{
    Display *d = display();
    if (d && g_ghost_x11.win) {
        XDestroyWindow(d, g_ghost_x11.win);
        XFreeGC(d, g_ghost_x11.gc);
        if (g_ghost_x11.font) XFreeFont(d, g_ghost_x11.font);
        XFlush(d);
    }
    g_ghost_x11 = GhostStateX11{};
}

void dragEventX11(const KvmEvent &e)
{
    ghostPumpX11();
    switch (e.type) {
    case KVM_EV_DRAG_INFO:
        if (e.a) {
            g_ghost_x11.frames_left = (int)e.c;
            g_ghost_x11.count = (int)e.b;
            g_ghost_x11.name.clear();
            ghostShowX11();
        } else {
            ghostHideX11();
            g_ghost_x11.frames_left = 0;
        }
        break;
    case KVM_EV_DRAG_TEXT:
        if (g_ghost_x11.frames_left > 0) {
            char chunk[12];
            std::memcpy(chunk, &e.a, 4);
            std::memcpy(chunk + 4, &e.b, 4);
            std::memcpy(chunk + 8, &e.c, 4);
            g_ghost_x11.name.append(chunk, 12);
            if (--g_ghost_x11.frames_left == 0) {
                while (!g_ghost_x11.name.empty() &&
                       g_ghost_x11.name.back() == '\0')
                    g_ghost_x11.name.pop_back();
                ghostShowX11();
            }
        }
        break;
    default:
        break;
    }
}

void sessionEndX11(void *)
{
    ghostEndX11();
}

void releaseAllX11()
{
    Display *d = display();
    if (!d) return;
    if (g_fakeKey)
        for (int kc = 0; kc < 256; kc++)
            if (g_held.key[kc]) {
                g_fakeKey(d, (unsigned)kc, False, CurrentTime);
                g_held.key[kc] = false;
            }
    if (g_fakeButton)
        for (int b = 0; b < 16; b++)
            if (g_held.btn[b]) {
                g_fakeButton(d, (unsigned)b, False, CurrentTime);
                g_held.btn[b] = false;
            }
    g_held.wheel_v = 0;
    g_held.wheel_h = 0;
    XFlush(d);
}

} /* namespace */

const KvmTargetOps &kvmTargetOps()
{
    static const KvmTargetOps ops = {
        initX11,
        metricsX11,
        0,                 /* no Windows drag-icon/image frames */
        injectMoveX11,
        injectButtonX11,
        injectWheelX11,
        injectKeyX11,
        releaseAllX11,
        dragEventX11,
        nullptr,           /* askConsent: receiver always supplies a hook */
        nullptr,           /* sessionBegin */
        sessionEndX11
    };
    return ops;
}

#endif /* __linux__ */
