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
 *      Listens on TCP 8084 and serves one controller at a time. Every
 *      connection obeys the receiver's configured policy (open / password /
 *      manual, see kvm.h); the manual prompt is a topmost MessageBox (default
 *      No, auto-dismissed after 30 s) when the receiver supplied no GUI hooks.
 *      8084 shares the process-wide single-session slot with 8081, so video
 *      and stitch can never be active together. Accepted events are replayed
 *      with SendInput (absolute virtual-desktop moves + scancode key events).
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
#include "drop.h"
#include "proto.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
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
#define KVM_WM_DROP       (WM_APP + 3)
#define KVM_TIMER_ID      1

/* File-drop capability negotiated after KvmReply. The token is per session and
 * only leaves the wire when both sides opted in. */
bool    g_drop_ready = false;
bool    g_file_drag = false;
uint8_t g_drop_token[DROP_TOKEN_BYTES] = {};
uint16_t g_drop_port = (uint16_t)DROP_PORT;
std::string g_peer_ip;
std::vector<std::string> g_drag_files;
std::atomic<bool> g_drop_cancel{false};
std::atomic<bool> g_drop_active{false};
std::atomic<bool> g_discard_drag{false};
std::thread g_drop_thread;

static std::mutex g_stat_out_mtx;

static void emitStat(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_stat_out_mtx);
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
 * starts from the same modifier state as the controller.
 *
 * The held tables are written only by the low-level hooks, and a key-up can
 * be missed: WH_KEYBOARD_LL does not run while a secure desktop is up
 * (Win+L, Ctrl+Alt+Del, UAC), another low-level hook may swallow the event
 * first, and Windows silently drops a hook that overruns
 * LowLevelHooksTimeout. An entry whose up was lost stays "down" for the rest
 * of the session, so every crossing re-arms it on the target. For a letter
 * key that types a stray character; for VK_LWIN it is worse — the target
 * gets a real Win down, and the KVM_EV_RELEASE_ALL that leaveRemote sends
 * completes it into a Win down/up pair, so the target's Start menu opens
 * even though the user never touched the Win key.
 *
 * g_key_local records whether the press actually reached the local desktop.
 * If it did, the OS must still report the key as down; if it does not, the
 * cached entry is stale, not something the user is holding. A press made
 * while control was Remote never reached the desktop, so its entry is
 * trusted as-is: GetAsyncKeyState knows nothing about input a hook
 * swallowed. */
static void syncHeldInput()
{
    for (int vk = 0; vk < 256; vk++) {
        if (!g_key_down[vk]) continue;
        if (g_key_local[vk] && !(GetAsyncKeyState(vk) & 0x8000)) {
            g_key_down[vk] = false;
            g_key_local[vk] = false;
            continue;
        }
        queueKey(1, (uint32_t)vk, g_key_scan[vk], g_key_ext[vk]);
    }
    /* Buttons 4/5 are the X buttons; 6/7 are never tracked. */
    static const int btn_vk[6] = {0, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                                  VK_XBUTTON1, VK_XBUTTON2};
    for (int b = 1; b < 8; b++) {
        if (!g_btn_down[b]) continue;
        if (b < 6 && g_btn_local[b] &&
            !(GetAsyncKeyState(btn_vk[b]) & 0x8000)) {
            g_btn_down[b] = false;
            g_btn_local[b] = false;
            continue;
        }
        queueButton((uint32_t)b, true);
    }
}

static void enterRemote()
{
    if (g_state == ControlState::Remote) return;

    /* A file drag is the one gesture where re-arming the held left button is
     * wrong: the target never received its DOWN, and the local Explorer drag
     * must keep running until the physical release. The helper captures the
     * selection at DragEnter, before the edge is crossed. */
    g_file_drag = false;
    g_drag_files.clear();
    if (g_btn_down[1] && g_drop_ready && dropHelperReady()) {
        /* Non-blocking: this runs on the hook/message-loop thread, and a
         * LowLevelHooksTimeout would silently remove the mouse hook. The
         * helper's 1-pixel strip sees DragEnter before the edge overshoot. */
        std::vector<std::string> files;
        if (dropHelperTakeFiles(files) && !files.empty()) {
            g_file_drag = true;
            g_drag_files.swap(files);
            emitStat("event=drop|state=ready|items=" +
                     std::to_string(g_drag_files.size()));
        }
    }

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
    if (!g_file_drag) syncHeldInput();
    emitStitchState("remote");
}

static void leaveRemote(bool warpCursor)
{
    if (g_state != ControlState::Remote) return;
    g_state = ControlState::Local;
    g_file_drag = false;
    g_drag_files.clear();

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
    dropHelperSetSide(side);
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
        if (g_state != ControlState::Remote) {
            g_btn_local[btn] = down;
            /* Defer the actual discard to the message loop: nothing blocking
             * may run inside a low-level hook. */
            if (btn == 1 && down) g_discard_drag = true;
        }
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

    if (g_file_drag && btn == 1 && !down) {
        /* End the local Explorer drag normally: the helper's 1-pixel strip
         * rejects the local drop, and the remote transfer starts from the
         * message loop with the release point. The target never received the
         * matching DOWN, so no button-up is sent to it. */
        g_btn_local[1] = false;
        if (g_hwnd) PostMessageW(g_hwnd, KVM_WM_DROP, 0, 0);
        return CallNextHookEx(nullptr, code, wp, lp);
    }

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
    /* A new physical button-down invalidates any selection captured for the
     * previous gesture. Done here (message loop), never in the hook. */
    if (g_discard_drag.exchange(false))
        dropHelperDiscard();

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

/* Starts the 8085 copy for the selection captured at the edge. Runs on its
 * own thread: a large folder must not stall the hook message loop, and the
 * 8084 session keeps forwarding input while the copy proceeds. */
static void startFileDrop(int screen_x, int screen_y)
{
    if (!g_drop_ready || g_drag_files.empty()) return;
    if (g_drop_active.exchange(true)) {
        emitStat("event=drop|state=busy|msg=another transfer is running");
        g_drag_files.clear();
        return;
    }
    g_drop_cancel = false;

    std::vector<std::string> files;
    files.swap(g_drag_files);
    std::vector<uint8_t> token(g_drop_token, g_drop_token + DROP_TOKEN_BYTES);
    const std::string peer = g_peer_ip;
    const uint16_t port = g_drop_port;
    emitStat("event=drop|state=sending|items=" +
             std::to_string(files.size()));

    if (g_drop_thread.joinable()) g_drop_thread.join();
    g_drop_thread = std::thread([files = std::move(files), token = std::move(token), peer,
                 port, screen_x, screen_y]() {
        DropOptions opt;
        opt.peer_ip = peer.c_str();
        opt.port = port;
        opt.x = screen_x;
        opt.y = screen_y;
        opt.transfer_id = (uint32_t)(GetTickCount64() ^
                                     (uint64_t)GetCurrentProcessId());
        const DropResult r = dropSendFiles(opt, files, token.data(),
                                           g_drop_cancel);
        std::string line = "event=drop|state=";
        line += (r.status == DROP_OK) ? "done" : "error";
        line += "|items=" + std::to_string(r.committed);
        line += "|failed=" + std::to_string(r.failed);
        line += "|fallback=" + std::string(r.fallback ? "1" : "0");
        if (!r.message.empty()) line += "|msg=" + r.message;
        emitStat(line);
        g_drop_active = false;
    });
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
        g_drop_cancel = true;
        g_file_drag = false;
        g_drag_files.clear();
        leaveRemote(true);
        return 0;
    case KVM_WM_DROP: {
        if (!g_file_drag) return 0;
        const int sx = g_tgt_x0 + g_remote_x;
        const int sy = g_tgt_y0 + g_remote_y;
        g_file_drag = false;
        startFileDrop(sx, sy);
        leaveRemote(true);
        return 0;
    }
    case KVM_WM_DISCONNECT:
        g_drop_cancel = true;
        g_file_drag = false;
        g_drag_files.clear();
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

/* This machine's name, sent to the target's 授权 popup (same cap as the 8081
 * extension frame). */
static std::string kvmLocalDeviceName()
{
    char name[256] = {0};
    DWORD n = (DWORD)sizeof(name);
    if (!GetComputerNameA(name, &n) || name[0] == '\0')
        return "Mivara";
    name[n < sizeof(name) ? n : sizeof(name) - 1] = '\0';
    std::string s(name);
    if (s.size() > 31) s.resize(31);
    return s;
}

/* The device name comes straight off the wire and is echoed into STAT lines
 * (and the app's approval dialog), where '|' separates fields. Replace control
 * characters and '|' the same way the 8081 receiver does, so a hostile client
 * cannot smuggle extra keys into `auth|...|dev=` and spoof the address text. */
static std::string kvmDeviceNameFromWire(const HsRequestExtV3 &ext)
{
    std::string d;
    for (size_t i = 0; i < sizeof(ext.device); i++) {
        const unsigned char c = (unsigned char)ext.device[i];
        if (!c) break;
        d.push_back((c < 0x20 || c == 0x7f || c == '|') ? '?' : (char)c);
    }
    if (d.size() > 31) d.resize(31);
    return d;
}

static bool kvmSendHsResponse(kvm_sock_t s, uint32_t status, uint32_t w,
                              uint32_t h)
{
    HsResponseV2 res = {};
    res.magic = htonl(TVHS_MAGIC);
    res.version = htonl(TVHS_VERSION);
    res.recv_width = htonl(w);
    res.recv_height = htonl(h);
    res.status = htonl(status);
    return kvmSendAll(s, &res, sizeof(res));
}

/* Human-readable text for the authorization verdicts. Never includes a
 * password: only the receiver's status code is used. */
static void kvmAuthErrorText(uint32_t status, std::string &out)
{
    switch (status) {
    case HS_ERR_AUTH:
        out = "连接密码错误，或尝试次数过多";
        break;
    case HS_ERR_AUTH_TIMEOUT:
        out = "对方未在 30 秒内确认拼接请求";
        break;
    case HS_ERR_DENIED:
        out = "对方拒绝了拼接请求";
        break;
    case HS_ERR_BUSY:
        out = "对方正在接收其他连接";
        break;
    case HS_ERR_AUTH_CONFIG:
        out = "对方接收授权配置异常，请在设置中重新设置连接密码";
        break;
    case HS_ERR_VERSION:
        out = "拼接协议版本不一致，请将两端更新到同一版本";
        break;
    default:
        out = "拼接请求被拒绝（错误码 " + std::to_string(status) + "）";
        break;
    }
}

enum class AuthOutcome { Ok, Status, Cancelled, Transport };

/* Controller half of the v2 authorization exchange: identity frame, optional
 * password proof, then the receiver's verdict. The client's verdict is left in
 * `status` for AuthOutcome::Status and a user-facing reason in `why`. */
static AuthOutcome kvmClientAuthorize(kvm_sock_t s, const KvmHello &hello,
                                      const HsAuthStart &as,
                                      const std::atomic<bool> &running,
                                      const KvmClientAuth *auth,
                                      uint32_t &status, std::string &why)
{
    const auto report = [auth](const char *state) {
        if (auth && auth->report) auth->report(state, nullptr, auth->ctx);
    };

    HsRequestExtV3 ext = {};
    ext.magic = htonl(TVHX_MAGIC);
    ext.version = htonl(TVHA_VERSION);
    const std::string device = kvmLocalDeviceName();
    std::memcpy(ext.device, device.data(),
                device.size() < sizeof(ext.device) ? device.size()
                                                   : sizeof(ext.device));
    if (!kvmSendAll(s, &ext, sizeof(ext))) {
        why = "拼接授权请求发送失败";
        return AuthOutcome::Transport;
    }

    const uint32_t mode = ntohl(as.mode);
    if (mode == AUTH_MODE_PASSWORD) {
        const uint32_t iter = ntohl(as.kdf_iter);
        if (iter < HS_KDF_ITER_MIN || iter > HS_KDF_ITER_MAX) {
            why = "对方下发的密码参数无效";
            return AuthOutcome::Transport;
        }
        std::string pw;
        if (!auth || !auth->password) {
            report("cancelled");
            why = "对方要求连接密码，但当前客户端无法输入";
            return AuthOutcome::Cancelled;
        }
        report("need_password");
        if (!auth->password(&pw, auth->ctx) || pw.empty()) {
            if (!pw.empty()) {
                volatile char *v = &pw[0];
                for (size_t i = 0; i < pw.size(); i++) v[i] = 0;
            }
            report("cancelled");
            why = "已取消输入连接密码";
            return AuthOutcome::Cancelled;
        }
        report("authenticating");

        uint8_t key[32] = {}, proof[32] = {};
        mivaraPasswordKey(pw, as.salt, sizeof(as.salt), iter, key);
        {
            volatile char *v = &pw[0];
            for (size_t i = 0; i < pw.size(); i++) v[i] = 0;
        }
        uint8_t bound[sizeof(KvmHello) + sizeof(HsRequestExtV3)];
        std::memcpy(bound, &hello, sizeof(hello));
        std::memcpy(bound + sizeof(hello), &ext, sizeof(ext));
        mivaraAuthProof(key, MIVARA_AUTH_PURPOSE_STITCH, as.nonce, bound,
                        sizeof(bound), proof);
        std::memset(key, 0, sizeof(key));

        HsAuthProof hp = {};
        hp.magic = htonl(TVHA_MAGIC);
        hp.version = htonl(TVHA_VERSION);
        std::memcpy(hp.proof, proof, sizeof(proof));
        std::memset(proof, 0, sizeof(proof));
        if (!kvmSendAll(s, &hp, sizeof(hp))) {
            why = "拼接授权应答发送失败";
            return AuthOutcome::Transport;
        }
    } else {
        report("waiting");
    }

    HsResponseV2 res = {};
    const int rv = kvmRecvAllPoll(s, &res, sizeof(res), running, 35000);
    if (rv == 0) {
        why = (mode == AUTH_MODE_MANUAL)
                  ? "对方未在 30 秒内确认拼接请求"
                  : "对方未在 35 秒内回应拼接授权";
        return AuthOutcome::Transport;
    }
    if (rv < 0) {
        why = "拼接授权过程中连接已断开";
        return AuthOutcome::Transport;
    }
    if (ntohl(res.magic) != TVHS_MAGIC || ntohl(res.version) != TVHS_VERSION) {
        why = "拼接授权应答无效";
        return AuthOutcome::Transport;
    }
    status = ntohl(res.status);
    return status == HS_OK ? AuthOutcome::Ok : AuthOutcome::Status;
}

} /* namespace */

int kvmRunController(const char *peer_ip, int peer_port, int side,
                     const std::atomic<bool> &running,
                     const KvmClientAuth *auth)
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

    g_peer_ip = peer_ip;
    g_drop_ready = false;
    g_file_drag = false;
    g_drag_files.clear();
    g_drop_cancel = false;
    g_drop_active = false;
    g_drop_port = (uint16_t)DROP_PORT;
    memset(g_drop_token, 0, sizeof(g_drop_token));

    /* The drag-capture helper must be up before the hello: its Medium token
     * target is what makes KVM_FEATURE_FILE_DROP safe to advertise. */
    struct HelperGuard {
        bool started = false;
        ~HelperGuard() { if (started) dropHelperStop(); }
    } helper_guard;
    bool file_drop_capable = false;
    if (auth && auth->file_drop) {
        std::string why;
        if (dropHelperStart(g_side, why)) {
            file_drop_capable = true;
            helper_guard.started = true;
        } else {
            emitStat("event=drop|state=disabled|msg=" + why);
        }
    }

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
        hello.reserved[1] = htonl(
            KVM_FEATURE_HEARTBEAT |
            (file_drop_capable ? KVM_FEATURE_FILE_DROP : 0u));
        if (!kvmSendAll(g_sock, &hello, sizeof(hello))) {
            emitStatError("拼接握手发送失败");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }

        /* The target answers with one of three frames; the first word tells
         * them apart (see kvm.h). Reading only that word keeps a refusal
         * (HsResponseV2 before any challenge) distinguishable from a
         * challenge and from the v1-style bare reply. */
        uint32_t first_raw = 0;
        int rv = kvmRecvAllPoll(g_sock, &first_raw, sizeof(first_raw),
                                running, 35000);
        if (rv == 0) {
            emitStatError("对方未在 35 秒内回应拼接请求");
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
        const uint32_t first = ntohl(first_raw);

        if (first == KVM_MAGIC) {
            reply = KvmReply{};
            reply.magic = first_raw;
            rv = kvmRecvAllPoll(g_sock, (char *)&reply + sizeof(uint32_t),
                                sizeof(reply) - sizeof(uint32_t), running,
                                35000);
            if (rv <= 0) {
                if (running.load()) emitStatError("拼接握手失败（对方应答不完整）");
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
            continue;
        }

        if (first == TVHS_MAGIC) {
            /* A refusal sent before the challenge: the receiver's password
             * record is missing/corrupt (fail closed), never a downgrade. */
            HsResponseV2 res = {};
            res.magic = first_raw;
            rv = kvmRecvAllPoll(g_sock, (char *)&res + sizeof(uint32_t),
                                sizeof(res) - sizeof(uint32_t), running, 35000);
            std::string why = "拼接授权被拒绝";
            if (rv > 0 && ntohl(res.magic) == TVHS_MAGIC &&
                ntohl(res.version) == TVHS_VERSION)
                kvmAuthErrorText(ntohl(res.status), why);
            emitStatError(why);
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 2;
        }

        if (first != TVHA_MAGIC) {
            emitStatError("拼接握手失败（对方不是 Mivara 接收端）");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }

        HsAuthStart as = {};
        as.magic = first_raw;
        rv = kvmRecvAllPoll(g_sock, (char *)&as + sizeof(uint32_t),
                            sizeof(as) - sizeof(uint32_t), running, 35000);
        if (rv <= 0) {
            if (running.load()) emitStatError("拼接授权握手失败（对方应答不完整）");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }
        if (ntohl(as.version) != TVHA_VERSION) {
            emitStatError("拼接协议版本不一致，请将两端更新到同一版本");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 1;
        }
        const uint32_t auth_mode = ntohl(as.mode);
        if (auth_mode != AUTH_MODE_OPEN && auth_mode != AUTH_MODE_PASSWORD &&
            auth_mode != AUTH_MODE_MANUAL) {
            if (auth && auth->report) auth->report("cancelled", nullptr, auth->ctx);
            emitStatError("对方下发的授权方式无效");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 2;
        }

        uint32_t auth_status = 0;
        std::string why;
        const AuthOutcome ao =
            kvmClientAuthorize(g_sock, hello, as, running, auth, auth_status,
                               why);
        if (ao != AuthOutcome::Ok) {
            if (auth && auth->report && ao != AuthOutcome::Cancelled)
                auth->report("cancelled", nullptr, auth->ctx);
            if (ao == AuthOutcome::Status) kvmAuthErrorText(auth_status, why);
            emitStatError(why);
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return ao == AuthOutcome::Transport ? 1 : 2;
        }
        /* Authorized: the metrics reply follows the verdict. */
        reply = KvmReply{};
        rv = kvmRecvAllPoll(g_sock, &reply, sizeof(reply), running, 35000);
        if (rv <= 0 || ntohl(reply.magic) != KVM_MAGIC) {
            if (running.load()) emitStatError("拼接授权后未收到会话参数");
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
        if (status != KVM_OK) {
            if (auth && auth->report) auth->report("cancelled", nullptr, auth->ctx);
            kvmAuthErrorText(status, why);
            emitStatError(why);
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            return 2;
        }
        if (auth && auth->report) auth->report("granted", nullptr, auth->ctx);
        break;
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

    /* File-drop token: a fixed 1 + 8 event sequence read before drainLoop()
     * starts, so no variable-length frame ever enters the 16-byte stream. */
    g_drop_ready = file_drop_capable &&
                   (ntohl(reply.reserved) & KVM_FEATURE_FILE_DROP) != 0;
    if (g_drop_ready) {
        KvmEvent ev = {};
        int rv2 = kvmRecvAllPoll(g_sock, &ev, sizeof(ev), running, 35000);
        if (rv2 != 1 || ntohl(ev.type) != KVM_EV_DROP_INFO ||
            ntohl(ev.a) != 0x4D564B58u || ntohl(ev.b) != 1u ||
            ntohl(ev.c) != 8u) {
            emitStatError("拼接文件通道握手失败");
            kvmClose(g_sock);
            g_sock = KVM_INVALID_SOCK;
            g_drop_ready = false;
            return 1;
        }
        for (int i = 0; i < 8; i++) {
            rv2 = kvmRecvAllPoll(g_sock, &ev, sizeof(ev), running, 35000);
            if (rv2 != 1 || ntohl(ev.type) != KVM_EV_DROP_INFO ||
                ntohl(ev.a) != (uint32_t)i) {
                emitStatError("拼接文件通道令牌不完整");
                kvmClose(g_sock);
                g_sock = KVM_INVALID_SOCK;
                g_drop_ready = false;
                return 1;
            }
            const uint32_t word = ntohl(ev.b);
            std::memcpy(g_drop_token + i * 4, &word, 4);
        }
        emitStat("event=drop|state=ready|port=" +
                 std::to_string((int)g_drop_port));
    } else if (auth && auth->file_drop) {
        emitStat("event=drop|state=disabled|msg=peer does not accept files");
    }

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
    g_drop_cancel = true;
    g_drop_ready = false;
    g_file_drag = false;
    g_drag_files.clear();
    if (g_drop_thread.joinable()) g_drop_thread.join();
    g_drop_active = false;
    mivaraSecureZero(g_drop_token, sizeof(g_drop_token));
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
 * that connects and stays silent cannot pin the only KVM session slot.
 * ULONGLONG (GetTickCount64) rather than DWORD: a receiver can run for weeks
 * and a 49-day tick wrap must not stretch a deadline. */
static bool recvAllRunning(kvm_sock_t s, void *buf, size_t len,
                           std::atomic<bool> &running,
                           ULONGLONG deadline_ms = 0,
                           HeartbeatState *hb = nullptr,
                           size_t *got = nullptr,
                           bool *timed_out = nullptr)
{
    char *p = (char *)buf;
    size_t off = 0;
    const ULONGLONG start = GetTickCount64();
    const auto finish = [&](bool ok, bool deadline) {
        if (got) *got = off;
        if (timed_out) *timed_out = deadline;
        return ok;
    };
    while (off < len) {
        if (!running.load()) return finish(false, false);
        if (deadline_ms && GetTickCount64() - start > deadline_ms)
            return finish(false, true);
        if (hb && hb->enabled) {
            const ULONGLONG now = GetTickCount64();
            if (now - hb->last_rx >= 9000) return finish(false, false);
            if (now >= hb->next_tx) {
                KvmEvent ping = {};
                ping.type = htonl(KVM_EV_PING);
                if (!kvmSendAll(s, &ping, sizeof(ping))) return finish(false, false);
                hb->next_tx = now + 3000;
            }
        }
        int n = recv(s, p + off, (int)(len - off), 0);
        if (n == 0) return finish(false, false);
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
            return finish(false, false);
        }
        if (hb) hb->last_rx = GetTickCount64();
        off += (size_t)n;
    }
    return finish(true, false);
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
    /* The deadline wins over a click that raced the watcher: an "allow" after
     * 30 s must not establish a session (same rule as 8081, issue 21). */
    if (reason.load() == 1) return CONSENT_TIMEOUT;
    if (reason.load() == 2 || reason.load() == 3) return CONSENT_CANCELLED;
    if (r == IDYES) return CONSENT_YES;
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

/* Password failures on 8084 go through the process-wide per-source throttle
 * in auth.cpp, the same instance the 8081 handshake uses: alternating ports
 * must not grant two free budgets or two cooldowns. Those helpers serialise
 * access internally, so no local mutex or throttle instance may be kept here.
 * Only a real password-proof failure feeds this budget; the local handshake
 * abuse counter below is separate (an abandoned handshake is not a guess). */
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

/* ── handshake budget and per-source admission ─────────────────────────────
 * kvmServiceRun() still serves one connection at a time (the session thread
 * stays responsive to everyone else with a busy reply), so an unauthenticated
 * peer that stalls would otherwise hold the only slot for the whole identity
 * or proof deadline. Every stage is bounded, the whole exchange has one
 * budget, and a source that keeps abandoning a handshake is refused early.
 * GetTickCount64 is used because a DWORD tick wraps after 49 days and would
 * make a long-running receiver misjudge a deadline. */
static const ULONGLONG KVM_HELLO_TIMEOUT_MS = 3000;
static const ULONGLONG KVM_IDENTITY_TIMEOUT_MS = 10000;
/* The controller's password prompt allows 120 s, so the proof window must not
 * close earlier than that (the per-source admission and abandon cooldown, not
 * a short timeout, are what bound a hostile peer). */
static const ULONGLONG KVM_PROOF_TIMEOUT_MS = 125000;
static const ULONGLONG KVM_HANDSHAKE_BUDGET_MS = 140000;

static const int KVM_HS_SLOTS = 8;
static const int KVM_HS_MAX_PENDING_PER_IP = 1;   /* same source: one at a time */
static const int KVM_HS_FREE_ABANDONS = 2;        /* tolerate transients first */
static const ULONGLONG KVM_HS_COOLDOWN_BASE_MS = 2000;
static const ULONGLONG KVM_HS_COOLDOWN_MAX_MS = 60000;

struct HsEntry {
    std::string ip;
    int         pending = 0;
    int         strikes = 0;
    ULONGLONG   until = 0;   /* abuse-cooldown end (0 = none) */
    ULONGLONG   last = 0;    /* last admission, for eviction */
};
static HsEntry g_kvm_hs[KVM_HS_SLOTS];
static unsigned g_kvm_hs_next = 0;
static std::mutex g_kvm_hs_mtx;

static HsEntry *hsFind(const std::string &ip)
{
    for (auto &e : g_kvm_hs)
        if (!e.ip.empty() && e.ip == ip) return &e;
    return nullptr;
}

/* Caller holds g_kvm_hs_mtx. Evicts the least recently used idle entry; an
 * entry with an unfinished handshake is never evicted. */
static HsEntry *hsSlot(const std::string &ip)
{
    if (HsEntry *e = hsFind(ip)) return e;
    for (auto &e : g_kvm_hs) {
        if (e.ip.empty()) {
            e.ip = ip;
            return &e;
        }
    }
    HsEntry *victim = nullptr;
    for (auto &e : g_kvm_hs) {
        if (e.pending > 0) continue;
        if (!victim || e.last < victim->last) victim = &e;
    }
    if (!victim) victim = &g_kvm_hs[g_kvm_hs_next++ % KVM_HS_SLOTS];
    *victim = HsEntry{};
    victim->ip = ip;
    return victim;
}

/* Admit one handshake from `ip`. False when the source is in its local abuse
 * cooldown or already has an unfinished handshake. */
static bool hsAdmit(const std::string &ip)
{
    std::lock_guard<std::mutex> lk(g_kvm_hs_mtx);
    HsEntry *e = hsSlot(ip);
    const ULONGLONG now = GetTickCount64();
    if (e->until) {
        if (now < e->until) return false;
        e->until = 0;
        e->strikes = 0;
    }
    if (e->pending >= KVM_HS_MAX_PENDING_PER_IP) return false;
    e->pending++;
    e->last = now;
    return true;
}

/* Release an admission. `abandoned` is only for a protocol-stage timeout/EOF
 * before authorization; a refusal, cancellation or claim failure must not
 * punish the source, and a completed authorization clears its strikes. */
static void hsRelease(const std::string &ip, bool admitted, bool abandoned,
                      bool success)
{
    /* A source that never reached admission and did not abandon anything
     * (version mismatch, cross-mode busy) leaves no trace. */
    if (!admitted && !abandoned && !success) return;
    std::lock_guard<std::mutex> lk(g_kvm_hs_mtx);
    HsEntry *e = hsSlot(ip);
    if (admitted && e->pending > 0) e->pending--;
    if (success) {
        e->strikes = 0;
        e->until = 0;
        return;
    }
    if (!abandoned) return;
    e->strikes++;
    if (e->strikes > KVM_HS_FREE_ABANDONS) {
        const int step = e->strikes - KVM_HS_FREE_ABANDONS - 1; /* 0,1,2... */
        ULONGLONG cd = KVM_HS_COOLDOWN_BASE_MS << std::min(step, 5);
        if (cd > KVM_HS_COOLDOWN_MAX_MS) cd = KVM_HS_COOLDOWN_MAX_MS;
        e->until = GetTickCount64() + cd;
    }
}

/* Milliseconds left in the whole-handshake budget; 0 = already spent. */
static ULONGLONG hsRemaining(ULONGLONG start)
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now >= start ? now - start : 0;
    return elapsed >= KVM_HANDSHAKE_BUDGET_MS
               ? 0 : KVM_HANDSHAKE_BUDGET_MS - elapsed;
}

/* recv bounded by both the stage limit and the remaining total budget. */
static bool recvHandshake(kvm_sock_t s, void *buf, size_t len,
                          std::atomic<bool> &running, ULONGLONG stage_ms,
                          ULONGLONG start, size_t *got = nullptr,
                          bool *timed_out = nullptr)
{
    const ULONGLONG left = hsRemaining(start);
    if (left == 0) {
        if (got) *got = 0;
        if (timed_out) *timed_out = true;
        return false;
    }
    const ULONGLONG wait = stage_ms < left ? stage_ms : left;
    return recvAllRunning(s, buf, len, running, wait, nullptr, got, timed_out);
}

/* Releases the source's pending slot on every serveClient exit path. */
struct HsGuard {
    explicit HsGuard(const std::string &peer) : ip(peer) {}
    ~HsGuard() { hsRelease(ip, admitted, abandoned, success); }

    bool admit() { admitted = hsAdmit(ip); return admitted; }
    void markAbandoned() { abandoned = true; }
    void markSuccess() { success = true; }

    std::string ip;
    bool admitted = false;
    bool abandoned = false;
    bool success = false;
};

/* Releases the process-wide session lease on every exit path after the claim
 * (HS_OK/metrics send failure, protocol error, session end). */
struct SessionLeaseGuard {
    MivaraSessionLease lease{};
    bool held = false;
    bool drop = false;
    ~SessionLeaseGuard()
    {
        if (drop) dropGateClear(lease.epoch);
        if (held) mivaraSessionRelease(lease);
    }
};

static void serveClient(kvm_sock_t client, const std::string &peer_ip,
                        std::atomic<bool> &running,
                        const KvmTargetAuth *auth)
{
    /* One budget covers hello + identity + proof; each stage additionally has
     * its own (shorter) limit. A stalled client must not pin the port forever,
     * and a controller that vanishes without a FIN must be detected (the
     * session is idle whenever the user is not typing, so TCP has nothing else
     * to time out). The guard releases this source's pending slot even when we
     * return early. */
    const ULONGLONG hs_start = GetTickCount64();
    HsGuard hs(peer_ip);

    kvmSetTimeouts(client, 1000, 1000);
    kvmEnableKeepAlive(client);

    KvmHello hello = {};
    size_t got = 0;
    bool timed_out = false;
    if (!recvHandshake(client, &hello, sizeof(hello), running,
                       KVM_HELLO_TIMEOUT_MS, hs_start, &got, &timed_out)) {
        /* A bare TCP probe (the GUI tests the port that way) connects and
         * closes without sending a byte; that is not an abandoned handshake
         * and must not arm the cooldown, otherwise normal discovery would
         * lock out the real controller. A partial hello or a source that
         * stays silent until the deadline is a real abandonment. A local
         * shutdown is not the peer's fault either. */
        if (running.load() && (got > 0 || timed_out)) hs.markAbandoned();
        return;
    }

    KvmReply reply = {};
    reply.magic = htonl(KVM_MAGIC);
    reply.version = htonl(KVM_VERSION);
    reply.reserved = htonl((ntohl(hello.reserved[1]) & KVM_FEATURE_HEARTBEAT)
                               ? KVM_FEATURE_HEARTBEAT : 0);

    /* A v1 peer (or a foreign service) is refused before authorization, so an
     * old client can never reach the event stream around the policy. */
    if (ntohl(hello.magic) != KVM_MAGIC ||
        ntohl(hello.version) != KVM_VERSION) {
        reply.status = htonl(KVM_ERR_VERSION);
        kvmSendAll(client, &reply, sizeof(reply));
        return;
    }

    /* 8081 and 8084 share one process-wide session slot (auth.h). Refuse
     * before asking for a password or showing a prompt: neither a video
     * session nor a video handshake that already claimed the slot may be
     * joined. HsResponseV2(HS_ERR_BUSY) is the cross-mode "receiving another
     * connection" answer the controller understands. */
    if (mivaraSessionOwner() != MIVARA_SESSION_NONE) {
        kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
        std::cout << "[kvm] request from " << peer_ip
                  << " refused: another session is active\n" << std::flush;
        return;
    }

    /* Same source: at most one unfinished handshake plus a local cooldown
     * after repeated protocol-stage abandons. Keep the bare KvmReply so the
     * controller retains its single 300 ms retry (a TVHS refusal ends it). */
    if (!hs.admit()) {
        reply.status = htonl(KVM_ERR_BUSY);
        kvmSendAll(client, &reply, sizeof(reply));
        std::cout << "[kvm] request from " << peer_ip
                  << " refused: source busy or in handshake cooldown\n"
                  << std::flush;
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
     * tailors the approval text). The controller encodes side + 1 so a zeroed
     * field (older build, or a raw KvmHello = {}) means "unspecified" rather
     * than KVM_SIDE_LEFT; anything out of range falls back to the default. */
    const uint32_t raw_side = ntohl(hello.reserved[0]);
    const int side = (raw_side >= 1 && raw_side <= (uint32_t)KVM_SIDE_BOTTOM + 1)
                         ? (int)raw_side - 1
                         : (int)KVM_SIDE_RIGHT;

    /* ══ 接收策略 (same one 8081 uses) ═══════════════════════════════════
     * No hook means manual: the v1 native prompt is kept, but a caller that
     * forgets the hooks must not expose an open desktop. */
    MivaraAuthMode mode = MIVARA_AUTH_MANUAL;
    uint8_t salt[16] = {}, hash[32] = {};
    uint32_t iter = 0;
    bool record_ok = false;
    if (auth && auth->policy &&
        !auth->policy(&mode, salt, hash, &iter, &record_ok, auth->ctx))
        mode = MIVARA_AUTH_MANUAL;

    if (mode == MIVARA_AUTH_PASSWORD && !record_ok) {
        /* Fail closed: a missing/corrupt record is never a downgrade. */
        kvmSendHsResponse(client, HS_ERR_AUTH_CONFIG, 0, 0);
        std::cout << "[kvm] request from " << peer_ip
                  << " refused: password record unusable\n" << std::flush;
        return;
    }

    int sx, sy;
    uint32_t sw, sh;
    targetMetrics(sx, sy, sw, sh);

    /* The process-wide slot is acquired before a manual prompt (so the human
     * decision cannot be stolen by a video handshake that finishes first) or,
     * for password/open, right before the grant. It is released on every exit
     * path by the guard. */
    SessionLeaseGuard lease;

    if (mode != MIVARA_AUTH_OPEN) {
        HsAuthStart as = {};
        as.magic = htonl(TVHA_MAGIC);
        as.version = htonl(TVHA_VERSION);
        as.mode = htonl(mode == MIVARA_AUTH_PASSWORD ? AUTH_MODE_PASSWORD
                                                     : AUTH_MODE_MANUAL);
        as.kdf_iter = htonl(iter);
        if (mode == MIVARA_AUTH_PASSWORD)
            std::memcpy(as.salt, salt, sizeof(as.salt));
        if (!mivaraRandomBytes(as.nonce, sizeof(as.nonce))) {
            std::cout << "[kvm] request from " << peer_ip
                      << " refused: no CSPRNG\n" << std::flush;
            return;
        }
        if (!kvmSendAll(client, &as, sizeof(as))) return;

        /* The identity frame arrives before any verdict so a manual prompt
         * can name the device and a password proof can bind it. The sender
         * sends it immediately, so a short stage limit is enough; the proof
         * gets the long one because it waits for a typed password. Both are
         * additionally bounded by what is left of the total budget. */
        HsRequestExtV3 ext = {};
        if (!recvHandshake(client, &ext, sizeof(ext), running,
                           KVM_IDENTITY_TIMEOUT_MS, hs_start)) {
            if (running.load()) hs.markAbandoned();
            return;
        }
        if (ntohl(ext.magic) != TVHX_MAGIC ||
            ntohl(ext.version) != TVHA_VERSION) {
            kvmSendHsResponse(client, HS_ERR_VERSION, 0, 0);
            return;
        }
        std::string device = kvmDeviceNameFromWire(ext);
        if (device.empty()) device = peer_ip;
        std::cout << "[kvm] stitch request from " << peer_ip << " (" << device
                  << ", peer on the " << kvmSideKey(side) << ")\n" << std::flush;

        if (mode == MIVARA_AUTH_PASSWORD) {
            /* One process-wide budget shared with the 8081 handshake: a source
             * cannot alternate ports for extra free guesses or a shorter
             * cooldown. The helpers lock internally. */
            const int wait_s = mivaraAuthThrottleRetryAfter(peer_ip);
            if (wait_s > 0) {
                kvmSendHsResponse(client, HS_ERR_AUTH, 0, 0);
                std::cout << "[kvm] request from " << peer_ip
                          << " throttled after failed passwords\n" << std::flush;
                return;
            }
            HsAuthProof proof = {};
            if (!recvHandshake(client, &proof, sizeof(proof), running,
                               KVM_PROOF_TIMEOUT_MS, hs_start)) {
                if (running.load()) hs.markAbandoned();
                return;
            }
            if (ntohl(proof.magic) != TVHA_MAGIC ||
                ntohl(proof.version) != TVHA_VERSION) {
                mivaraAuthThrottleNoteAbuse(peer_ip);
                kvmSendHsResponse(client, HS_ERR_AUTH, 0, 0);
                return;
            }
            uint8_t bound[sizeof(KvmHello) + sizeof(HsRequestExtV3)];
            std::memcpy(bound, &hello, sizeof(hello));
            std::memcpy(bound + sizeof(hello), &ext, sizeof(ext));
            uint8_t expect[32] = {};
            /* The password itself never reaches this machine; the proof is
             * domain-separated for 8084 (see auth.h). */
            mivaraAuthProof(hash, MIVARA_AUTH_PURPOSE_STITCH, as.nonce, bound,
                            sizeof(bound), expect);
            if (!mivaraConstEq(expect, proof.proof, sizeof(expect))) {
                mivaraAuthThrottleNoteFailure(peer_ip);
                kvmSendHsResponse(client, HS_ERR_AUTH, 0, 0);
                std::cout << "[kvm] request from " << peer_ip
                          << " refused: bad password\n" << std::flush;
                return;
            }
            mivaraAuthThrottleNoteSuccess(peer_ip);
        } else {
            /* Manual policy. Before opening a modal prompt, make sure the peer
             * is still there: a controller that sent the identity frame and
             * hung up must not hold the slot for the whole 30 s deadline. */
            if (peerClosed(client)) {
                std::cout << "[kvm] request from " << peer_ip
                          << " cancelled before approval\n" << std::flush;
                return;
            }
            /* Reserve the slot across the prompt: a video handshake that
             * grants while the user is deciding must not swallow the result. */
            if (!lease.held) {
                if (!mivaraSessionClaim(MIVARA_SESSION_STITCH, 60000,
                                        &lease.lease)) {
                    kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
                    std::cout << "[kvm] request from " << peer_ip
                              << " refused: another session claimed the slot\n"
                              << std::flush;
                    return;
                }
                lease.held = true;
            }
            KvmApprovalResult verdict = KVM_APPROVAL_DENY;
            if (auth && auth->approve) {
                verdict = auth->approve(peer_ip.c_str(), device.c_str(),
                                        (intptr_t)client, auth->ctx);
            } else {
                /* Native-prompt fallback (no GUI hooks): keep the v1 cancel
                 * backoff so a peer that connects + closes cannot flash the
                 * topmost dialog in a loop. */
                DWORD backoff = 0;
                if (throttle && throttle->cancel_tick) {
                    const DWORD age =
                        (DWORD)(GetTickCount() - throttle->cancel_tick);
                    if (age < KVM_CANCEL_BACKOFF_MS)
                        backoff = KVM_CANCEL_BACKOFF_MS - age;
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
                const ConsentResult cr =
                    askConsent(peer_ip, side, running, client);
                if (cr == CONSENT_CANCELLED) {
                    noteCancel(GetTickCount64());
                    if (PeerThrottle *p = peerThrottle(peer_ip, true))
                        p->cancel_tick = GetTickCount();
                    std::cout << "[kvm] request from " << peer_ip
                              << " cancelled before consent\n" << std::flush;
                    return;
                }
                switch (cr) {
                case CONSENT_YES: verdict = KVM_APPROVAL_ALLOW; break;
                case CONSENT_NO: verdict = KVM_APPROVAL_DENY; break;
                case CONSENT_TIMEOUT: verdict = KVM_APPROVAL_TIMEOUT; break;
                case CONSENT_CANCELLED: verdict = KVM_APPROVAL_CANCELLED; break;
                case CONSENT_PROMPT_FAILED:
                    verdict = KVM_APPROVAL_FAILED;
                    break;
                }
            }

            /* Handle the verdict value by value. Folding timeout, cancel and
             * prompt failure into "deny" would arm the 60 s cooldown for a
             * request that nobody actually refused. */
            switch (verdict) {
            case KVM_APPROVAL_ALLOW:
                break;
            case KVM_APPROVAL_DENY:
                /* Only an explicit human "No" starts the per-peer cooldown. */
                if (PeerThrottle *p = peerThrottle(peer_ip, true))
                    p->deny_tick = GetTickCount();
                kvmSendHsResponse(client, HS_ERR_DENIED, 0, 0);
                std::cout << "[kvm] request " << peer_ip << " declined\n"
                          << std::flush;
                return;
            case KVM_APPROVAL_TIMEOUT:
                /* An unanswered prompt is not a refusal: no cooldown. */
                kvmSendHsResponse(client, HS_ERR_AUTH_TIMEOUT, 0, 0);
                std::cout << "[kvm] request " << peer_ip << " timed out\n"
                          << std::flush;
                return;
            case KVM_APPROVAL_CANCELLED:
                /* Peer hung up or the receiver is stopping. Not a refusal and
                 * not the peer's fault; the socket is usually already gone, so
                 * skip the reply and free the slot now (the guard does it on
                 * return) instead of waiting out the 30 s deadline. */
                std::cout << "[kvm] request " << peer_ip
                          << " cancelled during approval\n" << std::flush;
                return;
            case KVM_APPROVAL_FAILED:
                /* No human was asked because the prompt could not be created.
                 * Log the real cause but answer like a refusal; a local
                 * failure must not arm the peer cooldown. */
                kvmSendHsResponse(client, HS_ERR_DENIED, 0, 0);
                std::cout << "[kvm] request " << peer_ip
                          << " denied: approval prompt failed\n" << std::flush;
                return;
            case KVM_APPROVAL_BUSY:
                /* Another device is already asking the local user. Answer
                 * "busy" instead of dropping the request silently; not a
                 * refusal, so no cooldown. */
                kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
                std::cout << "[kvm] request " << peer_ip
                          << " refused: another approval is pending\n"
                          << std::flush;
                return;
            default:
                kvmSendHsResponse(client, HS_ERR_DENIED, 0, 0);
                std::cout << "[kvm] request " << peer_ip
                          << " denied: unknown approval verdict\n" << std::flush;
                return;
            }
        }
    }

    /* Authorization succeeded (or the policy is open). Password/open claim
     * here; manual already holds the slot from before its prompt. Promote
     * before telling the controller it may start. */
    if (!lease.held) {
        if (!mivaraSessionClaim(MIVARA_SESSION_STITCH, 30000, &lease.lease)) {
            kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
            std::cout << "[kvm] request from " << peer_ip
                      << " refused: another session claimed the slot\n"
                      << std::flush;
            return;
        }
        lease.held = true;
    }
    if (!mivaraSessionPromote(lease.lease)) {
        /* The claim expired while we were authorizing; the destructor releases
         * whatever is left, and the caller is told the slot is busy. */
        kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
        std::cout << "[kvm] request from " << peer_ip
                  << " refused: session claim expired\n" << std::flush;
        return;
    }
    hs.markSuccess();

    /* File drop is a separate 8085 service and a separate opt-in. Install its
     * token before the KVM reply goes out, so a controller that dials 8085
     * immediately cannot race the gate. The guard clears it on every exit. */
    bool drop_ok = (auth &&
                    auth->accept_files.load(std::memory_order_relaxed) != 0) &&
                   dropServiceStatus().state == 1 &&
                   (ntohl(hello.reserved[1]) & KVM_FEATURE_FILE_DROP) != 0;
    uint8_t drop_token[DROP_TOKEN_BYTES] = {};
    if (drop_ok && !mivaraRandomBytes(drop_token, sizeof(drop_token)))
        drop_ok = false;
    if (drop_ok) {
        if (dropGateInstall(drop_token, lease.lease.epoch)) lease.drop = true;
        else drop_ok = false;
    }
    uint32_t feat = (ntohl(hello.reserved[1]) & KVM_FEATURE_HEARTBEAT)
                        ? KVM_FEATURE_HEARTBEAT : 0u;
    if (drop_ok) feat |= KVM_FEATURE_FILE_DROP;
    reply.reserved = htonl(feat);

    if (mode != MIVARA_AUTH_OPEN) {
        if (!kvmSendHsResponse(client, HS_OK, sw, sh)) return;
    }

    reply.status = htonl(KVM_OK);
    reply.screen_x = (int32_t)htonl((uint32_t)sx);
    reply.screen_y = (int32_t)htonl((uint32_t)sy);
    reply.screen_w = htonl(sw);
    reply.screen_h = htonl(sh);
    HeartbeatState hb;
    hb.enabled = (ntohl(reply.reserved) & KVM_FEATURE_HEARTBEAT) != 0;
    hb.last_rx = GetTickCount64();
    hb.next_tx = hb.last_rx + 3000;
    if (!kvmSendAll(client, &reply, sizeof(reply))) {
        mivaraSecureZero(drop_token, sizeof(drop_token));
        return;
    }

    /* Fixed 1 + 8 event sequence: the controller reads it before it starts
     * drainLoop(), so the 16-byte event parser never sees a variable frame. */
    if (drop_ok) {
        KvmEvent info = {};
        info.type = htonl(KVM_EV_DROP_INFO);
        info.a = htonl(0x4D564B58u);   /* 'MVKX' marker */
        info.b = htonl(1u);
        info.c = htonl(8u);
        if (!kvmSendAll(client, &info, sizeof(info))) {
            mivaraSecureZero(drop_token, sizeof(drop_token));
            return;
        }
        for (int i = 0; i < 8; i++) {
            uint32_t word = 0;
            std::memcpy(&word, drop_token + i * 4, 4);
            KvmEvent ev = {};
            ev.type = htonl(KVM_EV_DROP_INFO);
            ev.a = htonl((uint32_t)i);
            ev.b = htonl(word);
            if (!kvmSendAll(client, &ev, sizeof(ev))) {
                mivaraSecureZero(drop_token, sizeof(drop_token));
                return;
            }
        }
    }
    mivaraSecureZero(drop_token, sizeof(drop_token));

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

/* Real listener state, readable from any thread before, during or after
 * kvmServiceRun(). state: 0 = not started yet (starting), 1 = listening,
 * -1 = failed; stage: 0 none, 1 socket, 3 bind, 4 listen. */
static std::atomic<int> g_kvm_start_state{0};
static std::atomic<int> g_kvm_start_stage{0};
static std::atomic<int> g_kvm_start_error{0};

static void kvmReportStartup(int state, int stage, int error)
{
    g_kvm_start_error.store(error);
    g_kvm_start_stage.store(stage);
    g_kvm_start_state.store(state);   /* publish the state last */
}

KvmStartupStatus kvmServiceStatus()
{
    KvmStartupStatus st = {};
    st.state = g_kvm_start_state.load();
    st.stage = g_kvm_start_stage.load();
    st.error_code = g_kvm_start_error.load();
    st.port = (uint16_t)KVM_PORT;
    return st;
}

void kvmServiceRun(std::atomic<bool> &running, const KvmTargetAuth *auth)
{
    raiseThreadDpiAwareness();
    kvmReportStartup(0, 0, 0);

    kvm_sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == KVM_INVALID_SOCK) {
        const int err = WSAGetLastError();
        kvmReportStartup(-1, 1, err);
        std::cout << "[kvm] socket init failed (" << err
                  << "); stitch disabled\n" << std::flush;
        return;
    }
    /* SO_EXCLUSIVEADDRUSE, not SO_REUSEADDR: on Windows the latter allows a
     * second bind on the same port (and would let this process hide an
     * existing listener behind it), so a real port conflict would look like
     * success. Exclusive use makes bind() fail and the GUI sees the truth. */
    BOOL exclusive = TRUE;
    if (setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive, sizeof(exclusive)) != 0) {
        const int err = WSAGetLastError();
        kvmReportStartup(-1, 1, err);
        std::cout << "[kvm] SO_EXCLUSIVEADDRUSE failed (" << err
                  << "); stitch disabled\n" << std::flush;
        kvmClose(srv);
        return;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(KVM_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0) {
        const int err = WSAGetLastError();
        kvmReportStartup(-1, 3, err);
        std::cout << "[kvm] bind failed on " << KVM_PORT << " (error " << err
                  << "); stitch disabled\n" << std::flush;
        kvmClose(srv);
        return;
    }
    if (listen(srv, 4) < 0) {
        const int err = WSAGetLastError();
        kvmReportStartup(-1, 4, err);
        std::cout << "[kvm] listen failed on " << KVM_PORT << " (error " << err
                  << "); stitch disabled\n" << std::flush;
        kvmClose(srv);
        return;
    }
    kvmReportStartup(1, 4, 0);
    std::cout << "[kvm] stitch listener on TCP " << KVM_PORT << "\n"
              << std::flush;

    /* The session runs on its own thread so the listener stays responsive:
     * a second controller gets an immediate KVM_ERR_BUSY instead of sitting
     * in the accept backlog for the whole (possibly hours-long) session, and
     * shutdown never has to wait for the modal consent box to be answered.
     * Only one serveClient runs at a time. `session_active` is the same-port
     * admission only; the cross-mode (8081 vs 8084) slot is the shared gate
     * in auth.h, checked and claimed inside serveClient. */
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
        session = std::thread([client, peer, &running, &session_active, auth] {
            serveClient(client, peer, running, auth);
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
                     const std::atomic<bool> &running,
                     const KvmClientAuth *auth)
{
    (void)peer_ip;
    (void)peer_port;
    (void)side;
    (void)running;
    (void)auth;
    std::cout << "STAT|event=error|msg=拼接模式目前仅支持 Windows\n"
              << std::flush;
    return 3;
}

void kvmSetControllerSide(int side)
{
    (void)side;
}

void kvmServiceRun(std::atomic<bool> &running, const KvmTargetAuth *auth)
{
    (void)running;
    (void)auth;
}

/* 拼接 is Windows-only, so there is no listener to report on other platforms. */
KvmStartupStatus kvmServiceStatus()
{
    KvmStartupStatus st = {};
    st.state = 2;   /* not supported */
    st.stage = 0;
    st.error_code = 0;
    st.port = (uint16_t)KVM_PORT;
    return st;
}

#endif /* _WIN32 */
