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
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>
#include <iomanip>
#include <fstream>
#include <cstdint>
#include <algorithm>
#include "auth.h"
#include "discover.h"
#include "drop.h"
#include "gpu_accelerate.h"
#include "kvm.h"
#include "ports.h"
#include "vdd.h"
#include "proto.h"
#include "codec.h"
#include "version.h"

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
/* Local only: 拼接 is a KVM input channel (kvm.h), never a HsRequestV2 mode.
 * It must never be written into DISPLAY_MODE / sent on 8081. */
#define MODE_STITCH        2u

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

static std::atomic<bool> g_running{true};

/* ── v3 authorization ──────────────────────────────────────────────────────
 * The password arrives on stdin (never on the command line) and is only kept
 * in memory for the duration of one handshake. The derived key is not stored:
 * the sender recomputes PBKDF2 from the receiver's salt and the typed
 * password, then answers the nonce with an HMAC proof. */
static std::mutex              g_auth_mtx;
static std::condition_variable g_auth_cv;
static std::string             g_auth_password;
static bool                    g_auth_password_ready = false;
static bool                    g_auth_cancelled = false;
static std::atomic<bool>       g_auth_awaiting{false};

/* Session token issued by the receiver; 8082/8083 require it. */
static uint8_t g_session_token[HS_TOKEN_BYTES] = {};
static bool    g_session_token_valid = false;

/* This machine's display name, sent in HsRequestExtV3. */
static std::string g_device_name;

static int64_t steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

/* Wait for AUTH|password=… from the GUI (or a terminal line). Bounded so a
 * forgotten prompt cannot pin the process forever. */
static bool takePassword(std::string &pw)
{
    std::unique_lock<std::mutex> lk(g_auth_mtx);
    g_auth_awaiting = true;
    /* A cancel sent for an earlier prompt (or after it already finished) must
     * not abort this one before the user can type. */
    g_auth_cancelled = false;
    const int64_t deadline = steadyMs() + 120000;
    bool ok = false;
    while (!g_auth_password_ready && !g_auth_cancelled) {
        if (!g_running || steadyMs() > deadline) break;
        g_auth_cv.wait_for(lk, std::chrono::milliseconds(250));
    }
    if (g_auth_password_ready) {
        pw.swap(g_auth_password);
        g_auth_password_ready = false;
        ok = true;
    }
    /* The static hand-off buffer must not keep a plaintext password, whether
     * it was just retired by the swap or arrived after the wait gave up. */
    if (!g_auth_password.empty()) {
        mivaraSecureZero(&g_auth_password[0], g_auth_password.size());
        g_auth_password.clear();
    }
    g_auth_awaiting = false;
    return ok;
}

#ifdef _WIN32
/* True-Extend mode: capture a specific monitor instead of the primary.
 * g_cap_device empty  -> legacy primary-screen capture (GetDC(nullptr))
 * g_cap_device set    -> capture that monitor via CreateDC (e.g. a virtual
 *                        display provided by an indirect display driver).  */
static std::string g_cap_device;
static POINT       g_cap_origin = {0, 0};   /* monitor origin in virtual desktop */
#endif

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
    bool _quiet = false;    /* one-shot probes: no error chatter */

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
                 int timeout_ms = CONNECTION_TIMEOUT_MS, bool quiet = false)
    {
        _quiet = quiet;
        if (port < 1 || port > 65535 || timeout_ms < 0) return false;
        if (!create()) return false;
#ifndef _WIN32
        if (_s >= FD_SETSIZE) { close(); return false; }
#endif
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            if (!quiet)
                std::cerr << COL_RED << "Invalid IP: " << ip << COL_RESET << "\n";
            close();
            return false;
        }
#ifdef _WIN32
        u_long nb = 1;
        if (ioctlsocket(_s, FIONBIO, &nb) != 0) { close(); return false; }
#else
        int fl = fcntl(_s, F_GETFL, 0);
        if (fl < 0 || fcntl(_s, F_SETFL, fl | O_NONBLOCK) < 0) {
            close();
            return false;
        }
#endif
        const int rc = ::connect(_s, (struct sockaddr *)&addr, sizeof(addr));
        bool ok = rc == 0;
        bool pending = false;
        if (rc != 0) {
#ifdef _WIN32
            const int err = WSAGetLastError();
            pending = err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
            pending = errno == EINPROGRESS;
#endif
        }
        if (pending) {
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(_s, &wr);
            FD_SET(_s, &ex);
            timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (select((int)_s + 1, nullptr, &wr, &ex, &tv) > 0 &&
                FD_ISSET(_s, &wr)) {
                int err = 0;
                socklen_t len = sizeof(err);
                ok = getsockopt(_s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 &&
                     err == 0;
            }
        }
#ifdef _WIN32
        { u_long bl = 0; ioctlsocket(_s, FIONBIO, &bl); }
#else
        fcntl(_s, F_SETFL, fl);
#endif
        if (!ok) {
            if (!quiet)
                std::cerr << COL_RED << "Timeout or connection error: " << ip << ":"
                          << port << COL_RESET << "\n";
            close();
            return false;
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
        if (!quiet)
            std::cout << COL_GREEN << "Connected to "
                      << ip << ":" << port << COL_RESET << "\n";
        return true;
    }

    /* Shorten the recv timeout for one-shot requests (display probe). */
    void setRecvTimeout(int ms) {
        if (!valid()) return;
#ifdef _WIN32
        DWORD t = (DWORD)ms;
        setsockopt(_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof(t));
#else
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        setsockopt(_s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    bool sendAll(const void *data, size_t size) {
        const char *p = (const char *)data;
        size_t sent = 0;
        while (sent < size && g_running) {
            int n = (int)send(_s, p + sent, (int)(size - sent), 0);
#ifdef _WIN32
            if (n == SOCKET_ERROR) {
                if (!_quiet)
                    std::cerr << COL_RED << "Send error: "
                              << WSAGetLastError() << COL_RESET << "\n";
                return false;
            }
#else
            if (n < 0) {
                if (!_quiet)
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

    /* Like recvAll, but tells an idle timeout apart from a dead transport.
     * Returns 1 on a full read, 0 when the peer sent nothing before the
     * timeout expired, -1 when the connection closed or failed (including a
     * timeout in the middle of the chunk). Used for the first handshake frame
     * so "no response" and "connection dropped" stay distinguishable. */
    int recvAllEx(void *data, size_t size) {
        char *p = (char *)data;
        size_t got = 0;
        while (got < size) {
            int n = (int)recv(_s, p + got, (int)(size - got), 0);
            if (n > 0) { got += (size_t)n; continue; }
            if (n == 0) return -1;
#ifdef _WIN32
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK)
                return got == 0 ? 0 : -1;
#else
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return got == 0 ? 0 : -1;
#endif
            return -1;
        }
        return 1;
    }
};

/* ══════════════════════════════════════════════════════════════════════════
 * SCREEN CAPTURE  –  platform-specific, always returns RGB24
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef _WIN32
static std::vector<uint8_t> captureScreen()
{
    const size_t width = (size_t)SCREEN_WIDTH;
    const size_t height = (size_t)SCREEN_HEIGHT;
    const size_t stride = (width * 3 + 3) & ~(size_t)3;
    std::vector<uint8_t> px(width * height * 3, 0);
    std::vector<uint8_t> dib(stride * height, 0);
    HDC sdc = GetDC(nullptr);
    if (!sdc) return {};
    HDC mdc = CreateCompatibleDC(sdc);
    if (!mdc) { ReleaseDC(nullptr, sdc); return {}; }
    HBITMAP bmp = CreateCompatibleBitmap(sdc, SCREEN_WIDTH, SCREEN_HEIGHT);
    HGDIOBJ old = nullptr;
    const auto cleanup = [&] {
        if (old && old != HGDI_ERROR) SelectObject(mdc, old);
        if (bmp) DeleteObject(bmp);
        DeleteDC(mdc);
        ReleaseDC(nullptr, sdc);
    };
    if (!bmp) { cleanup(); return {}; }
    old = SelectObject(mdc, bmp);
    if (!old || old == HGDI_ERROR) { old = nullptr; cleanup(); return {}; }
    if (!BitBlt(mdc, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, sdc,
                g_cap_origin.x, g_cap_origin.y, SRCCOPY | CAPTUREBLT)) {
        cleanup();
        return {};
    }

    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    const bool haveCur = GetCursorInfo(&ci) != FALSE;
    if (haveCur && (ci.flags & CURSOR_SHOWING)) {
        const int cx = ci.ptScreenPos.x - g_cap_origin.x;
        const int cy = ci.ptScreenPos.y - g_cap_origin.y;
        if (cx >= 0 && cy >= 0 && cx < SCREEN_WIDTH && cy < SCREEN_HEIGHT) {
            ICONINFO ii = {};
            if (GetIconInfo(ci.hCursor, &ii)) {
                DrawIconEx(mdc, cx - ii.xHotspot, cy - ii.yHotspot, ci.hCursor,
                           GetSystemMetrics(SM_CXCURSOR), GetSystemMetrics(SM_CYCURSOR),
                           0, NULL, DI_NORMAL);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
        }
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = SCREEN_WIDTH;
    bi.bmiHeader.biHeight = -SCREEN_HEIGHT;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    const UINT rows = GetDIBits(mdc, bmp, 0, (UINT)SCREEN_HEIGHT, dib.data(),
                                &bi, DIB_RGB_COLORS);
    cleanup();
    if (rows != (UINT)SCREEN_HEIGHT) return {};
    for (size_t y = 0; y < height; y++) {
        const uint8_t *src = dib.data() + y * stride;
        for (size_t x = 0; x < width; x++) {
            const size_t di = (y * width + x) * 3;
            px[di] = src[x * 3 + 2];
            px[di + 1] = src[x * 3 + 1];
            px[di + 2] = src[x * 3];
        }
    }
    return px;
}

#elif defined(__APPLE__)
static std::vector<uint8_t> captureScreen()
{
    CGImageRef img = CGDisplayCreateImage(kCGDirectMainDisplay);
    if (!img) return {};
    const size_t width = CGImageGetWidth(img);
    const size_t height = CGImageGetHeight(img);
    const size_t bpr = CGImageGetBytesPerRow(img);
    const size_t bits = CGImageGetBitsPerPixel(img);
    bool valid = width == (size_t)SCREEN_WIDTH && height == (size_t)SCREEN_HEIGHT &&
                 bits >= 24 && bits % 8 == 0 && height > 0 &&
                 bpr >= width * (bits / 8) && bpr <= SIZE_MAX / height;
    CGDataProviderRef dp = valid ? CGImageGetDataProvider(img) : nullptr;
    CFDataRef raw = dp ? CGDataProviderCopyData(dp) : nullptr;
    if (!raw || (size_t)CFDataGetLength(raw) < bpr * height) {
        if (raw) CFRelease(raw);
        CGImageRelease(img);
        return {};
    }
    const uint8_t *src = CFDataGetBytePtr(raw);
    if (!src) { CFRelease(raw); CGImageRelease(img); return {}; }
    std::vector<uint8_t> px(width * height * 3, 0);
    const size_t bpp = bits / 8;
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            const size_t si = y * bpr + x * bpp;
            const size_t di = (y * width + x) * 3;
            px[di] = src[si + 2];
            px[di + 1] = src[si + 1];
            px[di + 2] = src[si];
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

/* Strict whole-token "WxH" parser (e.g. 2560x1600). */
static bool parseWxH(const std::string &s, int &w, int &h)
{
    const size_t x = s.find_first_of("xX");
    if (x == std::string::npos || x == 0 || x + 1 >= s.size())
        return false;
    auto whole = [](const std::string &t, int &v) {
        if (t.empty()) return false;
        long long n = 0;
        for (char c : t) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + (c - '0');
            if (n > 100000) return false;
        }
        v = (int)n;
        return true;
    };
    int pw = 0, ph = 0;
    if (!whole(s.substr(0, x), pw) || !whole(s.substr(x + 1), ph))
        return false;
    /* The encoder requires even dimensions; reject odd sizes up front so the
     * VDD never gets a mode the capture path cannot stream. */
    if (pw < 320 || pw > 7680 || ph < 240 || ph > 4320 ||
        (pw & 1) || (ph & 1))
        return false;
    w = pw;
    h = ph;
    return true;
}

/* Ask the user which resolution the virtual display should use.
 * Modes mirror the driver's vdd_settings.xml. When the receiver's display
 * size is known, "match the receiver" is the default (0). */
static void selectExtendResolution(int &w, int &h,
                                   int recv_w = 0, int recv_h = 0)
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
    const bool haveRecv = recv_w > 0 && recv_h > 0;

    std::cout << "\n"
              << COL_CYAN << COL_BOLD
              << "  Virtual display resolution:\n" << COL_RESET;
    if (haveRecv)
        std::cout << "  " << COL_GREEN << "0" << COL_RESET
                  << "  Match receiver (" << recv_w << "x" << recv_h << ")\n";
    for (int i = 0; i < nModes; i++)
        std::cout << "  " << (!haveRecv && i == 0 ? COL_GREEN : COL_YELLOW)
                  << (i + 1) << COL_RESET << "  " << modes[i].label << "\n";
    std::cout << "  (resolution can also be changed later in Windows "
                 "display settings – the stream adapts automatically)\n";
    std::cout << COL_BOLD << "  Choice [" << (haveRecv ? "0" : "1")
              << "]: " << COL_RESET;

    std::string line;
    std::getline(std::cin, line);
    int pick = haveRecv ? 0 : 1;
    if (!line.empty()) {
        try { pick = std::stoi(line); }
        catch (...) { pick = -1; }
    }
    if (haveRecv && pick == 0) {
        w = recv_w;
        h = recv_h;
    } else if (pick >= 1 && pick <= nModes) {
        w = modes[pick - 1].w;
        h = modes[pick - 1].h;
    } else {
        std::cout << COL_YELLOW << "  Using default "
                  << (haveRecv ? "receiver size" : "1920x1080") << "\n"
                  << COL_RESET;
        if (haveRecv) { w = recv_w; h = recv_h; }
        else          { w = modes[0].w; h = modes[0].h; }
    }
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
#ifdef _WIN32
              << "  " << COL_CYAN << "3" << COL_RESET
                      << "  Stitch        (software KVM – one keyboard/mouse for\n"
                      << "                   both PCs; no video is streamed)\n"
#endif
              << COL_BOLD << "  Choice [1]: " << COL_RESET;

    std::string line;
    std::getline(std::cin, line);

#ifdef _WIN32
    if (line == "2") return MODE_TRUE_EXTEND;
    if (line == "3") return MODE_STITCH;
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
        if (line.rfind("AUTH|", 0) == 0) {
            /* AUTH|password=… from the GUI. The value is taken verbatim (it
             * may contain '=' or spaces); the app validates the rule. */
            std::string body = line.substr(5);
            if (body.rfind("password=", 0) == 0) {
                {
                    std::lock_guard<std::mutex> lk(g_auth_mtx);
                    std::string next = body.substr(9);
                    g_auth_password.swap(next);   /* retired value stays in next */
                    mivaraSecureZero(&next[0], next.size());
                    g_auth_password_ready = true;
                    g_auth_cancelled = false;
                    g_auth_cv.notify_all();
                }
                /* Only the hand-off buffer keeps the secret now. */
                mivaraSecureZero(&body[0], body.size());
                mivaraSecureZero(&line[0], line.size());
            } else if (body == "cancel") {
                std::lock_guard<std::mutex> lk(g_auth_mtx);
                g_auth_cancelled = true;
                g_auth_cv.notify_all();
            }
            continue;
        }
        if (line.rfind("SETP|", 0) == 0) {
            std::lock_guard<std::mutex> lk(g_setp_mtx);
            g_pending_setp = line;              /* last-wins */
        } else if (line.rfind("STITCH|", 0) == 0) {
            /* STITCH|side=left — live direction switch while a 拼接 session
             * runs. kvmSetControllerSide() is thread-safe and a no-op when no
             * controller is active, so no mutex is needed here. */
            if (line.compare(7, 5, "side=") == 0) {
                const int side = kvmSideFromKey(line.c_str() + 12);
                if (side >= 0) kvmSetControllerSide(side);
            }
        } else if (line == "PING") {
            std::cout << "STAT|event=pong\n" << std::flush;
        } else if (line == "STOP|mivara") {
            /* GUI 停止 / 退出. Console control events are not delivered to a
             * child console on current Windows (the app's report), so the
             * owner asks over this pipe instead: leave the loop and let the
             * normal cleanup (stats flush, VDD teardown) run. The token is
             * longer than the 8-character password rule, so it can never be
             * mistaken for a password typed at the interactive prompt. */
            std::cout << "Stop requested by GUI\n" << std::flush;
            g_running = false;
        } else if (g_auth_awaiting.load() && !line.empty()) {
            /* Interactive (no GUI): the line typed at the prompt is the
             * password. */
            {
                std::lock_guard<std::mutex> lk(g_auth_mtx);
                std::string next = line;
                g_auth_password.swap(next);
                mivaraSecureZero(&next[0], next.size());
                g_auth_password_ready = true;
                g_auth_cancelled = false;
                g_auth_cv.notify_all();
            }
            mivaraSecureZero(&line[0], line.size());
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
    if (!g_session_token_valid) return;   /* 8082 needs the session token */
    std::cout << COL_MAGENTA << "GPU offload: " << g_recv_ip << ":"
              << GPU_ACCEL_PORT << " ...\n" << COL_RESET;
    g_gpu_sock = gpu_remote_connect(g_recv_ip.c_str(), g_session_token);
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

#ifdef _WIN32
/* Ask the receiver for its current display size before the virtual display
 * is created. Sends a protocol-v2 handshake with width=height=0: the
 * receiver answers HsResponseV2 with recv_width/recv_height and HS_ERR_DIMS,
 * then closes before SDL_Init, so no window is created and no decoder is
 * opened. A quiet failure is fine – callers keep the default mode and the
 * receiver letterboxes any residual aspect mismatch. */
static bool probeReceiverDisplay(const std::string &ip, int port,
                                 int &w, int &h)
{
    NetworkSocket probe;
    if (!probe.connect(ip, port, 1500, /*quiet=*/true))
        return false;
    probe.setRecvTimeout(2000);

    HsRequestV2 req = makeHsRequest(0, 0);
    if (!probe.sendAll(&req, sizeof(req)))
        return false;

    HsResponseV2 res = {};
    if (!probe.recvAll(&res, sizeof(res)))
        return false;
    if (ntohl(res.magic) != TVHS_MAGIC ||
        ntohl(res.version) != TVHS_VERSION ||
        ntohl(res.status) != HS_ERR_DIMS)
        return false;

    int rw = (int)ntohl(res.recv_width);
    int rh = (int)ntohl(res.recv_height);
    rw &= ~1;                       /* even dimensions for the encoder */
    rh &= ~1;
    if (rw < 320 || rw > 7680 || rh < 240 || rh > 4320)
        return false;
    w = rw;
    h = rh;
    return true;
}
#endif /* _WIN32 */

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
    case HS_ERR_BUSY:    return "对方忙：接收端已有连接";
    case HS_ERR_AUTH:    return "连接密码错误";
    case HS_ERR_AUTH_TIMEOUT: return "对方未在 30 秒内处理授权请求";
    case HS_ERR_DENIED:  return "对方拒绝了本次连接";
    case HS_ERR_AUTH_CONFIG: return "接收端授权配置异常";
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
    case HS_ERR_BUSY:    return "peer_busy";
    case HS_ERR_AUTH:    return "bad_password";
    case HS_ERR_AUTH_TIMEOUT: return "auth_timeout";
    case HS_ERR_DENIED:  return "auth_denied";
    case HS_ERR_AUTH_CONFIG: return "auth_config";
    default:             return "handshake_rejected";
    }
}

/* This machine's name for the receiver's approval dialog. */
static std::string localDeviceName()
{
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0')
        return "Mivara";
    host[sizeof(host) - 1] = '\0';
    std::string s(host);
    if (s.size() > 31) s.resize(31);
    return s;
}

static void emitAuthStat(const char *state, const std::string &extra = std::string())
{
    std::cout << "STAT|event=auth|state=" << state;
    if (!extra.empty()) std::cout << "|" << extra;
    std::cout << "\n" << std::flush;
}

/* File-drop worker threads emit STAT lines; serialise them so the GUI's
 * line parser never sees two records spliced together. */
static void dropStatSink(const char *line)
{
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::cout << "STAT|" << line << "\n" << std::flush;
}

/* 拼接 (8084) authorization hooks: the same password prompt and progress
 * lines as the 8081 handshake, so the GUI shows one password modal and one
 * wait state no matter which channel is being authorized. */
static bool kvmPasswordHook(std::string *out, void *ctx)
{
    (void)ctx;
    return takePassword(*out);
}

static void kvmReportHook(const char *state, const char *extra, void *ctx)
{
    (void)ctx;
    emitAuthStat(state, extra ? std::string(extra) : std::string());
}

/* Result of the v3 authorization exchange (see proto.h). */
struct HsOutcome {
    uint32_t status     = HS_ERR_LEGACY;
    bool     no_response = false;  /* peer sent nothing / not a handshake */
    bool     transport   = false;  /* connection died mid-exchange        */
    bool     legacy      = false;  /* peer answered with another version  */
    bool     protocol_error = false; /* same-version peer broke the protocol */
    bool     cancelled   = false;  /* user dismissed the password prompt  */
    uint32_t peer_version = 0;     /* HsResponseV2.version as received    */
    int      recv_w = 0, recv_h = 0;
    bool     have_token = false;
    uint8_t  token[HS_TOKEN_BYTES] = {};
};

/* v3 handshake: request → challenge → identity (+ proof) → response → token.
 * A receiver that refuses before the challenge answers with the bare
 * HsResponseV2, so its `version` decides what happened: the current version
 * is a same-version refusal (BUSY, corrupt auth config, ...) whose status must
 * be reported verbatim; any other version is a peer running an incompatible
 * protocol (`legacy`, update hint). A timeout with no bytes is `no_response`,
 * a transport that dies mid-frame is `transport`. The socket's recv timeout
 * must be set by the caller. */
static HsOutcome handshakeV3(NetworkSocket &conn, const HsRequestV2 &hs_out)
{
    HsOutcome out;

    if (!conn.sendAll(&hs_out, sizeof(hs_out))) {
        out.transport = true;
        return out;
    }

    uint32_t magic = 0;
    const int first = conn.recvAllEx(&magic, sizeof(magic));
    if (first != 1) {
        if (first == 0) out.no_response = true;  /* idle timeout: mute peer */
        else            out.transport = true;    /* closed / truncated frame */
        return out;
    }
    magic = ntohl(magic);

    if (magic == TVHS_MAGIC) {          /* refusal or reply before a challenge */
        HsResponseV2 res = {};
        res.magic = htonl(magic);
        if (!conn.recvAll((uint8_t *)&res + sizeof(magic),
                          sizeof(res) - sizeof(magic))) {
            out.transport = true;
            return out;
        }
        const uint32_t peer_version = ntohl(res.version);
        out.peer_version = peer_version;
        out.recv_w = (int)ntohl(res.recv_width);
        out.recv_h = (int)ntohl(res.recv_height);
        out.status = ntohl(res.status);
        if (peer_version == TVHS_VERSION) {
            /* Same protocol: the receiver refused before the challenge
             * (BUSY, AUTH_CONFIG, ...). Keep its status so the caller can
             * show the real reason instead of a bogus update hint. HS_OK
             * here would mean "authorized without any challenge", which is
             * a protocol violation – fail closed before reading a token. */
            if (out.status == HS_OK) out.protocol_error = true;
            return out;
        }
        out.legacy = true;              /* peer speaks another version */
        out.status = HS_ERR_VERSION;
        return out;
    }
    if (magic != TVHA_MAGIC) {          /* not a Mivara handshake at all */
        out.no_response = true;
        return out;
    }

    HsAuthStart as = {};
    as.magic = htonl(magic);
    if (!conn.recvAll((uint8_t *)&as + sizeof(magic),
                      sizeof(as) - sizeof(magic))) {
        out.transport = true;
        return out;
    }
    /* The challenge carries its own protocol generation. A mismatch (e.g. a
     * build using the old proof formula) is a version problem, not a bad
     * password: fail before sending an identity frame no one can verify. */
    if (ntohl(as.version) != TVHA_VERSION) {
        out.status = HS_ERR_VERSION;
        out.legacy = true;
        return out;
    }
    const uint32_t mode = ntohl(as.mode);

    /* Our identity, shown by the receiver in its approval dialog. */
    HsRequestExtV3 ext = {};
    ext.magic   = htonl(TVHX_MAGIC);
    ext.version = htonl(TVHA_VERSION);
    std::snprintf(ext.device, sizeof(ext.device), "%s", g_device_name.c_str());
    if (!conn.sendAll(&ext, sizeof(ext))) {
        out.transport = true;
        return out;
    }

    if (mode == AUTH_MODE_PASSWORD) {
        const uint32_t iter = ntohl(as.kdf_iter);
        if (iter < HS_KDF_ITER_MIN || iter > HS_KDF_ITER_MAX) {
            out.status = HS_ERR_AUTH_CONFIG;
            return out;
        }
        emitAuthStat("need_password");
        std::string pw;
        if (!takePassword(pw)) {
            out.cancelled = true;
            return out;
        }
        uint8_t key[32];
        mivaraPasswordKey(pw, as.salt, sizeof(as.salt), iter, key);
        mivaraSecureZero(&pw[0], pw.size());
        pw.clear();

        /* The proof binds the exact wire bytes of both requests, so it can
         * neither be replayed (fresh nonce) nor reused for other parameters. */
        uint8_t bound[sizeof(HsRequestV2) + sizeof(HsRequestExtV3)];
        std::memcpy(bound, &hs_out, sizeof(hs_out));
        std::memcpy(bound + sizeof(hs_out), &ext, sizeof(ext));
        HsAuthProof proof = {};
        proof.magic   = htonl(TVHA_MAGIC);
        proof.version = htonl(TVHA_VERSION);
        mivaraAuthProof(key, MIVARA_AUTH_PURPOSE_VIDEO, as.nonce, bound,
                        sizeof(bound), proof.proof);
        mivaraSecureZero(key, sizeof(key));

        emitAuthStat("authenticating");
        if (!conn.sendAll(&proof, sizeof(proof))) {
            out.transport = true;
            return out;
        }
    } else if (mode == AUTH_MODE_MANUAL) {
        /* The receiver user gets up to 30 s to answer the dialog. */
        emitAuthStat("waiting", "timeout=30");
    }
    /* AUTH_MODE_OPEN has no sender-side round trip. */

    conn.setRecvTimeout(mode == AUTH_MODE_MANUAL ? 60000 : 30000);

    HsResponseV2 res = {};
    if (!conn.recvAll(&res, sizeof(res))) {
        out.transport = true;
        return out;
    }
    out.status = ntohl(res.status);
    out.recv_w = (int)ntohl(res.recv_width);
    out.recv_h = (int)ntohl(res.recv_height);
    if (ntohl(res.magic) != TVHS_MAGIC) {
        out.protocol_error = true;      /* not a Mivara handshake response */
        return out;
    }
    const uint32_t peer_version = ntohl(res.version);
    if (peer_version != TVHS_VERSION) {
        out.legacy = true;
        out.peer_version = peer_version;
        out.status = HS_ERR_VERSION;
        return out;
    }
    if (out.status != HS_OK) return out;

    HsResponseExtV3 grant = {};
    if (!conn.recvAll(&grant, sizeof(grant)) ||
        ntohl(grant.magic) != TVHR_MAGIC ||
        ntohl(grant.version) != TVHA_VERSION) {
        out.transport = true;
        return out;
    }
    std::memcpy(out.token, grant.token, sizeof(out.token));
    out.have_token = true;
    return out;
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
    /* Pre-scan --mode before touching capture state: stitch never encodes a
     * frame, so the Source WxH banner (and GetSystemMetrics) is meaningless
     * there. The full argv parse below still owns every value. */
    bool arg_stitch = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 &&
            std::strcmp(argv[i + 1], "stitch") == 0)
            arg_stitch = true;
    }
    if (!arg_stitch)
        getScreenDimensions(SCREEN_WIDTH, SCREEN_HEIGHT);

    std::cout << COL_CYAN << COL_BOLD
              << "========================================\n"
                 "  Mivara SENDER v" MIVARA_VERSION "\n"
                 "========================================\n" << COL_RESET;
    if (arg_stitch)
        std::cout << "  Mode: 拼接 (software KVM – keyboard/mouse only)\n";
    else
        std::cout << "  Source: " << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
                  << "  @ " << TARGET_FPS << " FPS\n";
    std::cout << COL_CYAN << "========================================\n"
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
     * CLI: sender [ip] [port] [--mode mirror|extend|stitch]
     *          [--codec auto|h264|h265|av1|raw|rle] [--hw 0|1]
     *          [--bitrate <kbps>] [--quality <0-51>] [--fps <n>]
     *          [--scale <pct>] [--chroma 420|444]
     *          [--extend-res auto|WxH] [--vdd-add-modes 0|1]
     *          [--stitch-side left|right|top|bottom]
     *   --mode is non-interactive (GUI/automation): skips the prompts.
     *   --stitch-side names the edge the peer sits on (default right); the
     *   GUI asks the user and can switch it mid-session over stdin.
     *   Extend auto-probes the receiver and matches its display size; when
     *   that size is missing from the driver's settings file, --vdd-add-modes 1
     *   appends it (the GUI always sets it unless config.ini has
     *   vdd_mode_write=never; the CLI stays opt-in).
     *   stitch is a software-KVM channel (kvm.h): it never captures or
     *   encodes anything and exits before the display-mode/VDD code below. */
    std::string cli_mode;                       /* empty = ask interactively */
    std::string cli_ip;
    std::string cli_codec = "h264";
    std::string cli_extend_res = "auto";
    int cli_add_modes = -1;                     /* -1 = not specified */
    int cli_stitch_side = KVM_SIDE_RIGHT;       /* where the peer sits */
    int cli_file_drop = 0;                      /* 1 = edge drag sends files */
    int cli_port = 8081;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            cli_mode = argv[++i];
        } else if (std::strcmp(argv[i], "--stitch-side") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            cli_stitch_side = kvmSideFromKey(v);
            if (cli_stitch_side < 0) {
                std::cerr << COL_RED << "Unknown --stitch-side: " << v
                          << "  (expected left | right | top | bottom)\n"
                          << COL_RESET;
                cleanupSockets();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--file-drop") == 0 && i + 1 < argc) {
            cli_file_drop = (std::atoi(argv[++i]) != 0) ? 1 : 0;
        } else if (std::strcmp(argv[i], "--extend-res") == 0 && i + 1 < argc) {
            cli_extend_res = argv[++i];
        } else if (std::strcmp(argv[i], "--vdd-add-modes") == 0 && i + 1 < argc) {
            cli_add_modes = (std::atoi(argv[++i]) != 0) ? 1 : 0;
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

    /* Resolve the mode before probing: stitch dials the KVM port (8084),
     * mirror/extend dial the video port (8081). --mode skips the prompt.
     * Remember whether the choice came from the prompt: cli_mode is never
     * empty after this block, so the extend path must not use that as the
     * "interactive" test. */
    const bool mode_interactive = cli_mode.empty();
    if (mode_interactive) {
        const uint32_t picked = selectDisplayMode();
        cli_mode = (picked == MODE_TRUE_EXTEND) ? "extend"
                 : (picked == MODE_STITCH)      ? "stitch"
                                                : "mirror";
    } else if (cli_mode != "mirror" && cli_mode != "extend" &&
               cli_mode != "stitch") {
        std::cerr << COL_RED << "Unknown --mode: " << cli_mode
                  << "  (expected mirror | extend | stitch)\n" << COL_RESET;
        cleanupSockets();
        return 1;
    }

    std::vector<DiscoveredDevice> receivers;
    if (!cli_ip.empty()) {
        /* Direct connection: sender <ip> [port]
         * Stitch talks to KVM_PORT, not the video port – probing 8081 would
         * report success on a receiver whose 8084 listener failed to bind. */
        const int probe_port = (cli_mode == "stitch") ? KVM_PORT : cli_port;
        std::cout << COL_CYAN << "Direct connection mode: "
                  << cli_ip << ":" << probe_port << "\n" << COL_RESET;
        if (testTcpConnection(cli_ip, probe_port, 2000)) {
            receivers.emplace_back(cli_ip, probe_port);
        } else {
            std::cerr << COL_RED << "Cannot connect to "
                      << cli_ip << ":" << probe_port << "\n" << COL_RESET;
            cleanupSockets(); return 1;
        }
    } else {
        std::cout << COL_CYAN << "Discovering receivers...\n" << COL_RESET;
        receivers = discoverReceivers(5);
    }
    if (receivers.empty()) {
        std::cerr << COL_RED
                  << "No receivers found!\n"
                     "  Check firewall: UDP 1900, TCP 8081, TCP 8082, TCP 8084\n"
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

    /* 拼接 (stitch / software KVM) is not a display mode: the peer keeps its
     * own desktop and only the keyboard/mouse travel. Run the input channel
     * and leave before any capture, codec, VDD or 8081/8082/8083 traffic. */
    if (cli_mode == "stitch") {
        std::cout << COL_GREEN
                  << "Mode: Stitch (KVM input only – no video stream)\n"
                  << COL_RESET;
        /* The GUI switches sides with STITCH|side=… lines on stdin. Stitch
         * returns before the streaming path starts its own reader. */
        startStdinReader();
        dropSetStatSink(dropStatSink);
        KvmClientAuth kvm_auth = {};
        kvm_auth.password = kvmPasswordHook;
        kvm_auth.report = kvmReportHook;
        kvm_auth.file_drop = cli_file_drop;
        const int rc = kvmRunController(sel.ip_address.c_str(), KVM_PORT,
                                        cli_stitch_side, g_running, &kvm_auth);
        cleanupSockets();
        return rc == 0 ? 0 : 1;
    }

    /* cli_mode was resolved above (argv or prompt); stitch already returned. */
    DISPLAY_MODE = (cli_mode == "extend") ? MODE_TRUE_EXTEND : MODE_MIRROR;
    const char *modeLabel =
        DISPLAY_MODE == MODE_TRUE_EXTEND ? "Extend" : "Mirror";
    std::cout << COL_GREEN << "Mode: " << modeLabel << COL_RESET << "\n";

#ifdef _WIN32
    /* Extend mode: bring up the virtual display (attach + position +
     * aim the capture at it). Must happen before the handshake so the
     * correct dimensions are announced to the receiver. */
    if (DISPLAY_MODE == MODE_TRUE_EXTEND) {
        int vdd_w = 1920, vdd_h = 1080;      /* fallback resolution */
        bool manual = false;

        if (cli_extend_res != "auto") {
            manual = parseWxH(cli_extend_res, vdd_w, vdd_h);
            if (manual) {
                std::cout << COL_CYAN << "Extend resolution (manual): "
                          << vdd_w << "x" << vdd_h << "\n" << COL_RESET;
            } else {
                std::cout << COL_YELLOW << "Ignoring invalid --extend-res '"
                          << cli_extend_res << "' (want auto or WxH)\n"
                          << COL_RESET;
            }
        }

        if (!manual) {
            /* Ask the receiver for its current display size BEFORE creating
             * the virtual display: matching it 1:1 is what eliminates the
             * stretched picture. The probe starts no session on the
             * receiver (no window, no decoder). */
            int recv_w = 0, recv_h = 0;
            if (probeReceiverDisplay(sel.ip_address, sel.tcp_port,
                                     recv_w, recv_h)) {
                std::cout << COL_GREEN << "Receiver display: "
                          << recv_w << "x" << recv_h << COL_RESET << "\n";
                if (mode_interactive)
                    selectExtendResolution(vdd_w, vdd_h, recv_w, recv_h);
                else {
                    vdd_w = recv_w;
                    vdd_h = recv_h;
                }
            } else if (mode_interactive) {
                selectExtendResolution(vdd_w, vdd_h);
            }
        }

        /* Make sure the driver's mode list contains the target size. The GUI
         * enables writes unless vdd_mode_write=never; the CLI stays opt-in.
         * Without write permission the closest advertised mode is used and
         * the receiver letterboxes the difference. */
        const bool allowWrite = (cli_add_modes == 1);
        std::string mode_err;
        const VddModeResult mode_res =
            vddEnsureMode(vdd_w, vdd_h, allowWrite, mode_err);
        const bool reload = (mode_res == VDD_MODE_ADDED);
        if (mode_res == VDD_MODE_ABSENT && !mode_err.empty())
            std::cout << COL_YELLOW << "VDD: " << mode_err << "\n"
                      << COL_RESET;

        VddInfo vdd;
        if (!vddStartup(vdd_w, vdd_h, vdd, reload)) {
            emitStatError("vdd_startup_failed");
            cleanupSockets(); return 1;
        }
        if (vdd.w != vdd_w || vdd.h != vdd_h)
            std::cout << COL_YELLOW << "Virtual display: requested "
                      << vdd_w << "x" << vdd_h << ", using "
                      << vdd.w << "x" << vdd.h
                      << " (receiver will letterbox)\n" << COL_RESET;
        g_cap_device   = vdd.device;
        g_cap_origin.x = vdd.x;
        g_cap_origin.y = vdd.y;
        SCREEN_WIDTH   = vdd.w;
        SCREEN_HEIGHT  = vdd.h;
    }
#endif

    g_recv_ip = sel.ip_address;

    /* The authorization exchange may ask the GUI for a password over stdin,
     * so the reader has to be live before the handshake. Every interactive
     * prompt above is finished, so it cannot swallow their input. */
    startStdinReader();

    /* Connect stream socket */
    NetworkSocket conn;
    if (!conn.connect(sel.ip_address, sel.tcp_port)) {
        emitStatError("connect_failed");
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }

    /* ── Handshake v3 (proto.h): request → auth challenge → response + token ── */
    g_device_name = localDeviceName();
    HsRequestV2 hs_out = makeHsRequest(SCREEN_WIDTH, SCREEN_HEIGHT);
    conn.setRecvTimeout(30000);
    const HsOutcome hs = handshakeV3(conn, hs_out);
    if (hs.cancelled) {
        /* The user dismissed the password prompt: not an error worth a popup. */
        std::cout << COL_YELLOW << "Authorization cancelled\n" << COL_RESET;
        emitAuthStat("cancelled");
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    if (hs.no_response) {
        /* Nothing came back before the timeout, or the first bytes were not a
         * Mivara handshake at all. Distinct from "old version" and "peer
         * dropped the connection". */
        std::cerr << COL_RED
                  << "接收端无响应（接收服务未启动、被防火墙拦截或网络不可达）\n"
                  << COL_RESET;
        emitStatError("no_response");
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    if (hs.protocol_error) {
        std::cerr << COL_RED << "接收端握手应答异常（协议不兼容）\n"
                  << COL_RESET;
        emitStatError("protocol_error");
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    if (hs.transport) {
        std::cerr << COL_RED << "Connection lost during authorization\n"
                  << COL_RESET;
        emitStatError("auth_failed");
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    if (hs.legacy) {
        /* Only a genuinely different protocol version lands here; a
         * same-version refusal keeps its own status and is handled below. */
        if (hs.peer_version > TVHS_VERSION) {
            std::cerr << COL_RED
                      << "Handshake refused – the receiver speaks a newer "
                         "protocol (v" << hs.peer_version
                      << "); please update this sender\n"
                      << COL_RESET;
            emitStatError("version_mismatch");
        } else {
            std::cerr << COL_RED
                      << "Handshake refused – the peer is a legacy Mivara build "
                         "without connection authorization; please update both "
                         "Mivara ends\n"
                      << COL_RESET;
            emitStatError("legacy_peer");
        }
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    if (hs.status != HS_OK) {
        std::cerr << COL_RED << "Handshake rejected: "
                  << hsStatusMsg(hs.status) << "\n" << COL_RESET;
        emitStatError(hsStatusToken(hs.status));
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    if (!hs.have_token) {
        /* A v3 grant always carries the token; without it the sidecar
         * services could not be reached, so treat it as a broken peer. */
        emitStatError("auth_config");
#ifdef _WIN32
        vddShutdown();
#endif
        cleanupSockets(); return 1;
    }
    std::memcpy(g_session_token, hs.token, sizeof(g_session_token));
    g_session_token_valid = true;
    emitAuthStat("granted");

    RECV_WIDTH  = hs.recv_w;
    RECV_HEIGHT = hs.recv_h;

    /* Sidecar services belong to the session that was just authorized: dial
     * them only now, and only with its token. Remote compute offload (8082)
     * is used by the RLE path alone; 8083 backs the 'p' port inspector. */
    if (g_codec == CODEC_RLE)
        gpuOffloadConnect();

    std::cout << COL_MAGENTA << "Port inspector: "
              << sel.ip_address << ":" << PORTS_SERVICE_PORT << " ...\n"
              << COL_RESET;
    g_ports_sock = ports_remote_connect(sel.ip_address.c_str(), g_session_token);
    if (PORTS_SOCK_VALID(g_ports_sock)) {
        std::cout << COL_GREEN << "Port inspector active  (press 'p' during stream)\n"
                  << COL_RESET;
    } else {
        std::cout << COL_YELLOW << "Port inspector unavailable\n" << COL_RESET;
    }

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
    /* The stdin reader already runs (started before the handshake). */

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
        if (frame.empty()) {
            emitStatError("capture_failed_or_display_changed");
            break;
        }

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

    /* Release the stream socket BEFORE the virtual-display teardown: the
     * receiver leaves the session the moment it sees EOF, while vddShutdown()
     * (detach + device disable) can take seconds in extend mode. Otherwise
     * the peer kept showing the last frame for the whole teardown. */
    conn.close();

    if (g_gpu_active) gpu_remote_disconnect(g_gpu_sock);
    if (PORTS_SOCK_VALID(g_ports_sock)) ports_remote_disconnect(g_ports_sock);
#ifdef _WIN32
    vddShutdown();   /* detach the virtual display if we attached it */
#endif
    cleanupSockets();
    return 0;
}
