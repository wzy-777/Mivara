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
constexpr int  kIdShow = 1;
constexpr int  kIdQuit = 2;

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
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kIdShow, L"显示主界面");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdQuit, L"退出");
    /* Without foregrounding, the menu stays up after a click elsewhere. */
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == kIdShow)      g_show_req = true;
    else if (cmd == kIdQuit) g_quit_req = true;
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

TrayRequests trayTakeRequests()
{
    TrayRequests r;
    r.show = g_show_req.exchange(false);
    r.quit = g_quit_req.exchange(false);
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
}

#else  /* !_WIN32 */

bool traySingleInstance() { return true; }
void traySingleInstanceRelease() {}
bool trayInit(const std::string &) { return false; }
void traySetTooltip(const std::string &) {}
void trayNotify(const std::string &, const std::string &) {}
TrayRequests trayTakeRequests() { return TrayRequests(); }
void trayShutdown() {}

#endif
