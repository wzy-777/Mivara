/**
 * KVM.CPP – 拼接 (stitch) input channel.
 *
 * Two roles share this file:
 *
 *  - kvmRunController()  (bin/sender --mode stitch)
 *      Connects to the target's TCP 8084, waits for consent, then installs
 *      WH_MOUSE_LL / WH_KEYBOARD_LL plus a RIDEV_INPUTSINK Raw Input window.
 *      The Local/Remote state machine lives on that one message-loop thread:
 *      "Local" passes every event through untouched; pushing the cursor past
 *      the right edge of the local virtual desktop enters "Remote", where all
 *      local input is swallowed and forwarded to the target. Dragging the
 *      target cursor back over its left edge (or Ctrl+Alt+Shift+Q) returns.
 *
 *      Why Raw Input: once the cursor is clamped at the screen edge the
 *      low-level hook keeps reporting the clamped position, so pt/lLast*
 *      cannot tell "still pushing right" from "standing still". WM_INPUT
 *      still carries the device's relative delta, which is what drives both
 *      the edge crossing and the remote cursor.
 *
 *  - kvmServiceRun()     (bin/receiver)
 *      Listens on TCP 8084 and serves one controller. Before answering it
 *      asks the local user for consent with a topmost MessageBox (default
 *      No, auto-dismissed after 30 s). Accepted events are replayed with
 *      SendInput (absolute virtual-desktop moves + scancode key events).
 *      Every exit path sends a release-all so no key/button stays stuck.
 *
 * Threading / latency: a hook callback must never block on the network (a
 * low-level hook that overruns LowLevelHooksTimeout is silently removed).
 * The callback only appends to a small SPSC ring; a drain thread owns the
 * socket and sends. A send failure is posted back to the hook thread, which
 * releases remote control and quits.
 *
 * Windows only. On other platforms kvmRunController() reports that 拼接 is
 * unsupported and kvmServiceRun() returns immediately.
 */
#include "kvm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mstcpip.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* Side naming (kvmSideKey / kvmSideFromKey) is inline in kvm.h: `app`
 * includes the header for KVM_PORT but never links this translation unit. */

#ifdef _WIN32

/* ── small helpers ───────────────────────────────────────────────────────── */

typedef SOCKET kvm_sock_t;
#define KVM_INVALID_SOCK INVALID_SOCKET

static void kvmClose(kvm_sock_t s)
{
    if (s != KVM_INVALID_SOCK) closesocket(s);
}

static bool kvmSendAll(kvm_sock_t s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        int n = send(s, p + off, (int)(len - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static void kvmSetTimeouts(kvm_sock_t s, DWORD recv_ms, DWORD send_ms)
{
    if (recv_ms) setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&recv_ms,
                            sizeof(recv_ms));
    if (send_ms) setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_ms,
                            sizeof(send_ms));
}

/* A software KVM is silent whenever the user is idle, so an unplugged or
 * powered-off controller never produces a FIN/RST. TCP keepalive is the only
 * thing that notices; without it the target sits in serveClient forever and
 * the single session slot is wedged (and held modifiers stay down). */
static void kvmEnableKeepAlive(kvm_sock_t s)
{
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof(on));
    tcp_keepalive ka = {};
    ka.onoff = 1;
    ka.keepalivetime = 15000;   /* idle before the first probe  */
    ka.keepaliveinterval = 3000; /* between probes (10 on Win)   */
    DWORD ret = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &ret,
             nullptr, nullptr);
}

/* recv exactly len bytes, checking *running between 200 ms waits.
 * 1 = ok, 0 = deadline, -1 = closed/error/caller stopped. */
static int kvmRecvAllPoll(kvm_sock_t s, void *buf, size_t len,
                          const std::atomic<bool> &running, int timeout_ms)
{
    char *p = (char *)buf;
    size_t off = 0;
    const DWORD start = GetTickCount();
    while (off < len) {
        if (!running.load()) return -1;
        if (timeout_ms > 0 && (int)(GetTickCount() - start) > timeout_ms)
            return 0;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {0, 200000};
        int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel < 0) return -1;
        if (sel == 0) continue;
        int n = recv(s, p + off, (int)(len - off), 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 1;
}

static int clampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool isExtendedVk(DWORD vk)
{
    switch (vk) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_SNAPSHOT:
    case VK_APPS:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

/* ── controller ──────────────────────────────────────────────────────────── */

namespace {

/* SPSC ring between the hook thread and the socket drain thread. Mouse moves
 * are coalesced on the consumer side: a run of pending moves collapses to the
 * newest one, so a 1000 Hz mouse cannot grow the queue without bound. The
 * producer must never touch a published slot — an in-place "overwrite the
 * newest" optimisation races with the drain thread copying that same slot
 * (the consumer can advance tail between the producer's tail load and its
 * write, tearing a coordinate pair). */
class EventQueue {
public:
    static const unsigned CAP = 4096;

    bool push(const KvmEvent &e)
    {
        unsigned h = head.load(std::memory_order_relaxed);
        unsigned t = tail.load(std::memory_order_acquire);
        if (h - t >= CAP) return false;
        buf[h % CAP] = e;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pushMove(uint32_t x, uint32_t y)
    {
        KvmEvent e = {};
        e.type = KVM_EV_MOVE;
        e.a = x;
        e.b = y;
        return push(e);
    }

    bool pop(KvmEvent &e)
    {
        unsigned t = tail.load(std::memory_order_relaxed);
        unsigned h = head.load(std::memory_order_acquire);
        if (t == h) return false;
        e = buf[t % CAP];
        ++t;
        /* Consumer side is the only tail writer, so reading past t up to h is
         * safe; keep the last move of a contiguous run. */
        while (t != h) {
            const KvmEvent &n = buf[t % CAP];
            if (e.type != KVM_EV_MOVE || n.type != KVM_EV_MOVE) break;
            e = n;
            ++t;
        }
        tail.store(t, std::memory_order_release);
        return true;
    }

private:
    KvmEvent buf[CAP] = {};
    std::atomic<unsigned> head{0};
    std::atomic<unsigned> tail{0};
};

enum class ControlState { Local, Remote };

EventQueue              g_queue;
std::atomic<bool>       g_netOK{true};
std::atomic<bool>       g_heartbeat{false};
const std::atomic<bool> *g_running = nullptr;   /* true = keep the session */
kvm_sock_t              g_sock = KVM_INVALID_SOCK;

HWND  g_hwnd = nullptr;
HHOOK g_hMouse = nullptr;
HHOOK g_hKey = nullptr;
std::thread g_drain;
std::atomic<bool> g_drainQuit{false};

ControlState g_state = ControlState::Local;
int      g_tgt_x0 = 0;          /* target virtual-desktop origin */
int      g_tgt_y0 = 0;
uint32_t g_tgt_w = 1920;
uint32_t g_tgt_h = 1080;
int      g_remote_x = 0;        /* cursor inside the target desktop */
int      g_remote_y = 0;
int      g_edge_overshoot = 0;
DWORD    g_leave_tick = 0;
bool     g_cursor_hidden = false;

/* Which local edge hands control to the peer. Written by the message-loop
 * thread (and only read there); kvmSetControllerSide() queues a change in
 * g_side_req so the stdin thread never touches the hook-thread state. */
int              g_side = KVM_SIDE_RIGHT;
std::atomic<int> g_side_req{-1};

/* Held-input bookkeeping (hook thread only). Needed to re-arm a modifier or
 * button that was pressed before control crossed to the target. */
bool g_key_down[256] = {};
DWORD g_key_scan[256] = {};
bool g_key_ext[256] = {};
bool g_btn_down[8] = {};

/* Whether the matching DOWN was actually delivered to the local desktop.
 * Control can cross while a key or button is still held: the press reached
 * this machine, so its release has to reach it too, even though it happens
 * while the hook is in Remote and would otherwise be swallowed. Without this,
 * a modifier stays logically down locally (or a window keeps its drag /
 * capture state) until the user presses and releases it again. A press made
 * in Remote was swallowed locally, so its release is not passed through. */
bool g_key_local[256] = {};
bool g_btn_local[8] = {};

#define KVM_WM_DISCONNECT (WM_APP + 1)
#define KVM_WM_PANIC      (WM_APP + 2)
#define KVM_TIMER_ID      1

static void emitStat(const std::string &line)
{
    std::cout << "STAT|" << line << "\n" << std::flush;
}

static void emitStatError(const std::string &msg)
{
    emitStat("event=error|msg=" + msg);
}

/* state=…|side=… — the GUI renders the local/remote hint and the active
 * direction chip from this, so every crossing carries the current side. */
static void emitStitchState(const char *state)
{
    emitStat(std::string("event=stitch|state=") + state +
             "|mode=stitch|side=" + kvmSideKey(g_side));
}

static void setCursorVisible(bool visible)
{
    if (visible) {
        if (g_cursor_hidden) {
            while (ShowCursor(TRUE) < 0) {}
            g_cursor_hidden = false;
        }
    } else if (!g_cursor_hidden) {
        while (ShowCursor(FALSE) >= 0) {}
        g_cursor_hidden = true;
    }
}

static void virtualDesktop(int &x, int &y, int &w, int &h)
{
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
}

static void queueKey(uint32_t down, uint32_t vk, uint32_t scan, bool extended)
{
    KvmEvent e = {};
    e.type = KVM_EV_KEY;
    e.a = down;
    e.b = vk;
    e.c = (scan & 0xFFFFu) | (extended ? 0x10000u : 0u);
    g_queue.push(e);
}

/* Physical button state is tracked in BOTH modes: a press or release can
 * straddle a mode switch, and a stale entry is replayed by syncHeldInput on
 * the next entry (phantom drag on the peer / a local window left with a
 * captured button). */
static void noteButton(uint32_t btn, bool down)
{
    if (btn < 8) g_btn_down[btn] = down;
}

static void queueButton(uint32_t btn, bool down)
{
    KvmEvent e = {};
    e.type = KVM_EV_BUTTON;
    e.a = btn;
    e.b = down ? 1u : 0u;
    g_queue.push(e);
    noteButton(btn, down);
}

static void queueWheel(int delta, bool horizontal)
{
    KvmEvent e = {};
    e.type = KVM_EV_WHEEL;
    e.a = (uint32_t)delta;
    e.b = horizontal ? 1u : 0u;
    g_queue.push(e);
}

/* Re-arm every key/button the user is physically holding, so the target
 * starts from the same modifier state as the controller. */
static void syncHeldInput()
{
    for (int vk = 0; vk < 256; vk++)
        if (g_key_down[vk])
            queueKey(1, (uint32_t)vk, g_key_scan[vk], g_key_ext[vk]);
    for (int b = 1; b < 8; b++)
        if (g_btn_down[b]) queueButton((uint32_t)b, true);
}

static void enterRemote()
{
    if (g_state == ControlState::Remote) return;

    POINT p = {};
    GetCursorPos(&p);
    int vx, vy, vw, vh;
    virtualDesktop(vx, vy, vw, vh);

    /* Enter at the target's opposite edge (the one that faces this machine),
     * with the along-edge coordinate mapped proportionally from wherever the
     * local cursor is. For a side-by-side layout this is the old behaviour:
     * push past the local right edge, appear at the target's left edge. */
    const int px = clampInt(p.x - vx, 0, vw - 1);
    const int py = clampInt(p.y - vy, 0, vh - 1);
    const int rx = (int)(((long long)px * (long long)g_tgt_w) / vw);
    const int ry = (int)(((long long)py * (long long)g_tgt_h) / vh);
    switch (g_side) {
    case KVM_SIDE_LEFT:   g_remote_x = (int)g_tgt_w - 1; g_remote_y = ry; break;
    case KVM_SIDE_TOP:    g_remote_x = rx; g_remote_y = (int)g_tgt_h - 1; break;
    case KVM_SIDE_BOTTOM: g_remote_x = rx; g_remote_y = 0; break;
    default:              g_remote_x = 0; g_remote_y = ry; break;
    }
    g_remote_x = clampInt(g_remote_x, 0, (int)g_tgt_w - 1);
    g_remote_y = clampInt(g_remote_y, 0, (int)g_tgt_h - 1);

    g_state = ControlState::Remote;
    setCursorVisible(false);
    g_queue.pushMove((uint32_t)g_remote_x, (uint32_t)g_remote_y);
    syncHeldInput();
    emitStitchState("remote");
}

static void leaveRemote(bool warpCursor)
{
    if (g_state != ControlState::Remote) return;
    g_state = ControlState::Local;

    KvmEvent e = {};
    e.type = KVM_EV_RELEASE_ALL;
    g_queue.push(e);

    if (warpCursor) {
        /* Park the pointer on the local edge the peer is attached to, so it
         * reappears where the user pushed out (and knows where to push
         * again). The along-edge coordinate maps back the same way. */
        int vx, vy, vw, vh;
        virtualDesktop(vx, vy, vw, vh);
        int px, py;
        if (g_side == KVM_SIDE_LEFT || g_side == KVM_SIDE_RIGHT) {
            py = (int)(((long long)g_remote_y * (long long)vh) / g_tgt_h) + vy;
            py = clampInt(py, vy, vy + vh - 1);
            px = (g_side == KVM_SIDE_RIGHT) ? vx + vw - 1 : vx;
        } else {
            px = (int)(((long long)g_remote_x * (long long)vw) / g_tgt_w) + vx;
            px = clampInt(px, vx, vx + vw - 1);
            py = (g_side == KVM_SIDE_BOTTOM) ? vy + vh - 1 : vy;
        }
        SetCursorPos(px, py);
    }
    setCursorVisible(true);
    g_leave_tick = GetTickCount();
    g_edge_overshoot = 0;
    emitStitchState("local");
}

static void moveRemote(int dx, int dy)
{
    int nx = g_remote_x + dx;
    int ny = g_remote_y + dy;

    /* Pushing back out of the edge the cursor came in from hands control to
     * the controller: left edge for a peer on the right, and so on. */
    bool back = false;
    switch (g_side) {
    case KVM_SIDE_LEFT:   back = (nx > (int)g_tgt_w - 1); break;
    case KVM_SIDE_TOP:    back = (ny > (int)g_tgt_h - 1); break;
    case KVM_SIDE_BOTTOM: back = (ny < 0);                break;
    default:              back = (nx < 0);                break;
    }
    if (back) {
        leaveRemote(true);
        return;
    }
    nx = clampInt(nx, 0, (int)g_tgt_w - 1);
    ny = clampInt(ny, 0, (int)g_tgt_h - 1);
    g_remote_x = nx;
    g_remote_y = ny;
    g_queue.pushMove((uint32_t)nx, (uint32_t)ny);
}

/* Applies a queued side change on the message-loop thread. Returning control
 * first is deliberate: the way back just changed, and the target must not be
 * left holding a modifier while the user re-orients. */
static void applySide(int side)
{
    if (side < (int)KVM_SIDE_LEFT || side > (int)KVM_SIDE_BOTTOM ||
        side == g_side) return;
    if (g_state == ControlState::Remote) leaveRemote(true);
    g_side = side;
    g_edge_overshoot = 0;
    emitStitchState("local");
    std::cout << "[kvm] stitch side -> " << kvmSideKey(g_side) << "\n"
              << std::flush;
}

static bool releaseHotkey()
{
    const bool ctrl = g_key_down[VK_LCONTROL & 0xFF] || g_key_down[VK_RCONTROL & 0xFF];
    const bool alt  = g_key_down[VK_LMENU & 0xFF]    || g_key_down[VK_RMENU & 0xFF];
    const bool shift = g_key_down[VK_LSHIFT & 0xFF]  || g_key_down[VK_RSHIFT & 0xFF];
    return ctrl && alt && shift;
}

/* ── hook callbacks ──────────────────────────────────────────────────────── */

LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code < 0) return CallNextHookEx(nullptr, code, wp, lp);
    auto *ms = (MSLLHOOKSTRUCT *)lp;

    /* Which physical button this message is about (0 = not a button event). */
    int btn = 0;
    bool down = false;
    switch (wp) {
    case WM_LBUTTONDOWN: btn = 1; down = true;  break;
    case WM_LBUTTONUP:   btn = 1; down = false; break;
    case WM_RBUTTONDOWN: btn = 2; down = true;  break;
    case WM_RBUTTONUP:   btn = 2; down = false; break;
    case WM_MBUTTONDOWN: btn = 3; down = true;  break;
    case WM_MBUTTONUP:   btn = 3; down = false; break;
    case WM_XBUTTONDOWN:
        btn = HIWORD(ms->mouseData) == XBUTTON2 ? 5 : 4; down = true;  break;
    case WM_XBUTTONUP:
        btn = HIWORD(ms->mouseData) == XBUTTON2 ? 5 : 4; down = false; break;
    default: break;
    }

    /* Track the physical buttons whatever the mode is — see noteButton. */
    if (btn && !(ms->flags & LLMHF_INJECTED)) {
        noteButton((uint32_t)btn, down);
        /* Remember whether the press actually reached the local desktop; see
         * g_btn_local. A press made in Remote stays false (it was swallowed). */
        if (g_state != ControlState::Remote) g_btn_local[btn] = down;
    }

    if (g_state != ControlState::Remote)
        return CallNextHookEx(nullptr, code, wp, lp);

    /* Synthetic clicks (automation / remote-control tools) are not ours to
     * forward, matching the keyboard hook. */
    if (ms->flags & LLMHF_INJECTED)
        return CallNextHookEx(nullptr, code, wp, lp);

    switch (wp) {
    case WM_MOUSEMOVE:
        /* Raw Input already forwarded the motion; the hook only suppresses. */
        return 1;
    case WM_MOUSEWHEEL:
        queueWheel((int)(short)HIWORD(ms->mouseData), false);
        return 1;
    case WM_MOUSEHWHEEL:
        queueWheel((int)(short)HIWORD(ms->mouseData), true);
        return 1;
    default:
        break;
    }
    if (!btn) return 1;

    queueButton((uint32_t)btn, down);
    if (!down && g_btn_local[btn]) {
        /* The press went to a local window (control crossed while the button
         * was already down), so the release must clear it there too —
         * otherwise a drag or a captured selection never ends. */
        g_btn_local[btn] = false;
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    return 1;
}

LRESULT CALLBACK keyProc(int code, WPARAM wp, LPARAM lp)
{
    if (code < 0) return CallNextHookEx(nullptr, code, wp, lp);
    auto *kb = (KBDLLHOOKSTRUCT *)lp;
    const bool up = (kb->flags & LLKHF_UP) != 0;
    const DWORD vk = kb->vkCode;
    const bool injected = (kb->flags & LLKHF_INJECTED) != 0;

    if (vk < 256 && !injected) {
        g_key_down[vk] = !up;
        if (!up) {
            g_key_scan[vk] = kb->scanCode;
            g_key_ext[vk] = (kb->flags & LLKHF_EXTENDED) != 0;
        }
        /* Only a press made in Local reached this desktop (see g_key_local). */
        if (g_state != ControlState::Remote) g_key_local[vk] = !up;
    }

    if (g_state != ControlState::Remote)
        return CallNextHookEx(nullptr, code, wp, lp);

    /* Ctrl+Alt+Shift+Q is the local panic key: swallowed, never forwarded.
     * leaveRemote() must not run inside the hook callback (ShowCursor,
     * SetCursorPos and a flushed stdout write can overrun
     * LowLevelHooksTimeout); defer it to the message loop. */
    if (!up && !injected && (vk == 'Q' || vk == 'q') && releaseHotkey()) {
        if (g_hwnd) PostMessageW(g_hwnd, KVM_WM_PANIC, 0, 0);
        return 1;
    }
    /* Input the system/another tool injected is not ours to forward. */
    if (injected) return CallNextHookEx(nullptr, code, wp, lp);

    queueKey(up ? 0 : 1, (uint32_t)vk, kb->scanCode,
             (kb->flags & LLKHF_EXTENDED) != 0);
    /* The physical release happened while control was Remote, but the local OS
     * still believes the key (usually a modifier) is held: let the up through
     * as well, or every later local keystroke carries the stale modifier. */
    if (up && vk < 256 && g_key_local[vk]) {
        g_key_local[vk] = false;
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    return 1;
}

static void onRawMouse(const RAWMOUSE &m)
{
    /* Windows clamps the cursor to the virtual desktop, so the low-level hook
     * keeps reporting a stationary point when the user pushes further right.
     * The device delta in WM_INPUT is the only reliable signal. */
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
        /* RDP / tablet absolute devices are not worth emulating: the local
         * cursor already follows them, but remote deltas cannot be derived
         * without extra state. Physical mice are always relative. */
        return;
    }
    const int dx = (int)m.lLastX;
    const int dy = (int)m.lLastY;

    if (g_state == ControlState::Remote) {
        if (dx || dy) moveRemote(dx, dy);
        return;
    }

    /* Only motion further *out* of the chosen edge counts; any other
     * direction (including moving back into the desktop) resets the
     * accumulator so a stray jitter cannot cross. */
    const bool horizontal = (g_side == KVM_SIDE_LEFT || g_side == KVM_SIDE_RIGHT);
    const bool outward_positive = (g_side == KVM_SIDE_RIGHT ||
                                   g_side == KVM_SIDE_BOTTOM);
    const int d = horizontal ? dx : dy;
    if (d == 0 || (d > 0) != outward_positive) {
        g_edge_overshoot = 0;
        return;
    }
    POINT p = {};
    if (!GetCursorPos(&p)) return;
    int vx, vy, vw, vh;
    virtualDesktop(vx, vy, vw, vh);
    const bool at_edge =
        horizontal ? (outward_positive ? p.x >= vx + vw - 1 : p.x <= vx)
                   : (outward_positive ? p.y >= vy + vh - 1 : p.y <= vy);
    if (!at_edge) {
        g_edge_overshoot = 0;
        return;
    }
    if ((DWORD)(GetTickCount() - g_leave_tick) < 250) return;
    g_edge_overshoot += (d > 0 ? d : -d);
    if (g_edge_overshoot >= 3) enterRemote();
}

static LRESULT CALLBACK kvmWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INPUT: {
        BYTE raw[512];
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, raw, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            auto *ri = (RAWINPUT *)raw;
            if (ri->header.dwType == RIM_TYPEMOUSE)
                onRawMouse(ri->data.mouse);
        }
        /* Required: the system frees the per-message raw-input buffer in
         * DefWindowProc. Returning without it leaks one allocation per mouse
         * sample (a 1000 Hz mouse leaks indefinitely). */
        DefWindowProcW(hwnd, msg, wp, lp);
        return 0;
    }
    case WM_TIMER:
        if (wp == KVM_TIMER_ID) {
            /* A side switch arrives from the GUI's stdin thread; applying it
             * here keeps SetCursorPos/ShowCursor/STAT on the hook thread. */
            const int req = g_side_req.exchange(-1);
            if (req >= 0) applySide(req);
            if (!g_running || !g_running->load()) {
                leaveRemote(false);
                PostQuitMessage(0);
            }
        }
        return 0;
    case KVM_WM_PANIC:
        leaveRemote(true);
        return 0;
    case KVM_WM_DISCONNECT:
        leaveRemote(true);
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static void drainLoop()
{
    const ULONGLONG beat_ms = 3000, timeout_ms = 9000;
    ULONGLONG last_rx = GetTickCount64();
    ULONGLONG next_beat = last_rx + beat_ms;
    KvmEvent incoming = {};
    size_t incoming_bytes = 0;
    const auto disconnected = [] {
        g_netOK = false;
        if (g_hwnd) PostMessageW(g_hwnd, KVM_WM_DISCONNECT, 0, 0);
    };
    while (!g_drainQuit.load() && (!g_running || g_running->load())) {
        fd_set rd, ex;
        FD_ZERO(&rd);
        FD_ZERO(&ex);
        FD_SET(g_sock, &rd);
        FD_SET(g_sock, &ex);
        timeval tv = {0, 1000};
        const int selected = select(0, &rd, nullptr, &ex, &tv);
        if (selected < 0) { disconnected(); break; }
        if (selected > 0) {
            const int n = recv(g_sock, (char *)&incoming + incoming_bytes,
                               (int)(sizeof(incoming) - incoming_bytes), 0);
            if (n == 0) { disconnected(); break; }
            if (n < 0) {
                const int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINTR) {
                    disconnected();
                    break;
                }
            } else {
                incoming_bytes += (size_t)n;
                last_rx = GetTickCount64();
                if (incoming_bytes == sizeof(incoming)) {
                    const uint32_t type = ntohl(incoming.type);
                    if (type == KVM_EV_PING && g_heartbeat.load()) {
                        KvmEvent pong = {};
                        pong.type = htonl(KVM_EV_PONG);
                        if (!kvmSendAll(g_sock, &pong, sizeof(pong))) {
                            disconnected();
                            break;
                        }
                    } else if (type != KVM_EV_PONG || !g_heartbeat.load()) {
                        disconnected();
                        break;
                    }
                    incoming_bytes = 0;
                }
            }
        }
        if (g_heartbeat.load()) {
            const ULONGLONG now = GetTickCount64();
            if (now - last_rx >= timeout_ms) { disconnected(); break; }
            if (now >= next_beat) {
                KvmEvent ping = {};
                ping.type = htonl(KVM_EV_PING);
                if (!kvmSendAll(g_sock, &ping, sizeof(ping))) {
                    disconnected();
                    break;
                }
                next_beat = now + beat_ms;
            }
        }
        KvmEvent e;
        if (!g_queue.pop(e)) continue;
        KvmEvent wire = {};
        wire.type = htonl(e.type);
        wire.a = htonl(e.a);
        wire.b = htonl(e.b);
        wire.c = htonl(e.c);
        if (!kvmSendAll(g_sock, &wire, sizeof(wire))) {
            disconnected();
            break;
        }
    }
}

/* Connect with a short timeout so a powered-off peer does not hang the
 * launcher for the OS's full TCP retry budget. */
static kvm_sock_t connectTimeout(const char *ip, int port,
                                 const std::atomic<bool> &running, int ms)
{
    kvm_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == KVM_INVALID_SOCK) return KVM_INVALID_SOCK;

    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        kvmClose(s);
        return KVM_INVALID_SOCK;
    }

    int rc = connect(s, (sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        return s;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        kvmClose(s);
        return KVM_INVALID_SOCK;
    }

    const DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < ms) {
        if (!running.load()) {
            kvmClose(s);
            return KVM_INVALID_SOCK;
        }
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        struct timeval tv = {0, 100000};
        int sel = select(0, nullptr, &wr, nullptr, &tv);
        if (sel > 0) {
            int err = 0;
            int len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            if (err != 0) {
                kvmClose(s);
                return KVM_INVALID_SOCK;
            }
            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking);
            return s;
        }
        if (sel < 0) break;
    }
    kvmClose(s);
    return KVM_INVALID_SOCK;
}

} /* namespace */

int kvmRunController(const char *peer_ip, int peer_port, int side,
                     const std::atomic<bool> &running)
{
    if (!peer_ip || !*peer_ip) return 1;

    g_running = &running;
    g_netOK = true;
    g_drainQuit = false;
    g_state = ControlState::Local;
    g_cursor_hidden = false;
    g_edge_overshoot = 0;
    g_leave_tick = 0;
    g_side = (side >= (int)KVM_SIDE_LEFT && side <= (int)KVM_SIDE_BOTTOM)
                 ? side : (int)KVM_SIDE_RIGHT;
    /* A switch can arrive on stdin before this call (the GUI's chips are live
     * while waiting for consent). Honour it before the hello goes out, so the
     * target's consent text and the armed edge agree; later requests stay
     * queued for the message loop. */
    const int queued = g_side_req.exchange(-1);
    if (queued >= (int)KVM_SIDE_LEFT && queued <= (int)KVM_SIDE_BOTTOM)
        g_side = queued;
    memset(g_key_down, 0, sizeof(g_key_down));
    memset(g_btn_down, 0, sizeof(g_btn_down));
    memset(g_key_local, 0, sizeof(g_key_local));
    memset(g_btn_local, 0, sizeof(g_btn_local));

    emitStat("event=stitch|state=waiting|mode=stitch|dst=" +
             std::string(peer_ip) + "|side=" + kvmSideKey(g_side));

    /* ── connect / hello / consent ─────────────────────────────────────── *
     * A bare TCP probe (the GUI and this CLI both probe 8084 before
     * connecting) claims the target's single session slot for the moment its
     * session thread needs to read EOF. If our real connection lands inside
     * that window the target answers KVM_ERR_BUSY even though nobody is
     * using it, so a BUSY on the first attempt gets one retry after the
     * phantom has certainly cleared. A genuinely busy target answers BUSY
     * again and that is reported. */
    KvmReply reply = {};
    uint32_t status = KVM_ERR_BUSY;
    for (int attempt = 0; attempt < 2; attempt++) {
        g_sock = connectTimeout(peer_ip, peer_port, running, 5000);
        if (g_sock == KVM_INVALID_SOCK) {
            emitStatError("无法连接拼接端口 " + std::to_string(peer_port) +
                          "（对方需先启动接收）");
            return 1;
        }
        BOOL nodelay = TRUE;
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay,
                   sizeof(nodelay));
        kvmSetTimeouts(g_sock, 0, 300);
        kvmEnableKeepAlive(g_sock);

        KvmHello hello = {};
        hello.magic = htonl(KVM_MAGIC);
        hello.version = htonl(KVM_VERSION);
        /* reserved[0] = which side the peer is on + 1, so the target's consent
         * dialog can name the correct edge. The +1 keeps 0 free as "not
         * specified": a zero-filled hello from an older build (where the
         * field was reserved) must not decode as KVM_SIDE_LEFT — the target
         * then falls back to the default right side. Purely informational;
         * older targets ignore it and behave like the default (right). */
        hello.reserved[0] = htonl((uint32_t)g_side + 1u);
        hello.reserved[1] = htonl(KVM_FEATURE_HEARTBEAT);
        if (!kvmSendAll(g_sock, &hello, sizeof(hello))) {
            emitStatError("拼接握手发送失败");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }

        reply = KvmReply{};
        const int rv =
            kvmRecvAllPoll(g_sock, &reply, sizeof(reply), running, 35000);
        if (rv == 0) {
            emitStatError("对方未在 35 秒内确认拼接请求");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }
        if (rv < 0) {
            if (running.load()) emitStatError("拼接握手失败（连接已断开）");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }
        if (ntohl(reply.magic) != KVM_MAGIC) {
            emitStatError("拼接握手失败（对方不是 Mivara 接收端）");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }
        if (ntohl(reply.version) != KVM_VERSION) {
            emitStatError("拼接协议版本不一致，请将两端更新到同一版本");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }
        status = ntohl(reply.status);
        if (status != KVM_ERR_BUSY || attempt == 1) break;
        kvmClose(g_sock);
        g_sock = KVM_INVALID_SOCK;
        if (!running.load()) return 1;
        Sleep(300);
    }
    if (status != KVM_OK) {
        if (status == KVM_ERR_DENIED)
            emitStatError("对方拒绝了拼接请求");
        else if (status == KVM_ERR_BUSY)
            emitStatError("对方已有一个拼接会话");
        else
            emitStatError("拼接请求被拒绝（错误码 " + std::to_string(status) + "）");
        kvmClose(g_sock);
        g_sock = KVM_INVALID_SOCK;
        return 2;
    }

    g_heartbeat = (ntohl(reply.reserved) & KVM_FEATURE_HEARTBEAT) != 0;
    g_tgt_x0 = (int)(int32_t)ntohl((uint32_t)reply.screen_x);
    g_tgt_y0 = (int)(int32_t)ntohl((uint32_t)reply.screen_y);
    g_tgt_w = ntohl(reply.screen_w);
    g_tgt_h = ntohl(reply.screen_h);
    /* Never trust the peer with an unbounded range: these values scale every
     * injected coordinate, and a bogus 0xFFFFFFFF would make the clamps
     * below meaningless. No real desktop is larger than 32767 px. */
    if (g_tgt_w < 1) g_tgt_w = 1;
    if (g_tgt_h < 1) g_tgt_h = 1;
    if (g_tgt_w > 32768) g_tgt_w = 32768;
    if (g_tgt_h > 32768) g_tgt_h = 32768;

    /* ── hook thread setup ─────────────────────────────────────────────── */
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t *cls = L"MivaraStitchInput";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = kvmWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    if (!RegisterClassExW(&wc)) {
        emitStatError("拼接初始化失败（窗口类）");
        kvmClose(g_sock);
        g_sock = KVM_INVALID_SOCK;
        return 1;
    }
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"Mivara Stitch Input",
                             0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        emitStatError("拼接初始化失败（隐藏窗口）");
        kvmClose(g_sock);
        g_sock = KVM_INVALID_SOCK;
        return 1;
    }

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;          /* generic desktop */
    rid.usUsage = 0x02;              /* mouse           */
    rid.dwFlags = RIDEV_INPUTSINK;   /* even when not foreground */
    rid.hwndTarget = g_hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        emitStatError("拼接初始化失败（Raw Input）");
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        kvmClose(g_sock);
        g_sock = KVM_INVALID_SOCK;
        return 1;
    }

    g_hMouse = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, inst, 0);
    g_hKey = SetWindowsHookExW(WH_KEYBOARD_LL, keyProc, inst, 0);
    if (!g_hMouse || !g_hKey) {
        emitStatError("拼接初始化失败（键鼠钩子）");
        if (g_hMouse) UnhookWindowsHookEx(g_hMouse);
        if (g_hKey) UnhookWindowsHookEx(g_hKey);
        g_hMouse = g_hKey = nullptr;
        RAWINPUTDEVICE rm = {};
        rm.usUsagePage = 1;
        rm.usUsage = 2;
        rm.dwFlags = RIDEV_REMOVE;
        RegisterRawInputDevices(&rm, 1, sizeof(rm));
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        kvmClose(g_sock);
        g_sock = KVM_INVALID_SOCK;
        return 1;
    }

    g_drain = std::thread(drainLoop);
    SetTimer(g_hwnd, KVM_TIMER_ID, 50, nullptr);

    emitStat("event=start|mode=stitch|dst=" + std::string(peer_ip));
    emitStitchState("local");
    std::cout << "Stitch controller active – peer desktop " << g_tgt_w << "x"
              << g_tgt_h << " at (" << g_tgt_x0 << "," << g_tgt_y0 << ").\n"
              << "Peer is on the " << kvmSideKey(g_side)
              << ": push the cursor past that edge to control it "
                 "(Ctrl+Alt+Shift+Q releases).\n"
              << std::flush;

    /* ── message loop (the hook thread) ────────────────────────────────── */
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* ── teardown (also the Ctrl+C / disconnect path) ──────────────────── */
    KillTimer(g_hwnd, KVM_TIMER_ID);
    leaveRemote(false);
    g_drainQuit = true;
    if (g_sock != KVM_INVALID_SOCK) shutdown(g_sock, SD_BOTH);
    if (g_drain.joinable()) g_drain.join();
    /* The target releases every injected input when it observes EOF. Do not
       start another blocking send after the drain thread has stopped. */
    setCursorVisible(true);
    UnhookWindowsHookEx(g_hMouse);
    UnhookWindowsHookEx(g_hKey);
    g_hMouse = g_hKey = nullptr;

    RAWINPUTDEVICE rm = {};
    rm.usUsagePage = 1;
    rm.usUsage = 2;
    rm.dwFlags = RIDEV_REMOVE;
    rm.hwndTarget = nullptr;
    RegisterRawInputDevices(&rm, 1, sizeof(rm));

    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    UnregisterClassW(cls, inst);
    kvmClose(g_sock);
    g_sock = KVM_INVALID_SOCK;

    emitStat("event=stop|mode=stitch");
    if (!g_netOK.load() && running.load())
        emitStatError("拼接连接已断开");
    return 0;
}

void kvmSetControllerSide(int side)
{
    if (side < (int)KVM_SIDE_LEFT || side > (int)KVM_SIDE_BOTTOM) return;
    g_side_req.store(side);
}

/* ── target (receiver) ───────────────────────────────────────────────────── */

namespace {

/* Raise this thread's DPI awareness so GetSystemMetrics reports physical
 * pixels even when the receiver process itself is DPI-unaware (the video
 * window path may not have called SDL_Init yet). PER_MONITOR_AWARE_V2 is -4;
 * resolved dynamically so old SDK headers are not a build dependency. */
typedef void *KvmDpiCtx;
typedef KvmDpiCtx(WINAPI *SetThreadDpiCtxFn)(KvmDpiCtx);
static void raiseThreadDpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    auto fn = (SetThreadDpiCtxFn)GetProcAddress(user32,
                                                "SetThreadDpiAwarenessContext");
    if (!fn) return;
    fn((KvmDpiCtx)(intptr_t)-4);
}

static void targetMetrics(int &x, int &y, uint32_t &w, uint32_t &h)
{
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = (uint32_t)GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = (uint32_t)GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
}

struct HeldInput {
    bool key[256] = {};
    WORD key_scan[256] = {};   /* exactly what we injected: a VK->scan
                                * reconstruction is not lossless (numpad
                                * Enter, media keys, differing layouts) */
    bool key_ext[256] = {};
    bool btn[8] = {};
};

static void injectMove(int x, int y, uint32_t w, uint32_t h)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((long long)clampInt(x, 0, (int)w - 1) * 65535) /
                      (w > 1 ? (long long)(w - 1) : 1));
    in.mi.dy = (LONG)(((long long)clampInt(y, 0, (int)h - 1) * 65535) /
                      (h > 1 ? (long long)(h - 1) : 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                    MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

static void injectButton(uint32_t btn, bool down, HeldInput &held)
{
    if (btn < 1 || btn > 5) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    switch (btn) {
    case 1: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case 2: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case 3: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case 4:
    case 5:
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = (btn == 4) ? XBUTTON1 : XBUTTON2;
        break;
    }
    SendInput(1, &in, sizeof(in));
    held.btn[btn] = down;
}

static void injectWheel(int delta, bool horizontal)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(int32_t)delta;
    SendInput(1, &in, sizeof(in));
}

static void injectKey(uint32_t down, uint32_t vk, uint32_t scan, bool ext,
                      HeldInput &held)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    if (scan) {
        in.ki.wScan = (WORD)(scan & 0xFFFF);
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (ext) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    } else {
        /* Media keys and a few synthetic VKs arrive without a scan code. */
        in.ki.wVk = (WORD)vk;
    }
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
    if (vk < 256) {
        held.key[vk] = (down != 0);
        if (down) {
            held.key_scan[vk] = (WORD)(scan & 0xFFFF);
            held.key_ext[vk] = ext;
        }
    }
}

static void releaseAll(HeldInput &held)
{
    for (int b = 1; b < 8; b++)
        if (held.btn[b]) injectButton((uint32_t)b, false, held);
    for (int vk = 0; vk < 256; vk++) {
        if (!held.key[vk]) continue;
        /* Replay the key-up with the stored scan code. Fall back to the
         * VK mapping only for entries created before the first down. */
        WORD scan = held.key_scan[vk];
        bool ext = held.key_ext[vk];
        if (!scan) {
            scan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
            ext = isExtendedVk((DWORD)vk);
        }
        injectKey(0, (uint32_t)vk, scan, ext, held);
    }
}

struct HeartbeatState {
    bool enabled = false;
    ULONGLONG last_rx = 0;
    ULONGLONG next_tx = 0;
};

/* deadline_ms == 0 means "until shutdown/EOF"; used for the hello so a peer
 * that connects and stays silent cannot pin the only KVM session slot. */
static bool recvAllRunning(kvm_sock_t s, void *buf, size_t len,
                           std::atomic<bool> &running, DWORD deadline_ms = 0,
                           HeartbeatState *hb = nullptr)
{
    char *p = (char *)buf;
    size_t off = 0;
    const DWORD start = GetTickCount();
    while (off < len) {
        if (!running.load()) return false;
        if (deadline_ms && (DWORD)(GetTickCount() - start) > deadline_ms)
            return false;
        if (hb && hb->enabled) {
            const ULONGLONG now = GetTickCount64();
            if (now - hb->last_rx >= 9000) return false;
            if (now >= hb->next_tx) {
                KvmEvent ping = {};
                ping.type = htonl(KVM_EV_PING);
                if (!kvmSendAll(s, &ping, sizeof(ping))) return false;
                hb->next_tx = now + 3000;
            }
        }
        int n = recv(s, p + off, (int)(len - off), 0);
        if (n == 0) return false;
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
            return false;
        }
        if (hb) hb->last_rx = GetTickCount64();
        off += (size_t)n;
    }
    return true;
}

/* Dismiss the consent box as if its default button (No) was clicked.
 *
 * Posting WM_CLOSE (or WM_SYSCOMMAND/SC_CLOSE) to a MessageBox does NOT
 * dismiss it — verified on Windows 11 26200, where the box stays up forever
 * and the receiver then hangs on shutdown. WM_COMMAND(IDNO) does work and
 * returns IDNO. Enumerate the owner thread's windows so this never touches
 * another process's dialog with a coincidentally equal title. */
struct DismissCtx { DWORD tid; int posted; };

static BOOL CALLBACK dismissThreadDlg(HWND h, LPARAM lp)
{
    DismissCtx *ctx = (DismissCtx *)lp;
    wchar_t cls[64] = L"";
    if (GetClassNameW(h, cls, 64) && wcscmp(cls, L"#32770") == 0) {
        PostMessageW(h, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
        ++ctx->posted;
    }
    return TRUE;
}

/* Result of the consent dialog. Only an explicit "No" arms the per-peer
 * refusal cooldown: a timeout, a controller that hung up while the box was up
 * and a shutdown of this process are not refusals and must not lock the peer
 * out for a minute. */
enum ConsentResult {
    CONSENT_YES = 0,
    CONSENT_NO,
    CONSENT_TIMEOUT,
    CONSENT_CANCELLED,  /* controller disconnected / local shutdown */
    CONSENT_PROMPT_FAILED /* MessageBoxW returned 0: no human was asked */
};

/* Non-blocking check for a peer that has already closed (or reset) the
 * connection. Used before creating the topmost dialog, so a peer that sends a
 * hello and immediately hangs up cannot make the box flash on screen. */
static bool peerClosed(kvm_sock_t client)
{
    /* Drain (rather than MSG_PEEK) so a FIN queued behind protocol-violating
     * bytes is still seen; nothing legitimate arrives before the reply. The
     * loop is bounded so a peer that keeps sending cannot stall this check. */
    char sink[256];
    for (int i = 0; i < 8; i++) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(client, &rd);
        struct timeval zero = {0, 0};
        if (select(0, &rd, nullptr, nullptr, &zero) <= 0) return false;
        if (!FD_ISSET(client, &rd)) return false;
        const int n = recv(client, sink, sizeof(sink), 0);
        if (n == 0) return true; /* orderly close / FIN */
        if (n < 0) {
            const int err = WSAGetLastError();
            return err != WSAEWOULDBLOCK && err != WSAETIMEDOUT &&
                   err != WSAEINTR;
        }
    }
    return false;
}

/* Minimum spacing between two consent dialogs from ANY source. The per-IP
 * backoff only covers a repeat from the same address, and even the global
 * cancel backoff (g_kvm_last_cancel) is a floor, not a guarantee against a
 * rotating host that waits it out; this pacing is the last line of defence
 * against flickering the topmost box. Only the session thread opens a
 * dialog. */
static ULONGLONG g_kvm_last_prompt = 0;
static const DWORD KVM_PROMPT_INTERVAL_MS = 1500;

/* MessageBoxW is modal with no cancellation path; watch the running flag, the
 * 30 s deadline and the client socket from a helper thread and answer the box
 * with "No". Watching the socket matters: the controller may have already
 * given up (Ctrl+C / GUI stop) while the peer's user is deciding, and without
 * it the single session slot stays busy until the timeout. */
static ConsentResult askConsent(const std::string &peer_ip, int side,
                                std::atomic<bool> &running, kvm_sock_t client)
{
    std::wstring wip(peer_ip.begin(), peer_ip.end());
    /* `side` is where THIS machine sits relative to the controller, so it is
     * also the edge the controller pushes out of. */
    const wchar_t *enter_edge = L"右缘";
    const wchar_t *back_edge = L"左缘";
    switch (side) {
    case KVM_SIDE_LEFT:   enter_edge = L"左缘"; back_edge = L"右缘"; break;
    case KVM_SIDE_TOP:    enter_edge = L"上缘"; back_edge = L"下缘"; break;
    case KVM_SIDE_BOTTOM: enter_edge = L"下缘"; back_edge = L"上缘"; break;
    default: break;
    }
    std::wstring msg =
        L"来自 " + wip + L" 的设备请求以「拼接」模式接管本机的鼠标和键盘。\n\n";
    msg += L"允许后，对方把鼠标移到其屏幕";
    msg += enter_edge;
    msg += L"即可控制本机；\n把鼠标移出本机屏幕";
    msg += back_edge;
    msg += L"，或按 Ctrl+Alt+Shift+Q 即可交还控制。\n\n"
           L"仅在你信任这台设备时选择「是」。";

    if (!running.load() || peerClosed(client)) return CONSENT_CANCELLED;

    /* Global prompt pacing (see g_kvm_last_prompt); abort early if the peer
     * leaves while we wait, so a vanished peer never holds the slot. */
    if (g_kvm_last_prompt) {
        ULONGLONG elapsed = GetTickCount64() - g_kvm_last_prompt;
        while (elapsed < KVM_PROMPT_INTERVAL_MS) {
            if (!running.load() || peerClosed(client)) return CONSENT_CANCELLED;
            const ULONGLONG left = KVM_PROMPT_INTERVAL_MS - elapsed;
            Sleep(left < 50 ? (DWORD)left : 50);
            elapsed = GetTickCount64() - g_kvm_last_prompt;
        }
    }
    g_kvm_last_prompt = GetTickCount64();

    const DWORD owner_tid = GetCurrentThreadId();
    std::atomic<bool> done{false};
    std::atomic<int> reason{0}; /* 0 = answered, 1 timeout, 2 peer, 3 shutdown */
    std::thread watcher([&] {
        const ULONGLONG start = GetTickCount64();
        int why = 0;
        while (!done.load()) {
            if (!running.load()) { why = 3; break; }
            if (GetTickCount64() - start > 30000) { why = 1; break; }
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(client, &rd);
            struct timeval tv = {0, 100000}; /* 100 ms */
            const int sel = select(0, &rd, nullptr, nullptr, &tv);
            if (sel < 0) {
                /* Don't spin if select itself fails on this socket. */
                Sleep(100);
                continue;
            }
            if (sel > 0 && FD_ISSET(client, &rd)) {
                /* Read rather than MSG_PEEK: a peer cannot legitimately send
                 * anything before it has our consent reply, and consuming
                 * stray bytes is the only way a FIN queued behind them ever
                 * becomes visible (peeking would keep seeing the data and
                 * miss the close until the 30 s timeout). */
                char sink[256];
                const int n = recv(client, sink, sizeof(sink), 0);
                if (n == 0) { why = 2; break; } /* orderly close / FIN */
                if (n < 0) {
                    const int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK && err != WSAETIMEDOUT &&
                        err != WSAEINTR) { why = 2; break; }
                }
            }
        }
        if (done.load()) return; /* the user answered it */
        reason = why ? why : 1;
        /* The box may not exist yet if we fire right at start-up; retry. */
        const ULONGLONG give_up = GetTickCount64() + 3000;
        for (;;) {
            DismissCtx ctx{owner_tid, 0};
            EnumThreadWindows(owner_tid, dismissThreadDlg, (LPARAM)&ctx);
            if (ctx.posted || GetTickCount64() >= give_up) return;
            Sleep(50);
        }
    });
    const int r = MessageBoxW(nullptr, msg.c_str(), L"Mivara - 拼接请求",
                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                                  MB_TOPMOST | MB_SETFOREGROUND);
    done = true;
    if (watcher.joinable()) watcher.join();
    if (r == IDYES) return CONSENT_YES;
    if (reason.load() == 1) return CONSENT_TIMEOUT;
    if (reason.load() == 2 || reason.load() == 3) return CONSENT_CANCELLED;
    /* Only an actual IDNO is a human refusal. MessageBoxW returns 0 when it
     * could not create the box at all (no interactive window station, out of
     * resources); the watcher never recorded a reason in that case, so a bare
     * "not IDYES" fall-through must not be branded a refusal — that would arm
     * the 60 s cooldown without anyone having been asked. */
    if (r == IDNO) return CONSENT_NO;
    return CONSENT_PROMPT_FAILED;
}

/* Per-source throttles for the consent dialog. Keyed on the source address so
 * one hostile host cannot auto-deny (or prompt-flood) every other controller
 * on the LAN. A fixed table (no allocation, bounded memory) is enough: the
 * listener serves one connection at a time, so only the session thread ever
 * touches it.
 *
 *   deny_tick   — an explicit "No": silently refuse this source for 60 s.
 *   cancel_tick — the peer hung up before answering: delay the next prompt
 *                 for this source by a few seconds (not a refusal). A global
 *                 tick (g_kvm_last_cancel) applies the same delay to every
 *                 source, so rotating addresses cannot bypass it. */
struct PeerThrottle {
    std::string ip;
    DWORD deny_tick = 0;
    DWORD cancel_tick = 0;
};
static PeerThrottle g_kvm_peers[8];
static unsigned g_kvm_peer_next = 0;
static const DWORD KVM_DENY_COOLDOWN_MS = 60000;
static const DWORD KVM_CANCEL_BACKOFF_MS = 3000;

/* The cancel backoff is ALSO global: the per-source table alone is bypassed by
 * a host that rotates source addresses, which would then re-prompt after only
 * the 1.5 s dialog pacing. One shared tick means any cancelled prompt delays
 * the next one regardless of where it comes from. */
static ULONGLONG g_kvm_last_cancel = 0;

static void noteCancel(ULONGLONG now)
{
    g_kvm_last_cancel = now;
}

static PeerThrottle *peerThrottle(const std::string &ip, bool create)
{
    for (auto &p : g_kvm_peers)
        if (!p.ip.empty() && p.ip == ip) return &p;
    if (!create) return nullptr;
    for (auto &p : g_kvm_peers) {
        if (p.ip.empty()) {
            p.ip = ip;
            return &p;
        }
    }
    /* Full: evict round-robin. Only a hostile host that keeps changing
     * addresses can reach this. */
    PeerThrottle &p = g_kvm_peers[g_kvm_peer_next++ % 8];
    p = PeerThrottle{};
    p.ip = ip;
    return &p;
}

static void serveClient(kvm_sock_t client, const std::string &peer_ip,
                        std::atomic<bool> &running)
{
    /* A stalled client must not pin the port forever, and a controller that
     * vanishes without a FIN must be detected (the session is idle whenever
     * the user is not typing, so TCP has nothing else to time out). */
    kvmSetTimeouts(client, 1000, 1000);
    kvmEnableKeepAlive(client);

    KvmHello hello = {};
    if (!recvAllRunning(client, &hello, sizeof(hello), running, 5000)) return;

    KvmReply reply = {};
    reply.magic = htonl(KVM_MAGIC);
    reply.version = htonl(KVM_VERSION);
    reply.reserved = htonl((ntohl(hello.reserved[1]) & KVM_FEATURE_HEARTBEAT)
                               ? KVM_FEATURE_HEARTBEAT : 0);

    if (ntohl(hello.magic) != KVM_MAGIC) {
        reply.status = htonl(KVM_ERR_VERSION);
        kvmSendAll(client, &reply, sizeof(reply));
        return;
    }
    if (ntohl(hello.version) != KVM_VERSION) {
        reply.status = htonl(KVM_ERR_VERSION);
        kvmSendAll(client, &reply, sizeof(reply));
        return;
    }

    PeerThrottle *throttle = peerThrottle(peer_ip, false);
    if (throttle && throttle->deny_tick &&
        (DWORD)(GetTickCount() - throttle->deny_tick) < KVM_DENY_COOLDOWN_MS) {
        reply.status = htonl(KVM_ERR_DENIED);
        kvmSendAll(client, &reply, sizeof(reply));
        std::cout << "[kvm] request from " << peer_ip
                  << " auto-declined (recent refusal)\n" << std::flush;
        return;
    }

    /* Where the peer sits relative to the controller (informational: it only
     * tailors the consent text). The controller encodes side + 1 so a zeroed
     * field (older build, or a raw KvmHello = {}) means "unspecified" rather
     * than KVM_SIDE_LEFT; anything out of range falls back to the default. */
    const uint32_t raw_side = ntohl(hello.reserved[0]);
    const int side = (raw_side >= 1 && raw_side <= (uint32_t)KVM_SIDE_BOTTOM + 1)
                         ? (int)raw_side - 1
                         : (int)KVM_SIDE_RIGHT;

    std::cout << "[kvm] stitch request from " << peer_ip << " (peer on the "
              << kvmSideKey(side) << ")\n" << std::flush;
    /* Back off after a peer-cancelled prompt: without this, a host that keeps
     * connecting + closing would flash the topmost dialog repeatedly. Waiting
     * (rather than denying) keeps a legitimate rapid retry working. Poll the
     * socket while waiting so a peer that has already hung up frees the single
     * session slot immediately instead of keeping every other controller out
     * for the whole backoff. */
    DWORD backoff = 0;
    if (throttle && throttle->cancel_tick) {
        const DWORD age = (DWORD)(GetTickCount() - throttle->cancel_tick);
        if (age < KVM_CANCEL_BACKOFF_MS) backoff = KVM_CANCEL_BACKOFF_MS - age;
    }
    if (g_kvm_last_cancel) {
        const ULONGLONG age = GetTickCount64() - g_kvm_last_cancel;
        if (age < KVM_CANCEL_BACKOFF_MS &&
            (DWORD)(KVM_CANCEL_BACKOFF_MS - age) > backoff)
            backoff = (DWORD)(KVM_CANCEL_BACKOFF_MS - age);
    }
    while (backoff > 0) {
        if (!running.load() || peerClosed(client)) {
            noteCancel(GetTickCount64());
            if (PeerThrottle *p = peerThrottle(peer_ip, true))
                p->cancel_tick = GetTickCount();
            return;
        }
        const DWORD slice = backoff < 100 ? backoff : 100;
        Sleep(slice);
        backoff -= slice;
    }
    const ConsentResult verdict = askConsent(peer_ip, side, running, client);
    if (verdict != CONSENT_YES) {
        if (verdict == CONSENT_CANCELLED) {
            /* Controller left (or we are shutting down): no reply to send and
             * nothing to hold against this peer — only a short backoff. */
            noteCancel(GetTickCount64());
            if (PeerThrottle *p = peerThrottle(peer_ip, true))
                p->cancel_tick = GetTickCount();
            std::cout << "[kvm] request from " << peer_ip
                      << " cancelled before consent\n" << std::flush;
            return;
        }
        reply.status = htonl(KVM_ERR_DENIED);
        kvmSendAll(client, &reply, sizeof(reply));
        if (verdict == CONSENT_NO) {
            /* Only a human "No" starts the per-peer cooldown: an unanswered
             * box (30 s) is not a refusal and must not block the reconnect. */
            if (PeerThrottle *p = peerThrottle(peer_ip, true))
                p->deny_tick = GetTickCount();
        }
        std::cout << "[kvm] request " << peer_ip
                  << (verdict == CONSENT_TIMEOUT        ? " timed out\n"
                      : verdict == CONSENT_PROMPT_FAILED ? " refused (consent prompt could not be shown)\n"
                                                         : " declined\n")
                  << std::flush;
        return;
    }

    int sx, sy;
    uint32_t sw, sh;
    targetMetrics(sx, sy, sw, sh);
    reply.status = htonl(KVM_OK);
    reply.screen_x = (int32_t)htonl((uint32_t)sx);
    reply.screen_y = (int32_t)htonl((uint32_t)sy);
    reply.screen_w = htonl(sw);
    reply.screen_h = htonl(sh);
    HeartbeatState hb;
    hb.enabled = (ntohl(reply.reserved) & KVM_FEATURE_HEARTBEAT) != 0;
    hb.last_rx = GetTickCount64();
    hb.next_tx = hb.last_rx + 3000;
    if (!kvmSendAll(client, &reply, sizeof(reply))) return;

    std::cout << "[kvm] stitch session started (" << sw << "x" << sh
              << "); the peer now controls this desktop\n" << std::flush;

    HeldInput held;
    KvmEvent wire;
    bool io_ok = true;
    while (io_ok) {
        if (!running.load()) break;
        if (!recvAllRunning(client, &wire, sizeof(wire), running, 0, &hb)) break;
        KvmEvent e;
        e.type = ntohl(wire.type);
        e.a = ntohl(wire.a);
        e.b = ntohl(wire.b);
        e.c = ntohl(wire.c);
        switch (e.type) {
        case KVM_EV_MOVE:
            injectMove((int)e.a, (int)e.b, sw, sh);
            break;
        case KVM_EV_BUTTON:
            injectButton(e.a, e.b != 0, held);
            break;
        case KVM_EV_WHEEL:
            injectWheel((int)(int32_t)e.a, e.b != 0);
            break;
        case KVM_EV_KEY:
            injectKey(e.a, e.b, e.c & 0xFFFFu, (e.c & 0x10000u) != 0, held);
            break;
        case KVM_EV_RELEASE_ALL:
            releaseAll(held);
            break;
        case KVM_EV_PING:
            if (hb.enabled) {
                KvmEvent pong = {};
                pong.type = htonl(KVM_EV_PONG);
                io_ok = kvmSendAll(client, &pong, sizeof(pong));
            }
            break;
        case KVM_EV_PONG:
            break;
        default:
            break;
        }
    }
    releaseAll(held);
    std::cout << "[kvm] stitch session from " << peer_ip << " ended\n"
              << std::flush;
}

} /* namespace */

void kvmServiceRun(std::atomic<bool> &running)
{
    raiseThreadDpiAwareness();

    kvm_sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == KVM_INVALID_SOCK) {
        std::cout << "[kvm] socket init failed; stitch disabled\n";
        return;
    }
    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
               sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(KVM_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 4) < 0) {
        std::cout << "[kvm] bind failed on " << KVM_PORT
                  << "; stitch disabled\n" << std::flush;
        kvmClose(srv);
        return;
    }
    std::cout << "[kvm] stitch listener on TCP " << KVM_PORT << "\n"
              << std::flush;

    /* The session runs on its own thread so the listener stays responsive:
     * a second controller gets an immediate KVM_ERR_BUSY instead of sitting
     * in the accept backlog for the whole (possibly hours-long) session, and
     * shutdown never has to wait for the modal consent box to be answered. */
    std::atomic<bool> session_active{false};
    std::thread session;

    while (running.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        struct timeval tv = {1, 0};
        int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in cli = {};
        int cli_len = sizeof(cli);
        kvm_sock_t client = accept(srv, (sockaddr *)&cli, &cli_len);
        if (client == KVM_INVALID_SOCK) continue;

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));

        if (session_active.load()) {
            /* Read the peer's hello first. Closing with unread data makes
             * Windows send an RST, which discards the BUSY reply in flight
             * and surfaces as WSAECONNRESET on the controller. */
            kvmSetTimeouts(client, 500, 500);
            KvmHello hello = {};
            recvAllRunning(client, &hello, sizeof(hello), running, 1500);

            KvmReply busy = {};
            busy.magic = htonl(KVM_MAGIC);
            busy.version = htonl(KVM_VERSION);
            busy.status = htonl(KVM_ERR_BUSY);
            kvmSendAll(client, &busy, sizeof(busy));

            /* Drain anything still in flight before the graceful close. The
             * socket carries a 500 ms receive timeout, but a peer that keeps
             * sending would spin this loop forever — bound it by time and by
             * the service's running flag so shutdown is never pinned. */
            shutdown(client, SD_SEND);
            char sink[64];
            const ULONGLONG drain_deadline = GetTickCount64() + 1000;
            while (running.load() && GetTickCount64() < drain_deadline &&
                   recv(client, sink, sizeof(sink), 0) > 0) {}
            kvmClose(client);
            std::cout << "[kvm] busy, refused extra controller " << ip << "\n"
                      << std::flush;
            continue;
        }

        if (session.joinable()) session.join();
        session_active = true;
        std::string peer(ip);
        session = std::thread([client, peer, &running, &session_active] {
            serveClient(client, peer, running);
            kvmClose(client);
            session_active = false;
        });
    }

    if (session.joinable()) session.join();
    kvmClose(srv);
    std::cout << "[kvm] stitch listener stopped\n" << std::flush;
}

#else /* !_WIN32 */

int kvmRunController(const char *peer_ip, int peer_port, int side,
                     const std::atomic<bool> &running)
{
    (void)peer_ip;
    (void)peer_port;
    (void)side;
    (void)running;
    std::cout << "STAT|event=error|msg=拼接模式目前仅支持 Windows\n"
              << std::flush;
    return 3;
}

void kvmSetControllerSide(int side)
{
    (void)side;
}

void kvmServiceRun(std::atomic<bool> &running)
{
    (void)running;
}

#endif /* _WIN32 */
