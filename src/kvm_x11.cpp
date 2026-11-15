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
 * kvm_x11_ctl.cpp (XInput2 raw events + grabs). It also hosts the reverse
 * drag strip: when the controller accepts reverse copies, a 3 px XdndAware
 * window on the edge facing the controller captures a local file drag and
 * sends it to the controller's 8087 listener. The drag card/real image is
 * drawn by the shared kvm_ghost_x11 class.
 */
#include "kvm_core.h"
#include "kvm_ghost_x11.h"
#include "kvm_target.h"

#ifdef __linux__

#include <X11/XF86keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/select.h>
#include <unistd.h>

namespace {

Display *g_dpy = nullptr;
KvmGhostX11 g_ghost;

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
    if (!g_dpy) {
        g_dpy = XOpenDisplay(nullptr);
        if (g_dpy) g_ghost.attach(g_dpy);
    }
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
    Display *d = display();
    if (!d || !g_fakeMotion) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= w) x = (int)w - 1;
    if ((uint32_t)y >= h) y = (int)h - 1;
    g_fakeMotion(d, DefaultScreen(d), x, y, CurrentTime);
    g_ghost.move(x + sx, y + sy);
    g_ghost.pump();
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
 * The shared KvmGhostX11 renders the source's real premultiplied BGRA drag
 * image when a compositor and a 32-bit visual are available, otherwise a
 * small opaque card with the item name/icon. Frame sequences are accumulated
 * here; drawing never blocks the input loop. */
struct DragGhostX11 {
    int count = 0;
    int text_left = 0;
    std::string name;
    int icon_w = 0, icon_h = 0, icon_left = 0;
    std::vector<uint8_t> icon;
    int img_w = 0, img_h = 0, img_ox = 0, img_oy = 0, img_left = 0;
    bool img_premult = true;
    std::vector<uint8_t> image;
};
DragGhostX11 g_drag;

void dragGhostReset()
{
    g_drag = DragGhostX11{};
}

void unpackDimOff(uint32_t v, int &dim, int &off)
{
    dim = (int)(v & 0xFFFFu);
    off = (int)(int16_t)(v >> 16);
}

void appendGhostPixels(const KvmEvent &e, std::vector<uint8_t> &out)
{
    char chunk[12];
    std::memcpy(chunk, &e.a, 4);
    std::memcpy(chunk + 4, &e.b, 4);
    std::memcpy(chunk + 8, &e.c, 4);
    out.insert(out.end(), chunk, chunk + 12);
}

void dragEventX11(const KvmEvent &e)
{
    g_ghost.pump();
    switch (e.type) {
    case KVM_EV_DRAG_INFO:
        if (e.a) {
            dragGhostReset();
            g_drag.count = (int)e.b;
            g_drag.text_left = (int)e.c;
            g_ghost.begin(g_drag.count);
            if (g_drag.text_left <= 0) g_ghost.setText(std::string());
        } else {
            g_ghost.end();
            dragGhostReset();
        }
        break;
    case KVM_EV_DRAG_TEXT:
        if (g_drag.text_left > 0) {
            char chunk[12];
            std::memcpy(chunk, &e.a, 4);
            std::memcpy(chunk + 4, &e.b, 4);
            std::memcpy(chunk + 8, &e.c, 4);
            g_drag.name.append(chunk, 12);
            if (--g_drag.text_left == 0) {
                while (!g_drag.name.empty() && g_drag.name.back() == '\0')
                    g_drag.name.pop_back();
                g_ghost.setText(g_drag.name);
            }
        }
        break;
    case KVM_EV_DRAG_ICON_INFO: {
        const int w = (int)e.a, h = (int)e.b;
        g_drag.icon_w = w;
        g_drag.icon_h = h;
        g_drag.icon_left = (int)e.c;
        g_drag.icon.clear();
        if (w > 0 && h > 0 && w <= 2048 && h <= 2048 &&
            (uint64_t)w * (uint64_t)h <= 36864u)
            g_drag.icon.reserve((size_t)w * (size_t)h * 4);
        if (g_drag.icon_left == 0 &&
            g_drag.icon.size() == (size_t)w * (size_t)h * 4)
            g_ghost.setIcon(w, h, g_drag.icon);
        break;
    }
    case KVM_EV_DRAG_ICON_DATA:
        if (g_drag.icon_left > 0) {
            appendGhostPixels(e, g_drag.icon);
            if (--g_drag.icon_left == 0)
                g_ghost.setIcon(g_drag.icon_w, g_drag.icon_h, g_drag.icon);
        }
        break;
    case KVM_EV_DRAG_IMAGE_INFO: {
        int w = 0, h = 0, ox = 0, oy = 0;
        unpackDimOff(e.a, w, ox);
        unpackDimOff(e.b, h, oy);
        g_drag.img_w = w;
        g_drag.img_h = h;
        g_drag.img_ox = ox;
        g_drag.img_oy = oy;
        g_drag.img_left = (int)e.c;
        g_drag.img_premult = true;
        g_drag.image.clear();
        if (w > 0 && h > 0 && w <= 2048 && h <= 2048 &&
            (uint64_t)w * (uint64_t)h <= 36864u)
            g_drag.image.reserve((size_t)w * (size_t)h * 4);
        if (g_drag.img_left == 0 &&
            g_drag.image.size() == (size_t)w * (size_t)h * 4)
            g_ghost.setImage(w, h, ox, oy, g_drag.image, true);
        break;
    }
    case KVM_EV_DRAG_IMAGE_DATA:
        if (g_drag.img_left > 0) {
            appendGhostPixels(e, g_drag.image);
            if (--g_drag.img_left == 0)
                g_ghost.setImage(g_drag.img_w, g_drag.img_h, g_drag.img_ox,
                                 g_drag.img_oy, g_drag.image,
                                 g_drag.img_premult);
        }
        break;
    default:
        break;
    }
}

/* ── reverse drag: XDND strip on the edge facing the controller ────────────
 * The user controls this (target) desktop; a drag started here and carried to
 * the edge facing the controller becomes a reverse copy. The strip is a 3 px
 * InputOnly XdndAware window. On a drop it starts dropSendFiles() with
 * DROP_FLAG_REVERSE against the controller's 8087 listener, using the session
 * token. All KVM frames are written under targetLoop's tx mutex. */
struct ReverseX11 {
    std::atomic<bool> run{false};
    std::thread strip_thread;
    std::thread worker;
    std::atomic<bool> worker_busy{false};
    kvm_sock_t client = KVM_INVALID_SOCK;
    std::mutex *tx = nullptr;
    std::string peer_ip;
    uint16_t rev_port = (uint16_t)DROP_REVERSE_PORT;
    uint8_t token[DROP_TOKEN_BYTES] = {};
    int strip_side = KVM_SIDE_LEFT;
    uint32_t sw = 1, sh = 1;

    /* Written only by the strip thread. */
    Display *dpy = nullptr;
    Window win = 0;
    Window source = 0;
    bool uri_ok = false;
    bool waiting = false;
    bool early = false;
    bool announced = false;
    bool rdrag_on = false;
    int count = 0;
    std::string name;
    std::vector<std::string> paths;
    Atom A_aware = 0, A_enter = 0, A_pos = 0, A_status = 0, A_leave = 0,
         A_drop = 0, A_finished = 0, A_sel = 0, A_types = 0, A_action = 0,
         A_uri = 0, A_prop = 0;
};

std::string revPercentDecode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool revUrisToPaths(const std::string &data, std::vector<std::string> &out)
{
    size_t pos = 0;
    while (pos <= data.size()) {
        const size_t nl = data.find('\n', pos);
        std::string line = data.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = nl == std::string::npos ? data.size() + 1 : nl + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("file://", 0) != 0) continue;
        std::string rest = line.substr(7);
        if (rest.rfind("localhost/", 0) == 0) rest = rest.substr(9);
        else if (!rest.empty() && rest[0] != '/') continue;
        const std::string path = revPercentDecode(rest);
        if (path.empty() || path[0] != '/' ||
            path.find('\0') != std::string::npos)
            continue;
        out.push_back(path);
    }
    return !out.empty();
}

std::string revBaseName(const std::string &p)
{
    size_t end = p.size();
    while (end > 0 && p[end - 1] == '/') end--;
    if (end == 0) return std::string();
    const size_t b = p.find_last_of('/', end - 1);
    return p.substr(b == std::string::npos ? 0 : b + 1,
                    end - (b == std::string::npos ? 0 : b + 1));
}

bool revSend(ReverseX11 *st, uint32_t type, uint32_t a, uint32_t b,
             uint32_t c)
{
    if (!st->tx || st->client == KVM_INVALID_SOCK) return false;
    KvmEvent e = {};
    e.type = htonl(type);
    e.a = htonl(a);
    e.b = htonl(b);
    e.c = htonl(c);
    std::lock_guard<std::mutex> lk(*st->tx);
    return kvmSendAll(st->client, &e, sizeof(e));
}

bool revSendBytes(ReverseX11 *st, uint32_t type, const unsigned char *p)
{
    uint32_t w[3] = {};
    std::memcpy(w, p, 12);
    return revSend(st, type, w[0], w[1], w[2]);
}

void revRdrag(ReverseX11 *st, bool on)
{
    if (on == st->rdrag_on) return;
    if (revSend(st, KVM_EV_RDRAG, on ? 1u : 0u, 0, 0))
        st->rdrag_on = on;
}

void revAnnounce(ReverseX11 *st)
{
    if (st->announced || st->paths.empty()) return;
    st->announced = true;
    st->count = (int)st->paths.size();
    st->name = revBaseName(st->paths.front());
    if (!st->rdrag_on) revRdrag(st, true);
    if (st->name.empty()) return;
    revSend(st, KVM_EV_DRAG_INFO, 1, (uint32_t)st->count, 4);
    for (int i = 0; i < 4; i++) {
        unsigned char chunk[12] = {};
        const size_t off = (size_t)i * 12;
        if (off < st->name.size())
            std::memcpy(chunk, st->name.data() + off,
                        std::min<size_t>(12, st->name.size() - off));
        revSendBytes(st, KVM_EV_DRAG_TEXT, chunk);
    }
}

void revReset(ReverseX11 *st)
{
    st->source = 0;
    st->uri_ok = false;
    st->waiting = false;
    st->early = false;
    st->announced = false;
    st->count = 0;
    st->name.clear();
    st->paths.clear();
}

void revStartTransfer(ReverseX11 *st)
{
    if (st->paths.empty()) return;
    if (st->worker_busy.exchange(true)) return;
    if (st->worker.joinable()) st->worker.join();
    std::vector<std::string> paths = st->paths;
    st->worker = std::thread([st, paths] {
        DropOptions opt;
        opt.peer_ip = st->peer_ip.c_str();
        opt.port = st->rev_port;
        opt.flags = DROP_FLAG_REVERSE;
        opt.transfer_id = (uint32_t)(kvmNowMs() ^ (uint64_t)getpid());
        const DropResult r = dropSendFiles(opt, paths, st->token, st->run);
        std::cout << "[kvm] reverse drop to " << st->peer_ip << ":"
                  << st->rev_port << " status=" << r.status
                  << " committed=" << r.committed
                  << " failed=" << r.failed
                  << (r.message.empty() ? "" : " msg=" + r.message)
                  << "\n" << std::flush;
        st->worker_busy.store(false);
    });
}

void revSendXdnd(ReverseX11 *st, Window dst, Atom type, long l0, long l1,
                 long l2, long l3, long l4)
{
    XEvent ev = {};
    ev.xclient.type = ClientMessage;
    ev.xclient.display = st->dpy;
    ev.xclient.window = dst;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = l0;
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    ev.xclient.data.l[3] = l3;
    ev.xclient.data.l[4] = l4;
    XSendEvent(st->dpy, dst, False, NoEventMask, &ev);
    XFlush(st->dpy);
}

void revRequestUris(ReverseX11 *st, Time when)
{
    XConvertSelection(st->dpy, st->A_sel, st->A_uri, st->A_prop, st->win,
                      when);
    XFlush(st->dpy);
}

void revSelectionNotify(ReverseX11 *st, XSelectionEvent &e)
{
    if (!st->win || e.requestor != st->win) return;
    Atom actual = 0;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *prop = nullptr;
    if (e.property == None ||
        XGetWindowProperty(st->dpy, st->win, e.property, 0, 1 << 20, False,
                           AnyPropertyType, &actual, &fmt, &n, &rem,
                           &prop) != Success ||
        !prop) {
        if (st->waiting) {
            if (st->source) revSendXdnd(st, st->source, st->A_finished,
                                        (long)st->win, 0, 0, 0, 0);
            revRdrag(st, false);
            revReset(st);
        }
        return;
    }
    std::string data;
    if (fmt == 8) data.assign((const char *)prop, n);
    else if (fmt == 32) data.assign((const char *)prop, n * 4);
    XFree(prop);
    XDeleteProperty(st->dpy, st->win, e.property);

    std::vector<std::string> paths;
    if (!revUrisToPaths(data, paths)) {
        if (st->waiting) {
            if (st->source) revSendXdnd(st, st->source, st->A_finished,
                                        (long)st->win, 0, 0, 0, 0);
            revRdrag(st, false);
            revReset(st);
        }
        return;
    }
    st->paths = paths;
    revAnnounce(st);
    if (st->waiting) {
        if (st->source) revSendXdnd(st, st->source, st->A_finished,
                                    (long)st->win, 1,
                                    (long)st->A_action, 0, 0);
        revStartTransfer(st);
        revRdrag(st, false);
        revReset(st);
    }
}

void revClientMessage(ReverseX11 *st, XClientMessageEvent &c)
{
    if (!st->win || c.window != st->win) return;
    if (c.message_type == st->A_enter) {
        st->source = (Window)c.data.l[0];
        st->paths.clear();
        st->count = 0;
        st->name.clear();
        st->announced = false;
        st->waiting = false;
        st->early = false;
        bool uri = false;
        if (c.data.l[1] & 1) {
            Atom actual = 0;
            int fmt = 0;
            unsigned long n = 0, rem = 0;
            unsigned char *prop = nullptr;
            if (XGetWindowProperty(st->dpy, st->source, st->A_types, 0, 64,
                                   False, XA_ATOM, &actual, &fmt, &n, &rem,
                                   &prop) == Success && prop) {
                const Atom *types = (const Atom *)prop;
                for (unsigned long i = 0; i < n; i++)
                    if (types[i] == st->A_uri) uri = true;
                XFree(prop);
            }
        } else {
            for (int i = 2; i <= 4; i++)
                if ((Atom)c.data.l[i] == st->A_uri) uri = true;
        }
        st->uri_ok = uri;
        if (uri) {
            revRdrag(st, true);
            if (!st->early) {
                st->early = true;
                revRequestUris(st, CurrentTime);
            }
        }
        return;
    }
    if (c.message_type == st->A_pos) {
        if ((Window)c.data.l[0] != st->source) return;
        revSendXdnd(st, st->source, st->A_status, (long)st->win,
                    st->uri_ok ? 1 : 0, 0, 0,
                    st->uri_ok ? (long)st->A_action : 0);
        if (st->uri_ok && !st->announced && !st->paths.empty())
            revAnnounce(st);
        return;
    }
    if (c.message_type == st->A_leave) {
        revRdrag(st, false);
        revReset(st);
        return;
    }
    if (c.message_type == st->A_drop) {
        if (!st->uri_ok || !st->source) {
            if (st->source)
                revSendXdnd(st, st->source, st->A_finished, (long)st->win, 0,
                            0, 0, 0);
            revRdrag(st, false);
            revReset(st);
            return;
        }
        st->waiting = true;
        if (!st->paths.empty()) {
            revSendXdnd(st, st->source, st->A_finished, (long)st->win, 1,
                        (long)st->A_action, 0, 0);
            revStartTransfer(st);
            revRdrag(st, false);
            revReset(st);
        } else {
            revRequestUris(st, (Time)c.data.l[2]);
        }
        return;
    }
}

void reverseStripThread(ReverseX11 *st)
{
    Display *d = XOpenDisplay(nullptr);
    if (!d) return;
    st->dpy = d;
    const int scr = DefaultScreen(d);
    const Window root = RootWindow(d, scr);
    st->A_aware = XInternAtom(d, "XdndAware", False);
    st->A_enter = XInternAtom(d, "XdndEnter", False);
    st->A_pos = XInternAtom(d, "XdndPosition", False);
    st->A_status = XInternAtom(d, "XdndStatus", False);
    st->A_leave = XInternAtom(d, "XdndLeave", False);
    st->A_drop = XInternAtom(d, "XdndDrop", False);
    st->A_finished = XInternAtom(d, "XdndFinished", False);
    st->A_sel = XInternAtom(d, "XdndSelection", False);
    st->A_types = XInternAtom(d, "XdndTypeList", False);
    st->A_action = XInternAtom(d, "XdndActionCopy", False);
    st->A_uri = XInternAtom(d, "text/uri-list", False);
    st->A_prop = XInternAtom(d, "MivaraRevData", False);

    int x = 0, y = 0, w = 3, h = 3;
    const int sw = (int)(st->sw ? st->sw : (uint32_t)DisplayWidth(d, scr));
    const int sh = (int)(st->sh ? st->sh : (uint32_t)DisplayHeight(d, scr));
    switch (st->strip_side) {
    case KVM_SIDE_LEFT:   x = 0;      y = 0;      w = 3; h = sh; break;
    case KVM_SIDE_TOP:    x = 0;      y = 0;      w = sw; h = 3; break;
    case KVM_SIDE_BOTTOM: x = 0;      y = sh - 3; w = sw; h = 3; break;
    default:              x = sw - 3; y = 0;      w = 3; h = sh; break;
    }
    XSetWindowAttributes a = {};
    a.override_redirect = True;
    st->win = XCreateWindow(d, root, x, y, (unsigned)w, (unsigned)h, 0, 0,
                            InputOnly, CopyFromParent, CWOverrideRedirect, &a);
    if (!st->win) {
        XCloseDisplay(d);
        st->dpy = nullptr;
        return;
    }
    long version = 5;
    XChangeProperty(d, st->win, st->A_aware, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&version, 1);
    XMapRaised(d, st->win);
    XFlush(d);

    while (st->run.load()) {
        const int fd = ConnectionNumber(d);
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv = {0, 100000};
        const int sel = select(fd + 1, &rd, nullptr, nullptr, &tv);
        if (sel < 0 && errno != EINTR) break;
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type == ClientMessage) revClientMessage(st, ev.xclient);
            else if (ev.type == SelectionNotify)
                revSelectionNotify(st, ev.xselection);
        }
    }
    revRdrag(st, false);
    if (st->win) XDestroyWindow(d, st->win);
    XFlush(d);
    XCloseDisplay(d);
    st->dpy = nullptr;
    st->win = 0;
}

void *reverseSessionBegin(const KvmTargetGrant &grant,
                          const std::string &peer_ip, kvm_sock_t client,
                          const KvmTargetAuth *, std::atomic<bool> &running,
                          std::mutex &tx)
{
    (void)running;
    if (!grant.reverse_ok) return nullptr;
    auto *st = new ReverseX11();
    st->run.store(true);
    st->client = client;
    st->tx = &tx;
    st->peer_ip = peer_ip;
    st->rev_port = (grant.rev_port >= 1 && grant.rev_port <= 65535)
                       ? (uint16_t)grant.rev_port
                       : (uint16_t)DROP_REVERSE_PORT;
    std::memcpy(st->token, grant.drop_token, sizeof(st->token));
    st->sw = grant.sw;
    st->sh = grant.sh;
    switch (grant.side) {
    case KVM_SIDE_LEFT:   st->strip_side = KVM_SIDE_RIGHT; break;
    case KVM_SIDE_RIGHT:  st->strip_side = KVM_SIDE_LEFT; break;
    case KVM_SIDE_TOP:    st->strip_side = KVM_SIDE_BOTTOM; break;
    default:              st->strip_side = KVM_SIDE_TOP; break;
    }
    st->strip_thread = std::thread(reverseStripThread, st);
    return st;
}

void reverseSessionEnd(void *p)
{
    if (!p) return;
    auto *st = (ReverseX11 *)p;
    st->run.store(false);
    if (st->strip_thread.joinable()) st->strip_thread.join();
    if (st->worker.joinable()) st->worker.join();
    mivaraSecureZero(st->token, sizeof(st->token));
    delete st;
}

void sessionEndX11(void *state)
{
    reverseSessionEnd(state);
    g_ghost.end();
    dragGhostReset();
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
        KVM_FEATURE_DRAG_ICON | KVM_FEATURE_DRAG_IMAGE,
        injectMoveX11,
        injectButtonX11,
        injectWheelX11,
        injectKeyX11,
        releaseAllX11,
        dragEventX11,
        nullptr,           /* askConsent: receiver always supplies a hook */
        reverseSessionBegin,
        sessionEndX11
    };
    return ops;
}

#endif /* __linux__ */
