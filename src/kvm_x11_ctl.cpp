/**
 * KVM_X11_CTL.CPP – Linux/X11 stitch controller (控制端).
 *
 * Windows uses low-level hooks + Raw Input; on X11 the equivalent is:
 *
 *   - XInput2 raw events (XI_RawMotion / XI_RawButton* / XI_RawKey*) for
 *     global, grab-independent input capture and relative motion deltas;
 *   - XGrabKeyboard in Remote to swallow keys (its regular KeyPress events
 *     are the key source; raw key events are suppressed by the grab) and
 *     passive XGrabButton for buttons 1..9. An active XGrabPointer would also
 *     suppress raw motion, so it is deliberately not used;
 *   - XQueryPointer / DisplayWidth for the local virtual desktop;
 *   - XFixes to hide the local cursor in Remote;
 *   - XTest only to release keys/buttons that reached the local desktop but
 *     were released while Remote (avoiding a stale local modifier/button).
 *
 * Edge crossing, side semantics, held-input re-arming, the 250 ms re-entry
 * guard and the STAT/state machine match the Windows controller in kvm.cpp.
 * File drop: forward drags are captured by an XDND strip on the shared edge;
 * reverse drags run a 8087 listener and are carried back with an X11 ghost.
 */
#include "kvm.h"
#include "kvm_core.h"
#include "kvm_event_queue.h"
#include "drop.h"
#include "kvm_ghost_x11.h"
#include "proto.h"

#ifdef __linux__

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>
#include <X11/keysym.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include <netinet/tcp.h>
#include <sys/select.h>

namespace {

/* ── shared state ────────────────────────────────────────────────────────── */

enum class ControlState { Local, Remote };

enum ControllerLoss {
    LOSS_NONE = 0,
    LOSS_PEER_CLOSED,
    LOSS_RECV_ERROR,
    LOSS_SELECT_ERROR,
    LOSS_HEARTBEAT,
    LOSS_SEND_ERROR,
    LOSS_PROTOCOL_EVENT
};

const char *lossToken(int code)
{
    switch (code) {
    case LOSS_PEER_CLOSED:    return "peer_closed";
    case LOSS_RECV_ERROR:     return "recv_error";
    case LOSS_SELECT_ERROR:   return "select_error";
    case LOSS_HEARTBEAT:      return "heartbeat_timeout";
    case LOSS_SEND_ERROR:     return "send_error";
    case LOSS_PROTOCOL_EVENT: return "protocol_event";
    default:                  return "unknown";
    }
}

KvmEventQueue g_queue;
std::atomic<bool> g_netOK{true};
std::atomic<bool> g_drainQuit{false};
std::atomic<int>  g_loss_code{LOSS_NONE};
std::atomic<int>  g_loss_err{0};
uint64_t          g_ctrl_session_start = 0;
std::atomic<int>  g_side_req{-1};
int               g_side = KVM_SIDE_RIGHT;

/* ── file drop (8085 + XDND on the shared edge) ─────────────────────────── */
bool        g_drop_enabled = false;
bool        g_drop_ready = false;
uint8_t     g_drop_token[DROP_TOKEN_BYTES] = {};
std::string g_peer_ip;
std::atomic<bool> g_drop_run{false};
std::thread g_drop_thread;

Window g_drop_strip = 0;
Atom   A_XdndAware = 0, A_XdndEnter = 0, A_XdndPosition = 0;
Atom   A_XdndStatus = 0, A_XdndLeave = 0, A_XdndDrop = 0;
Atom   A_XdndFinished = 0, A_XdndSelection = 0, A_XdndTypeList = 0;
Atom   A_XdndActionCopy = 0, A_text_uri_list = 0, A_drop_prop = 0;

Window g_drag_source = 0;
bool   g_file_drag = false;
bool   g_drag_uri_ok = false;
bool   g_drag_ghost_sent = false;
bool   g_drop_waiting = false;
bool   g_early_requested = false;
int    g_drag_count = 0;
int    g_drop_root_x = 0, g_drop_root_y = 0;
std::string g_drag_name;
std::vector<std::string> g_drag_paths;

void dropStripSetSide(int side);
void dropResetDrag();

/* ── reverse drag (target -> controller) ────────────────────────────────── */
bool g_reverse_enabled = false;
bool g_reverse_ok = false;
bool g_drag_icon_ok = false;
bool g_drag_image_ok = false;
std::atomic<bool> g_reverse_service_run{false};
std::thread g_reverse_service;
std::atomic<bool> g_reverse_drag{false};
std::atomic<bool> g_rev_cancel_req{false};
bool g_rev_held = false;          /* target's button kept down (main thread) */
bool g_rev_pointer_grab = false;  /* active XGrabPointer during the carry */
bool g_rev_visible = false;       /* ghost currently mapped (main thread) */
uint64_t g_rev_applied = ~0ull;
int g_rev_last_count = -1;
KvmGhostX11 g_rev_ghost;

struct ReverseDragState {
    std::mutex m;
    bool active = false;
    int count = 0;
    bool name_ready = false;
    std::string name;
    bool have_icon = false;
    int icon_w = 0, icon_h = 0;
    std::vector<uint8_t> icon;
    bool have_image = false;
    int img_w = 0, img_h = 0, img_ox = 0, img_oy = 0;
    std::vector<uint8_t> image;
    uint64_t version = 0;
    /* Accumulators are touched only by the drain thread while m is held. */
    int text_left = 0, icon_left = 0, image_left = 0;
    std::vector<uint8_t> icon_acc, image_acc;
};
ReverseDragState g_rev;

void revResetLocked()
{
    g_rev.active = false;
    g_rev.count = 0;
    g_rev.name_ready = false;
    g_rev.name.clear();
    g_rev.have_icon = false;
    g_rev.icon_w = g_rev.icon_h = 0;
    g_rev.icon.clear();
    g_rev.have_image = false;
    g_rev.img_w = g_rev.img_h = 0;
    g_rev.img_ox = g_rev.img_oy = 0;
    g_rev.image.clear();
    g_rev.text_left = g_rev.icon_left = g_rev.image_left = 0;
    g_rev.icon_acc.clear();
    g_rev.image_acc.clear();
    ++g_rev.version;
}

void revReset()
{
    std::lock_guard<std::mutex> lk(g_rev.m);
    revResetLocked();
}

void revAppendLocked(const KvmEvent &e, std::vector<uint8_t> &out)
{
    char chunk[12];
    std::memcpy(chunk, &e.a, 4);
    std::memcpy(chunk + 4, &e.b, 4);
    std::memcpy(chunk + 8, &e.c, 4);
    out.insert(out.end(), chunk, chunk + 12);
}

void revDimOff(uint32_t v, int &dim, int &off)
{
    dim = (int)(v & 0xFFFFu);
    off = (int)(int16_t)(v >> 16);
}

void revAppendNameLocked(const KvmEvent &e)
{
    char chunk[12];
    std::memcpy(chunk, &e.a, 4);
    std::memcpy(chunk + 4, &e.b, 4);
    std::memcpy(chunk + 8, &e.c, 4);
    g_rev.name.append(chunk, 12);
}

int g_wake[2] = {-1, -1};

/* ── X state ─────────────────────────────────────────────────────────────── */

Display *g_dpy = nullptr;
int      g_xi_op = -1;
bool     g_xfixes = false;
int      g_screen_w = 1, g_screen_h = 1;

uint32_t g_tgt_w = 1, g_tgt_h = 1;
int      g_tgt_x0 = 0, g_tgt_y0 = 0;

ControlState g_state = ControlState::Local;
int      g_remote_x = 0, g_remote_y = 0;
/* While a button is down, X11 can suppress XI2 raw motion (the server routes
 * pointer motion to the implicit grab). Poll the local pointer instead so a
 * held drag still moves the remote cursor; raw motion resumes on release. */
bool     g_motion_poll = false;
int      g_poll_x = 0, g_poll_y = 0;
int      g_edge_overshoot = 0;
uint64_t g_leave_tick = 0;
uint64_t g_grab_fail_tick = 0;
bool     g_cursor_hidden = false;

bool g_key_down[256] = {};
bool g_key_local[256] = {};   /* the local desktop saw this press */
bool g_btn_down[8] = {};
bool g_btn_local[8] = {};

bool g_ctrl = false, g_alt = false, g_shift = false;
int  g_ctrl_l = 0, g_ctrl_r = 0;
int  g_alt_l = 0, g_alt_r = 0;
int  g_shift_l = 0, g_shift_r = 0;
int  g_q_key = 0;
int  g_panic_key = -1;

double g_motion_acc_x = 0.0;
double g_motion_acc_y = 0.0;

/* Some pointing devices report absolute raw valuators (tablets, RDP/VM
 * pointer forwarding, nested X servers). Their position is scaled to screen
 * coordinates and differentiated against the previous sample; a large jump is
 * treated as a re-anchor rather than a motion. Relative mice are passed
 * through unchanged. */
struct DevMode {
    int    id = 0;
    bool   absolute = false;
    double min_x = 0, max_x = 0;
    double min_y = 0, max_y = 0;
    bool   last_valid = false;
    double last_x = 0, last_y = 0;
};
DevMode g_dev_mode[64] = {};
int g_dev_count = 0;
/* Absolute devices cannot cross an edge (once the pointer is clamped there is
 * no further outward delta), so they are ignored unless this diagnostic is
 * explicitly enabled (MIVARA_KVM_ALLOW_ABS=1, used by the Xephyr test rig). */
bool g_allow_abs = false;
bool g_force_abs_motion = false;
bool g_trace = false;
bool g_root_input_ok = true;
bool g_x_error = false;

int kvmXErrorHandler(Display *display, XErrorEvent *e)
{
    (void)display;
    g_x_error = true;
    if (g_trace)
        std::cerr << "[kvm-trace] X error code=" << (int)e->error_code
                  << " request=" << (int)e->request_code
                  << " resource=" << (unsigned long)e->resourceid << "\n";
    return 0;
}

DevMode *deviceMode(int id)
{
    for (int i = 0; i < g_dev_count; i++)
        if (g_dev_mode[i].id == id) return &g_dev_mode[i];

    DevMode dm;
    dm.id = id;
    int ndev = 0;
    XIDeviceInfo *info = XIQueryDevice(g_dpy, id, &ndev);
    if (info && ndev > 0) {
        for (int i = 0; i < info->num_classes; i++) {
            if (info->classes[i]->type != XIValuatorClass) continue;
            auto *vc = (XIValuatorClassInfo *)info->classes[i];
            if (vc->mode != XIModeAbsolute) continue;
            if (vc->number == 0) {
                dm.absolute = true;
                dm.min_x = vc->min;
                dm.max_x = vc->max;
            } else if (vc->number == 1) {
                dm.absolute = true;
                dm.min_y = vc->min;
                dm.max_y = vc->max;
            }
        }
    }
    if (info) XIFreeDeviceInfo(info);
    if (g_dev_count < (int)(sizeof(g_dev_mode) / sizeof(g_dev_mode[0])))
        g_dev_mode[g_dev_count++] = dm;
    return g_dev_count ? &g_dev_mode[g_dev_count - 1] : nullptr;
}

static const int WHEEL_STEP = 120;

typedef int (*XTestFakeKeyEventFn)(Display *, unsigned int, Bool,
                                   unsigned long);
typedef int (*XTestFakeButtonEventFn)(Display *, unsigned int, Bool,
                                      unsigned long);
XTestFakeKeyEventFn    g_xtest_key = nullptr;
XTestFakeButtonEventFn g_xtest_button = nullptr;

/* ── stdout / STAT ───────────────────────────────────────────────────────── */

std::mutex g_stat_mtx;

void emitStat(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_stat_mtx);
    std::cout << "STAT|" << line << "\n" << std::flush;
}

void emitStatError(const std::string &msg)
{
    emitStat("event=error|msg=" + msg);
}

void emitLog(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_stat_mtx);
    std::cout << "[kvm] " << line << "\n" << std::flush;
}

/* state=…|side=… — the GUI renders the local/remote hint and the active
 * direction chip from this. */
void emitStitchState(const char *state)
{
    emitStat(std::string("event=stitch|state=") + state +
             "|mode=stitch|side=" + kvmSideKey(g_side));
}

/* ── queue helpers (single producer: the X event loop) ───────────────────── */

void wakeDrain()
{
    if (g_wake[1] < 0) return;
    const char b = 1;
    const ssize_t n = write(g_wake[1], &b, 1);
    (void)n;   /* a full pipe means the drain thread is already awake */
}

void pushEvent(const KvmEvent &e)
{
    if (g_queue.push(e)) wakeDrain();
}

void pushMove(uint32_t x, uint32_t y)
{
    if (g_queue.pushMove(x, y)) wakeDrain();
}

void queueKey(bool down, uint32_t vk, uint32_t scan, bool ext)
{
    KvmEvent e = {};
    e.type = KVM_EV_KEY;
    e.a = down ? 1u : 0u;
    e.b = vk;
    e.c = (scan & 0xFFFFu) | (ext ? 0x10000u : 0u);
    pushEvent(e);
}

void queueButton(uint32_t btn, bool down)
{
    KvmEvent e = {};
    e.type = KVM_EV_BUTTON;
    e.a = btn;
    e.b = down ? 1u : 0u;
    pushEvent(e);
}

void queueWheel(int delta, bool horizontal)
{
    KvmEvent e = {};
    e.type = KVM_EV_WHEEL;
    e.a = (uint32_t)delta;
    e.b = horizontal ? 1u : 0u;
    pushEvent(e);
}

void queueReleaseAll()
{
    KvmEvent e = {};
    e.type = KVM_EV_RELEASE_ALL;
    pushEvent(e);
}

/* ── key translation: X keycode -> Windows set-1 scan code ───────────────
 * X key codes are evdev + 8. The unextended set-1 block is numerically the
 * same as evdev, so most keys map directly; the E0-prefixed keys need the
 * reverse of the table in kvm_x11.cpp. VK is left 0: the target prefers the
 * scan code, which is layout-independent. */
struct ExtScan { int evdev; unsigned scan; };

const ExtScan kExtScan[] = {
    {165, 0x10}, {163, 0x19}, {96, 0x1C}, {97, 0x1D}, {113, 0x20},
    {164, 0x22}, {166, 0x24}, {114, 0x2E}, {115, 0x30}, {98, 0x35},
    {99, 0x37}, {100, 0x38}, {102, 0x47}, {103, 0x48}, {104, 0x49},
    {105, 0x4B}, {106, 0x4D}, {107, 0x4F}, {108, 0x50}, {109, 0x51},
    {110, 0x52}, {111, 0x53}, {125, 0x5B}, {126, 0x5C}, {127, 0x5D}
};

bool translateKey(int keycode, uint32_t &vk, uint32_t &scan, bool &ext)
{
    vk = 0;
    scan = 0;
    ext = false;
    const int evdev = keycode - 8;
    if (evdev < 1) return false;
    for (const ExtScan &m : kExtScan) {
        if (m.evdev == evdev) {
            scan = m.scan;
            ext = true;
            return true;
        }
    }
    if (evdev <= 0x7F) {
        scan = (uint32_t)evdev;
        return true;
    }
    return false;
}

/* ── local desktop / pointer ─────────────────────────────────────────────── */

void localMetrics(int &w, int &h)
{
    if (!g_dpy) {
        w = h = 1;
        return;
    }
    const int scr = DefaultScreen(g_dpy);
    w = DisplayWidth(g_dpy, scr);
    h = DisplayHeight(g_dpy, scr);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
}

bool pointerPos(int &x, int &y)
{
    if (!g_dpy) return false;
    Window root_ret = 0, child_ret = 0;
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned mask = 0;
    if (!XQueryPointer(g_dpy, DefaultRootWindow(g_dpy), &root_ret, &child_ret,
                       &root_x, &root_y, &win_x, &win_y, &mask))
        return false;
    x = root_x;
    y = root_y;
    return true;
}

int clampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── cursor visibility (XFixes) ──────────────────────────────────────────── */

void setCursorVisible(bool visible)
{
    if (g_cursor_hidden == !visible) return;
    if (!g_dpy) return;
    if (g_xfixes) {
        const Window root = DefaultRootWindow(g_dpy);
        if (visible) XFixesShowCursor(g_dpy, root);
        else XFixesHideCursor(g_dpy, root);
        XFlush(g_dpy);
    }
    g_cursor_hidden = !visible;
}

/* ── XTest: release keys/buttons that reached the LOCAL desktop ────────────
 * A release that happens while Remote is swallowed by the grab, so a local
 * application would otherwise keep the modifier/button state forever. On
 * leave we synthesize the matching local release after ungrabbing. */
void loadXtst()
{
    void *lib = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libXtst.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    g_xtest_key = (XTestFakeKeyEventFn)dlsym(lib, "XTestFakeKeyEvent");
    g_xtest_button =
        (XTestFakeButtonEventFn)dlsym(lib, "XTestFakeButtonEvent");
}

int xButtonFor(uint32_t btn)
{
    switch (btn) {
    case 1: return 1;
    case 2: return 3;
    case 3: return 2;
    case 4: return 8;
    case 5: return 9;
    default: return 0;
    }
}

void injectLocalReleases()
{
    if (!g_dpy) return;
    if (g_trace) {
        Window f = 0;
        int rev = 0;
        XGetInputFocus(g_dpy, &f, &rev);
        std::cerr << "[kvm-trace] local-release focus=" << (unsigned long)f
                  << " revert=" << rev << "\n";
    }
    bool did = false;
    if (g_xtest_key) {
        for (int kc = 0; kc < 256; kc++) {
            if (!g_key_local[kc]) continue;
            if (g_trace)
                std::cerr << "[kvm-trace] local release key kc=" << kc
                          << "\n";
            g_xtest_key(g_dpy, (unsigned)kc, False, CurrentTime);
            g_key_local[kc] = false;
            did = true;
        }
    }
    if (g_xtest_button) {
        for (int b = 1; b < 6; b++) {
            if (!g_btn_local[b]) continue;
            const int xb = xButtonFor((uint32_t)b);
            if (xb) {
                if (g_trace)
                    std::cerr << "[kvm-trace] local release button pb=" << b
                              << "\n";
                g_xtest_button(g_dpy, (unsigned)xb, False, CurrentTime);
                did = true;
            }
            g_btn_local[b] = false;
        }
    }
    if (did) XFlush(g_dpy);
}

/* ── grabs ───────────────────────────────────────────────────────────────── */

bool g_btn_grabbed[10] = {};
bool g_btn_grab_warned = false;

/* Keyboard: an active grab. Its regular KeyPress/KeyRelease events are the
 * Remote key source, because X suppresses raw key events while the grab is
 * active.
 *
 * Pointer: passive grabs for buttons 1..9, NOT XGrabPointer. An active pointer
 * grab also suppresses raw motion (and even MotionNotify) on Xorg, which
 * would freeze the remote cursor; passive button grabs leave raw motion
 * flowing while still swallowing clicks and wheel events. */
bool grabInput()
{
    if (!g_dpy) return false;
    const Window root = DefaultRootWindow(g_dpy);
    for (int attempt = 0; attempt < 5; attempt++) {
        const int kg = XGrabKeyboard(g_dpy, root, False, GrabModeAsync,
                                     GrabModeAsync, CurrentTime);
        if (kg == GrabSuccess) {
            int failed = 0;
            for (unsigned b = 1; b <= 9; b++) {
                const int g = XGrabButton(
                    g_dpy, b, AnyModifier, root, False,
                    ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                    GrabModeAsync, None, None);
                g_btn_grabbed[b] = (g == GrabSuccess);
                if (!g_btn_grabbed[b]) failed++;
            }
            if (failed && !g_btn_grab_warned) {
                g_btn_grab_warned = true;
                emitLog(std::to_string(failed) +
                        " mouse button grab(s) refused; those clicks stay "
                        "local");
            }
            XSync(g_dpy, False);
            return true;
        }
        XSync(g_dpy, False);
        kvmSleepMs(20);
    }
    return false;
}

void ungrabInput()
{
    if (!g_dpy) return;
    const Window root = DefaultRootWindow(g_dpy);
    for (unsigned b = 1; b <= 9; b++) {
        if (g_btn_grabbed[b])
            XUngrabButton(g_dpy, b, AnyModifier, root);
        g_btn_grabbed[b] = false;
    }
    XUngrabKeyboard(g_dpy, CurrentTime);
    XFlush(g_dpy);
}

/* While a reverse drag is carried back, keep receiving the physical button-up
 * even though the pointer is now over an arbitrary local window. An active
 * pointer grab routes it to our root selection; the local cursor still moves
 * and the ghost follows it. */
void grabReversePointer()
{
    if (!g_dpy) return;
    const int r = XGrabPointer(
        g_dpy, DefaultRootWindow(g_dpy), False,
        ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
        None, None, CurrentTime);
    g_rev_pointer_grab = (r == GrabSuccess);
    if (!g_rev_pointer_grab && g_trace)
        std::cerr << "[kvm-trace] reverse pointer grab failed " << r
                  << "\n";
    XFlush(g_dpy);
}

void ungrabReversePointer()
{
    if (g_dpy && g_rev_pointer_grab)
        XUngrabPointer(g_dpy, CurrentTime);
    g_rev_pointer_grab = false;
    if (g_dpy) XFlush(g_dpy);
}

/* ── state machine ───────────────────────────────────────────────────────── */

void startMotionPoll()
{
    int px = 0, py = 0;
    if (pointerPos(px, py)) {
        g_poll_x = px;
        g_poll_y = py;
    }
    g_motion_poll = true;
}

void stopMotionPoll()
{
    g_motion_poll = false;
}

void syncHeldInput()
{
    for (int kc = 8; kc < 256; kc++) {
        if (!g_key_down[kc]) continue;
        uint32_t vk = 0, scan = 0;
        bool ext = false;
        if (translateKey(kc, vk, scan, ext))
            queueKey(true, vk, scan, ext);
    }
    for (int b = 1; b < 6; b++) {
        if (b == 1 && g_file_drag) continue;
        if (g_btn_down[b]) queueButton((uint32_t)b, true);
    }
}

void hideReverseGhost()
{
    if (g_rev_visible) {
        g_rev_ghost.end();
        g_rev_visible = false;
    }
    g_rev_last_count = -1;
}

void cancelReverseDrag(bool sendReleaseAll)
{
    g_rev_held = false;
    g_reverse_drag.store(false);
    g_rev_cancel_req.store(false);
    revReset();
    hideReverseGhost();
    ungrabReversePointer();
    if (sendReleaseAll) queueReleaseAll();
}

void finishReverseDrag()
{
    if (g_rev_held) queueButton(1, false);
    g_rev_held = false;
    g_reverse_drag.store(false);
    revReset();
    hideReverseGhost();
    ungrabReversePointer();
    g_btn_local[1] = false;
}

void handleRevCancel()
{
    if (!g_rev_cancel_req.exchange(false)) return;
    if (g_rev_held) {
        queueReleaseAll();
        g_rev_held = false;
    }
    g_reverse_drag.store(false);
    revReset();
    hideReverseGhost();
}

/* Keeps the reverse-drag ghost on the local cursor after control has been
 * carried back, and drops it when the gesture ends. Runs on the Xlib loop. */
void syncReverseGhost()
{
    /* Fallback for a missed ButtonRelease (an active grab can fail): once the
     * server reports the local left button up, complete the target's drop. */
    if (g_rev_held && g_dpy) {
        Window root_ret = None, child = None;
        int rx = 0, ry = 0, wx = 0, wy = 0;
        unsigned mask = 0;
        if (XQueryPointer(g_dpy, DefaultRootWindow(g_dpy), &root_ret, &child,
                          &rx, &ry, &wx, &wy, &mask) &&
            !(mask & Button1Mask)) {
            finishReverseDrag();
            return;
        }
    }
    const bool armed = g_reverse_drag.load() && g_btn_down[1] &&
                       g_state == ControlState::Local;
    if (!armed) {
        hideReverseGhost();
        return;
    }
    if (!g_dpy) return;
    Window root_ret = None, child = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned mask = 0;
    if (!XQueryPointer(g_dpy, DefaultRootWindow(g_dpy), &root_ret, &child,
                       &rx, &ry, &wx, &wy, &mask))
        return;

    bool changed = false, ready = false;
    bool have_icon = false, have_image = false;
    int count = 0, iw = 0, ih = 0, imgw = 0, imgh = 0, ox = 0, oy = 0;
    std::string name;
    std::vector<uint8_t> icon, image;
    uint64_t ver = 0;
    {
        std::lock_guard<std::mutex> lk(g_rev.m);
        ver = g_rev.version;
        if (!g_rev_visible || ver != g_rev_applied) {
            ready = g_rev.name_ready;
            if (!ready) {
                if (g_rev_visible) hideReverseGhost();
                return;
            }
            count = g_rev.count;
            name = g_rev.name;
            have_icon = g_rev.have_icon;
            iw = g_rev.icon_w;
            ih = g_rev.icon_h;
            icon = g_rev.icon;
            have_image = g_rev.have_image;
            imgw = g_rev.img_w;
            imgh = g_rev.img_h;
            ox = g_rev.img_ox;
            oy = g_rev.img_oy;
            image = g_rev.image;
            changed = true;
        }
    }
    if (changed) {
        /* begin() clears a previous drag's image/icon, so a new drag never
         * keeps showing the old drag image. */
        g_rev_ghost.begin(count);
        g_rev_last_count = count;
        g_rev_ghost.setText(name);
        if (have_icon) g_rev_ghost.setIcon(iw, ih, icon);
        if (have_image)
            g_rev_ghost.setImage(imgw, imgh, ox, oy, image, true);
        g_rev_applied = ver;
        g_rev_visible = true;
    }
    g_rev_ghost.move(rx, ry);
    g_rev_ghost.pump();
}

void enterRemote(int local_x, int local_y)
{
    if (g_state == ControlState::Remote) return;
    if (!g_root_input_ok) return;
    if (kvmNowMs() - g_grab_fail_tick < 500) return;

    /* Any key/button the local desktop already saw as down must be released to
     * it BEFORE the grab. We cannot forward the physical release later (the
     * grab swallows it, and a fake release for an already-up key generates no
     * local event), so clear the local state now; syncHeldInput() re-arms the
     * physical state on the target immediately after. */
    injectLocalReleases();

    if (!grabInput()) {
        g_grab_fail_tick = kvmNowMs();
        emitLog("cannot grab the keyboard; staying local (another client "
                "holds an active grab?)");
        return;
    }

    int lw = 1, lh = 1;
    localMetrics(lw, lh);
    const int rx = (int)(((long long)clampInt(local_x, 0, lw - 1) *
                          (long long)g_tgt_w) / lw);
    const int ry = (int)(((long long)clampInt(local_y, 0, lh - 1) *
                          (long long)g_tgt_h) / lh);
    switch (g_side) {
    case KVM_SIDE_LEFT:   g_remote_x = (int)g_tgt_w - 1; g_remote_y = ry; break;
    case KVM_SIDE_TOP:    g_remote_x = rx; g_remote_y = (int)g_tgt_h - 1; break;
    case KVM_SIDE_BOTTOM: g_remote_x = rx; g_remote_y = 0; break;
    default:              g_remote_x = 0; g_remote_y = ry; break;
    }
    g_remote_x = clampInt(g_remote_x, 0, (int)g_tgt_w - 1);
    g_remote_y = clampInt(g_remote_y, 0, (int)g_tgt_h - 1);

    if (g_trace)
        std::cerr << "[kvm-trace] enterRemote remote=" << g_remote_x << ","
                  << g_remote_y << " side=" << g_side << "\n";
    g_state = ControlState::Remote;
    setCursorVisible(false);
    pushMove((uint32_t)g_remote_x, (uint32_t)g_remote_y);
    syncHeldInput();
    for (int b = 1; b < 6; b++)
        if (g_btn_down[b]) { startMotionPoll(); break; }
    emitStitchState("remote");
}

void leaveRemote(bool warpCursor, bool keepHeldInput = false)
{
    if (g_state != ControlState::Remote) return;
    if (g_trace)
        std::cerr << "[kvm-trace] leaveRemote warp=" << warpCursor
                  << " remote=" << g_remote_x << "," << g_remote_y << "\n";

    /* Mark Local before any XTest release so those synthetic events are not
     * forwarded back to the peer. */
    g_state = ControlState::Local;
    stopMotionPoll();
    if (keepHeldInput) {
        /* The target's drag is still live. Keep its injected button down until
         * the local release completes the drop. */
        g_rev_held = true;
    } else {
        g_rev_held = false;
        if (g_reverse_drag.exchange(false)) revReset();
        hideReverseGhost();
        queueReleaseAll();
    }
    ungrabInput();
    setCursorVisible(true);
    if (keepHeldInput) grabReversePointer();
    else ungrabReversePointer();

    if (warpCursor && g_dpy) {
        int lw = 1, lh = 1;
        localMetrics(lw, lh);
        int px = 0, py = 0;
        if (g_side == KVM_SIDE_LEFT || g_side == KVM_SIDE_RIGHT) {
            py = (int)(((long long)g_remote_y * (long long)lh) / g_tgt_h);
            py = clampInt(py, 0, lh - 1);
            px = (g_side == KVM_SIDE_RIGHT) ? lw - 1 : 0;
        } else {
            px = (int)(((long long)g_remote_x * (long long)lw) / g_tgt_w);
            px = clampInt(px, 0, lw - 1);
            py = (g_side == KVM_SIDE_BOTTOM) ? lh - 1 : 0;
        }
        XWarpPointer(g_dpy, None, DefaultRootWindow(g_dpy), 0, 0, 0, 0,
                     px, py);
        XFlush(g_dpy);
    }

    g_leave_tick = kvmNowMs();
    g_edge_overshoot = 0;
    emitStitchState("local");
}

void moveRemote(int dx, int dy)
{
    int nx = g_remote_x + dx;
    int ny = g_remote_y + dy;

    /* Distance past the facing edge (>0 = pushing back out). */
    int out = 0;
    switch (g_side) {
    case KVM_SIDE_LEFT:   out = nx - ((int)g_tgt_w - 1); break;
    case KVM_SIDE_TOP:    out = ny - ((int)g_tgt_h - 1); break;
    case KVM_SIDE_BOTTOM: out = -ny; break;
    default:              out = -nx; break;
    }
    if (out > 0) {
        /* A reverse drag crossing back CARRIES the files to this desktop:
         * return control, keep the target's button held (so its drag
         * survives) and let the local release choose the drop point. */
        if (g_reverse_drag.load() && g_btn_down[1])
            leaveRemote(false, /*keepHeldInput=*/true);
        else
            leaveRemote(true);
        return;
    }
    nx = clampInt(nx, 0, (int)g_tgt_w - 1);
    ny = clampInt(ny, 0, (int)g_tgt_h - 1);
    g_remote_x = nx;
    g_remote_y = ny;
    pushMove((uint32_t)nx, (uint32_t)ny);
}

void pollRemoteMotion()
{
    if (!g_motion_poll || g_state != ControlState::Remote) return;
    int px = 0, py = 0;
    if (!pointerPos(px, py)) return;
    const int dx = px - g_poll_x;
    const int dy = py - g_poll_y;
    if (!dx && !dy) return;
    g_poll_x = px;
    g_poll_y = py;
    moveRemote(dx, dy);
}

void applySide(int side)
{
    if (side < (int)KVM_SIDE_LEFT || side > (int)KVM_SIDE_BOTTOM ||
        side == g_side)
        return;
    if (g_rev_held || g_reverse_drag.load()) cancelReverseDrag(true);
    if (g_state == ControlState::Remote) leaveRemote(true);
    g_side = side;
    dropStripSetSide(g_side);
    g_edge_overshoot = 0;
    emitStitchState("local");
    emitLog(std::string("stitch side -> ") + kvmSideKey(g_side));
}

/* ── file drop: XDND capture on the shared edge ────────────────────────────
 * A 3-pixel InputOnly strip on the chosen edge advertises XdndAware. When the
 * user drags a local selection onto it, we take the text/uri-list, forward the
 * paths over 8085 with the token obtained from the 8084 handshake, and show a
 * small ghost on the peer. The strip never grabs input, so normal edge
 * crossing and the drag source's own pointer grab are unaffected. */

std::string dropStatSafe(const std::string &s)
{
    std::string out;
    out.reserve(std::min<size_t>(s.size(), 200));
    for (unsigned char c : s) {
        if (out.size() >= 200) break;
        out.push_back((c < 0x20 || c == 0x7f || c == '|') ? ' ' : (char)c);
    }
    return out;
}

std::string percentDecode(const std::string &s)
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

bool uriListToPaths(const std::string &data, std::vector<std::string> &out)
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
        else if (!rest.empty() && rest[0] != '/') continue;  /* remote host */
        std::string path = percentDecode(rest);
        if (path.empty() || path[0] != '/' ||
            path.find('\0') != std::string::npos)
            continue;
        out.push_back(path);
    }
    return !out.empty();
}

std::string pathBaseName(const std::string &p)
{
    size_t end = p.size();
    while (end > 0 && p[end - 1] == '/') end--;
    if (end == 0) return std::string();
    const size_t b = p.find_last_of('/', end - 1);
    return p.substr(b == std::string::npos ? 0 : b + 1,
                    end - (b == std::string::npos ? 0 : b + 1));
}

void dropSendGhost(bool begin)
{
    if (!g_drop_ready) return;
    KvmEvent info = {};
    info.type = KVM_EV_DRAG_INFO;
    info.a = begin ? 1u : 0u;
    info.b = (uint32_t)g_drag_count;
    info.c = begin && !g_drag_name.empty() ? 4u : 0u;
    pushEvent(info);
    if (!begin || g_drag_name.empty()) {
        if (!begin) g_drag_ghost_sent = false;
        return;
    }
    std::string text = g_drag_name;
    size_t cut = std::min<size_t>(text.size(), 4 * 12);
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--;
    text.resize(cut);
    for (int i = 0; i < 4; i++) {
        char chunk[12] = {};
        const size_t off = (size_t)i * 12;
        if (off < text.size())
            std::memcpy(chunk, text.data() + off,
                        std::min<size_t>(12, text.size() - off));
        KvmEvent t = {};
        t.type = KVM_EV_DRAG_TEXT;
        std::memcpy(&t.a, chunk, 4);
        std::memcpy(&t.b, chunk + 4, 4);
        std::memcpy(&t.c, chunk + 8, 4);
        pushEvent(t);
    }
    g_drag_ghost_sent = true;
}

void dropRequestUris(Time when)
{
    if (!g_dpy || !g_drop_strip) return;
    XConvertSelection(g_dpy, A_XdndSelection, A_text_uri_list, A_drop_prop,
                      g_drop_strip, when);
    XFlush(g_dpy);
}

void dropStripInit()
{
    if (!g_dpy || g_drop_strip) return;
    A_XdndAware = XInternAtom(g_dpy, "XdndAware", False);
    A_XdndEnter = XInternAtom(g_dpy, "XdndEnter", False);
    A_XdndPosition = XInternAtom(g_dpy, "XdndPosition", False);
    A_XdndStatus = XInternAtom(g_dpy, "XdndStatus", False);
    A_XdndLeave = XInternAtom(g_dpy, "XdndLeave", False);
    A_XdndDrop = XInternAtom(g_dpy, "XdndDrop", False);
    A_XdndFinished = XInternAtom(g_dpy, "XdndFinished", False);
    A_XdndSelection = XInternAtom(g_dpy, "XdndSelection", False);
    A_XdndTypeList = XInternAtom(g_dpy, "XdndTypeList", False);
    A_XdndActionCopy = XInternAtom(g_dpy, "XdndActionCopy", False);
    A_text_uri_list = XInternAtom(g_dpy, "text/uri-list", False);
    A_drop_prop = XInternAtom(g_dpy, "MivaraXdndData", False);

    XSetWindowAttributes attrs = {};
    attrs.override_redirect = True;
    attrs.event_mask = EnterWindowMask | LeaveWindowMask |
                       StructureNotifyMask;
    g_drop_strip = XCreateWindow(
        g_dpy, DefaultRootWindow(g_dpy), 0, 0, 3, 1, 0, 0, InputOnly,
        CopyFromParent, CWOverrideRedirect | CWEventMask, &attrs);
    if (!g_drop_strip) return;
    long version = 5;
    XChangeProperty(g_dpy, g_drop_strip, A_XdndAware, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&version, 1);
    XMapRaised(g_dpy, g_drop_strip);
    XFlush(g_dpy);
}

void dropStripSetSide(int side)
{
    if (!g_dpy || !g_drop_strip) return;
    const int scr = DefaultScreen(g_dpy);
    const int sw = DisplayWidth(g_dpy, scr);
    const int sh = DisplayHeight(g_dpy, scr);
    int x = 0, y = 0, w = 3, h = sh;
    switch (side) {
    case KVM_SIDE_LEFT:   x = 0;      y = 0;      w = 3; h = sh; break;
    case KVM_SIDE_TOP:    x = 0;      y = 0;      w = sw; h = 3; break;
    case KVM_SIDE_BOTTOM: x = 0;      y = sh - 3; w = sw; h = 3; break;
    default:              x = sw - 3; y = 0;      w = 3; h = sh; break;
    }
    XMoveResizeWindow(g_dpy, g_drop_strip, x, y, (unsigned)w, (unsigned)h);
    XRaiseWindow(g_dpy, g_drop_strip);
    XFlush(g_dpy);
}

void sendXdndFinished(bool ok)
{
    if (!g_dpy || !g_drag_source || !A_XdndFinished) return;
    XEvent ev = {};
    ev.xclient.type = ClientMessage;
    ev.xclient.display = g_dpy;
    ev.xclient.window = g_drag_source;
    ev.xclient.message_type = A_XdndFinished;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)g_drop_strip;
    ev.xclient.data.l[1] = ok ? 1 : 0;
    ev.xclient.data.l[2] = (long)A_XdndActionCopy;
    XSendEvent(g_dpy, g_drag_source, False, NoEventMask, &ev);
    XFlush(g_dpy);
}

void dropResetDrag()
{
    g_drag_source = 0;
    g_file_drag = false;
    g_drag_uri_ok = false;
    g_drag_ghost_sent = false;
    g_drop_waiting = false;
    g_early_requested = false;
    g_drag_count = 0;
    g_drag_name.clear();
    g_drag_paths.clear();
}

void dropStripDestroy()
{
    if (!g_dpy) return;
    if (g_drag_ghost_sent) dropSendGhost(false);
    if (g_drop_strip) {
        XDestroyWindow(g_dpy, g_drop_strip);
        g_drop_strip = 0;
    }
    dropResetDrag();
    XFlush(g_dpy);
}

void dropStartTransfer()
{
    if (!g_drop_ready || g_drag_paths.empty()) return;
    if (g_drop_run.exchange(true)) {
        emitStat("event=drop|state=busy|msg=another transfer is running");
        return;
    }
    if (g_drop_thread.joinable()) g_drop_thread.join();

    std::vector<std::string> paths = g_drag_paths;
    const std::string peer = g_peer_ip;
    uint8_t token[DROP_TOKEN_BYTES];
    std::memcpy(token, g_drop_token, sizeof(token));
    const int tx = g_remote_x, ty = g_remote_y;
    emitStat("event=drop|state=sending|items=" +
             std::to_string(paths.size()) + "|x=" + std::to_string(tx) +
             "|y=" + std::to_string(ty));
    g_drop_thread = std::thread([paths, peer, token, tx, ty]() mutable {
        DropOptions opt;
        opt.peer_ip = peer.c_str();
        opt.port = (uint16_t)DROP_PORT;
        opt.x = tx;
        opt.y = ty;
        opt.transfer_id = (uint32_t)(kvmNowMs() ^
                                     (uint64_t)getpid());
        DropResult r =
            dropSendFiles(opt, paths, token, g_drop_run);
        std::string line = "event=drop|state=";
        line += (r.status == DROP_OK) ? "done" : "error";
        line += "|items=" + std::to_string(r.committed);
        line += "|failed=" + std::to_string(r.failed);
        line += "|fallback=" + std::string(r.fallback ? "1" : "0");
        line += "|code=" + std::to_string(r.status);
        if (!r.message.empty())
            line += "|msg=" + dropStatSafe(r.message);
        emitStat(line);
        mivaraSecureZero(token, sizeof(token));
        g_drop_run.store(false);
    });
}

void dropClientMessage(XClientMessageEvent &c)
{
    if (!g_drop_strip || c.window != g_drop_strip) return;

    if (c.message_type == A_XdndEnter) {
        g_drag_source = (Window)c.data.l[0];
        g_file_drag = true;
        g_drag_paths.clear();
        g_drag_name.clear();
        g_drag_count = 0;
        g_drag_ghost_sent = false;
        g_drop_waiting = false;
        g_early_requested = false;
        bool uri = false;
        if (c.data.l[1] & 1) {
            Atom actual = 0;
            int fmt = 0;
            unsigned long n = 0, rem = 0;
            unsigned char *prop = nullptr;
            if (XGetWindowProperty(g_dpy, g_drag_source, A_XdndTypeList, 0,
                                   64, False, XA_ATOM, &actual, &fmt, &n,
                                   &rem, &prop) == Success && prop) {
                const Atom *types = (const Atom *)prop;
                for (unsigned long i = 0; i < n; i++)
                    if (types[i] == A_text_uri_list) uri = true;
                XFree(prop);
            }
        } else {
            for (int i = 2; i <= 4; i++)
                if ((Atom)c.data.l[i] == A_text_uri_list) uri = true;
        }
        g_drag_uri_ok = uri && g_drop_ready;
        if (g_drag_uri_ok && !g_early_requested) {
            g_early_requested = true;
            dropRequestUris(CurrentTime);
        }
        return;
    }

    if (c.message_type == A_XdndPosition) {
        if ((Window)c.data.l[0] != g_drag_source) return;
        g_drop_root_x = (int)((c.data.l[2] >> 16) & 0xFFFF);
        g_drop_root_y = (int)(c.data.l[2] & 0xFFFF);
        XEvent ev = {};
        ev.xclient.type = ClientMessage;
        ev.xclient.display = g_dpy;
        ev.xclient.window = g_drag_source;
        ev.xclient.message_type = A_XdndStatus;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)g_drop_strip;
        ev.xclient.data.l[1] = g_drag_uri_ok ? 1 : 0;
        ev.xclient.data.l[2] = 0;
        ev.xclient.data.l[3] = 0;
        ev.xclient.data.l[4] = g_drag_uri_ok ? (long)A_XdndActionCopy : 0;
        XSendEvent(g_dpy, g_drag_source, False, NoEventMask, &ev);
        XFlush(g_dpy);
        if (g_drag_uri_ok && !g_drag_ghost_sent) dropSendGhost(true);
        return;
    }

    if (c.message_type == A_XdndLeave) {
        if (g_drag_ghost_sent) dropSendGhost(false);
        dropResetDrag();
        return;
    }

    if (c.message_type == A_XdndDrop) {
        if (!g_drag_uri_ok || !g_drop_ready) {
            sendXdndFinished(false);
            dropResetDrag();
            return;
        }
        g_drop_waiting = true;
        if (!g_drag_paths.empty()) {
            sendXdndFinished(true);
            dropStartTransfer();
            if (g_drag_ghost_sent) dropSendGhost(false);
            dropResetDrag();
        } else {
            dropRequestUris((Time)c.data.l[2]);
        }
        return;
    }
}

void dropSelectionNotify(XSelectionEvent &e)
{
    if (!g_drop_strip || e.requestor != g_drop_strip) return;
    if (e.property == None) {
        if (g_drop_waiting) {
            sendXdndFinished(false);
            dropResetDrag();
        }
        return;
    }
    Atom actual = 0;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(g_dpy, g_drop_strip, e.property, 0, 1 << 20,
                           False, AnyPropertyType, &actual, &fmt, &n, &rem,
                           &prop) != Success || !prop) {
        if (g_drop_waiting) {
            sendXdndFinished(false);
            dropResetDrag();
        }
        return;
    }
    std::string data;
    if (fmt == 8)
        data.assign((const char *)prop, n);
    else if (fmt == 32)
        data.assign((const char *)prop, n * 4);
    XFree(prop);
    XDeleteProperty(g_dpy, g_drop_strip, e.property);

    std::vector<std::string> paths;
    if (!uriListToPaths(data, paths)) {
        if (g_drop_waiting) {
            sendXdndFinished(false);
            dropResetDrag();
        }
        return;
    }
    g_drag_paths = paths;
    g_drag_count = (int)paths.size();
    g_drag_name = pathBaseName(paths.front());
    dropSendGhost(true);

    if (g_drop_waiting) {
        sendXdndFinished(true);
        dropStartTransfer();
        dropSendGhost(false);
        dropResetDrag();
    }
}

/* ── raw input ───────────────────────────────────────────────────────────── */

void updateModifiers()
{
    g_ctrl = (g_ctrl_l && g_key_down[g_ctrl_l]) ||
             (g_ctrl_r && g_key_down[g_ctrl_r]);
    g_alt = (g_alt_l && g_key_down[g_alt_l]) ||
            (g_alt_r && g_key_down[g_alt_r]);
    g_shift = (g_shift_l && g_key_down[g_shift_l]) ||
              (g_shift_r && g_key_down[g_shift_r]);
}

void onMotion(int dx, int dy)
{
    if (!dx && !dy) return;

    if (g_state == ControlState::Remote) {
        if (g_trace)
            std::cerr << "[kvm-trace] remote motion dx=" << dx << " dy=" << dy
                      << "\n";
        moveRemote(dx, dy);
        return;
    }

    /* A local file drag is handled by the XDND strip. Do not grab the
     * keyboard/pointer or replay the held left button mid-drag. */
    if (g_file_drag) {
        g_edge_overshoot = 0;
        return;
    }

    /* Carrying a reverse drag on the local desktop: never re-enter Remote. */
    if (g_rev_held) {
        g_edge_overshoot = 0;
        return;
    }

    /* Only motion further out of the chosen edge counts; any other direction
     * resets the accumulator so jitter cannot cross. */
    const bool horizontal =
        (g_side == KVM_SIDE_LEFT || g_side == KVM_SIDE_RIGHT);
    const bool outward_positive =
        (g_side == KVM_SIDE_RIGHT || g_side == KVM_SIDE_BOTTOM);
    const int d = horizontal ? dx : dy;
    if (d == 0 || (d > 0) != outward_positive) {
        g_edge_overshoot = 0;
        return;
    }

    int px = 0, py = 0;
    if (!pointerPos(px, py)) return;
    int lw = 1, lh = 1;
    localMetrics(lw, lh);
    const bool at_edge =
        horizontal ? (outward_positive ? px >= lw - 1 : px <= 0)
                   : (outward_positive ? py >= lh - 1 : py <= 0);
    if (!at_edge) {
        g_edge_overshoot = 0;
        return;
    }
    if (kvmNowMs() - g_leave_tick < 250) return;
    g_edge_overshoot += (d > 0 ? d : -d);
    if (g_trace)
        std::cerr << "[kvm-trace] local motion dx=" << dx << " dy=" << dy
                  << " side=" << g_side << " ptr=" << px << "," << py
                  << " over=" << g_edge_overshoot << "\n";
    if (g_edge_overshoot >= 3) enterRemote(px, py);
}

/* `grabbed` is true for regular ButtonPress/ButtonRelease events delivered by
 * the active XGrabPointer; raw button events are used only in Local mode
 * (the pointer grab suppresses them while Remote, matching the key path). */
void onButton(int button, bool down, bool grabbed)
{
    if (g_trace)
        std::cerr << "[kvm-trace] button " << (grabbed ? "grab" : "raw")
                  << " b=" << button << " down=" << down
                  << " state=" << (int)g_state << "\n";
    /* X11 wheel buttons: 4/5 vertical, 6/7 horizontal. */
    if (button >= 4 && button <= 7) {
        if (!grabbed || !down || g_state != ControlState::Remote) return;
        const int delta = (button == 4) ? WHEEL_STEP
                        : (button == 5) ? -WHEEL_STEP
                        : (button == 6) ? -WHEEL_STEP
                                        : WHEEL_STEP;
        queueWheel(delta, button >= 6);
        return;
    }

    int pb = 0;
    switch (button) {
    case 1: pb = 1; break;   /* left    */
    case 2: pb = 3; break;   /* middle  */
    case 3: pb = 2; break;   /* right   */
    case 8: pb = 4; break;   /* back    */
    case 9: pb = 5; break;   /* forward */
    default: return;
    }

    /* The active pointer grab installed for a carried reverse drag reports
     * the physical release here even though the local state is not Remote. */
    if (grabbed && !down && pb == 1 && g_state == ControlState::Local &&
        g_rev_held) {
        g_btn_down[1] = false;
        finishReverseDrag();
        return;
    }

    g_btn_down[pb] = down;
    if (!down) {
        bool any = false;
        for (int b = 1; b < 6; b++) any = any || g_btn_down[b];
        if (!any) stopMotionPoll();
    }
    if (!grabbed) {
        /* Local release after carrying a reverse drag back: forward the UP so
         * the target's drop fires, then keep the local desktop from seeing an
         * unmatched release. */
        if (pb == 1 && !down && (g_rev_held || g_reverse_drag.load())) {
            finishReverseDrag();
            return;
        }
        if (g_state == ControlState::Local) g_btn_local[pb] = down;
        return;
    }
    if (g_state != ControlState::Remote) return;
    if (g_trace)
        std::cerr << "[kvm-trace] forward button pb=" << pb
                  << " down=" << down << "\n";
    queueButton((uint32_t)pb, down);
    bool any_down = false;
    for (int b = 1; b < 6; b++) any_down = any_down || g_btn_down[b];
    if (any_down) startMotionPoll();
    else stopMotionPoll();
    /* Keep g_btn_local[pb] as-is on a Remote release: if the press reached the
     * local desktop, injectLocalReleases() must still send its up on leave. */
}

/* `grabbed` is true for the regular KeyPress/KeyRelease events an active
 * XGrabKeyboard delivers to the root window. Raw key events are only used in
 * Local mode: the X server suppresses them while the grab is active (they are
 * delivered as regular key events instead). */
void onKey(int keycode, bool down, bool grabbed)
{
    if (keycode < 8 || keycode > 255) return;
    if (g_trace)
        std::cerr << "[kvm-trace] key " << (grabbed ? "grab" : "raw")
                  << " kc=" << keycode << " down=" << down
                  << " state=" << (int)g_state << "\n";
    g_key_down[keycode] = down;
    updateModifiers();

    if (!grabbed) {
        /* Local: track what the physical keyboard is doing and whether the
         * press reached this desktop. A raw event while Remote would be a
         * duplicate of a grabbed event; ignore it. */
        if (g_state == ControlState::Local) {
            if (down && keycode == g_q_key && g_ctrl && g_alt && g_shift &&
                (g_rev_held || g_reverse_drag.load())) {
                cancelReverseDrag(true);
                return;
            }
            g_key_local[keycode] = down;
            if (g_panic_key == keycode && !down) g_panic_key = -1;
        }
        return;
    }

    if (g_state != ControlState::Remote) return;

    /* Ctrl+Alt+Shift+Q is the local panic key: swallowed, never forwarded. */
    if (down && keycode == g_q_key && g_ctrl && g_alt && g_shift) {
        g_panic_key = keycode;
        g_drop_run.store(false);
        leaveRemote(true);
        return;
    }
    if (g_panic_key == keycode) {
        if (!down) g_panic_key = -1;
        return;
    }

    uint32_t vk = 0, scan = 0;
    bool ext = false;
    if (!translateKey(keycode, vk, scan, ext)) {
        if (g_trace)
            std::cerr << "[kvm-trace] key kc=" << keycode
                      << " down=" << down << " unmapped\n";
        return;
    }
    if (g_trace)
        std::cerr << "[kvm-trace] forward key kc=" << keycode
                  << " down=" << down << " scan=" << scan
                  << " ext=" << ext << "\n";
    queueKey(down, vk, scan, ext);
}

/* ── X event processing ──────────────────────────────────────────────────── */

void handleEvent(XEvent &ev)
{
    if (ev.type == ClientMessage) {
        dropClientMessage(ev.xclient);
        return;
    }
    if (ev.type == SelectionNotify) {
        dropSelectionNotify(ev.xselection);
        return;
    }
    /* An active XGrabKeyboard reports keys here (raw key events are not
     * delivered while it is active). */
    if (ev.type == KeyPress || ev.type == KeyRelease) {
        if (g_state == ControlState::Remote)
            onKey(ev.xkey.keycode, ev.type == KeyPress, true);
        return;
    }
    if (ev.type == ButtonPress || ev.type == ButtonRelease) {
        if (g_state == ControlState::Remote) {
            onButton(ev.xbutton.button, ev.type == ButtonPress, true);
        } else if (g_rev_held && ev.xbutton.button == 1 &&
                   ev.type == ButtonRelease) {
            onButton(1, false, true);
        }
        return;
    }
    if (ev.type != GenericEvent || ev.xcookie.extension != g_xi_op) return;
    XGenericEventCookie *cookie = &ev.xcookie;
    if (!XGetEventData(g_dpy, cookie)) return;

    switch (cookie->evtype) {
    case XI_RawMotion: {
        auto *re = (XIRawEvent *)cookie->data;
        DevMode *dm = deviceMode(re->deviceid);
        if (!dm) break;
        const bool absolute = dm->absolute || g_force_abs_motion;
        if (absolute && !g_allow_abs) break;

        double ax = 0.0, ay = 0.0;
        bool have_x = false, have_y = false;
        int vi = 0;
        for (int v = 0; v < re->valuators.mask_len * 8; v++) {
            if (!XIMaskIsSet(re->valuators.mask, v)) continue;
            const double val = re->raw_values[vi++];
            if (v == 0) { ax = val; have_x = true; }
            else if (v == 1) { ay = val; have_y = true; }
        }

        double dx = 0.0, dy = 0.0;
        if (absolute) {
            int px = 0, py = 0;
            pointerPos(px, py);
            const double sx = have_x
                ? (dm->max_x > dm->min_x
                       ? (ax - dm->min_x) * (g_screen_w - 1) /
                             (dm->max_x - dm->min_x)
                       : ax)
                : (double)px;
            const double sy = have_y
                ? (dm->max_y > dm->min_y
                       ? (ay - dm->min_y) * (g_screen_h - 1) /
                             (dm->max_y - dm->min_y)
                       : ay)
                : (double)py;
            if (!dm->last_valid) {
                dm->last_valid = true;
                dm->last_x = sx;
                dm->last_y = sy;
                break;
            }
            dx = sx - dm->last_x;
            dy = sy - dm->last_y;
            dm->last_x = sx;
            dm->last_y = sy;
            /* A warp/device hand-off is not user motion. */
            if (dx > 500 || dx < -500 || dy > 500 || dy < -500) break;
        } else {
            dx = have_x ? ax : 0.0;
            dy = have_y ? ay : 0.0;
        }

        if (g_motion_poll) break;
        g_motion_acc_x += dx;
        g_motion_acc_y += dy;
        const int ix = (int)g_motion_acc_x;
        const int iy = (int)g_motion_acc_y;
        g_motion_acc_x -= ix;
        g_motion_acc_y -= iy;
        if (ix || iy) onMotion(ix, iy);
        break;
    }
    case XI_RawButtonPress:
    case XI_RawButtonRelease: {
        auto *re = (XIRawEvent *)cookie->data;
        onButton(re->detail, cookie->evtype == XI_RawButtonPress, false);
        break;
    }
    case XI_RawKeyPress:
    case XI_RawKeyRelease: {
        auto *re = (XIRawEvent *)cookie->data;
        onKey(re->detail, cookie->evtype == XI_RawKeyPress, false);
        break;
    }
    default:
        break;
    }
    XFreeEventData(g_dpy, cookie);
}

/* ── X setup ─────────────────────────────────────────────────────────────── */

bool initX11(std::string &why)
{
    XInitThreads();
    XSetErrorHandler(kvmXErrorHandler);
    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) {
        why = "无法打开 X11 显示";
        return false;
    }
    g_rev_ghost.attach(g_dpy);
    int event_base = 0, error_base = 0;
    if (!XQueryExtension(g_dpy, "XInputExtension", &g_xi_op, &event_base,
                         &error_base)) {
        why = "X 服务器不支持 XInput2";
        return false;
    }
    int major = 2, minor = 2;
    if (XIQueryVersion(g_dpy, &major, &minor) != Success) {
        why = "XInput2 版本过低";
        return false;
    }

    int xf_event = 0, xf_error = 0;
    g_xfixes = XFixesQueryExtension(g_dpy, &xf_event, &xf_error) != 0;
    (void)xf_event;
    (void)xf_error;

    loadXtst();

    g_ctrl_l = XKeysymToKeycode(g_dpy, XK_Control_L);
    g_ctrl_r = XKeysymToKeycode(g_dpy, XK_Control_R);
    g_alt_l = XKeysymToKeycode(g_dpy, XK_Alt_L);
    g_alt_r = XKeysymToKeycode(g_dpy, XK_Alt_R);
    g_shift_l = XKeysymToKeycode(g_dpy, XK_Shift_L);
    g_shift_r = XKeysymToKeycode(g_dpy, XK_Shift_R);
    g_q_key = XKeysymToKeycode(g_dpy, XK_q);

    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(mask, XI_RawMotion);
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XISetMask(mask, XI_RawKeyPress);
    XISetMask(mask, XI_RawKeyRelease);
    XIEventMask em = {};
    em.deviceid = XIAllMasterDevices;
    em.mask_len = sizeof(mask);
    em.mask = mask;
    XISelectEvents(g_dpy, DefaultRootWindow(g_dpy), &em, 1);
    /* An active XGrabKeyboard delivers key events to the grab window; select
     * them so the Remote path can forward keys while raw key events are
     * suppressed by the grab. Another client holding a passive button grab on
     * the root can make this selection fail (BadAccess); do not let that kill
     * the process, but do not enter Remote either — key/button delivery would
     * be incomplete. */
    g_x_error = false;
    XSelectInput(g_dpy, DefaultRootWindow(g_dpy),
                 KeyPressMask | KeyReleaseMask | ButtonPressMask |
                     ButtonReleaseMask);
    XSync(g_dpy, False);
    if (g_x_error) {
        g_root_input_ok = false;
        emitLog("cannot select root input events (another client holds a "
                "root grab); remote input disabled, file drop still works");
    }

    localMetrics(g_screen_w, g_screen_h);
    XSync(g_dpy, False);
    return true;
}

void closeX11()
{
    if (!g_dpy) return;
    setCursorVisible(true);
    ungrabInput();
    g_rev_ghost.detach();
    XCloseDisplay(g_dpy);
    g_dpy = nullptr;
}

/* ── drain thread: owns the socket ───────────────────────────────────────── */

void drainFail(int code, int err)
{
    g_loss_code.store(code, std::memory_order_relaxed);
    g_loss_err.store(err, std::memory_order_relaxed);
    g_netOK.store(false, std::memory_order_relaxed);
}

void drainLoop(kvm_sock_t sock, bool heartbeat,
               const std::atomic<bool> &running)
{
    uint64_t last_rx = kvmNowMs();
    uint64_t next_tx = last_rx + KVM_HEARTBEAT_INTERVAL_MS;
    unsigned char rx[16];
    size_t rx_off = 0;

    while (!g_drainQuit.load() && running.load() && g_netOK.load()) {
        /* Send everything queued, coalescing runs of moves in pop(). */
        bool send_failed = false;
        for (;;) {
            KvmEvent batch[256];
            size_t n = 0;
            while (n < 256 && g_queue.pop(batch[n])) n++;
            if (!n) break;
            for (size_t i = 0; i < n; i++) {
                batch[i].type = htonl(batch[i].type);
                batch[i].a = htonl(batch[i].a);
                batch[i].b = htonl(batch[i].b);
                batch[i].c = htonl(batch[i].c);
            }
            if (!kvmSendAll(sock, batch, n * sizeof(KvmEvent))) {
                send_failed = true;
                break;
            }
        }
        if (send_failed) {
            drainFail(LOSS_SEND_ERROR, kvmLastSockError());
            break;
        }

        const uint64_t now = kvmNowMs();
        if (heartbeat && now >= next_tx) {
            KvmEvent ping = {};
            ping.type = htonl(KVM_EV_PING);
            if (!kvmSendAll(sock, &ping, sizeof(ping))) {
                drainFail(LOSS_SEND_ERROR, kvmLastSockError());
                break;
            }
            next_tx = now + KVM_HEARTBEAT_INTERVAL_MS;
        }
        if (heartbeat && now - last_rx >= KVM_HEARTBEAT_TIMEOUT_MS) {
            drainFail(LOSS_HEARTBEAT, 0);
            break;
        }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        int maxfd = (int)sock;
        if (g_wake[0] >= 0) {
            FD_SET(g_wake[0], &rd);
            if (g_wake[0] > maxfd) maxfd = g_wake[0];
        }
        struct timeval tv = {0, 100000};   /* 100 ms */
        const int sel = select(maxfd + 1, &rd, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            drainFail(LOSS_SELECT_ERROR, errno);
            break;
        }
        if (sel == 0) continue;

        if (g_wake[0] >= 0 && FD_ISSET(g_wake[0], &rd)) {
            char sink[256];
            while (read(g_wake[0], sink, sizeof(sink)) > 0) {}
        }
        if (!FD_ISSET(sock, &rd)) continue;

        const int n = (int)recv(sock, rx + rx_off, sizeof(rx) - rx_off, 0);
        if (n == 0) {
            drainFail(LOSS_PEER_CLOSED, 0);
            break;
        }
        if (n < 0) {
            const int err = kvmLastSockError();
            if (kvmSockWouldBlock(err) || kvmSockTimedOut(err) ||
                kvmSockInterrupted(err))
                continue;
            drainFail(LOSS_RECV_ERROR, err);
            break;
        }
        rx_off += (size_t)n;
        last_rx = kvmNowMs();
        if (rx_off < sizeof(rx)) continue;

        rx_off = 0;
        KvmEvent incoming = {};
        std::memcpy(&incoming, rx, sizeof(incoming));
        const uint32_t type = ntohl(incoming.type);
        if (type == KVM_EV_PING) {
            if (heartbeat) {
                KvmEvent pong = {};
                pong.type = htonl(KVM_EV_PONG);
                if (!kvmSendAll(sock, &pong, sizeof(pong))) {
                    drainFail(LOSS_SEND_ERROR, kvmLastSockError());
                    break;
                }
            }
        } else if (type == KVM_EV_PONG) {
            /* Heartbeat reply. */
        } else if (type == KVM_EV_RDRAG && g_reverse_ok) {
            const bool on = ntohl(incoming.a) != 0;
            g_reverse_drag.store(on);
            std::lock_guard<std::mutex> lk(g_rev.m);
            g_rev.active = on;
            if (!on) {
                revResetLocked();
                g_rev_cancel_req.store(true);
            }
        } else if (type == KVM_EV_DRAG_INFO && g_reverse_ok) {
            std::lock_guard<std::mutex> lk(g_rev.m);
            const bool begin = ntohl(incoming.a) != 0;
            revResetLocked();
            if (begin) {
                g_rev.active = true;
                g_rev.count = (int)ntohl(incoming.b);
                g_rev.text_left = (int)ntohl(incoming.c);
                g_rev.name_ready = g_rev.text_left <= 0;
            }
        } else if (type == KVM_EV_DRAG_TEXT && g_reverse_ok) {
            std::lock_guard<std::mutex> lk(g_rev.m);
            if (g_rev.text_left > 0) {
                revAppendNameLocked(incoming);
                if (--g_rev.text_left == 0) {
                    while (!g_rev.name.empty() &&
                           g_rev.name.back() == '\0')
                        g_rev.name.pop_back();
                    g_rev.name_ready = true;
                    ++g_rev.version;
                }
            }
        } else if (type == KVM_EV_DRAG_ICON_INFO && g_reverse_ok &&
                   g_drag_icon_ok) {
            std::lock_guard<std::mutex> lk(g_rev.m);
            const int w = (int)ntohl(incoming.a);
            const int h = (int)ntohl(incoming.b);
            const int frames = (int)ntohl(incoming.c);
            g_rev.have_icon = false;
            g_rev.icon.clear();
            g_rev.icon_acc.clear();
            g_rev.icon_w = w;
            g_rev.icon_h = h;
            g_rev.icon_left = 0;
            if (w > 0 && h > 0 && w <= 2048 && h <= 2048 &&
                (uint64_t)w * (uint64_t)h <= 36864u &&
                frames >= 0 && frames <= 12288) {
                g_rev.icon_acc.reserve((size_t)w * (size_t)h * 4);
                g_rev.icon_left = frames;
                if (frames == 0) {
                    g_rev.have_icon = true;
                    ++g_rev.version;
                }
            }
        } else if (type == KVM_EV_DRAG_ICON_DATA && g_reverse_ok &&
                   g_drag_icon_ok) {
            std::lock_guard<std::mutex> lk(g_rev.m);
            if (g_rev.icon_left > 0) {
                revAppendLocked(incoming, g_rev.icon_acc);
                if (--g_rev.icon_left == 0 &&
                    g_rev.icon_acc.size() ==
                        (size_t)g_rev.icon_w * (size_t)g_rev.icon_h * 4) {
                    g_rev.icon = g_rev.icon_acc;
                    g_rev.have_icon = true;
                    ++g_rev.version;
                }
            }
        } else if (type == KVM_EV_DRAG_IMAGE_INFO && g_reverse_ok &&
                   g_drag_image_ok) {
            std::lock_guard<std::mutex> lk(g_rev.m);
            int w = 0, h = 0, ox = 0, oy = 0;
            revDimOff(ntohl(incoming.a), w, ox);
            revDimOff(ntohl(incoming.b), h, oy);
            const int frames = (int)ntohl(incoming.c);
            g_rev.have_image = false;
            g_rev.image.clear();
            g_rev.image_acc.clear();
            g_rev.img_w = w;
            g_rev.img_h = h;
            g_rev.img_ox = ox;
            g_rev.img_oy = oy;
            g_rev.image_left = 0;
            if (w > 0 && h > 0 && w <= 2048 && h <= 2048 &&
                (uint64_t)w * (uint64_t)h <= 36864u &&
                frames >= 0 && frames <= 12288) {
                g_rev.image_acc.reserve((size_t)w * (size_t)h * 4);
                g_rev.image_left = frames;
                if (frames == 0) {
                    g_rev.have_image = true;
                    ++g_rev.version;
                }
            }
        } else if (type == KVM_EV_DRAG_IMAGE_DATA && g_reverse_ok &&
                   g_drag_image_ok) {
            std::lock_guard<std::mutex> lk(g_rev.m);
            if (g_rev.image_left > 0) {
                revAppendLocked(incoming, g_rev.image_acc);
                if (--g_rev.image_left == 0 &&
                    g_rev.image_acc.size() ==
                        (size_t)g_rev.img_w * (size_t)g_rev.img_h * 4) {
                    g_rev.image = g_rev.image_acc;
                    g_rev.have_image = true;
                    ++g_rev.version;
                }
            }
        } else {
            drainFail(LOSS_PROTOCOL_EVENT, 0);
            break;
        }
    }
}

/* ── connect / authorization ─────────────────────────────────────────────── */

/* The target sends the 8085 token as one fixed marker frame plus eight data
 * frames immediately after KvmReply, before any event-stream parser runs.
 * Read it when EITHER direction is enabled: a reverse-only session still
 * needs it for our own 8087 gate. */
bool readDropToken(kvm_sock_t sock, const KvmReply &reply,
                   const std::atomic<bool> &running)
{
    const uint32_t feat = ntohl(reply.reserved);
    g_drop_ready = g_drop_enabled && (feat & KVM_FEATURE_FILE_DROP) != 0;
    g_reverse_ok = g_reverse_enabled &&
                   (feat & KVM_FEATURE_REVERSE_DROP) != 0;
    g_drag_icon_ok = (feat & KVM_FEATURE_DRAG_ICON) != 0;
    g_drag_image_ok = (feat & KVM_FEATURE_DRAG_IMAGE) != 0;
    if (!g_drop_ready && !g_reverse_ok) return true;
    KvmEvent ev = {};
    const auto read = [&] {
        return kvmRecvAllPoll(sock, &ev, sizeof(ev), running, 35000) == 1;
    };
    if (!read() || ntohl(ev.type) != KVM_EV_DROP_INFO ||
        ntohl(ev.a) != 0x4D564B58u || ntohl(ev.b) != 1u ||
        ntohl(ev.c) != 8u) {
        g_drop_ready = false;
        g_reverse_ok = false;
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (!read() || ntohl(ev.type) != KVM_EV_DROP_INFO ||
            ntohl(ev.a) != (uint32_t)i) {
            mivaraSecureZero(g_drop_token, sizeof(g_drop_token));
            g_drop_ready = false;
            g_reverse_ok = false;
            return false;
        }
        const uint32_t word = ntohl(ev.b);
        std::memcpy(g_drop_token + i * 4, &word, 4);
    }
    if (g_drop_ready)
        emitStat("event=drop|state=ready|port=" +
                 std::to_string((int)DROP_PORT));
    return true;
}

int connectHandshake(const char *peer_ip, int peer_port, int side,
                     const std::atomic<bool> &running,
                     const KvmClientAuth *auth, kvm_sock_t &out_sock,
                     KvmReply &out_reply, bool &heartbeat)
{
    emitStat("event=stitch|state=waiting|mode=stitch|dst=" +
             std::string(peer_ip) + "|side=" + kvmSideKey(side));

    KvmReply reply = {};
    uint32_t status = KVM_ERR_BUSY;
    for (int attempt = 0; attempt < 2; attempt++) {
        kvm_sock_t sock =
            kvmConnectTimeout(peer_ip, peer_port, running, 5000);
        if (sock == KVM_INVALID_SOCK) {
            if (!running.load()) return KVM_RUN_OK;
            emitStatError("无法连接拼接端口 " + std::to_string(peer_port) +
                          "（对方需先启动接收）");
            return KVM_RUN_TRANSPORT;
        }
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                   sizeof(nodelay));
        kvmSetTimeouts(sock, 0, 2000);
        kvmEnableKeepAlive(sock);

        KvmHello hello = {};
        hello.magic = htonl(KVM_MAGIC);
        hello.version = htonl(KVM_VERSION);
        hello.reserved[0] = htonl((uint32_t)side + 1u);
        /* Low 16 bits are capabilities; bits 16..31 carry our reverse-drop
         * port so the target never has to guess (a local receiver may own
         * 8085). */
        hello.reserved[1] = htonl(
            KVM_FEATURE_HEARTBEAT | KVM_FEATURE_DRAG_ICON |
            KVM_FEATURE_DRAG_IMAGE |
            (g_drop_enabled ? KVM_FEATURE_FILE_DROP : 0u) |
            (g_reverse_enabled ? KVM_FEATURE_REVERSE_DROP : 0u) |
            (g_reverse_enabled
                 ? ((uint32_t)DROP_REVERSE_PORT << 16) : 0u));
        if (!kvmSendAll(sock, &hello, sizeof(hello))) {
            if (running.load()) emitStatError("拼接握手发送失败");
            kvmClose(sock);
            return KVM_RUN_TRANSPORT;
        }

        uint32_t first_raw = 0;
        int rv = kvmRecvAllPoll(sock, &first_raw, sizeof(first_raw),
                                running, 35000);
        if (rv <= 0) {
            if (running.load()) emitStatError("拼接握手失败（对方无响应）");
            kvmClose(sock);
            return KVM_RUN_TRANSPORT;
        }
        const uint32_t first = ntohl(first_raw);

        if (first == KVM_MAGIC) {
            reply = KvmReply{};
            reply.magic = first_raw;
            rv = kvmRecvAllPoll(sock, (char *)&reply + sizeof(uint32_t),
                                sizeof(reply) - sizeof(uint32_t), running,
                                35000);
            if (rv <= 0) {
                if (running.load())
                    emitStatError("拼接握手失败（对方应答不完整）");
                kvmClose(sock);
                return KVM_RUN_TRANSPORT;
            }
            if (ntohl(reply.version) != KVM_VERSION) {
                emitStatError("拼接协议版本不一致，请将两端更新到同一版本");
                kvmClose(sock);
                return KVM_RUN_PROTOCOL;
            }
            status = ntohl(reply.status);
            if (status == KVM_ERR_BUSY && attempt == 0) {
                kvmClose(sock);
                if (!running.load()) return KVM_RUN_TRANSPORT;
                kvmSleepMs(300);
                continue;
            }
            if (status == KVM_OK) {
                if (!readDropToken(sock, reply, running)) {
                    emitStatError("拼接文件通道握手失败");
                    kvmClose(sock);
                    return KVM_RUN_TRANSPORT;
                }
                out_sock = sock;
                out_reply = reply;
                heartbeat =
                    (ntohl(reply.reserved) & KVM_FEATURE_HEARTBEAT) != 0;
                return KVM_RUN_OK;
            }
            kvmClose(sock);
            break;
        }

        if (first == TVHS_MAGIC) {
            HsResponseV2 res = {};
            res.magic = first_raw;
            rv = kvmRecvAllPoll(sock, (char *)&res + sizeof(uint32_t),
                                sizeof(res) - sizeof(uint32_t), running,
                                35000);
            if (rv <= 0) {
                if (running.load())
                    emitStatError("拼接授权握手失败（对方应答不完整）");
                kvmClose(sock);
                return KVM_RUN_TRANSPORT;
            }
            std::string why = "拼接授权被拒绝";
            uint32_t res_status = 0;
            if (ntohl(res.magic) == TVHS_MAGIC &&
                ntohl(res.version) == TVHS_VERSION) {
                res_status = ntohl(res.status);
                kvmAuthErrorText(res_status, why);
            }
            emitStatError(why);
            kvmClose(sock);
            return res_status == HS_ERR_BUSY ? KVM_RUN_BUSY : KVM_RUN_DENIED;
        }

        if (first != TVHA_MAGIC) {
            emitStatError("拼接握手失败（对方不是 Mivara 接收端）");
            kvmClose(sock);
            return KVM_RUN_PROTOCOL;
        }

        HsAuthStart as = {};
        as.magic = first_raw;
        rv = kvmRecvAllPoll(sock, (char *)&as + sizeof(uint32_t),
                            sizeof(as) - sizeof(uint32_t), running, 35000);
        if (rv <= 0) {
            if (running.load())
                emitStatError("拼接授权握手失败（对方应答不完整）");
            kvmClose(sock);
            return KVM_RUN_TRANSPORT;
        }
        if (ntohl(as.version) != TVHA_VERSION) {
            emitStatError("拼接协议版本不一致，请将两端更新到同一版本");
            kvmClose(sock);
            return KVM_RUN_PROTOCOL;
        }
        const uint32_t auth_mode = ntohl(as.mode);
        if (auth_mode != AUTH_MODE_OPEN && auth_mode != AUTH_MODE_PASSWORD &&
            auth_mode != AUTH_MODE_MANUAL) {
            if (auth && auth->report)
                auth->report("cancelled", nullptr, auth->ctx);
            emitStatError("对方下发的授权方式无效");
            kvmClose(sock);
            return KVM_RUN_PROTOCOL;
        }

        uint32_t auth_status = 0;
        std::string why;
        const AuthOutcome ao = kvmClientAuthorize(
            sock, hello, as, running, auth, auth_status, why);
        if (ao != AuthOutcome::Ok) {
            if (auth && auth->report && ao != AuthOutcome::Cancelled)
                auth->report("cancelled", nullptr, auth->ctx);
            if (ao == AuthOutcome::Rejected) kvmAuthErrorText(auth_status, why);
            if (running.load()) emitStatError(why);
            kvmClose(sock);
            if (ao == AuthOutcome::Transport) return KVM_RUN_TRANSPORT;
            if (ao == AuthOutcome::Protocol) return KVM_RUN_PROTOCOL;
            if (ao == AuthOutcome::Cancelled) return KVM_RUN_DENIED;
            return auth_status == HS_ERR_BUSY ? KVM_RUN_BUSY
                                              : KVM_RUN_DENIED;
        }

        reply = KvmReply{};
        rv = kvmRecvAllPoll(sock, &reply, sizeof(reply), running, 35000);
        if (rv <= 0) {
            if (running.load())
                emitStatError("拼接授权后未收到会话参数");
            kvmClose(sock);
            return KVM_RUN_TRANSPORT;
        }
        if (ntohl(reply.magic) != KVM_MAGIC) {
            if (running.load()) emitStatError("拼接授权后应答格式无效");
            kvmClose(sock);
            return KVM_RUN_PROTOCOL;
        }
        if (ntohl(reply.version) != KVM_VERSION) {
            emitStatError("拼接协议版本不一致，请将两端更新到同一版本");
            kvmClose(sock);
            return KVM_RUN_PROTOCOL;
        }
        status = ntohl(reply.status);
        if (status != KVM_OK) {
            if (auth && auth->report)
                auth->report("cancelled", nullptr, auth->ctx);
            kvmAuthErrorText(status, why);
            emitStatError(why);
            kvmClose(sock);
            return status == KVM_ERR_BUSY ? KVM_RUN_BUSY : KVM_RUN_DENIED;
        }
        if (auth && auth->report) auth->report("granted", nullptr, auth->ctx);

        if (!readDropToken(sock, reply, running)) {
            emitStatError("拼接文件通道握手失败");
            kvmClose(sock);
            return KVM_RUN_TRANSPORT;
        }
        out_sock = sock;
        out_reply = reply;
        heartbeat = (ntohl(reply.reserved) & KVM_FEATURE_HEARTBEAT) != 0;
        return KVM_RUN_OK;
    }

    if (status == KVM_ERR_DENIED) emitStatError("对方拒绝了拼接请求");
    else if (status == KVM_ERR_BUSY) emitStatError("对方已有一个拼接会话");
    else emitStatError("拼接请求被拒绝（错误码 " + std::to_string(status) + "）");
    return status == KVM_ERR_BUSY ? KVM_RUN_BUSY
                                 : status == KVM_ERR_DENIED ? KVM_RUN_DENIED
                                                            : KVM_RUN_PROTOCOL;
}

/* ── session ─────────────────────────────────────────────────────────────── */

void resetSessionState(int side)
{
    const char *allow_abs = getenv("MIVARA_KVM_ALLOW_ABS");
    g_allow_abs = allow_abs && std::strcmp(allow_abs, "1") == 0;
    const char *force_abs = getenv("MIVARA_KVM_FORCE_ABS");
    g_force_abs_motion = force_abs && std::strcmp(force_abs, "1") == 0;
    const char *trace = getenv("MIVARA_KVM_TRACE");
    g_trace = trace && std::strcmp(trace, "1") == 0;
    g_queue.reset();
    g_netOK = true;
    g_drainQuit = false;
    g_loss_code = LOSS_NONE;
    g_loss_err = 0;
    g_ctrl_session_start = 0;
    g_side_req.store(-1);
    g_side = (side >= (int)KVM_SIDE_LEFT && side <= (int)KVM_SIDE_BOTTOM)
                 ? side : (int)KVM_SIDE_RIGHT;
    g_state = ControlState::Local;
    g_remote_x = g_remote_y = 0;
    g_motion_poll = false;
    g_poll_x = g_poll_y = 0;
    g_edge_overshoot = 0;
    g_leave_tick = 0;
    g_grab_fail_tick = 0;
    g_cursor_hidden = false;
    std::memset(g_key_down, 0, sizeof(g_key_down));
    std::memset(g_key_local, 0, sizeof(g_key_local));
    std::memset(g_btn_down, 0, sizeof(g_btn_down));
    std::memset(g_btn_local, 0, sizeof(g_btn_local));
    g_ctrl = g_alt = g_shift = false;
    g_panic_key = -1;
    g_motion_acc_x = g_motion_acc_y = 0.0;
    g_dev_count = 0;
    std::memset(g_btn_grabbed, 0, sizeof(g_btn_grabbed));
    g_btn_grab_warned = false;
    g_root_input_ok = true;
    g_x_error = false;
    g_drop_enabled = false;
    g_drop_ready = false;
    g_reverse_enabled = false;
    g_reverse_ok = false;
    g_drag_icon_ok = g_drag_image_ok = false;
    g_reverse_drag.store(false);
    g_rev_cancel_req.store(false);
    g_rev_held = false;
    g_rev_pointer_grab = false;
    g_rev_visible = false;
    g_rev_applied = ~0ull;
    g_rev_last_count = -1;
    revReset();
    std::memset(g_drop_token, 0, sizeof(g_drop_token));
    g_peer_ip.clear();
    g_drop_run.store(false);
    dropResetDrag();
    g_wake[0] = g_wake[1] = -1;
}

} /* namespace */

int kvmRunController(const char *peer_ip, int peer_port, int side,
                     const std::atomic<bool> &running,
                     const KvmClientAuth *auth)
{
    if (!peer_ip || !*peer_ip) return KVM_RUN_PROTOCOL;

    resetSessionState(side);
    g_drop_enabled = auth && auth->file_drop;
    g_reverse_enabled = auth && auth->accept_files;
    g_peer_ip = peer_ip;

    kvm_sock_t sock = KVM_INVALID_SOCK;
    KvmReply reply = {};
    bool heartbeat = false;
    const int hs = connectHandshake(peer_ip, peer_port, g_side, running, auth,
                                    sock, reply, heartbeat);
    if (hs != KVM_RUN_OK) return hs;

    g_tgt_x0 = (int)(int32_t)ntohl((uint32_t)reply.screen_x);
    g_tgt_y0 = (int)(int32_t)ntohl((uint32_t)reply.screen_y);
    g_tgt_w = ntohl(reply.screen_w);
    g_tgt_h = ntohl(reply.screen_h);
    if (g_tgt_w < 1) g_tgt_w = 1;
    if (g_tgt_h < 1) g_tgt_h = 1;
    if (g_tgt_w > 32768) g_tgt_w = 32768;
    if (g_tgt_h > 32768) g_tgt_h = 32768;

    std::string why;
    if (!initX11(why)) {
        emitStatError("拼接初始化失败（" + why + "）");
        kvmClose(sock);
        return KVM_RUN_UNSUPPORTED;
    }

    if (g_drop_ready) {
        dropStripInit();
        dropStripSetSide(g_side);
    }

    if (pipe(g_wake) != 0) {
        emitStatError("拼接初始化失败（唤醒管道）");
        dropStripDestroy();
        closeX11();
        kvmClose(sock);
        return KVM_RUN_TRANSPORT;
    }
    fcntl(g_wake[0], F_SETFL, fcntl(g_wake[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(g_wake[1], F_SETFL, fcntl(g_wake[1], F_GETFL, 0) | O_NONBLOCK);

    bool reverse_started = false;
    if (g_reverse_ok) {
        dropGateInstall(g_drop_token, /*epoch=*/1);
        dropServiceStatusReset();
        g_reverse_service_run.store(true);
        g_reverse_service = std::thread([] {
            dropServiceRun(g_reverse_service_run,
                           (uint16_t)DROP_REVERSE_PORT);
        });
        int st = 0;
        for (int i = 0; i < 100 && st == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            st = dropServiceStatus().state;
        }
        if (st == 1) {
            reverse_started = true;
            emitStat("event=drop|state=reverse_ready|port=" +
                     std::to_string((int)DROP_REVERSE_PORT));
        } else {
            emitLog("reverse file-drop listener failed on port " +
                    std::to_string((int)DROP_REVERSE_PORT) + " (state=" +
                    std::to_string(st) + ")");
            g_reverse_service_run.store(false);
            if (g_reverse_service.joinable()) g_reverse_service.join();
            dropGateClear(/*epoch=*/1);
            g_reverse_ok = false;
            emitStat("event=drop|state=reverse_error|port=" +
                     std::to_string((int)DROP_REVERSE_PORT));
        }
    }

    std::thread drain(drainLoop, sock, heartbeat, std::cref(running));

    g_ctrl_session_start = kvmNowMs();
    emitStat("event=start|mode=stitch|dst=" + std::string(peer_ip));
    emitStitchState("local");
    std::cout << "Stitch controller active – peer desktop " << g_tgt_w << "x"
              << g_tgt_h << " at (" << g_tgt_x0 << "," << g_tgt_y0 << ").\n"
              << "Peer is on the " << kvmSideKey(g_side)
              << ": push the cursor past that edge to control it "
                 "(Ctrl+Alt+Shift+Q releases).\n"
              << std::flush;

    const int xfd = ConnectionNumber(g_dpy);
    while (running.load()) {
        const int req = g_side_req.exchange(-1);
        if (req >= 0) applySide(req);
        handleRevCancel();
        syncReverseGhost();
        pollRemoteMotion();
        if (!g_netOK.load()) break;

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(xfd, &rd);
        /* 100 Hz while a drag/held button needs pointer polling; 20 Hz is
         * enough for the idle side/running checks. */
        struct timeval tv = {0, g_motion_poll ? 10000 : 50000};
        const int sel = select(xfd + 1, &rd, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            g_loss_code.store(LOSS_SELECT_ERROR, std::memory_order_relaxed);
            g_loss_err.store(errno, std::memory_order_relaxed);
            g_netOK.store(false, std::memory_order_relaxed);
            break;
        }
        if (sel == 0) {
            handleRevCancel();
            syncReverseGhost();
            continue;
        }
        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            handleEvent(ev);
        }
        handleRevCancel();
        syncReverseGhost();
    }

    cancelReverseDrag(true);
    if (reverse_started) {
        g_reverse_service_run.store(false);
        if (g_reverse_service.joinable()) g_reverse_service.join();
        dropGateClear(/*epoch=*/1);
        reverse_started = false;
    }
    g_reverse_ok = false;
    g_drop_run.store(false);
    if (g_drop_thread.joinable()) g_drop_thread.join();
    dropStripDestroy();
    mivaraSecureZero(g_drop_token, sizeof(g_drop_token));
    g_drop_ready = false;
    leaveRemote(false);
    g_drainQuit = true;
    shutdown(sock, SHUT_RDWR);
    if (drain.joinable()) drain.join();
    closeX11();
    if (g_wake[0] >= 0) close(g_wake[0]);
    if (g_wake[1] >= 0) close(g_wake[1]);
    g_wake[0] = g_wake[1] = -1;
    kvmClose(sock);

    if (!g_netOK.load() && running.load()) return KVM_RUN_LOST;
    emitStat("event=stop|mode=stitch");
    return KVM_RUN_OK;
}

/* ── resilient wrapper ───────────────────────────────────────────────────── */

static bool sleepRetry(int ms, const std::atomic<bool> &running)
{
    const uint64_t deadline = kvmNowMs() + (uint64_t)ms;
    while (running.load()) {
        const uint64_t now = kvmNowMs();
        if (now >= deadline) return true;
        const uint64_t left = deadline - now;
        kvmSleepMs((unsigned)(left < 200 ? left : 200));
    }
    return false;
}

int kvmRunControllerResilient(const char *peer_ip, int peer_port, int side,
                              const std::atomic<bool> &running,
                              const KvmClientAuth *auth)
{
    bool had_session = false;
    int retry = 0;
    int busy_streak = 0;
    int last_loss = LOSS_NONE;
    int last_err = 0;
    for (;;) {
        const int rc = kvmRunController(peer_ip, peer_port, side, running,
                                        auth);
        if (!running.load()) return KVM_RUN_OK;
        if (rc == KVM_RUN_OK || rc == KVM_RUN_DENIED ||
            rc == KVM_RUN_UNSUPPORTED || rc == KVM_RUN_PROTOCOL)
            return rc;
        if (rc == KVM_RUN_BUSY) {
            if (++busy_streak >= 20) {
                emitStatError("对方正在接收其他连接，已停止自动重连");
                return KVM_RUN_BUSY;
            }
        } else {
            busy_streak = 0;
        }
        if (rc == KVM_RUN_LOST) {
            last_loss = g_loss_code.load(std::memory_order_relaxed);
            last_err = g_loss_err.load(std::memory_order_relaxed);
            if (last_loss == LOSS_PROTOCOL_EVENT) {
                emitStatError("拼接事件流校验失败，请将两端更新到同一版本");
                return KVM_RUN_PROTOCOL;
            }
            had_session = true;
            const uint64_t lasted = g_ctrl_session_start
                                        ? kvmNowMs() - g_ctrl_session_start
                                        : 0;
            if (lasted >= 30000) retry = 1;
            else if (retry < 16) ++retry;
            if (retry < 1) retry = 1;
        } else if (!had_session) {
            return rc;
        } else if (retry < 16) {
            ++retry;
        }
        int delay_ms = 1000;
        for (int i = 1; i < retry && delay_ms < 30000; ++i) delay_ms *= 2;
        if (delay_ms > 30000) delay_ms = 30000;

        int side_now = g_side;
        const int queued = g_side_req.load(std::memory_order_relaxed);
        if (queued >= (int)KVM_SIDE_LEFT && queued <= (int)KVM_SIDE_BOTTOM)
            side_now = queued;
        side = side_now;

        emitStat(std::string("event=stitch|state=reconnecting|mode=stitch|side=") +
                 kvmSideKey(side_now) + "|reason=" + lossToken(last_loss) +
                 "|err=" + std::to_string(last_err) +
                 "|retry=" + std::to_string(retry) +
                 "|delay_ms=" + std::to_string(delay_ms));
        emitLog("reconnect attempt " + std::to_string(retry) + " in " +
                std::to_string(delay_ms) + " ms (reason=" +
                lossToken(last_loss) +
                (last_err ? ", err=" + std::to_string(last_err) : "") + ")");
        if (!sleepRetry(delay_ms, running)) return KVM_RUN_OK;
    }
}

void kvmSetControllerSide(int side)
{
    if (side < (int)KVM_SIDE_LEFT || side > (int)KVM_SIDE_BOTTOM) return;
    g_side_req.store(side);
}

#endif /* __linux__ */
