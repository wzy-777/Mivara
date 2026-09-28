/**
 * SENDER.CPP - SCREEN CAPTURE, STREAMING, AND VIRTUAL DISPLAY EXTENSION
 *
 * Cross-platform: Windows (GDI), Linux (X11), macOS (CoreGraphics).
 *
 * Screen Extender Mode
 * --------------------
 * The sender manages a virtual display (indirect display driver, e.g.
 * VirtualDrivers/Virtual-Display-Driver "MttVDD"). In extend mode the
 * virtual monitor is ATTACHED to the desktop for the duration of the
 * session, positioned right of the existing screens, and its content is
 * captured and streamed. The result is a REAL second desktop: windows
 * and the mouse can be moved across the boundary. When the session ends
 * (or Ctrl+C) the virtual display is detached again.
 *
 * In mirror mode the primary screen is streamed as-is.
 *
 * Handshake (sender → receiver):
 *   uint32_t  sender_width     – sender screen width
 *   uint32_t  sender_height    – sender screen height
 *   uint32_t  fps              – target frame rate
 *   uint32_t  mode             – 0=mirror  1=true-extend
 *
 * Handshake response (receiver → sender):
 *   uint32_t  receiver_width   – receiver display width
 *   uint32_t  receiver_height  – receiver display height
 *   uint32_t  status           – 0=ok
 *
 * After the handshake the sender streams frames exactly as before.
 * In extend_right mode the sender also registers a virtual desktop offset
 * so that OS-level pointer warp (optional, per platform) can move the
 * mouse to the receiver screen edge naturally.
 *
 * Platform notes
 * --------------
 * Windows : GDI capture; Winsock2.
 * Linux   : X11 capture; POSIX sockets. Wayland: set WAYLAND_DISPLAY="".
 * macOS   : CoreGraphics capture; POSIX sockets.
 *           Link: -framework CoreGraphics -framework CoreFoundation
 *
 * Build deps: X11 (Linux), pthreads  (no SDL – console program)
 */

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iomanip>
#include <fstream>
#include <cstdint>
#include <algorithm>
#include "discover.h"
#include "gpu_accelerate.h"
#include "ports.h"

/* ── Platform headers ───────────────────────────────────────────────────── */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601   /* QueryDisplayConfig / SetDisplayConfig */
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <mstcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#elif defined(__APPLE__)
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <CoreGraphics/CoreGraphics.h>
#else  /* Linux */
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#endif

/* ── Display mode (values go over the wire – keep in sync with receiver) ── */
#define MODE_MIRROR        0u
#define MODE_TRUE_EXTEND   1u

/* ── Constants ──────────────────────────────────────────────────────────── */
#define BYTES_PER_PIXEL       3
#define CONNECTION_TIMEOUT_MS 10000//5000
#define STATS_INTERVAL_SEC    5
#define MAX_FRAME_SKIP        3
#define MAX_FRAME_BYTES       (7680u * 4320u * 3u)

static const int SOCK_BUF = 4 * 1024 * 1024;

/* ── ANSI colours (Windows CMD does not support VT100 by default) ────────── */
#ifdef _WIN32
#  define COL_RESET   ""
#  define COL_RED     ""
#  define COL_GREEN   ""
#  define COL_YELLOW  ""
#  define COL_CYAN    ""
#  define COL_MAGENTA ""
#  define COL_BOLD    ""
#else
#  define COL_RESET   "\033[0m"
#  define COL_RED     "\033[31m"
#  define COL_GREEN   "\033[32m"
#  define COL_YELLOW  "\033[33m"
#  define COL_CYAN    "\033[36m"
#  define COL_MAGENTA "\033[35m"
#  define COL_BOLD    "\033[1m"
#endif

/* ── Globals ─────────────────────────────────────────────────────────────── */
static int SCREEN_WIDTH  = 1920;
static int SCREEN_HEIGHT = 1080;
static int TARGET_FPS    = 120;
static uint32_t DISPLAY_MODE = MODE_MIRROR;      /* default: mirror */

/* Receiver display dimensions (filled after handshake response) */
static int RECV_WIDTH  = 1920;
static int RECV_HEIGHT = 1080;

#ifdef _WIN32
/* True-Extend mode: capture a specific monitor instead of the primary.
 * g_cap_device empty  -> legacy primary-screen capture (GetDC(nullptr))
 * g_cap_device set    -> capture that monitor via CreateDC (e.g. a virtual
 *                        display provided by an indirect display driver).  */
static std::string g_cap_device;
static POINT       g_cap_origin = {0, 0};   /* monitor origin in virtual desktop */
#endif

static std::atomic<bool> g_running{true};

/* GPU offload */
static gpu_sock_t g_gpu_sock   = GPU_INVALID_SOCK;
static bool       g_gpu_active = false;

/* Port inspection */
static ports_sock_t g_ports_sock = PORTS_INVALID_SOCK;

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN DIMENSIONS
 * ══════════════════════════════════════════════════════════════════════════ */
static void getScreenDimensions(int &w, int &h)
{
#ifdef _WIN32
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    std::cout << "Display: " << w << "x" << h << " (GDI)\n";

#elif defined(__APPLE__)
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(kCGDirectMainDisplay);
    if (mode) {
        w = (int)CGDisplayModeGetWidth(mode);
        h = (int)CGDisplayModeGetHeight(mode);
        CGDisplayModeRelease(mode);
        std::cout << "Display: " << w << "x" << h << " (CoreGraphics)\n";
    } else {
        w = 1920; h = 1080;
        std::cerr << COL_YELLOW
                  << "CoreGraphics query failed; using 1920x1080\n"
                  << COL_RESET;
    }

#else   /* Linux / X11 */
    Display *dpy = XOpenDisplay(nullptr);
    if (dpy) {
        int sn = DefaultScreen(dpy);
        w = DisplayWidth(dpy, sn);
        h = DisplayHeight(dpy, sn);
        XCloseDisplay(dpy);
        std::cout << "Display: " << w << "x" << h << " (X11)\n";
    } else {
        w = 1920; h = 1080;
        std::cerr << COL_YELLOW
                  << "X11 open failed; defaulting to 1920x1080\n"
                  << COL_RESET;
    }
#endif
}

/* ══════════════════════════════════════════════════════════════════════════
 * NETWORK SOCKET
 * ══════════════════════════════════════════════════════════════════════════ */
class NetworkSocket
{
#ifdef _WIN32
    SOCKET _s = INVALID_SOCKET;
    bool valid()    const { return _s != INVALID_SOCKET; }
    void raw_close()      { closesocket(_s); _s = INVALID_SOCKET; }
#else
    int _s = -1;
    bool valid()    const { return _s >= 0; }
    void raw_close()      { ::close(_s); _s = -1; }
#endif

public:
    ~NetworkSocket() { close(); }

    void close() { if (valid()) raw_close(); }

    bool create() {
        close();
        _s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
        return _s != INVALID_SOCKET;
#else
        return _s >= 0;
#endif
    }

    bool connect(const std::string &ip, int port,
                 int timeout_ms = CONNECTION_TIMEOUT_MS)
    {
        if (!create()) return false;

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            std::cerr << COL_RED << "Invalid IP: " << ip << COL_RESET << "\n";
            return false;
        }

        /* Non-blocking connect with timeout */
#ifdef _WIN32
        u_long nb = 1; ioctlsocket(_s, FIONBIO, &nb);
#else
        int fl = fcntl(_s, F_GETFL, 0);
        fcntl(_s, F_SETFL, fl | O_NONBLOCK);
#endif
        ::connect(_s, (struct sockaddr *)&addr, sizeof(addr));

        fd_set fds; FD_ZERO(&fds); FD_SET(_s, &fds);
        struct timeval tv = { timeout_ms / 1000,
                              (timeout_ms % 1000) * 1000 };
        bool ok = false;
        if (select((int)_s + 1, nullptr, &fds, nullptr, &tv) == 1) {
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(_s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            ok = (err == 0);
        }

#ifdef _WIN32
        { u_long bl = 0; ioctlsocket(_s, FIONBIO, &bl); }
#else
        fcntl(_s, F_SETFL, fl);
#endif
        if (!ok) {
            std::cerr << COL_RED << "Timeout: " << ip << ":"
                      << port << COL_RESET << "\n";
            close(); return false;
        }

        int nd = 1, sb = SOCK_BUF;
        setsockopt(_s, IPPROTO_TCP, TCP_NODELAY, (char *)&nd, sizeof(nd));
        setsockopt(_s, SOL_SOCKET,  SO_SNDBUF,   (char *)&sb, sizeof(sb));
        std::cout << COL_GREEN << "Connected to "
                  << ip << ":" << port << COL_RESET << "\n";
        return true;
    }

    bool sendAll(const void *data, size_t size) {
        const char *p = (const char *)data;
        size_t sent = 0;
        while (sent < size && g_running) {
            int n = (int)send(_s, p + sent, (int)(size - sent), 0);
#ifdef _WIN32
            if (n == SOCKET_ERROR) {
                std::cerr << COL_RED << "Send error: "
                          << WSAGetLastError() << COL_RESET << "\n";
                return false;
            }
#else
            if (n < 0) {
                std::cerr << COL_RED << "Send error: "
                          << strerror(errno) << COL_RESET << "\n";
                return false;
            }
#endif
            if (n == 0) return false;
            sent += (size_t)n;
        }
        return sent == size;
    }

    bool recvAll(void *data, size_t size) {
        char *p = (char *)data;
        size_t got = 0;
        while (got < size) {
            int n = (int)recv(_s, p + got, (int)(size - got), 0);
            if (n <= 0) return false;
            got += (size_t)n;
        }
        return true;
    }
};

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN CAPTURE  –  platform-specific, always returns RGB24
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef _WIN32
static std::vector<uint8_t> captureScreen()
{
    std::vector<uint8_t> px(SCREEN_WIDTH * SCREEN_HEIGHT * BYTES_PER_PIXEL, 0);
    /* Always capture through the screen DC (GetDC(nullptr)) and offset by
     * g_cap_origin.  A per-monitor CreateDC uses device coordinates which do
     * NOT match the screen coordinates returned by GetCursorInfo /
     * GetMonitorInfo under DPI scaling – that mismatch put the cursor draw
     * position outside the frame on the extended (virtual) display.  The
     * screen DC keeps capture + cursor in one single coordinate system, and
     * CAPTUREBLT (layered windows) is only reliable here. */
    HDC sdc = GetDC(nullptr);
    if (!sdc) return px;
    HDC     mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!bmp) {
        DeleteDC(mdc);
        ReleaseDC(nullptr, sdc);
        return px;
    }
    SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, sdc,
           g_cap_origin.x, g_cap_origin.y, SRCCOPY | CAPTUREBLT);

    /* Draw the mouse cursor into the frame so it is visible on the receiver.
     * This also implements pointer "handoff": when the user moves the cursor
     * into the streamed monitor's area, the receiver shows it. */
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    bool haveCur = GetCursorInfo(&ci) != FALSE;

    if (haveCur && (ci.flags & CURSOR_SHOWING)) {
        int cx = ci.ptScreenPos.x - g_cap_origin.x;
        int cy = ci.ptScreenPos.y - g_cap_origin.y;
        if (cx >= 0 && cy >= 0 && cx < SCREEN_WIDTH && cy < SCREEN_HEIGHT) {
            ICONINFO ii;
            if (GetIconInfo(ci.hCursor, &ii)) {
                int cw = GetSystemMetrics(SM_CXCURSOR);
                int ch = GetSystemMetrics(SM_CYCURSOR);
                DrawIconEx(mdc, cx - ii.xHotspot, cy - ii.yHotspot,
                           ci.hCursor, cw, ch, 0, NULL, DI_NORMAL);
                if (ii.hbmMask)  DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
        }
    }

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = SCREEN_WIDTH;
    bi.biHeight = -SCREEN_HEIGHT;          /* top-down, no flip needed */
    bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    GetDIBits(mdc, bmp, 0, SCREEN_HEIGHT, px.data(),
              (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    DeleteObject(bmp); DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    /* GDI returns BGR; swap to RGB for network transmission */
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++)
        std::swap(px[i*3], px[i*3+2]);
    return px;
}

#elif defined(__APPLE__)
static std::vector<uint8_t> captureScreen()
{
    std::vector<uint8_t> px(SCREEN_WIDTH * SCREEN_HEIGHT * BYTES_PER_PIXEL, 0);
    CGImageRef img = CGDisplayCreateImage(kCGDirectMainDisplay);
    if (!img) return px;
    CGDataProviderRef dp  = CGImageGetDataProvider(img);
    CFDataRef         raw = CGDataProviderCopyData(dp);
    if (!raw) { CGImageRelease(img); return px; }
    const uint8_t *src = CFDataGetBytePtr(raw);
    size_t bpr = CGImageGetBytesPerRow(img);
    size_t bpp = CGImageGetBitsPerPixel(img) / 8; /* usually 4: BGRA */
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            size_t si = y * bpr + x * bpp;
            size_t di = ((size_t)y * SCREEN_WIDTH + x) * 3;
            px[di+0] = src[si+2]; /* R */
            px[di+1] = src[si+1]; /* G */
            px[di+2] = src[si+0]; /* B */
        }
    }
    CFRelease(raw);
    CGImageRelease(img);
    return px;
}

#else   /* Linux / X11 */
static std::vector<uint8_t> captureScreen()
{
    std::vector<uint8_t> px(SCREEN_WIDTH * SCREEN_HEIGHT * BYTES_PER_PIXEL, 0);
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) return px;
    Window root = RootWindow(dpy, DefaultScreen(dpy));
    XImage *xi  = XGetImage(dpy, root, 0, 0,
                             SCREEN_WIDTH, SCREEN_HEIGHT, AllPlanes, ZPixmap);
    if (!xi) { XCloseDisplay(dpy); return px; }
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            unsigned long p = XGetPixel(xi, x, y);
            size_t i = ((size_t)y * SCREEN_WIDTH + x) * 3;
            px[i+0] = (uint8_t)((p >> 16) & 0xFF); /* R */
            px[i+1] = (uint8_t)((p >>  8) & 0xFF); /* G */
            px[i+2] = (uint8_t)( p        & 0xFF); /* B */
        }
    }
    XDestroyImage(xi);
    XCloseDisplay(dpy);
    return px;
}
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * VIRTUAL DISPLAY MANAGEMENT (Windows)
 *
 * Deep-integrated lifecycle management for the virtual display driver
 * (VirtualDrivers/Virtual-Display-Driver, device id "MTT1337"):
 *   - vddStartup()   : activate the virtual monitor (CCD SetDisplayConfig),
 *                      place it right of the current desktop, 1920x1080@60
 *   - vddShutdown()  : deactivate it again (only if we activated it)
 *
 * The virtual display is only attached to the desktop while a True-Extend
 * streaming session is running.
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef _WIN32
static bool g_vdd_we_attached = false;   /* we turned it on -> we turn it off */

/* Match a CCD target whose friendly monitor name identifies the VDD
 * ("VDD by MTT" / "MTT1337" family). Real monitors never match. */
static bool vddNameMatch(const WCHAR *wname)
{
    char buf[128];
    int  n = 0;
    for (; wname && wname[n] && n < 127; n++)
        buf[n] = (char)wname[n];
    buf[n] = '\0';
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(buf, "vdd") || strstr(buf, "mtt");
}

/* Iterate all CCD paths; run fn(path) for every path whose target monitor
 * is the VDD. fn returns true to keep scanning. Returns true if any VDD
 * path was seen. */
template <typename F>
static bool vddForEachPath(F fn)
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &nPaths, &nModes) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &nPaths, paths.data(),
                           &nModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    bool seen = false;
    for (UINT32 i = 0; i < nPaths; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
        tn.header.type       = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size       = sizeof(tn);
        tn.header.adapterId  = paths[i].targetInfo.adapterId;
        tn.header.id         = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS)
            continue;
        if (!vddNameMatch(tn.monitorFriendlyDeviceName))
            continue;
        seen = true;
        if (!fn(paths[i], nPaths, paths, nModes, modes))
            break;
    }
    return seen;
}

/* Does the VDD driver expose a monitor at all (active or not)? */
static bool vddInstalled()
{
    return vddForEachPath([](DISPLAYCONFIG_PATH_INFO &,
                             UINT32, std::vector<DISPLAYCONFIG_PATH_INFO> &,
                             UINT32, std::vector<DISPLAYCONFIG_MODE_INFO> &) {
        return true;
    });
}

/* Activate / deactivate every VDD path via the CCD topology. */
static bool vddApplyTopology(bool active)
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &nPaths, &nModes) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &nPaths, paths.data(),
                           &nModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    bool touched = false;
    for (UINT32 i = 0; i < nPaths; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
        tn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size      = sizeof(tn);
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
        if (!vddNameMatch(tn.monitorFriendlyDeviceName))            continue;

        if (active) paths[i].flags |=  DISPLAYCONFIG_PATH_ACTIVE;
        else        paths[i].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
        touched = true;
    }
    if (!touched) return false;

    LONG st = SetDisplayConfig(nPaths, paths.data(), nModes, modes.data(),
                               SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY);
    return st == ERROR_SUCCESS;
}

/* Find the GDI device name (\\.\DISPLAYn) of the *active* VDD monitor. */
static bool vddFindActiveDevice(std::string &out)
{
    for (DWORD i = 0; ; i++) {
        DISPLAY_DEVICEA dd = {};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE) ||
            !(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        DISPLAY_DEVICEA mon = {};
        mon.cb = sizeof(mon);
        if (EnumDisplayDevicesA(dd.DeviceName, 0, &mon, 0) &&
            (strstr(mon.DeviceID, "MTT") || strstr(mon.DeviceString, "VDD"))) {
            out = dd.DeviceName;
            return true;
        }
    }
    return false;
}

/* Set resolution/refresh/position of a display via the classic CDS API.
 * IMPORTANT: use dynamic apply (flags = 0). CDS_UPDATEREGISTRY is rejected
 * by this IddCx virtual display (DISP_CHANGE_FAILED); the dynamic apply
 * works reliably right after attach. The mode is re-applied at every
 * session start, so persistence is not needed. */
static bool vddConfigure(const std::string &device, int x, int y,
                         int w, int h, int hz)
{
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExA(device.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
        return false;
    dm.dmFields           = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT |
                            DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    dm.dmBitsPerPel       = 32;
    dm.dmPosition.x       = x;
    dm.dmPosition.y       = y;
    dm.dmPelsWidth        = w;
    dm.dmPelsHeight       = h;
    dm.dmDisplayFrequency = hz;
    LONG r = ChangeDisplaySettingsExA(device.c_str(), &dm, NULL, 0, NULL);
    return r == DISP_CHANGE_SUCCESSFUL;
}

/* Is this GDI display device backed by the virtual display driver? */
static bool isVddMonitorDevice(const char *gdiDevice)
{
    DISPLAY_DEVICEA mon = {};
    mon.cb = sizeof(mon);
    if (EnumDisplayDevicesA(gdiDevice, 0, &mon, 0))
        return strstr(mon.DeviceID, "MTT")   ||
               strstr(mon.DeviceID, "VDD")   ||
               strstr(mon.DeviceString, "VDD");
    return false;
}

/* Rightmost desktop edge EXCLUDING virtual-display monitors, so the VDD
 * can be placed right after the real desktop (not after itself). */
struct EdgeScanCtx { int right; };
static BOOL CALLBACK edgeScanProc(HMONITOR hmon, HDC, LPRECT rc, LPARAM lp)
{
    auto *ctx = (EdgeScanCtx *)lp;
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(hmon, (MONITORINFO *)&mi) &&
        !isVddMonitorDevice(mi.szDevice)) {
        if (rc->right > ctx->right) ctx->right = rc->right;
    }
    return TRUE;
}

static int desktopRightEdge()
{
    EdgeScanCtx ctx = {0};
    EnumDisplayMonitors(NULL, NULL, edgeScanProc, (LPARAM)&ctx);
    return ctx.right;
}

/* Locate a monitor rect in the virtual desktop by GDI device name. */
struct MonRectSearch { const char *dev; RECT rc; bool found; };
static BOOL CALLBACK findMonRectProc(HMONITOR hmon, HDC, LPRECT, LPARAM lp)
{
    auto *s = (MonRectSearch *)lp;
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(hmon, (MONITORINFO *)&mi) &&
        _stricmp(mi.szDevice, s->dev) == 0) {
        s->rc    = mi.rcMonitor;
        s->found = true;
        return FALSE;
    }
    return TRUE;
}

/* Full startup: attach VDD, place it right of the desktop, aim capture.
 * w/h = requested virtual display resolution. */
static void vddShutdown();   /* forward – used by vddStartup on error paths */

static bool vddStartup(int w, int h)
{
    if (!vddInstalled()) {
        std::cerr << COL_RED
                  << "Virtual display driver not found.\n"
                     "  Install VirtualDrivers/Virtual-Display-Driver (MttVDD)\n"
                     "  first – see README.md.\n"
                  << COL_RESET;
        return false;
    }

    bool alreadyActive = false;
    vddForEachPath([&](DISPLAYCONFIG_PATH_INFO &p, UINT32,
                       std::vector<DISPLAYCONFIG_PATH_INFO> &,
                       UINT32, std::vector<DISPLAYCONFIG_MODE_INFO> &) {
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) alreadyActive = true;
        return !alreadyActive;   /* stop early once found */
    });

    /* Compute the anchor BEFORE attaching: right edge of the real desktop
     * (VDD excluded), so the virtual display lands right after it. */
    int x = desktopRightEdge();

    if (!alreadyActive) {
        std::cout << COL_CYAN << "Activating virtual display...\n" << COL_RESET;
        if (!vddApplyTopology(true)) {
            std::cerr << COL_RED << "Failed to activate virtual display\n"
                      << COL_RESET;
            return false;
        }
        g_vdd_we_attached = true;
        Sleep(1500);              /* let the topology settle */
    }

    std::string dev;
    if (!vddFindActiveDevice(dev)) {
        std::cerr << COL_RED
                  << "Virtual display did not come up (no active MTT device)\n"
                  << COL_RESET;
        vddShutdown();
        return false;
    }

    /* Deterministic placement: requested mode @60 right of the real desktop.
     * A freshly attached display may reject mode changes for a moment,
     * so retry a few times before falling back. */
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        ok = vddConfigure(dev, x, 0, w, h, 60);
        if (!ok) Sleep(600);
    }
    if (!ok && (w != 1920 || h != 1080)) {
        std::cerr << COL_YELLOW
                  << "Requested mode " << w << "x" << h
                  << " rejected – falling back to 1920x1080\n" << COL_RESET;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            ok = vddConfigure(dev, x, 0, 1920, 1080, 60);
            if (!ok) Sleep(600);
        }
    }
    if (!ok)
        std::cerr << COL_YELLOW
                  << "Mode change not applied – streaming current mode\n"
                  << COL_RESET;
    Sleep(800);

    MonRectSearch s = {};
    s.dev = dev.c_str();
    EnumDisplayMonitors(NULL, NULL, findMonRectProc, (LPARAM)&s);
    if (!s.found) {
        std::cerr << COL_RED << "Virtual display rect not found\n" << COL_RESET;
        vddShutdown();
        return false;
    }

    g_cap_device   = dev;
    g_cap_origin.x = s.rc.left;
    g_cap_origin.y = s.rc.top;
    SCREEN_WIDTH   = s.rc.right  - s.rc.left;
    SCREEN_HEIGHT  = s.rc.bottom - s.rc.top;

    std::cout << COL_GREEN << "Virtual display ready: " << dev << "  "
              << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
              << " @(" << s.rc.left << "," << s.rc.top << ")\n" << COL_RESET;
    return true;
}

/* Tear down: detach the virtual display again if we attached it. */
static void vddShutdown()
{
    if (!g_vdd_we_attached) return;
    g_vdd_we_attached = false;
    std::cout << COL_CYAN << "Deactivating virtual display...\n" << COL_RESET;
    vddApplyTopology(false);
}

/* Ask the user which resolution the virtual display should use.
 * Modes mirror the driver's vdd_settings.xml. Default: 1920x1080. */
static void selectExtendResolution(int &w, int &h)
{
    struct Mode { int w, h; const char *label; };
    static const Mode modes[] = {
        {1920, 1080, "1920x1080  (FHD)"},
        {2560, 1440, "2560x1440  (QHD)"},
        {3840, 2160, "3840x2160  (4K)"},
        {1366,  768, "1366x768"},
        { 800,  600, "800x600"},
    };
    const int nModes = (int)(sizeof(modes) / sizeof(modes[0]));

    std::cout << "\n"
              << COL_CYAN << COL_BOLD
              << "  Virtual display resolution:\n" << COL_RESET;
    for (int i = 0; i < nModes; i++)
        std::cout << "  " << (i == 0 ? COL_GREEN : COL_YELLOW) << (i + 1)
                  << COL_RESET << "  " << modes[i].label << "\n";
    std::cout << "  (resolution can also be changed later in Windows "
                 "display settings – the stream adapts automatically)\n";
    std::cout << COL_BOLD << "  Choice [1]: " << COL_RESET;

    std::string line;
    std::getline(std::cin, line);
    int idx = 0;
    if (!line.empty()) {
        try { idx = std::stoi(line) - 1; }
        catch (...) { idx = -1; }
    }
    if (idx < 0 || idx >= nModes) {
        std::cout << COL_YELLOW << "  Using default 1920x1080\n" << COL_RESET;
        idx = 0;
    }
    w = modes[idx].w;
    h = modes[idx].h;
    std::cout << COL_GREEN << "  Resolution: " << w << "x" << h << "\n"
              << COL_RESET;
}
#endif /* _WIN32 */

/* ══════════════════════════════════════════════════════════════════════════
 * DISPLAY MODE SELECTION
 * ══════════════════════════════════════════════════════════════════════════ */
static uint32_t selectDisplayMode()
{
    std::cout << "\n"
              << COL_CYAN << COL_BOLD
              << "  Select display mode:\n" << COL_RESET
              << "  " << COL_CYAN << "1" << COL_RESET
                      << "  Mirror        (duplicate this screen)\n"
#ifdef _WIN32
              << "  " << COL_GREEN << "2" << COL_RESET
                      << "  Extend        (real 2nd desktop – virtual display is\n"
                      << "                   attached automatically for the session)\n"
#endif
              << COL_BOLD << "  Choice [1]: " << COL_RESET;

    std::string line;
    std::getline(std::cin, line);

#ifdef _WIN32
    if (line == "2") return MODE_TRUE_EXTEND;
#endif
    return MODE_MIRROR;             /* default */
}

/* ══════════════════════════════════════════════════════════════════════════
 * STATS
 * ══════════════════════════════════════════════════════════════════════════ */
static void showStats(int frames, long elapsed, size_t bytes)
{
    if (elapsed < 1) elapsed = 1;
    float fps  = (float)frames / (float)elapsed;
    float mbps = (float)(bytes / (1024.0 * 1024.0)) / (float)elapsed;
    const char *modeStr =
        DISPLAY_MODE == MODE_TRUE_EXTEND ? "extend" : "mirror";

    std::cout << COL_CYAN
              << "Frames: " << frames
              << "  FPS: "  << std::fixed << std::setprecision(1) << fps
                            << "/" << TARGET_FPS
              << "  BW: "   << std::setprecision(2) << mbps << " MB/s"
              << "  Src: "  << SCREEN_WIDTH  << "x" << SCREEN_HEIGHT
              << "  Dst: "  << RECV_WIDTH    << "x" << RECV_HEIGHT
              << "  Mode: " << modeStr
              << (g_gpu_active ? "  GPU:remote" : "  GPU:local")
              << COL_RESET << "\n";
}

/* ══════════════════════════════════════════════════════════════════════════
 * PORTS INTERACTIVE MENU  –  inspect/manage receiver's ports
 * ══════════════════════════════════════════════════════════════════════════ */
static void print_port_entries(const PortEntry *entries, uint32_t count)
{
    if (count == 0) {
        std::cout << COL_YELLOW << "  (no entries)\n" << COL_RESET;
        return;
    }
    std::cout << COL_CYAN << COL_BOLD
              << std::left
              << std::setw(4)  << "Pro"
              << std::setw(22) << "Local Address"
              << std::setw(22) << "Remote Address"
              << std::setw(14) << "State"
              << std::setw(8)  << "PID"
              << "Process\n"
              << std::string(80, '-') << "\n"
              << COL_RESET;
    for (uint32_t i = 0; i < count; i++) {
        std::cout << ports_entry_to_string(entries[i]) << "\n";
    }
    std::cout << COL_GREEN << "  Total: " << count << " entries\n" << COL_RESET;
}

static void ports_interactive_menu()
{
    if (!PORTS_SOCK_VALID(g_ports_sock)) {
        std::cout << COL_RED << "Port service not connected.\n" << COL_RESET;
        return;
    }

    while (true) {
        std::cout << "\n" << COL_CYAN << COL_BOLD
                  << "╔════════════════════════════════╗\n"
                  << "║   RECEIVER PORT INSPECTOR      ║\n"
                  << "╠════════════════════════════════╣\n"
                  << "║  1. List all TCP ports         ║\n"
                  << "║  2. List all UDP ports         ║\n"
                  << "║  3. List ALL ports             ║\n"
                  << "║  4. Query specific port        ║\n"
                  << "║  5. Kill process on port       ║\n"
                  << "║  0. Back to stream             ║\n"
                  << "╚════════════════════════════════╝\n"
                  << COL_RESET
                  << COL_BOLD << "Choice: " << COL_RESET;

        std::string line;
        std::getline(std::cin, line);
        if (line.empty()) continue;
        int choice = -1;
        try { choice = std::stoi(line); } catch (...) {}

        if (choice == 0) break;

        PortEntry *entries = nullptr;
        uint32_t   count  = 0;

        switch (choice) {
        case 1:
            std::cout << COL_CYAN << "\n── TCP Ports ──\n" << COL_RESET;
            ports_remote_list_tcp(g_ports_sock, &entries, &count);
            print_port_entries(entries, count);
            ports_free_entries(entries);
            break;

        case 2:
            std::cout << COL_CYAN << "\n── UDP Ports ──\n" << COL_RESET;
            ports_remote_list_udp(g_ports_sock, &entries, &count);
            print_port_entries(entries, count);
            ports_free_entries(entries);
            break;

        case 3:
            std::cout << COL_CYAN << "\n── All Ports ──\n" << COL_RESET;
            ports_remote_list_all(g_ports_sock, &entries, &count);
            print_port_entries(entries, count);
            ports_free_entries(entries);
            break;

        case 4: {
            std::cout << COL_BOLD << "Enter port number: " << COL_RESET;
            std::string p; std::getline(std::cin, p);
            int pn = -1;
            try { pn = std::stoi(p); } catch (...) {}
            if (pn < 1 || pn > 65535) {
                std::cout << COL_RED << "Invalid port\n" << COL_RESET;
                break;
            }
            int r = ports_remote_get_port(g_ports_sock, (uint16_t)pn, &entries, &count);
            if (r < 0) {
                std::cout << COL_RED << "Query failed\n" << COL_RESET;
            } else {
                std::cout << COL_CYAN << "\n── Port " << pn << " ──\n" << COL_RESET;
                print_port_entries(entries, count);
                ports_free_entries(entries);
            }
            break;
        }

        case 5: {
            std::cout << COL_BOLD << "Enter port number to kill: " << COL_RESET;
            std::string p; std::getline(std::cin, p);
            int pn = -1;
            try { pn = std::stoi(p); } catch (...) {}
            if (pn < 1 || pn > 65535) {
                std::cout << COL_RED << "Invalid port\n" << COL_RESET;
                break;
            }
            std::cout << COL_YELLOW
                      << "⚠  Kill process on port " << pn
                      << " on the RECEIVER? [y/N]: " << COL_RESET;
            std::string confirm; std::getline(std::cin, confirm);
            if (confirm == "y" || confirm == "Y") {
                int r = ports_remote_kill_port(g_ports_sock, (uint16_t)pn);
                if (r == 0)
                    std::cout << COL_GREEN << "Kill signal sent\n" << COL_RESET;
                else
                    std::cout << COL_RED << "Kill failed or no process found\n" << COL_RESET;
            } else {
                std::cout << "Cancelled\n";
            }
            break;
        }

        default:
            std::cout << COL_RED << "Invalid choice\n" << COL_RESET;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════════ */
/* Graceful Ctrl+C: stop the streaming loop so cleanup (incl. virtual
 * display teardown) runs instead of being killed mid-flight. */
#ifdef _WIN32
static BOOL WINAPI consoleCtrlHandler(DWORD type)
{
    (void)type;
    g_running = false;
    return TRUE;
}
#else
static void consoleCtrlHandler(int)
{
    g_running = false;
}
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    /* Enable ANSI colours + UTF-8 output on legacy conhost consoles */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD conMode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &conMode))
        SetConsoleMode(hOut, conMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(65001);
#endif
    getScreenDimensions(SCREEN_WIDTH, SCREEN_HEIGHT);

    std::cout << COL_CYAN << COL_BOLD
              << "========================================\n"
                 "  TwinView SENDER v0.8.0\n"
                 "========================================\n" << COL_RESET
              << "  Source: " << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
              << "  @ " << TARGET_FPS << " FPS\n"
              << COL_CYAN << "========================================\n"
              << COL_RESET;

    if (!initSockets()) {
        std::cerr << COL_RED << "Socket init failed\n" << COL_RESET;
        return 1;
    }
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    signal(SIGINT, consoleCtrlHandler);
    signal(SIGTERM, consoleCtrlHandler);
#endif

    /* Discover receivers (or use direct IP from command line).
     * CLI: sender [ip] [port] [--mode mirror|extend]
     *   --mode is non-interactive (GUI/automation): skips the prompts
     *   and, for extend, uses the default 1920x1080 virtual display. */
    std::string cli_mode;                       /* empty = ask interactively */
    std::string cli_ip;
    int cli_port = 8081;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            cli_mode = argv[++i];
        } else if (cli_ip.empty()) {
            cli_ip = argv[i];
        } else {
            cli_port = std::atoi(argv[i]);
        }
    }

    std::vector<DiscoveredDevice> receivers;
    if (!cli_ip.empty()) {
        /* Direct connection: sender <ip> [port] */
        std::cout << COL_CYAN << "Direct connection mode: "
                  << cli_ip << ":" << cli_port << "\n" << COL_RESET;
        if (testTcpConnection(cli_ip, cli_port, 2000)) {
            receivers.emplace_back(cli_ip, cli_port);
        } else {
            std::cerr << COL_RED << "Cannot connect to "
                      << cli_ip << ":" << cli_port << "\n" << COL_RESET;
            cleanupSockets(); return 1;
        }
    } else {
        std::cout << COL_CYAN << "Discovering receivers...\n" << COL_RESET;
        receivers = discoverReceivers(5);
    }
    if (receivers.empty()) {
        std::cerr << COL_RED
                  << "No receivers found!\n"
                     "  Check firewall: UDP 1900, TCP 8081, TCP 8082\n"
                  << COL_RESET;
        cleanupSockets(); return 1;
    }
    std::cout << listDevices(receivers);

    /* Select receiver */
    size_t choice = 0;
    if (receivers.size() > 1) {
        std::cout << COL_BOLD << "Select receiver (0-"
                  << receivers.size() - 1 << "): " << COL_RESET;
        std::string sel_line;
        std::getline(std::cin, sel_line);
        try {
            choice = (size_t)std::stoul(sel_line);
        } catch (...) {
            choice = receivers.size();  /* force invalid */
        }
        if (choice >= receivers.size()) {
            std::cerr << COL_RED << "Invalid selection\n" << COL_RESET;
            cleanupSockets(); return 1;
        }
    }

    const auto &sel = receivers[choice];
    std::cout << COL_GREEN << "Selected: " << sel.toString()
              << COL_RESET << "\n";

    /* Choose display mode (CLI --mode skips the prompt) */
    if (cli_mode == "extend")      DISPLAY_MODE = MODE_TRUE_EXTEND;
    else if (cli_mode == "mirror") DISPLAY_MODE = MODE_MIRROR;
    else                           DISPLAY_MODE = selectDisplayMode();
    const char *modeLabel =
        DISPLAY_MODE == MODE_TRUE_EXTEND ? "Extend" : "Mirror";
    std::cout << COL_GREEN << "Mode: " << modeLabel << COL_RESET << "\n";

#ifdef _WIN32
    /* Extend mode: bring up the virtual display (attach + position +
     * aim the capture at it). Must happen before the handshake so the
     * correct dimensions are announced to the receiver. */
    if (DISPLAY_MODE == MODE_TRUE_EXTEND) {
        int vdd_w = 1920, vdd_h = 1080;      /* default resolution */
        if (cli_mode.empty()) selectExtendResolution(vdd_w, vdd_h);
        if (!vddStartup(vdd_w, vdd_h)) {
            cleanupSockets(); return 1;
        }
    }
#endif

    /* Attempt GPU offload */
    std::cout << COL_MAGENTA << "GPU offload: "
              << sel.ip_address << ":" << GPU_ACCEL_PORT << " ...\n"
              << COL_RESET;
    g_gpu_sock = gpu_remote_connect(sel.ip_address.c_str());
    if (GPU_SOCK_VALID(g_gpu_sock)) {
        g_gpu_active = true;
        std::cout << COL_GREEN << "Remote compute (CPU offload) active\n" << COL_RESET;
    } else {
        std::cout << COL_YELLOW << "Compute service unavailable – local CPU only\n"
                  << COL_RESET;
    }

    /* Connect to port inspection service */
    std::cout << COL_MAGENTA << "Port inspector: "
              << sel.ip_address << ":" << PORTS_SERVICE_PORT << " ...\n"
              << COL_RESET;
    g_ports_sock = ports_remote_connect(sel.ip_address.c_str());
    if (PORTS_SOCK_VALID(g_ports_sock)) {
        std::cout << COL_GREEN << "Port inspector active  (press 'p' during stream)\n"
                  << COL_RESET;
    } else {
        std::cout << COL_YELLOW << "Port inspector unavailable\n" << COL_RESET;
    }

    /* Connect stream socket */
    NetworkSocket conn;
    if (!conn.connect(sel.ip_address, sel.tcp_port)) {
        if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
        if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }

    /* ── Extended handshake ── */
    struct {
        uint32_t sender_width;
        uint32_t sender_height;
        uint32_t fps;
        uint32_t mode;        /* MODE_MIRROR / MODE_TRUE_EXTEND */
    } hs_out = {
        htonl((uint32_t)SCREEN_WIDTH),
        htonl((uint32_t)SCREEN_HEIGHT),
        htonl((uint32_t)TARGET_FPS),
        htonl(DISPLAY_MODE)
    };
    if (!conn.sendAll(&hs_out, sizeof(hs_out))) {
        if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
        if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }

    /* Receive receiver's display size */
    struct {
        uint32_t recv_width;
        uint32_t recv_height;
        uint32_t status;
    } hs_in = {};
    if (!conn.recvAll(&hs_in, sizeof(hs_in)) || ntohl(hs_in.status) != 0) {
        std::cerr << COL_RED << "Handshake response failed\n" << COL_RESET;
        if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
        if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    RECV_WIDTH  = (int)ntohl(hs_in.recv_width);
    RECV_HEIGHT = (int)ntohl(hs_in.recv_height);

    std::cout << COL_GREEN
              << "Extended desktop active:\n"
              << "  Sender:   " << SCREEN_WIDTH  << "x" << SCREEN_HEIGHT << "\n"
              << "  Receiver: " << RECV_WIDTH    << "x" << RECV_HEIGHT   << "\n"
              << "  Layout:   " << modeLabel << "\n"
              << "  Total:    ";
    if (DISPLAY_MODE == MODE_TRUE_EXTEND)
        std::cout << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
                  << " (real desktop space – move windows & mouse across)\n";
    else
        std::cout << SCREEN_WIDTH << "x" << SCREEN_HEIGHT << " (mirror)\n";
    std::cout << COL_RESET;

    std::cout << COL_GREEN << "Streaming – Ctrl+C to stop\n" << COL_RESET;

    /* ── Streaming loop ── */
    const auto frame_dur   = std::chrono::microseconds(1000000 / TARGET_FPS);
    auto       last_stats  = std::chrono::steady_clock::now();
    auto       sess_start  = last_stats;
    int        frames_sent = 0;
    size_t     total_bytes = 0;
    int        fbehind     = 0;
    std::vector<uint8_t> comp_buf;

    while (g_running) {
        auto t0 = std::chrono::steady_clock::now();

#ifdef _WIN32
        /* Live resolution change: if the user resized the virtual display
         * in Windows display settings mid-stream, follow it. Tell the
         * receiver via a 0xFFFFFFFF marker + fresh 16-byte header. */
        if (DISPLAY_MODE == MODE_TRUE_EXTEND && (frames_sent & 31) == 0) {
            MonRectSearch ms = {};
            ms.dev = g_cap_device.c_str();
            EnumDisplayMonitors(NULL, NULL, findMonRectProc, (LPARAM)&ms);
            if (!ms.found) {
                std::cerr << COL_RED
                          << "\nVirtual display is gone – ending session\n"
                          << COL_RESET;
                break;
            }
            int nw = ms.rc.right - ms.rc.left;
            int nh = ms.rc.bottom - ms.rc.top;
            if (nw != SCREEN_WIDTH || nh != SCREEN_HEIGHT ||
                ms.rc.left != g_cap_origin.x || ms.rc.top != g_cap_origin.y)
            {
                SCREEN_WIDTH   = nw;
                SCREEN_HEIGHT  = nh;
                g_cap_origin.x = ms.rc.left;
                g_cap_origin.y = ms.rc.top;

                struct { uint32_t w, h, fps, mode; } rs = {
                    htonl((uint32_t)SCREEN_WIDTH),
                    htonl((uint32_t)SCREEN_HEIGHT),
                    htonl((uint32_t)TARGET_FPS),
                    htonl(DISPLAY_MODE)
                };
                uint32_t marker = 0xFFFFFFFFu;
                if (!conn.sendAll(&marker, 4) ||
                    !conn.sendAll(&rs, sizeof(rs))) {
                    std::cerr << COL_RED << "Resize notify failed\n"
                              << COL_RESET;
                    break;
                }
                std::cout << COL_GREEN << "Resolution changed → "
                          << SCREEN_WIDTH << "x" << SCREEN_HEIGHT << "\n"
                          << COL_RESET;
                continue;   /* skip this partial frame */
            }
        }
#endif

        auto frame = captureScreen();

        /* GPU offload: RLE compress on receiver */
        const uint8_t *send_ptr  = frame.data();
        uint32_t       send_size = (uint32_t)frame.size();

        if (g_gpu_active) {
            uint8_t *out = nullptr;
            int csz = gpu_remote_compress(g_gpu_sock, frame.data(),
                                          (uint32_t)SCREEN_WIDTH,
                                          (uint32_t)SCREEN_HEIGHT, &out);
            if (csz > 0 && out) {
                comp_buf.assign(out, out + csz);
                free(out);
                send_ptr  = comp_buf.data();
                send_size = (uint32_t)csz;
            } else {
                gpu_remote_disconnect(g_gpu_sock);
                g_gpu_sock   = GPU_INVALID_SOCK;
                g_gpu_active = false;
                std::cerr << COL_YELLOW
                          << "GPU lost – CPU fallback\n" << COL_RESET;
            }
        }

        uint32_t net_sz = htonl(send_size);
        if (!conn.sendAll(&net_sz, 4) ||
            !conn.sendAll(send_ptr, send_size)) {
            std::cerr << COL_RED << "Frame send failed\n" << COL_RESET;
            break;
        }

        frames_sent++;
        total_bytes += 4 + send_size;

        auto now  = std::chrono::steady_clock::now();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_stats).count();
        if (secs >= STATS_INTERVAL_SEC) {
            showStats(frames_sent, secs, total_bytes);
            last_stats = now;
        }

        /* Adaptive timing */
        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed > frame_dur) {
            if (++fbehind > MAX_FRAME_SKIP) { fbehind = 0; continue; }
        } else {
            fbehind = 0;
            std::this_thread::sleep_for(frame_dur - elapsed);
        }
    }

    /* Final stats */
    auto total_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - sess_start).count();
    float total_mb = (float)total_bytes / (1024.0f * 1024.0f);

    std::cout << COL_CYAN << COL_BOLD
              << "\n========================================\n"
                 "  SESSION STATISTICS\n"
                 "========================================\n" << COL_RESET
              << "  Mode       : " << modeLabel << "\n"
              << "  Source res : " << SCREEN_WIDTH  << "x" << SCREEN_HEIGHT << "\n"
              << "  Recv res   : " << RECV_WIDTH    << "x" << RECV_HEIGHT   << "\n"
              << "  Frames     : " << frames_sent << "\n"
              << "  Duration   : " << total_s << " s\n";
    if (total_s > 0)
        std::cout << "  Avg FPS    : " << frames_sent / total_s << "\n"
                  << "  Data       : " << std::fixed << std::setprecision(2)
                                       << total_mb << " MB\n"
                  << "  Avg BW     : " << total_mb / (float)total_s
                                       << " MB/s\n";
    std::cout << COL_CYAN << "========================================\n"
              << COL_RESET;

    if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
    if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
    vddShutdown();   /* detach the virtual display if we attached it */
#endif
    cleanupSockets();
    return 0;
}
