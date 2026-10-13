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
 * Handshake: protocol v2, see proto.h.
 *   sender → receiver : HsRequestV2  (48 bytes, magic 'TVHS', version 2)
 *   receiver → sender : HsResponseV2 (36 bytes, status = HsStatus)
 * Stream frames: u32 pkt_size + payload (RAW / RLE / one coded AU).
 * Control: u32 CTRL_MARKER + u32 CtrlType + body (CTRL_RESIZE = HsRequestV2).
 *
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
#include <mutex>
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
#include "vdd.h"
#include "proto.h"
#include "codec.h"

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

/* Codec / stream configuration (CLI, see plan §7). Defaults match config.ini. */
static CodecId    g_codec       = CODEC_H264;
static bool       g_want_hw     = true;
static int        g_bitrate_kbps = 0;    /* 0 = quality/CRF mode */
static int        g_quality     = 23;
static int        g_scale_pct   = 100;   /* coded size = capture * pct / 100 */
static int        g_chroma_444  = 0;
static std::string g_enc_impl   = "none"; /* Encoder::implName() once open */
static bool       g_enc_hw      = false;

/* Runtime reconfigure: GUI writes SETP|k=v|… lines to our stdin (default
 * hidden spawn mode). Last-wins pending line, applied at the loop top. */
static std::mutex        g_setp_mtx;
static std::string       g_pending_setp;
static std::string       g_recv_ip;    /* receiver address (RLE 8082 lifecycle) */

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

        /* I/O timeouts: a half-dead peer must not block send/recv forever –
         * the session has to end (and the virtual display detach) even when
         * the connection dies silently. Windows takes DWORD milliseconds. */
#ifdef _WIN32
        DWORD tmo = 2000;
        setsockopt(_s, SOL_SOCKET, SO_SNDTIMEO, (char *)&tmo, sizeof(tmo));
        tmo = 5000;
        setsockopt(_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
#else
        struct timeval tmo = { 2, 0 };
        setsockopt(_s, SOL_SOCKET, SO_SNDTIMEO, (char *)&tmo, sizeof(tmo));
        tmo.tv_sec = 5;
        setsockopt(_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
#endif
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
 * Lifecycle lives in vdd.cpp / vdd.h so the GUI can force-detach the
 * virtual monitor if the sender ever has to be hard-killed:
 *   - vddStartup()     : activate + place + size, adopt ownership
 *   - vddShutdown()    : detach on EVERY exit path (normal end, send
 *                        failure, Ctrl+C, console close, atexit)
 *   - vddForceDetach() : unconditional detach (GUI safety net)
 *
 * The virtual display is only attached to the desktop while an extend
 * streaming session is running.
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef _WIN32

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
 * MACHINE-READABLE STATS  –  one line per STATS_INTERVAL_SEC (GUI contract)
 *   STAT|t=..|frames=..|fps=..|kbps=..|codec=..|hw=..|mode=..|src=WxH|dst=WxH|quality=..
 * plus STAT|event=start|..  /  STAT|event=stop|..  /  STAT|event=error|msg=..
 * Values never contain '|'.
 * ══════════════════════════════════════════════════════════════════════════ */
static const char *statCodecName()
{
    if (g_codec == CODEC_RAW) return "raw";
    if (g_codec == CODEC_RLE) return "rle";
    return g_enc_impl.c_str();
}

static const char *statModeName()
{
    return DISPLAY_MODE == MODE_TRUE_EXTEND ? "extend" : "mirror";
}

static void emitStatLine(long tsec, long frames, double fps, long kbps,
                         const char *event = nullptr)
{
    std::cout << "STAT|";
    if (event) std::cout << "event=" << event << "|";
    std::cout << "t=" << tsec
              << "|frames=" << frames
              << "|fps=" << std::fixed << std::setprecision(1) << fps
              << std::defaultfloat
              << "|kbps=" << kbps
              << "|codec=" << statCodecName()
              << "|hw=" << (g_enc_hw ? 1 : 0)
              << "|mode=" << statModeName()
              << "|src=" << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
              << "|dst=" << RECV_WIDTH << "x" << RECV_HEIGHT
              << "|quality=" << g_quality
              << "\n" << std::flush;
}

static void emitStatStart()
{
    std::cout << "STAT|event=start"
              << "|codec=" << statCodecName()
              << "|hw=" << (g_enc_hw ? 1 : 0)
              << "|mode=" << statModeName()
              << "|src=" << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
              << "|dst=" << RECV_WIDTH << "x" << RECV_HEIGHT
              << "|quality=" << g_quality
              << "\n" << std::flush;
}

static void emitStatStop(long tsec, long frames)
{
    std::cout << "STAT|event=stop"
              << "|t=" << tsec
              << "|frames=" << frames
              << "|codec=" << statCodecName()
              << "\n" << std::flush;
}

static void emitStatError(const char *token)
{
    std::cout << "STAT|event=error|msg=" << token << "\n" << std::flush;
}

/* ── runtime reconfigure via stdin (SETP|k=v|… line protocol) ────────────── */
struct SetpPatch {
    bool has_codec = false;  CodecId codec = CODEC_H264;
    bool has_hw = false;     bool hw = true;
    bool has_bitrate = false; int bitrate = 0;
    bool has_quality = false; int quality = 23;
    bool has_fps = false;    int fps = 60;
    bool has_scale = false;  int scale = 100;
    bool has_chroma = false; int chroma444 = 0;
    std::string err;         /* non-empty = reject the whole patch */
};

/* Strict integer parse: the WHOLE token must be decimal digits, so "1000abc",
 * "" and "12x" are rejected instead of being silently truncated by atoi. */
static bool parseIntStrict(const std::string &v, int &out)
{
    if (v.empty()) return false;
    size_t i = 0;
    if (v[0] == '+' || v[0] == '-') i = 1;
    if (i >= v.size()) return false;
    long long n = 0;
    for (size_t j = i; j < v.size(); j++) {
        if (v[j] < '0' || v[j] > '9') return false;
        n = n * 10 + (v[j] - '0');
        if (n > 2147483647LL) return false;
    }
    out = (v[0] == '-') ? (int)-n : (int)n;
    return true;
}

static bool parseSetpLine(const std::string &line, SetpPatch &p)
{
    /* SETP|k=v|k=v|… — values carry no '|' (same grammar as STAT|). */
    size_t pos = 5;
    while (pos < line.size()) {
        size_t bar = line.find('|', pos);
        std::string tok = (bar == std::string::npos) ? line.substr(pos)
                                                     : line.substr(pos, bar - pos);
        pos = (bar == std::string::npos) ? line.size() : bar + 1;
        size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        if (k == "codec") {
            CodecId c = codecFromString(v);
            /* "auto" maps to h264; reject anything unknown */
            if (v != "auto" && v != "h264" && v != "h265" && v != "av1" &&
                v != "raw" && v != "rle") {
                p.err = "bad_value_codec"; return false;
            }
            p.has_codec = true; p.codec = c;
        } else if (k == "hw") {
            if (v != "0" && v != "1") { p.err = "bad_value_hw"; return false; }
            p.has_hw = true; p.hw = (v == "1");
        } else if (k == "bitrate") {
            int n = 0;
            if (!parseIntStrict(v, n) || n < 0 || n > 100000) {
                p.err = "bad_value_bitrate"; return false;
            }
            p.has_bitrate = true; p.bitrate = n;
        } else if (k == "quality") {
            int n = 0;
            if (!parseIntStrict(v, n) || n < 0 || n > 51) {
                p.err = "bad_value_quality"; return false;
            }
            p.has_quality = true; p.quality = n;
        } else if (k == "fps") {
            int n = 0;
            if (!parseIntStrict(v, n) || n < 1 || n > 240) {
                p.err = "bad_value_fps"; return false;
            }
            p.has_fps = true; p.fps = n;
        } else if (k == "scale") {
            int n = 0;
            if (!parseIntStrict(v, n) || n < 25 || n > 100) {
                p.err = "bad_value_scale"; return false;
            }
            p.has_scale = true; p.scale = n;
        } else if (k == "chroma") {
            if (v != "420" && v != "444") { p.err = "bad_value_chroma"; return false; }
            p.has_chroma = true; p.chroma444 = (v == "444") ? 1 : 0;
        }
        /* unknown keys ignored (forward compatible) */
    }
    return true;
}

/* Dedicated thread: stdin is a pipe when spawned by the GUI (SETP lines) and
 * the console when run by hand (lines typed during the stream). Detached —
 * it blocks on getline until process exit. */
static void stdinReaderLoop()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("SETP|", 0) == 0) {
            std::lock_guard<std::mutex> lk(g_setp_mtx);
            g_pending_setp = line;              /* last-wins */
        } else if (line == "PING") {
            std::cout << "STAT|event=pong\n" << std::flush;
        }
    }
}

static void startStdinReader()
{
    std::thread(stdinReaderLoop).detach();
}

/* Take the pending SETP line, if any. */
static bool takePendingSetp(std::string &out)
{
    std::lock_guard<std::mutex> lk(g_setp_mtx);
    if (g_pending_setp.empty()) return false;
    out.swap(g_pending_setp);
    return true;
}

/* 8082 remote-RLE offload: connect only while the codec is RLE. */
static void gpuOffloadConnect()
{
    if (GPU_SOCK_VALID(g_gpu_sock) || g_recv_ip.empty()) return;
    std::cout << COL_MAGENTA << "GPU offload: " << g_recv_ip << ":"
              << GPU_ACCEL_PORT << " ...\n" << COL_RESET;
    g_gpu_sock = gpu_remote_connect(g_recv_ip.c_str());
    if (GPU_SOCK_VALID(g_gpu_sock)) {
        g_gpu_active = true;
        std::cout << COL_GREEN << "Remote compute (CPU offload) active\n" << COL_RESET;
    } else {
        std::cout << COL_YELLOW << "Compute service unavailable – local CPU only\n"
                  << COL_RESET;
    }
}

static void gpuOffloadDisconnect()
{
    if (GPU_SOCK_VALID(g_gpu_sock)) {
        gpu_remote_disconnect(g_gpu_sock);
        g_gpu_sock = GPU_INVALID_SOCK;
    }
    g_gpu_active = false;
}

/* ── protocol v2 helpers ─────────────────────────────────────────────────── */
static bool isCodedCodec() { return g_codec >= CODEC_H264; }

static HsRequestV2 makeHsRequest(int w, int h)
{
    HsRequestV2 r = {};
    r.magic        = htonl(TVHS_MAGIC);
    r.version      = htonl(TVHS_VERSION);
    r.width        = htonl((uint32_t)w);
    r.height       = htonl((uint32_t)h);
    r.fps          = htonl((uint32_t)TARGET_FPS);
    r.mode         = htonl(DISPLAY_MODE);
    r.codec        = htonl((uint32_t)g_codec);
    r.codec_flags  = htonl(g_want_hw ? CODEC_FLAG_WANT_HW : 0u);
    r.bitrate_kbps = htonl((uint32_t)(g_bitrate_kbps > 0 ? g_bitrate_kbps : 0));
    r.quality      = htonl((uint32_t)g_quality);
    r.gop_frames   = 0;                 /* 0 = default 2*fps */
    r.reserved     = 0;
    return r;
}

/* Coded (stream) size for a given capture size, honoring --scale.
 * Coded dims are even; src stays the capture size so the encoder's
 * swscale pass performs the downscale. */
static void codedDims(int src_w, int src_h, int &cw, int &ch)
{
    cw = src_w;
    ch = src_h;
    if (g_scale_pct < 100) {
        cw = (src_w * g_scale_pct) / 100;
        ch = (src_h * g_scale_pct) / 100;
    }
    cw &= ~1;
    ch &= ~1;
    if (cw < 2) cw = 2;
    if (ch < 2) ch = 2;
}

static const char *hsStatusMsg(uint32_t st)
{
    switch (st) {
    case HS_OK:          return "OK";
    case HS_ERR_VERSION: return "protocol version mismatch — please update both Mivara ends";
    case HS_ERR_CODEC:   return "receiver rejected the codec";
    case HS_ERR_DIMS:    return "receiver rejected the stream dimensions";
    case HS_ERR_DECODER: return "receiver has no decoder for this codec";
    case HS_ERR_LEGACY:  return "peer is a legacy Mivara build — please update both Mivara ends";
    default:             return "unknown handshake error";
    }
}

static const char *hsStatusToken(uint32_t st)
{
    switch (st) {
    case HS_ERR_VERSION: return "version_mismatch";
    case HS_ERR_CODEC:   return "codec_rejected";
    case HS_ERR_DIMS:    return "dims_rejected";
    case HS_ERR_DECODER: return "decoder_unavailable";
    case HS_ERR_LEGACY:  return "legacy_peer";
    default:             return "handshake_rejected";
    }
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
    /* Console window close / logoff / shutdown: the process is terminated
     * right after this handler returns – the streaming loop will never
     * see g_running go false. Detach the virtual display HERE or it stays
     * attached to the desktop forever. */
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        vddShutdown();
        g_running = false;
        return TRUE;
    }
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
                 "  Mivara SENDER v0.9.1\n"
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
    /* Belt and braces: if anything calls exit() on an error path that
     * skipped the explicit vddShutdown(), the virtual display is still
     * detached. vddShutdown() is idempotent. */
    atexit(vddShutdown);
#else
    signal(SIGINT, consoleCtrlHandler);
    signal(SIGTERM, consoleCtrlHandler);
#endif

    /* Discover receivers (or use direct IP from command line).
     * CLI: sender [ip] [port] [--mode mirror|extend]
     *          [--codec auto|h264|h265|av1|raw|rle] [--hw 0|1]
     *          [--bitrate <kbps>] [--quality <0-51>] [--fps <n>]
     *          [--scale <pct>] [--chroma 420|444]
     *   --mode is non-interactive (GUI/automation): skips the prompts
     *   and, for extend, uses the default 1920x1080 virtual display. */
    std::string cli_mode;                       /* empty = ask interactively */
    std::string cli_ip;
    std::string cli_codec = "h264";
    int cli_port = 8081;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            cli_mode = argv[++i];
        } else if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            cli_codec = argv[++i];
        } else if (std::strcmp(argv[i], "--hw") == 0 && i + 1 < argc) {
            g_want_hw = (std::atoi(argv[++i]) != 0);
        } else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            g_bitrate_kbps = std::atoi(argv[++i]);
            if (g_bitrate_kbps < 0) g_bitrate_kbps = 0;
        } else if (std::strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            g_quality = std::atoi(argv[++i]);
            if (g_quality < 0)  g_quality = 0;
            if (g_quality > 51) g_quality = 51;
        } else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            TARGET_FPS = std::atoi(argv[++i]);
            if (TARGET_FPS < 1)   TARGET_FPS = 1;
            if (TARGET_FPS > 240) TARGET_FPS = 240;
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            g_scale_pct = std::atoi(argv[++i]);
            if (g_scale_pct < 25)   g_scale_pct = 25;
            if (g_scale_pct > 100)  g_scale_pct = 100;
        } else if (std::strcmp(argv[i], "--chroma") == 0 && i + 1 < argc) {
            g_chroma_444 = (std::atoi(argv[++i]) == 444) ? 1 : 0;
        } else if (cli_ip.empty()) {
            cli_ip = argv[i];
        } else {
            cli_port = std::atoi(argv[i]);
        }
    }
    g_codec = codecFromString(cli_codec);   /* "auto" → CODEC_H264 */

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
        VddInfo vdd;
        if (!vddStartup(vdd_w, vdd_h, vdd)) {
            emitStatError("vdd_startup_failed");
            cleanupSockets(); return 1;
        }
        g_cap_device   = vdd.device;
        g_cap_origin.x = vdd.x;
        g_cap_origin.y = vdd.y;
        SCREEN_WIDTH   = vdd.w;
        SCREEN_HEIGHT  = vdd.h;
    }
#endif

    /* Remote compute offload (8082) is only used by the RLE path – connecting
     * it for RAW or coded streams would traverse the LAN twice for nothing. */
    g_recv_ip = sel.ip_address;
    if (g_codec == CODEC_RLE)
        gpuOffloadConnect();

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

    /* ── Handshake v2 (proto.h): HsRequestV2 → HsResponseV2 ── */
    HsRequestV2 hs_out = makeHsRequest(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!conn.sendAll(&hs_out, sizeof(hs_out))) {
        emitStatError("handshake_send_failed");
        if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
        if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }

    HsResponseV2 hs_in = {};
    if (!conn.recvAll(&hs_in, sizeof(hs_in))) {
        std::cerr << COL_RED
                  << "No handshake response – the peer is probably a legacy "
                     "Mivara build; please update both Mivara ends\n"
                  << COL_RESET;
        emitStatError("legacy_peer");
        if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
        if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    uint32_t hs_magic  = ntohl(hs_in.magic);
    uint32_t hs_status = ntohl(hs_in.status);
    if (hs_magic != TVHS_MAGIC) {
        std::cerr << COL_RED
                  << "Bad handshake response (no TVHS magic) – peer is "
                     "probably a legacy Mivara build; please update both "
                     "Mivara ends\n"
                  << COL_RESET;
        emitStatError("legacy_peer");
        if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
        if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    if (hs_status != HS_OK) {
        std::cerr << COL_RED << "Handshake rejected: "
                  << hsStatusMsg(hs_status) << "\n" << COL_RESET;
        emitStatError(hsStatusToken(hs_status));
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

    /* ── Encoder for coded streams (raw/RLE pass through untouched) ── */
    Encoder encoder;
    EncoderConfig enc_cfg;
    if (isCodedCodec()) {
        enc_cfg.codec       = g_codec;
        codedDims(SCREEN_WIDTH, SCREEN_HEIGHT, enc_cfg.width, enc_cfg.height);
        enc_cfg.src_width   = SCREEN_WIDTH;
        enc_cfg.src_height  = SCREEN_HEIGHT;
        enc_cfg.fps         = TARGET_FPS;
        enc_cfg.bitrate_kbps = g_bitrate_kbps;
        enc_cfg.quality     = g_quality;
        enc_cfg.want_hw     = g_want_hw;
        enc_cfg.gop_frames  = 0;
        enc_cfg.chroma_444  = g_chroma_444;

        std::string enc_err = encoder.open(enc_cfg);
        if (!enc_err.empty()) {
            std::cerr << COL_RED << "Encoder open failed: " << enc_err
                      << "\n" << COL_RESET;
            emitStatError("encoder_open_failed");
            if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
            if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
            vddShutdown();
#endif
            cleanupSockets(); return 1;
        }
        g_enc_impl = encoder.implName();
        g_enc_hw   = encoder.isHw();
        std::cout << COL_GREEN << "Using encoder: " << g_enc_impl
                  << (g_enc_hw ? " (hw)" : " (sw)")
                  << " quality=" << g_quality
                  << " gop=" << encoder.gopFrames()
                  << COL_RESET << "\n" << std::flush;
    } else {
        std::cout << COL_GREEN << "Using raw/RLE transport" << COL_RESET
                  << "\n" << std::flush;
    }

    std::cout << COL_GREEN << "Streaming – Ctrl+C to stop\n" << COL_RESET;
    emitStatStart();
    startStdinReader();   /* SETP|… runtime reconfigure lines from the GUI */

    /* ── Streaming loop ── */
    auto       frame_dur   = std::chrono::microseconds(1000000 / TARGET_FPS);
    auto       last_stats  = std::chrono::steady_clock::now();
    auto       sess_start  = last_stats;
    int        frames_sent = 0;
    size_t     total_bytes = 0;
    size_t     stat_bytes  = 0;   /* payload since the last stats interval */
    int        stat_frames = 0;
    int        fbehind     = 0;
    std::vector<uint8_t> comp_buf;
    std::vector<uint8_t> au;

    while (g_running) {
        auto t0 = std::chrono::steady_clock::now();

        /* ── Runtime reconfigure (SETP from the GUI) ── */
        {
            std::string setp;
            if (takePendingSetp(setp)) {
                SetpPatch p;
                if (!parseSetpLine(setp, p)) {
                    emitStatError(p.err.c_str());
                } else {
                    const bool codec_changed = p.has_codec && p.codec != g_codec;
                    /* Receiver-side impact: codec / scale (geometry) / chroma
                     * (profile) require a decoder reset; fps/bitrate/quality/
                     * hw are sender-local. */
                    const bool need_notify =
                        codec_changed || p.has_scale || p.has_chroma;
                    const bool need_rebuild =
                        codec_changed ||
                        (p.has_hw && p.hw != g_want_hw) ||
                        (p.has_bitrate && p.bitrate != g_bitrate_kbps) ||
                        (p.has_quality && p.quality != g_quality) ||
                        (p.has_scale && p.scale != g_scale_pct) ||
                        (p.has_chroma && p.chroma444 != g_chroma_444) ||
                        (isCodedCodec() && p.has_fps && p.fps != TARGET_FPS);

                    /* 8082 offload follows the codec */
                    if (codec_changed) {
                        if (p.codec == CODEC_RLE) gpuOffloadConnect();
                        else                      gpuOffloadDisconnect();
                    }

                    if (p.has_codec)  g_codec = p.codec;
                    if (p.has_hw)     g_want_hw = p.hw;
                    if (p.has_bitrate) g_bitrate_kbps = p.bitrate;
                    if (p.has_quality) g_quality = p.quality;
                    if (p.has_scale)  g_scale_pct = p.scale;
                    if (p.has_chroma) g_chroma_444 = p.chroma444;
                    if (p.has_fps) {
                        TARGET_FPS = p.fps;
                        frame_dur = std::chrono::microseconds(1000000 / TARGET_FPS);
                    }

                    /* Mirror the VDD-resize pattern: rebuild first so the
                     * next AU is an IDR of the new config, then notify. */
                    if (isCodedCodec() && need_rebuild) {
                        enc_cfg.codec = g_codec;
                        codedDims(SCREEN_WIDTH, SCREEN_HEIGHT,
                                  enc_cfg.width, enc_cfg.height);
                        enc_cfg.src_width   = SCREEN_WIDTH;
                        enc_cfg.src_height  = SCREEN_HEIGHT;
                        enc_cfg.fps         = TARGET_FPS;
                        enc_cfg.bitrate_kbps = g_bitrate_kbps;
                        enc_cfg.quality     = g_quality;
                        enc_cfg.want_hw     = g_want_hw;
                        enc_cfg.gop_frames  = 0;
                        enc_cfg.chroma_444  = g_chroma_444;
                        std::string enc_err = encoder.open(enc_cfg);
                        if (!enc_err.empty()) {
                            std::cerr << COL_RED
                                      << "Encoder rebuild failed: " << enc_err
                                      << "\n" << COL_RESET;
                            emitStatError("encoder_rebuild_failed");
                            break;
                        }
                        g_enc_impl = encoder.implName();
                        g_enc_hw   = encoder.isHw();
                        encoder.forceKeyframe();
                    } else if (codec_changed && !isCodedCodec()) {
                        encoder.close();   /* left coded mode behind */
                    }

                    /* Tell the receiver BEFORE the next bitstream bytes so it
                     * re-inits its decoder in time. */
                    if (need_notify) {
                        HsRequestV2 rs = makeHsRequest(SCREEN_WIDTH, SCREEN_HEIGHT);
                        uint32_t marker = htonl(CTRL_MARKER);
                        uint32_t ctype  = htonl(CTRL_RECONFIG);
                        if (!conn.sendAll(&marker, 4) ||
                            !conn.sendAll(&ctype, 4) ||
                            !conn.sendAll(&rs, sizeof(rs))) {
                            std::cerr << COL_RED << "Reconfig notify failed\n"
                                      << COL_RESET;
                            emitStatError("reconfig_notify_failed");
                            break;
                        }
                    }
                    std::cout << COL_GREEN << "Reconfigured: codec="
                              << codecToString(g_codec)
                              << " quality=" << g_quality
                              << " bitrate=" << g_bitrate_kbps
                              << " fps=" << TARGET_FPS
                              << " scale=" << g_scale_pct
                              << " chroma=" << (g_chroma_444 ? "444" : "420")
                              << "\n" << COL_RESET;
                    double elapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - sess_start).count();
                    long kbps_now = elapsed > 0
                                        ? (long)(total_bytes * 8.0 / elapsed / 1000.0)
                                        : 0;
                    emitStatLine((long)elapsed, frames_sent,
                                 elapsed > 0 ? frames_sent / elapsed : 0.0,
                                 kbps_now, "applied");
                    continue;   /* skip this frame */
                }
            }
        }

#ifdef _WIN32
        /* Live resolution change: if the user resized the virtual display
         * in Windows display settings mid-stream, follow it. Tell the
         * receiver via CTRL_MARKER + CTRL_RESIZE + a fresh HsRequestV2. */
        if (DISPLAY_MODE == MODE_TRUE_EXTEND && (frames_sent & 31) == 0) {
            int nx = 0, ny = 0, nw = 0, nh = 0;
            if (!vddFindMonitorRect(g_cap_device, nx, ny, nw, nh)) {
                std::cerr << COL_RED
                          << "\nVirtual display is gone – ending session\n"
                          << COL_RESET;
                emitStatError("vdd_lost");
                break;
            }
            if (nw != SCREEN_WIDTH || nh != SCREEN_HEIGHT ||
                nx != g_cap_origin.x || ny != g_cap_origin.y)
            {
                SCREEN_WIDTH   = nw;
                SCREEN_HEIGHT  = nh;
                g_cap_origin.x = nx;
                g_cap_origin.y = ny;

                /* Rebuild the encoder at the new size first so the AU that
                 * follows the control packet is an IDR of the new geometry. */
                if (isCodedCodec()) {
                    codedDims(SCREEN_WIDTH, SCREEN_HEIGHT,
                              enc_cfg.width, enc_cfg.height);
                    enc_cfg.src_width  = SCREEN_WIDTH;
                    enc_cfg.src_height = SCREEN_HEIGHT;
                    std::string enc_err = encoder.open(enc_cfg);
                    if (!enc_err.empty()) {
                        std::cerr << COL_RED
                                  << "Encoder rebuild failed: " << enc_err
                                  << "\n" << COL_RESET;
                        emitStatError("encoder_rebuild_failed");
                        break;
                    }
                    g_enc_impl = encoder.implName();
                    g_enc_hw   = encoder.isHw();
                    encoder.forceKeyframe();
                }

                HsRequestV2 rs = makeHsRequest(SCREEN_WIDTH, SCREEN_HEIGHT);
                uint32_t marker = htonl(CTRL_MARKER);
                uint32_t ctype  = htonl(CTRL_RESIZE);
                if (!conn.sendAll(&marker, 4) ||
                    !conn.sendAll(&ctype, 4) ||
                    !conn.sendAll(&rs, sizeof(rs))) {
                    std::cerr << COL_RED << "Resize notify failed\n"
                              << COL_RESET;
                    emitStatError("resize_notify_failed");
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

        if (isCodedCodec()) {
            /* Coded: one access unit per frame, framed like any DATA packet */
            std::string enc_err = encoder.encode(frame.data(), au);
            if (!enc_err.empty()) {
                std::cerr << COL_RED << "Encode failed: " << enc_err
                          << "\n" << COL_RESET;
                emitStatError("encode_failed");
                break;
            }
            if (!au.empty()) {           /* empty = no packet this frame */
                uint32_t net_sz = htonl((uint32_t)au.size());
                if (!conn.sendAll(&net_sz, 4) ||
                    !conn.sendAll(au.data(), au.size())) {
                    std::cerr << COL_RED << "Frame send failed\n" << COL_RESET;
                    emitStatError("send_failed");
                    break;
                }
                frames_sent++;
                total_bytes += 4 + au.size();
                stat_bytes  += 4 + au.size();
                stat_frames++;
            }
        } else {
            /* RAW / RLE transport (unchanged). GPU offload: RLE compress on
             * the receiver, only active when the negotiated codec is RLE. */
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
                emitStatError("send_failed");
                break;
            }

            frames_sent++;
            total_bytes += 4 + send_size;
            stat_bytes  += 4 + send_size;
            stat_frames++;
        }

        auto now  = std::chrono::steady_clock::now();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_stats).count();
        if (secs >= STATS_INTERVAL_SEC) {
            showStats(frames_sent, secs, total_bytes);
            double ifps = secs > 0 ? (double)stat_frames / (double)secs : 0.0;
            long ikbps = (long)((double)stat_bytes / 1024.0 /
                                (double)(secs > 0 ? secs : 1));
            long tsec = (long)std::chrono::duration_cast<std::chrono::seconds>(
                            now - sess_start).count();
            emitStatLine(tsec, frames_sent, ifps, ikbps);
            last_stats  = now;
            stat_bytes  = 0;
            stat_frames = 0;
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

    emitStatStop((long)total_s, frames_sent);

    if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
    if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
    vddShutdown();   /* detach the virtual display if we attached it */
#endif
    cleanupSockets();
    return 0;
}
