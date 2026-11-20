/**
 * TRAY.CPP - Windows notification-area icon + single instance
 *
 * Design notes
 * ------------
 * - The icon is owned by a dedicated thread with a message-only window. The
 *   GUI thread only exchanges atomics / sleeps on one ready event, so tray
 *   callbacks never race the SDL loop.
 * - Tooltip and balloons are marshalled to that thread with WM_APP messages;
 *   only the newest pending text is kept (the shell coalesces anyway).
 * - Single instance uses a Local\ named mutex. A second launch wakes the first
 *   instance's tray window (HWND_MESSAGE child lookup) instead of demanding
 *   the user find the running copy.
 * - Nothing here persists settings: hiding to the tray and quitting must not
 *   silently change 接收开关 / 自动接收.
 */
#include "tray.h"

#include <atomic>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef UNICODE
#    define UNICODE
#  endif
#  ifndef _UNICODE
#    define _UNICODE
#  endif
#  include <windows.h>
#  include <shellapi.h>

namespace {

constexpr UINT kMsgCallback = WM_APP + 21;  /* lParam = mouse message    */
constexpr UINT kMsgShow     = WM_APP + 22;  /* another launch wants focus */
constexpr UINT kMsgTip      = WM_APP + 23;
constexpr UINT kMsgNotify   = WM_APP + 24;
constexpr int  kIdShow   = 1;
constexpr int  kIdQuit   = 2;
constexpr int  kIdStatus = 3;           /* 显示主界面 (status submenus) */
constexpr int  kIdStopSender = 4;
constexpr int  kIdStopReceiver = 5;
/* Host submenu command ids: kIdHostBase + hostIndex * kHostIdStride + action.
 * Actions 0/1/2 are 投屏/扩展/拼接, 3 is 文件传输. The stride leaves room to
 * grow; 20 history entries stay far below the 32-bit command range. */
constexpr int  kIdHostBase   = 100;
constexpr int  kHostIdStride = 8;
constexpr int  kHostActions  = 4;

const wchar_t *kClassName = L"MivaraTrayWnd";
const wchar_t *kMutexName = L"Local\\Mivara.App.SingleInstance";
/* Wake-up channel for a second launch. Local\ like the mutex, so it is
 * per-logon-session. It does not depend on the tray icon being created. */
const wchar_t *kShowEventName = L"Local\\Mivara.App.Show";

std::thread               g_thread;
std::atomic<HWND>         g_hwnd{nullptr};
std::atomic<bool>         g_live{false};
HANDLE                    g_ready = nullptr;
HANDLE                    g_mutex = nullptr;
std::atomic<bool>         g_show_req{false};
std::atomic<bool>         g_quit_req{false};

/* Primary instance only: signalled by later launches; g_wake_stop ends the
 * wait thread. Both are closed by stopWake(). */
HANDLE                    g_show_event = nullptr;
HANDLE                    g_wake_stop = nullptr;
std::thread               g_wake_thread;

/* Waits on the wake channel and only raises the atomic show request. It must
 * never touch SDL or foreground APIs: those belong to the GUI thread. */
void wakeMain()
{
    HANDLE hs[2] = {g_show_event, g_wake_stop};
    for (;;) {
        const DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0)
            g_show_req = true;
        else
            return;     /* stop signalled (or handle failure) */
    }
}

/* Idempotent: signal and join the wake thread before closing its handles, so
 * a joinable std::thread can never reach its destructor on an exit path. */
void stopWake()
{
    if (g_wake_stop) SetEvent(g_wake_stop);
    if (g_wake_thread.joinable()) g_wake_thread.join();
    if (g_show_event) { CloseHandle(g_show_event); g_show_event = nullptr; }
    if (g_wake_stop)  { CloseHandle(g_wake_stop);  g_wake_stop = nullptr; }
}

std::mutex  g_tip_mtx;
std::wstring g_tip;

/* Menu snapshot + the launch request it can raise. Both cross the GUI/tray
 * thread boundary under their own mutex; the tray thread never reads app
 * state. */
std::mutex    g_menu_mtx;
TrayMenuState g_menu;
std::mutex    g_launch_mtx;
struct LaunchReq {
    int         action = TRAY_LAUNCH_NONE;
    int         stop = TRAY_STOP_NONE;
    std::string ip, name;
    int         port = 8081;
};
LaunchReq g_launch;
bool      g_launch_pending = false;

std::mutex  g_notify_mtx;
struct PendingNotify {
    std::wstring title, text;
};
PendingNotify g_notify;

std::wstring wide(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

void copyTip(wchar_t (&dst)[128], const std::wstring &src)
{
    const size_t n = src.size() < 127 ? src.size() : 127;
    wmemcpy(dst, src.c_str(), n);
    dst[n] = L'\0';
}

/* Menu text is a Win32 resource string: a single '&' marks an accelerator,
 * and control characters would break the layout. Peer names come off the
 * network, so escape both instead of trusting them. */
std::wstring menuWide(const std::string &s)
{
    const std::wstring src = wide(s);
    std::wstring out;
    out.reserve(src.size());
    for (wchar_t c : src) {
        if (c < 0x20) c = L' ';
        out.push_back(c);
        if (c == L'&') out.push_back(L'&');
    }
    return out;
}

/* Keep a pathological name from overflowing the menu string limit; never split
 * a UTF-16 surrogate pair. */
std::wstring clampLabel(std::wstring s)
{
    constexpr size_t kMax = 96;
    if (s.size() <= kMax) return s;
    s.resize(kMax);
    /* Never split an escaped "&&" pair or a UTF-16 surrogate pair. */
    if (!s.empty() && s.back() == L'&') s.pop_back();
    if (!s.empty() && s.back() >= 0xD800 && s.back() <= 0xDBFF)
        s.pop_back();
    s += L'…';
    return s;
}

std::wstring hostLabel(const TrayHostEntry &h)
{
    std::wstring name = menuWide(h.name);
    if (name.empty() || h.name == h.ip) {
        /* No usable friendly name (missing, equal to the IP, or not valid
         * UTF-8): fall back to the endpoint alone. */
        const std::wstring ip = menuWide(h.ip);
        return clampLabel(ip.empty() ? L"Mivara 主机" : ip);
    }
    return clampLabel(name + L" (" + menuWide(h.ip) + L")");
}

bool sameMenuState(const TrayMenuState &a, const TrayMenuState &b)
{
    if (a.session != b.session || a.session_mode != b.session_mode ||
        a.receiver != b.receiver || a.hosts.size() != b.hosts.size())
        return false;
    for (size_t i = 0; i < a.hosts.size(); i++) {
        const TrayHostEntry &x = a.hosts[i], &y = b.hosts[i];
        if (x.name != y.name || x.ip != y.ip || x.port != y.port ||
            x.last_mode != y.last_mode || x.detail != y.detail)
            return false;
    }
    return true;
}

const wchar_t *const kModeNames[3] = {L"投屏", L"扩展", L"拼接"};

/* One history host: the floating submenu Windows opens on hover. The first
 * line states what the last session was; the rest are the actions the user
 * can run with a click. */
void fillHostMenu(HMENU sub, const TrayHostEntry &h, int index)
{
    if (!h.detail.empty()) {
        const std::wstring detail = menuWide(h.detail);
        AppendMenuW(sub, MF_STRING | MF_DISABLED, 0, detail.c_str());
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    }
    const int base = kIdHostBase + index * kHostIdStride;
    for (int m = 0; m < 3; m++) {
        std::wstring label = kModeNames[m];
        if (m == h.last_mode) label += L"（上次）";
        AppendMenuW(sub, MF_STRING, base + m, label.c_str());
    }
    AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sub, MF_STRING, base + 3, L"文件传输");
}

NOTIFYICONDATAW makeIcon(HWND hwnd)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    return nid;
}

void applyTip(HWND hwnd, const std::wstring &tip)
{
    NOTIFYICONDATAW nid = makeIcon(hwnd);
    nid.uFlags = NIF_TIP;
    copyTip(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void showBalloon(HWND hwnd, const std::wstring &title, const std::wstring &text)
{
    NOTIFYICONDATAW nid = makeIcon(hwnd);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    const size_t tn = title.size() < 63 ? title.size() : 63;
    wmemcpy(nid.szInfoTitle, title.c_str(), tn);
    nid.szInfoTitle[tn] = L'\0';
    const size_t bn = text.size() < 255 ? text.size() : 255;
    wmemcpy(nid.szInfo, text.c_str(), bn);
    nid.szInfo[bn] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void showMenu(HWND hwnd)
{
    /* Build from the GUI's snapshot, taken once: the menu must stay stable
     * while it is open even if the app's state changes underneath. */
    TrayMenuState st;
    {
        std::lock_guard<std::mutex> lk(g_menu_mtx);
        st = g_menu;
    }
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kIdShow, L"显示主界面");
    /* Live sessions only; the whole block disappears when nothing is running
     * (a failing/stopped child must not leave a stale 正在投屏 line behind). */
    if (!st.session.empty() || !st.receiver.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        /* A live row is a hover submenu: the floating menu keeps the old
         * click-to-show action and adds the matching stop action. */
        if (!st.session.empty()) {
            HMENU sub = CreatePopupMenu();
            if (sub) {
                AppendMenuW(sub, MF_STRING, kIdStatus, L"显示主界面");
                AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
                std::wstring stop = L"停止";
                stop += (st.session_mode >= 0 && st.session_mode <= 2)
                            ? kModeNames[st.session_mode] : L"会话";
                AppendMenuW(sub, MF_STRING, kIdStopSender, stop.c_str());
                const std::wstring label = clampLabel(menuWide(st.session));
                if (!AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, label.c_str()))
                    DestroyMenu(sub);
            }
        }
        if (!st.receiver.empty()) {
            HMENU sub = CreatePopupMenu();
            if (sub) {
                AppendMenuW(sub, MF_STRING, kIdStatus, L"显示主界面");
                AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(sub, MF_STRING, kIdStopReceiver, L"停止接收");
                const std::wstring label = clampLabel(menuWide(st.receiver));
                if (!AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, label.c_str()))
                    DestroyMenu(sub);
            }
        }
    }
    if (!st.hosts.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"历史连接");
        for (size_t i = 0; i < st.hosts.size(); i++) {
            HMENU sub = CreatePopupMenu();
            if (!sub) continue;
            fillHostMenu(sub, st.hosts[i], (int)i);
            const std::wstring label = hostLabel(st.hosts[i]);
            if (!AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, label.c_str()))
                DestroyMenu(sub);
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdQuit, L"退出");
    /* Without foregrounding, the menu stays up after a click elsewhere. */
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == kIdShow || cmd == kIdStatus) {
        g_show_req = true;
    } else if (cmd == kIdQuit) {
        g_quit_req = true;
    } else if (cmd == kIdStopSender || cmd == kIdStopReceiver) {
        std::lock_guard<std::mutex> lk(g_launch_mtx);
        g_launch.stop = (cmd == kIdStopSender) ? TRAY_STOP_SENDER
                                               : TRAY_STOP_RECEIVER;
        g_launch_pending = true;
    } else if (cmd >= (UINT)kIdHostBase) {
        const int rel = (int)cmd - kIdHostBase;
        const size_t idx = (size_t)(rel / kHostIdStride);
        const int act = rel % kHostIdStride;
        if (idx < st.hosts.size() && act >= 0 && act < kHostActions) {
            const TrayHostEntry &h = st.hosts[idx];
            std::lock_guard<std::mutex> lk(g_launch_mtx);
            g_launch.action = (act == 0) ? TRAY_LAUNCH_MIRROR
                            : (act == 1) ? TRAY_LAUNCH_EXTEND
                            : (act == 2) ? TRAY_LAUNCH_STITCH
                                         : TRAY_LAUNCH_FILES;
            g_launch.ip = h.ip;
            g_launch.name = h.name;
            g_launch.port = h.port;
            g_launch_pending = true;
        }
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case kMsgCallback:
        if (LOWORD(lp) == WM_LBUTTONUP)      g_show_req = true;
        else if (LOWORD(lp) == WM_RBUTTONUP) showMenu(hwnd);
        return 0;
    case kMsgShow:
        g_show_req = true;
        return 0;
    case kMsgTip: {
        std::wstring t;
        {
            std::lock_guard<std::mutex> lk(g_tip_mtx);
            t = g_tip;
        }
        if (!t.empty()) applyTip(hwnd, t);
        return 0;
    }
    case kMsgNotify: {
        PendingNotify pn;
        {
            std::lock_guard<std::mutex> lk(g_notify_mtx);
            pn = g_notify;
            g_notify = PendingNotify();
        }
        if (!pn.title.empty() || !pn.text.empty())
            showBalloon(hwnd, pn.title, pn.text);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void threadMain()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);          /* already registered is fine */

    HWND hwnd = CreateWindowExW(0, kClassName, L"Mivara", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, inst, nullptr);
    if (!hwnd) {
        if (g_ready) SetEvent(g_ready);
        return;
    }
    g_hwnd = hwnd;

    NOTIFYICONDATAW nid = makeIcon(hwnd);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kMsgCallback;
    nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    {
        std::lock_guard<std::mutex> lk(g_tip_mtx);
        copyTip(nid.szTip, g_tip);
    }
    const bool added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    g_live = added;
    if (g_ready) SetEvent(g_ready);
    /* A rejected icon is not fatal: keep the message-only window and its
     * loop alive so it still serves as the FindWindowExW wake-up fallback.
     * NIM_DELETE below is harmless for an icon that was never added. */

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

} // namespace

bool traySingleInstance()
{
    g_mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!g_mutex) return true;      /* no mutex: behave as a normal single run */
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        /* Primary instance: own the wake-up channel. It is independent of the
         * tray icon, which may never be added (no shell / NIM_ADD failure). */
        g_show_event = CreateEventW(nullptr, FALSE, FALSE, kShowEventName);
        g_wake_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_show_event && g_wake_stop)
            g_wake_thread = std::thread(wakeMain);
        else
            stopWake();             /* no channel: rely on the message window */
        return true;
    }

    /* Second instance: ask the primary to show itself. The named event works
     * even when the tray icon could not be created; the tray message-only
     * window stays as a fallback (and is the only option for older builds).
     * Both are tried each round; either succeeding is enough. */
    for (int i = 0; i < 30; i++) {
        bool woken = false;
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                               kShowEventName);
        if (ev) {
            if (SetEvent(ev)) woken = true;
            CloseHandle(ev);
        }
        HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, kClassName, nullptr);
        if (hwnd) {
            PostMessageW(hwnd, kMsgShow, 0, 0);
            woken = true;
        }
        if (woken) break;
        Sleep(100);
    }
    CloseHandle(g_mutex);
    g_mutex = nullptr;
    return false;
}

void traySingleInstanceRelease()
{
    stopWake();
    if (g_mutex) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
}

bool trayInit(const std::string &tooltip)
{
    if (g_thread.joinable()) return g_live.load();
    {
        std::lock_guard<std::mutex> lk(g_tip_mtx);
        g_tip = wide(tooltip);
    }
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = std::thread(threadMain);
    if (g_ready) {
        WaitForSingleObject(g_ready, 3000);
        CloseHandle(g_ready);
        g_ready = nullptr;
    }
    return g_live.load();
}

void traySetTooltip(const std::string &tooltip)
{
    const std::wstring w = wide(tooltip);
    {
        std::lock_guard<std::mutex> lk(g_tip_mtx);
        g_tip = w;
    }
    HWND hwnd = g_hwnd.load();
    if (hwnd) PostMessageW(hwnd, kMsgTip, 0, 0);
}

void trayNotify(const std::string &title, const std::string &text)
{
    {
        std::lock_guard<std::mutex> lk(g_notify_mtx);
        g_notify.title = wide(title);
        g_notify.text = wide(text);
    }
    HWND hwnd = g_hwnd.load();
    if (hwnd) PostMessageW(hwnd, kMsgNotify, 0, 0);
}

void traySetMenuState(const TrayMenuState &state)
{
    std::lock_guard<std::mutex> lk(g_menu_mtx);
    if (sameMenuState(g_menu, state)) return;
    g_menu = state;
}

TrayRequests trayTakeRequests()
{
    TrayRequests r;
    r.show = g_show_req.exchange(false);
    r.quit = g_quit_req.exchange(false);
    {
        std::lock_guard<std::mutex> lk(g_launch_mtx);
        if (g_launch_pending) {
            r.launch = g_launch.action;
            r.stop = g_launch.stop;
            r.ip = g_launch.ip;
            r.name = g_launch.name;
            r.port = g_launch.port;
            g_launch = LaunchReq();
            g_launch_pending = false;
        }
    }
    return r;
}

void trayShutdown()
{
    /* Stop the wake thread first: trayShutdown/Release may be called back to
     * back, and a still-joinable std::thread must never be destructed. */
    stopWake();
    if (g_thread.joinable()) {
        HWND hwnd = g_hwnd.load();
        if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        g_thread.join();
    }
    g_hwnd = nullptr;
    g_live = false;
    {
        std::lock_guard<std::mutex> lk(g_menu_mtx);
        g_menu = TrayMenuState();
    }
    {
        std::lock_guard<std::mutex> lk(g_launch_mtx);
        g_launch = LaunchReq();
        g_launch_pending = false;
    }
}

#else  /* !_WIN32 */

bool traySingleInstance() { return true; }
void traySingleInstanceRelease() {}
bool trayInit(const std::string &) { return false; }
void traySetTooltip(const std::string &) {}
void trayNotify(const std::string &, const std::string &) {}
void traySetMenuState(const TrayMenuState &) {}
TrayRequests trayTakeRequests() { return TrayRequests(); }
void trayShutdown() {}

#endif
