/**
 * RECEIVER.CPP - SCREEN EXTENDER DISPLAY, SSDP ADVERTISING, GPU SERVICE
 *
 * Cross-platform: Windows (Winsock2) / Linux / macOS (POSIX).
 *
 * Screen Extender Mode
 * --------------------
 * When the sender sends MODE_TRUE_EXTEND, the receiver opens a BORDERLESS
 * FULLSCREEN window that covers its entire display, presenting itself as a
 * seamless second monitor. The streamed content is a REAL desktop space on
 * the sender (a virtual display managed automatically by the sender), so
 * windows and the mouse cursor genuinely move across.
 *
 * In extend mode the window:
 *  - Is borderless and positioned at (0, 0) on the receiver display
 *  - Covers the full receiver display (SDL_WINDOW_FULLSCREEN_DESKTOP)
 *  - Has no title bar or window chrome, so it looks like a real monitor
 *
 * In mirror mode a normal resizable window is used (original behaviour).
 *
 * Handshake: protocol v2 (proto.h). The first u32 must be TVHS_MAGIC —
 * anything else is a legacy peer and is rejected with a visible error.
 *   sender → receiver : HsRequestV2  (48 bytes)
 *   receiver → sender : HsResponseV2 (36 bytes, status = HsStatus)
 * Stream frames: u32 pkt_size + payload (RAW / RLE / one coded AU).
 * Control: u32 CTRL_MARKER + u32 CtrlType + body (CTRL_RESIZE = HsRequestV2).
 *
 * Cross-platform notes
 * --------------------
 * - plat_sock_t / PLAT_VALID / PLAT_CLOSE unify all socket use.
 * - ACCEPT_LEN_T = int (Windows) / socklen_t (POSIX).
 * - Signal: SetConsoleCtrlHandler (Windows) / sigaction (POSIX).
 * - SDL_WINDOW_FULLSCREEN_DESKTOP is cross-platform in SDL2.
 */

#include <iostream>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <iomanip>
#include <algorithm>
#include <ctime>
#include <string>
#include <fstream>
#include <mutex>
#include <map>

#include <SDL2/SDL.h>
#include "discover.h"
#include "gpu_accelerate.h"
#include "ports.h"
#include "proto.h"
#include "codec.h"

/* ── Platform headers ───────────────────────────────────────────────────── */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET plat_sock_t;
#define PLAT_INVALID INVALID_SOCKET
#define PLAT_CLOSE(s) closesocket(s)
#define PLAT_VALID(s) ((s) != INVALID_SOCKET)
#define ACCEPT_LEN_T int
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
typedef int plat_sock_t;
#define PLAT_INVALID (-1)
#define PLAT_CLOSE(s) close(s)
#define PLAT_VALID(s) ((s) >= 0)
#define ACCEPT_LEN_T socklen_t
#endif

/* ── GPU Operation constants (must match gpu_accelerate.h) ──────────────── */
#ifndef GPU_OP_COMPRESS
#define GPU_OP_PING 0xFF
#define GPU_OP_COMPRESS 0x01
#define GPU_OP_COLORCONV 0x03
#endif

/* ── ANSI colours ───────────────────────────────────────────────────────── */
#ifdef _WIN32
#define COLOR_RESET ""
#define COLOR_RED ""
#define COLOR_GREEN ""
#define COLOR_YELLOW ""
#define COLOR_BLUE ""
#define COLOR_MAGENTA ""
#define COLOR_CYAN ""
#define COLOR_WHITE ""
#define COLOR_BOLD ""
#else
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN "\033[36m"
#define COLOR_WHITE "\033[37m"
#define COLOR_BOLD "\033[1m"
#endif

/* ── Display mode constants (must match sender.cpp) ─────────────────────── */
#define MODE_MIRROR 0u
#define MODE_TRUE_EXTEND 1u

/* ── Constants ──────────────────────────────────────────────────────────── */
#define TCP_STREAM_PORT 8081
#define BYTES_PER_PIXEL 3
#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900
#define MAX_FRAME_BYTES (7680u * 4320u * 3u)
#define GPU_STATS_FILE "gpu_stats.json"
#define GPU_STATS_INTERVAL 60 // Write stats every 60 seconds

static const int SOCK_BUF = 4 * 1024 * 1024;

/* ── GPU Statistics Structure ───────────────────────────────────────────── */
struct GPUStats
{
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point last_stats_write;

    // Operation counts
    uint64_t total_operations = 0;
    uint64_t compress_ops = 0;
    uint64_t colorfix_ops = 0;
    uint64_t ping_ops = 0;

    // Data processed
    uint64_t total_bytes_in = 0;
    uint64_t total_bytes_out = 0;
    uint64_t compressed_bytes_in = 0;
    uint64_t compressed_bytes_out = 0;

    // Performance metrics
    double total_processing_ms = 0.0;
    double avg_compression_ratio = 0.0;
    uint64_t compression_samples = 0;

    // Client tracking
    uint64_t total_clients = 0;
    uint64_t active_clients = 0;
    uint64_t peak_clients = 0;

    // Per-client tracking
    std::map<std::string, uint64_t> client_connections;

    // Mutex for thread safety
    std::mutex stats_mutex;

    GPUStats()
    {
        start_time = std::chrono::system_clock::now();
        last_stats_write = start_time;
    }

    void recordOperation(uint8_t op, uint32_t bytes_in, uint32_t bytes_out, double ms)
    {
        std::lock_guard<std::mutex> lock(stats_mutex);

        total_operations++;
        total_bytes_in += bytes_in;
        total_bytes_out += bytes_out;
        total_processing_ms += ms;

        switch (op)
        {
        case GPU_OP_COMPRESS:
            compress_ops++;
            compressed_bytes_in += bytes_in;
            compressed_bytes_out += bytes_out;
            compression_samples++;
            // Update running average
            {
                double ratio = (double)bytes_out / (double)bytes_in;
                avg_compression_ratio = avg_compression_ratio * (compression_samples - 1) / compression_samples +
                                        ratio / compression_samples;
            }
            break;
        case GPU_OP_COLORCONV:
            colorfix_ops++;
            break;
        case GPU_OP_PING:
            ping_ops++;
            break;
        }
    }

    void recordClient(const std::string &client_ip)
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        active_clients++;
        total_clients++;
        if (active_clients > peak_clients)
            peak_clients = active_clients;

        auto it = client_connections.find(client_ip);
        if (it != client_connections.end())
        {
            it->second++;
        }
        else
        {
            client_connections[client_ip] = 1;
        }
    }

    void recordClientDisconnect()
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        if (active_clients > 0)
            active_clients--;
    }

    void writeStatsToFile()
    {
        std::lock_guard<std::mutex> lock(stats_mutex);

        auto now = std::chrono::system_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        std::ofstream file(GPU_STATS_FILE);
        if (!file.is_open())
            return;

        file << "{\n";
        file << "  \"timestamp\": \"" << std::time(nullptr) << "\",\n";
        file << "  \"uptime_seconds\": " << uptime << ",\n";
        file << "  \"gpu_stats\": {\n";
        file << "    \"operations\": {\n";
        file << "      \"total\": " << total_operations << ",\n";
        file << "      \"compress\": " << compress_ops << ",\n";
        file << "      \"colorfix\": " << colorfix_ops << ",\n";
        file << "      \"ping\": " << ping_ops << "\n";
        file << "    },\n";
        file << "    \"data_processed\": {\n";
        file << "      \"bytes_in\": " << total_bytes_in << ",\n";
        file << "      \"bytes_out\": " << total_bytes_out << ",\n";
        file << "      \"bytes_in_mb\": " << std::fixed << std::setprecision(2)
             << (total_bytes_in / (1024.0 * 1024.0)) << ",\n";
        file << "      \"bytes_out_mb\": " << (total_bytes_out / (1024.0 * 1024.0)) << "\n";
        file << "    },\n";
        file << "    \"compression\": {\n";
        file << "      \"bytes_in\": " << compressed_bytes_in << ",\n";
        file << "      \"bytes_out\": " << compressed_bytes_out << ",\n";
        file << "      \"avg_ratio\": " << std::fixed << std::setprecision(4) << avg_compression_ratio << ",\n";
        file << "      \"saved_bytes\": " << (compressed_bytes_in - compressed_bytes_out) << ",\n";
        file << "      \"saved_mb\": " << ((compressed_bytes_in - compressed_bytes_out) / (1024.0 * 1024.0)) << "\n";
        file << "    },\n";
        file << "    \"performance\": {\n";
        file << "      \"total_ms\": " << total_processing_ms << ",\n";
        file << "      \"avg_ms_per_op\": " << (total_operations > 0 ? total_processing_ms / total_operations : 0) << ",\n";
        file << "      \"ops_per_second\": " << (uptime > 0 ? (double)total_operations / uptime : 0) << "\n";
        file << "    },\n";
        file << "    \"clients\": {\n";
        file << "      \"active\": " << active_clients << ",\n";
        file << "      \"peak\": " << peak_clients << ",\n";
        file << "      \"total_unique\": " << client_connections.size() << ",\n";
        file << "      \"connections_by_ip\": [\n";

        bool first = true;
        for (const auto &[ip, count] : client_connections)
        {
            if (!first)
                file << ",\n";
            file << "        { \"ip\": \"" << ip << "\", \"connections\": " << count << " }";
            first = false;
        }
        file << "\n      ]\n";
        file << "    }\n";
        file << "  }\n";
        file << "}\n";

        file.close();
        last_stats_write = now;
    }

    void printStats()
    {
        std::lock_guard<std::mutex> lock(stats_mutex);

        auto now = std::chrono::system_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        std::cout << "\n"
                  << COLOR_CYAN << COLOR_BOLD
                  << "╔══════════════════════════════════════════════════════════╗\n"
                  << "║                 GPU SERVICE STATISTICS                   ║\n"
                  << "╠══════════════════════════════════════════════════════════╣\n"
                  << "║ Operations:                                              ║\n"
                  << "║   Total: " << std::setw(35) << std::left << (std::to_string(total_operations) + " ops") << "║\n"
                  << "║   Compress: " << std::setw(34) << std::left << (std::to_string(compress_ops) + " ops") << "║\n"
                  << "║   Colorfix: " << std::setw(34) << std::left << (std::to_string(colorfix_ops) + " ops") << "║\n"
                  << "║   Ping: " << std::setw(38) << std::left << (std::to_string(ping_ops) + " ops") << "║\n"
                  << "║                                                          ║\n"
                  << "║ Data Processed:                                         ║\n"
                  << "║   In:  " << std::setw(37) << std::left
                  << (std::to_string(total_bytes_in / (1024 * 1024)) + " MB") << "║\n"
                  << "║   Out: " << std::setw(37) << std::left
                  << (std::to_string(total_bytes_out / (1024 * 1024)) + " MB") << "║\n"
                  << "║                                                          ║\n"
                  << "║ Compression:                                            ║\n"
                  << "║   Avg Ratio: " << std::setw(33) << std::left
                  << (std::to_string(avg_compression_ratio * 100.0) + "%") << "║\n"
                  << "║   Saved: " << std::setw(37) << std::left
                  << (std::to_string((compressed_bytes_in - compressed_bytes_out) / (1024 * 1024)) + " MB") << "║\n"
                  << "║                                                          ║\n"
                  << "║ Performance:                                            ║\n"
                  << "║   Avg ms/op: " << std::setw(32) << std::left
                  << (std::to_string(total_operations > 0 ? total_processing_ms / total_operations : 0.0) + " ms") << "║\n"
                  << "║   Ops/sec: " << std::setw(34) << std::left
                  << (std::to_string(uptime > 0 ? (double)total_operations / uptime : 0.0)) << "║\n"
                  << "║                                                          ║\n"
                  << "║ Clients:                                                ║\n"
                  << "║   Active: " << std::setw(35) << std::left << std::to_string(active_clients) << "║\n"
                  << "║   Peak: " << std::setw(37) << std::left << std::to_string(peak_clients) << "║\n"
                  << "║   Unique IPs: " << std::setw(31) << std::left << std::to_string(client_connections.size()) << "║\n"
                  << "╚══════════════════════════════════════════════════════════╝\n"
                  << COLOR_RESET << std::endl;
    }
};

/* ── Globals ─────────────────────────────────────────────────────────────── */
static int SCREEN_WIDTH = 1920; /* sender's resolution     */
static int SCREEN_HEIGHT = 1080;
static int TARGET_FPS = 60;
static uint32_t DISPLAY_MODE = MODE_MIRROR;

/* This receiver's own display size (queried via SDL2) */
static int MY_DISPLAY_WIDTH = 1920;
static int MY_DISPLAY_HEIGHT = 1080;

static std::atomic<bool> g_running{true};
static GPUStats g_gpu_stats;

/* ── Platform signal handler ─────────────────────────────────────────────── */
#ifdef _WIN32
static BOOL WINAPI consoleHandler(DWORD sig)
{
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT ||
        sig == CTRL_CLOSE_EVENT)
    {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}
#else
static void sigHandler(int) { g_running = false; }
#endif

/* ── I/O helpers ─────────────────────────────────────────────────────────── */
static int recv_all(plat_sock_t s, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len)
    {
        int n = (int)recv(s, p + got, (int)(len - got), 0);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static int send_all_p(plat_sock_t s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len)
    {
        int n = (int)send(s, p + sent, (int)(len - sent), 0);
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ── Forward declaration ─────────────────────────────────────────────────── */
static bool handleClientConnection(plat_sock_t client);

/* ── Machine-readable status lines (same contract as the sender) ─────────── */
static void emitRxStat(const char *event, const std::string &extra)
{
    std::cout << "STAT|event=" << event;
    if (!extra.empty())
        std::cout << "|" << extra;
    std::cout << "\n" << std::flush;
}

/* SSDP friendly name: this machine's hostname (fallback "TwinView"). */
static std::string localHostName()
{
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0')
        return "TwinView";
    host[sizeof(host) - 1] = '\0';
    return std::string(host);
}

/* Convert a wire-order HsRequestV2 to host order (in place). */
static void hsReqFromWire(HsRequestV2 &r)
{
    r.magic        = ntohl(r.magic);
    r.version      = ntohl(r.version);
    r.width        = ntohl(r.width);
    r.height       = ntohl(r.height);
    r.fps          = ntohl(r.fps);
    r.mode         = ntohl(r.mode);
    r.codec        = ntohl(r.codec);
    r.codec_flags  = ntohl(r.codec_flags);
    r.bitrate_kbps = ntohl(r.bitrate_kbps);
    r.quality      = ntohl(r.quality);
    r.gop_frames   = ntohl(r.gop_frames);
    r.reserved     = ntohl(r.reserved);
}

/* A legacy (pre-v2) peer must NOT end as a black screen: log the error,
 * show it in a small SDL window title, then close the connection. */
static void showLegacyPeerError()
{
    const char *msg =
        "Handshake from legacy TwinView build \xe2\x80\x94 please update both ends";
    std::cerr << COLOR_RED << msg << COLOR_RESET << "\n";
    emitRxStat("error", "msg=legacy_peer");

    if (SDL_Init(SDL_INIT_VIDEO) == 0) {
        SDL_Window *w = SDL_CreateWindow(msg, SDL_WINDOWPOS_CENTERED,
                                         SDL_WINDOWPOS_CENTERED, 640, 60,
                                         SDL_WINDOW_SHOWN);
        if (w) {
            auto until = std::chrono::steady_clock::now() +
                         std::chrono::seconds(8);
            SDL_Event ev;
            while (std::chrono::steady_clock::now() < until) {
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT || ev.type == SDL_KEYDOWN ||
                        ev.type == SDL_MOUSEBUTTONDOWN)
                        until = std::chrono::steady_clock::now();
                }
                SDL_Delay(50);
            }
            SDL_DestroyWindow(w);
        }
        SDL_Quit();
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * QUERY THIS RECEIVER'S DISPLAY SIZE via SDL2
 * ══════════════════════════════════════════════════════════════════════════ */
static void queryMyDisplaySize()
{
    /* Temporarily init SDL just to get display bounds */
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return;
    SDL_DisplayMode dm = {};
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0)
    {
        MY_DISPLAY_WIDTH = dm.w;
        MY_DISPLAY_HEIGHT = dm.h;
    }
    SDL_Quit();
}

/* ══════════════════════════════════════════════════════════════════════════
 * SSDP ADVERTISEMENT THREAD
 * ══════════════════════════════════════════════════════════════════════════ */
static void ssdpAdvertisementThread()
{
    std::cout << "SSDP: starting on port " << SSDP_PORT << "\n";

    plat_sock_t rsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (!PLAT_VALID(rsock))
    {
        std::cerr << "SSDP: socket() failed\n";
        return;
    }

    int reuse = 1, sb = SOCK_BUF;
    setsockopt(rsock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
    setsockopt(rsock, SOL_SOCKET, SO_RCVBUF, (char *)&sb, sizeof(sb));

    struct sockaddr_in baddr = {};
    baddr.sin_family = AF_INET;
    baddr.sin_addr.s_addr = INADDR_ANY;
    baddr.sin_port = htons(SSDP_PORT);
    if (bind(rsock, (struct sockaddr *)&baddr, sizeof(baddr)) < 0)
    {
        std::cerr << "SSDP: bind failed on port " << SSDP_PORT << "\n";
        PLAT_CLOSE(rsock);
        return;
    }

    /* Binding is enough to receive unicast and broadcast M-SEARCH packets.
       Multicast membership is best-effort: VPNs and some enterprise networks
       reject the join, but that must not disable discovery altogether. */
    struct ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(rsock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (char *)&mreq, sizeof(mreq)) < 0)
        std::cerr << "SSDP: multicast join failed; broadcast discovery remains available\n";
    std::cout << "SSDP: listening\n";

    /* M-SEARCH response thread */
    std::thread resp([rsock]()
                     {
        char buf[2048];
        struct sockaddr_in from = {};
        ACCEPT_LEN_T flen = sizeof(from);
        while (g_running) {
#ifdef _WIN32
            DWORD tv = 1000; /* SO_RCVTIMEO is DWORD ms on Windows */
            setsockopt(rsock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
#else
            struct timeval tv = {1, 0};
            setsockopt(rsock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
#endif
            int n = (int)recvfrom(rsock, buf, (int)sizeof(buf) - 1, 0,
                                  (struct sockaddr *)&from,
                                  (ACCEPT_LEN_T *)&flen);
            if (!g_running || n <= 0) continue;
            buf[n] = '\0';
            std::string req(buf);
            if (req.find("M-SEARCH")                == std::string::npos) continue;
            if (req.find("urn:screen-share:receiver") == std::string::npos) continue;
            /* Advertise the IP of the NIC facing the requester (multi-homed safe) */
            std::string lip = getLocalIPForPeer(inet_ntoa(from.sin_addr));
            if (lip.empty()) lip = getLocalIPAddress();
            std::string resp_str =
                "HTTP/1.1 200 OK\r\n"
                "CACHE-CONTROL: max-age=30\r\n"
                "DATE: " + std::to_string((long long)time(nullptr)) + "\r\n"
                "LOCATION: http://" + lip + ":" +
                    std::to_string(TCP_STREAM_PORT) + "/\r\n"
                "SERVER: ScreenShare/1.0\r\n"
                "ST: urn:screen-share:receiver\r\n"
                "NAME: " + localHostName() + "\r\n"
                "USN: uuid:screen-share-" + lip + "\r\n\r\n";
            sendto(rsock, resp_str.c_str(), (int)resp_str.size(), 0,
                   (struct sockaddr *)&from, (ACCEPT_LEN_T)flen);
            std::cout << "SSDP: replied to " << inet_ntoa(from.sin_addr)
                      << " (as " << lip << ")\n";
        } });

    /* NOTIFY thread */
    plat_sock_t nsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (PLAT_VALID(nsock))
    {
        int bc = 1, ttl = 4;
        setsockopt(nsock, SOL_SOCKET, SO_BROADCAST, (char *)&bc, sizeof(bc));
        setsockopt(nsock, IPPROTO_IP, IP_MULTICAST_TTL, (char *)&ttl, sizeof(ttl));
        std::string lip = getLocalIPAddress();
        std::string notify =
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: " +
            std::string(SSDP_ADDR) + ":" +
            std::to_string(SSDP_PORT) + "\r\n"
                                        "CACHE-CONTROL: max-age=30\r\n"
                                        "LOCATION: http://" +
            lip + ":" +
            std::to_string(TCP_STREAM_PORT) + "/\r\n"
                                              "NT: urn:screen-share:receiver\r\n"
                                              "NTS: ssdp:alive\r\n"
                                              "SERVER: ScreenShare/1.0\r\n"
                                              "NAME: " +
            localHostName() + "\r\n"
                              "USN: uuid:screen-share-" +
            lip + "\r\n\r\n";
        struct sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(SSDP_PORT);
        inet_pton(AF_INET, SSDP_ADDR, &dest.sin_addr);
        struct sockaddr_in bdest = {};
        bdest.sin_family = AF_INET;
        bdest.sin_port = htons(SSDP_PORT);
        bdest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        int nc = 0;
        while (g_running)
        {
            sendto(nsock, notify.c_str(), (int)notify.size(), 0,
                   (struct sockaddr *)&dest, sizeof(dest));
            sendto(nsock, notify.c_str(), (int)notify.size(), 0,
                   (struct sockaddr *)&bdest, sizeof(bdest));
            std::cout << "SSDP: NOTIFY #" << ++nc << "\n";
            for (int i = 0; i < 30 && g_running; i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        PLAT_CLOSE(nsock);
    }

    resp.join();
    PLAT_CLOSE(rsock);
    std::cout << "SSDP: stopped\n";
}

/* ══════════════════════════════════════════════════════════════════════════
 * GPU SERVICE THREAD
 * ══════════════════════════════════════════════════════════════════════════ */
static void gpuServiceThread()
{
    gpu_service_run();
}

/* ══════════════════════════════════════════════════════════════════════════
 * PORT SERVICE THREAD
 * ══════════════════════════════════════════════════════════════════════════ */
static void portServiceThread()
{
    ports_service_run();
}

/* ══════════════════════════════════════════════════════════════════════════
 * GPU STATS WRITER THREAD
 * ══════════════════════════════════════════════════════════════════════════ */
static void gpuStatsWriterThread()
{
    auto last_print = std::chrono::steady_clock::now();

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_print).count();

        // Write JSON stats every GPU_STATS_INTERVAL seconds
        auto stats_age = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now() - g_gpu_stats.last_stats_write)
                             .count();

        if (stats_age >= GPU_STATS_INTERVAL)
        {
            g_gpu_stats.writeStatsToFile();
        }

        // Print to console every 30 seconds
        if (elapsed >= 30)
        {
            g_gpu_stats.printStats();
            last_print = now;
        }
    }

    // Final stats write on exit
    g_gpu_stats.writeStatsToFile();
    g_gpu_stats.printStats();
}

/* ══════════════════════════════════════════════════════════════════════════
 * CLIENT CONNECTION HANDLER  –  screen extender + mirror
 * ══════════════════════════════════════════════════════════════════════════ */
static bool handleClientConnection(plat_sock_t client)
{
    int sb = SOCK_BUF;
    setsockopt(client, SOL_SOCKET, SO_RCVBUF, (char *)&sb, sizeof(sb));
#ifdef _WIN32
    DWORD to = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
#else
    struct timeval tv = {10, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    /* Get client IP for stats */
    struct sockaddr_in cli_addr;
    ACCEPT_LEN_T addr_len = sizeof(cli_addr);
    getpeername(client, (struct sockaddr *)&cli_addr, &addr_len);
    std::string client_ip = inet_ntoa(cli_addr.sin_addr);

    /* Record client connection */
    g_gpu_stats.recordClient(client_ip);

    /* ── Handshake v2 (proto.h) ──
     * The first u32 must be TVHS_MAGIC. Anything else is a legacy
     * (pre-v2) TwinView build: report it visibly and close – no silence. */
    Decoder decoder; /* opened below for coded streams */
    CodecId sess_codec = CODEC_RAW;
    HsRequestV2 req = {};

    if (recv_all(client, &req.magic, 4) < 0)
    {
        std::cerr << "Handshake receive failed\n";
        g_gpu_stats.recordClientDisconnect();
        return false;
    }
    if (ntohl(req.magic) != TVHS_MAGIC)
    {
        showLegacyPeerError();
        g_gpu_stats.recordClientDisconnect();
        return false;
    }
    if (recv_all(client, ((char *)&req) + 4, sizeof(req) - 4) < 0)
    {
        std::cerr << "Handshake receive failed\n";
        g_gpu_stats.recordClientDisconnect();
        return false;
    }
    hsReqFromWire(req);

    SCREEN_WIDTH = (int)req.width;
    SCREEN_HEIGHT = (int)req.height;
    TARGET_FPS = (int)req.fps;
    DISPLAY_MODE = req.mode;

    /* Validate dimensions / codec; coded streams need a working decoder
     * (reject, no silent downgrade – see plan §1). */
    uint32_t hs_status = HS_OK;
    if (req.version != TVHS_VERSION)
        hs_status = HS_ERR_VERSION;
    else if (SCREEN_WIDTH <= 0 || SCREEN_WIDTH > 7680 ||
             SCREEN_HEIGHT <= 0 || SCREEN_HEIGHT > 4320 ||
             TARGET_FPS <= 0 || TARGET_FPS > 240)
        hs_status = HS_ERR_DIMS;
    else if (req.codec > (uint32_t)CODEC_AV1)
        hs_status = HS_ERR_CODEC;

    if (hs_status == HS_OK && req.codec >= (uint32_t)CODEC_H264)
    {
        sess_codec = (CodecId)req.codec;
        std::string derr = decoder.open(sess_codec, SCREEN_WIDTH, SCREEN_HEIGHT);
        if (!derr.empty())
        {
            std::cerr << "Decoder open failed: " << derr << "\n";
            hs_status = HS_ERR_DECODER;
        }
    }
    else if (hs_status == HS_OK)
    {
        sess_codec = (CodecId)req.codec;
    }

    if (hs_status != HS_OK)
        std::cerr << "Handshake rejected (status " << hs_status << ")\n";

    const char *modeStr =
        DISPLAY_MODE == MODE_TRUE_EXTEND ? "extend" : "mirror";

    std::cout << "Sender: " << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
              << " @ " << TARGET_FPS << " FPS  mode=" << modeStr
              << "  codec=" << codecToString(sess_codec) << "\n";

    /* ── Send back our display size + status ── */
    HsResponseV2 hs_out = {};
    hs_out.magic = htonl(TVHS_MAGIC);
    hs_out.version = htonl(TVHS_VERSION);
    hs_out.recv_width = htonl((uint32_t)MY_DISPLAY_WIDTH);
    hs_out.recv_height = htonl((uint32_t)MY_DISPLAY_HEIGHT);
    hs_out.status = htonl(hs_status);
    hs_out.codec = htonl((uint32_t)sess_codec); /* echo */
    hs_out.codec_flags = 0;
    hs_out.reserved[0] = 0;
    hs_out.reserved[1] = 0;
    if (send_all_p(client, &hs_out, sizeof(hs_out)) < 0)
    {
        std::cerr << "Handshake response send failed\n";
        g_gpu_stats.recordClientDisconnect();
        return false;
    }
    if (hs_status != HS_OK)
    {
        g_gpu_stats.recordClientDisconnect();
        return false;
    }

    /* ── SDL init ── */
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        std::cerr << "SDL_Init: " << SDL_GetError() << "\n";
        g_gpu_stats.recordClientDisconnect();
        return false;
    }

    /*
     * Window strategy
     * ───────────────
     * EXTEND modes → borderless fullscreen on the receiver's own display.
     *   SDL_WINDOW_FULLSCREEN_DESKTOP makes the window cover the entire
     *   display without changing the video mode — cross-platform and safe.
     *
     * MIRROR mode → normal resizable window, scaled to fit.
     */
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;

    if (DISPLAY_MODE == MODE_TRUE_EXTEND)
    {
        /* Borderless fullscreen — looks like a real second monitor */
        win = SDL_CreateWindow(
            "TwinView Extended Display",
            SDL_WINDOWPOS_UNDEFINED_DISPLAY(0),
            SDL_WINDOWPOS_UNDEFINED_DISPLAY(0),
            MY_DISPLAY_WIDTH, MY_DISPLAY_HEIGHT,
            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    else
    {
        /* Mirror: scale to a normal window */
        int ww = SCREEN_WIDTH, wh = SCREEN_HEIGHT;
        const int MAX_W = MY_DISPLAY_WIDTH - 60;
        const int MAX_H = MY_DISPLAY_HEIGHT - 100;
        if (ww > MAX_W || wh > MAX_H)
        {
            float sc = std::min((float)MAX_W / (float)ww,
                                (float)MAX_H / (float)wh);
            ww = (int)(ww * sc);
            wh = (int)(wh * sc);
        }
        win = SDL_CreateWindow(
            "TwinView Mirror",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            ww, wh,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    }

    if (!win)
    {
        std::cerr << "Window: " << SDL_GetError() << "\n";
        SDL_Quit();
        g_gpu_stats.recordClientDisconnect();
        return false;
    }

    ren = SDL_CreateRenderer(win, -1,
                             SDL_RENDERER_ACCELERATED |
                                 SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
    {
        std::cerr << "Renderer: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(win);
        SDL_Quit();
        g_gpu_stats.recordClientDisconnect();
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    /* In extend mode: scale sender's stream to fill our display */
    if (DISPLAY_MODE == MODE_TRUE_EXTEND)
    {
        SDL_RenderSetLogicalSize(ren, MY_DISPLAY_WIDTH, MY_DISPLAY_HEIGHT);
    }

    /* Streaming texture. Raw/RLE = RGB24 at the sender's resolution.
     * Coded streams create whichever layout the decoder emits (I420/NV12/
     * RGB24) on the first frame, and again on resize. */
    SDL_Texture *tex = nullptr;
    Uint32 tex_pixfmt = 0;
    int tex_w = 0, tex_h = 0;

    auto ensureTex = [&](Uint32 fmt, int w, int h) -> bool {
        if (tex && tex_pixfmt == fmt && tex_w == w && tex_h == h)
            return true;
        if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
        tex = SDL_CreateTexture(ren, fmt, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!tex) {
            std::cerr << "Texture: " << SDL_GetError() << "\n";
            return false;
        }
        tex_pixfmt = fmt;
        tex_w = w;
        tex_h = h;
        return true;
    };

    if (sess_codec < CODEC_H264 &&
        !ensureTex(SDL_PIXELFORMAT_RGB24, SCREEN_WIDTH, SCREEN_HEIGHT))
    {
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        g_gpu_stats.recordClientDisconnect();
        return false;
    }

    std::cout << "Display ready: "
              << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
              << " → " << MY_DISPLAY_WIDTH << "x" << MY_DISPLAY_HEIGHT
              << " (" << modeStr << ")\n"
              << "ESC or Q to disconnect\n";

    /* Draw an initial "connecting" background so the screen goes dark
       rather than showing desktop bleed-through while waiting for frames */
    SDL_SetRenderDrawColor(ren, 5, 5, 15, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);

    /* ── Receive and render frames ── */
    size_t full_frame = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * BYTES_PER_PIXEL;
    std::vector<uint8_t> frame;
    frame.reserve(full_frame);
    std::vector<uint8_t> rgb_scratch; /* FMT_OTHER → RGB24 conversion buffer */
    DecodedFrame dec;
    /* After CTRL_RESIZE the decoder is fresh: skip non-key frames (bounded
     * so a mis-flagged IDR can never stall the stream into a black screen). */
    int skip_until_key = 0;

    SDL_Event ev;
    bool streaming = true;
    int frames_rx = 0;
    auto start_time = std::chrono::steady_clock::now();

    emitRxStat("start",
               std::string("codec=") + codecToString(sess_codec) +
                   "|mode=" + modeStr +
                   "|src=" + std::to_string(SCREEN_WIDTH) + "x" +
                   std::to_string(SCREEN_HEIGHT) +
                   "|dst=" + std::to_string(MY_DISPLAY_WIDTH) + "x" +
                   std::to_string(MY_DISPLAY_HEIGHT));

    while (streaming && g_running)
    {

        /* Event pump */
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
            {
                streaming = false;
                g_running = false;
            }
            else if (ev.type == SDL_KEYDOWN)
            {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE || k == SDLK_q)
                {
                    streaming = false;
                    g_running = false;
                }
                /* F11: toggle fullscreen (extend mode only) */
                if (k == SDLK_F11 && DISPLAY_MODE == MODE_TRUE_EXTEND)
                {
                    Uint32 flags = SDL_GetWindowFlags(win);
                    SDL_SetWindowFullscreen(
                        win,
                        (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
                            ? 0
                            : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
            }
        }
        if (!streaming)
            break;

        /* 4-byte size header */
        uint32_t net_sz = 0;
        int n = (int)recv(client, (char *)&net_sz, 4, 0);
        if (n <= 0)
        {
            if (n == 0)
                std::cout << "Sender disconnected\n";
            else
                std::cerr << "Recv error (size)\n";
            break;
        }
        uint32_t frame_sz = ntohl(net_sz);

        /* Control packet: CTRL_MARKER + u32 CtrlType + body (proto.h) */
        if (frame_sz == CTRL_MARKER)
        {
            uint32_t ctype = 0;
            if (recv_all(client, &ctype, 4) < 0)
            {
                streaming = false;
                break;
            }
            ctype = ntohl(ctype);

            if (ctype == CTRL_RESIZE || ctype == CTRL_RECONFIG)
            {
                HsRequestV2 rs = {};
                if (recv_all(client, &rs, sizeof(rs)) < 0)
                {
                    streaming = false;
                    break;
                }
                hsReqFromWire(rs);
                int nw = (int)rs.width, nh = (int)rs.height;
                if (nw <= 0 || nw > 7680 || nh <= 0 || nh > 4320)
                {
                    std::cerr << "Bad resize header\n";
                    break;
                }
                /* CTRL_RECONFIG carries live param changes: codec and fps may
                 * differ from the handshake. Always re-open the decoder so a
                 * new SPS/profile (e.g. chroma 420→444) is accepted. */
                if (ctype == CTRL_RECONFIG && rs.codec <= (uint32_t)CODEC_AV1)
                    sess_codec = (CodecId)rs.codec;
                if (rs.fps > 0 && rs.fps <= 240)
                    TARGET_FPS = (int)rs.fps;
                SCREEN_WIDTH = nw;
                SCREEN_HEIGHT = nh;

                if (sess_codec >= CODEC_H264)
                {
                    std::string derr =
                        decoder.open(sess_codec, SCREEN_WIDTH, SCREEN_HEIGHT);
                    if (!derr.empty())
                    {
                        std::cerr << "Decoder rebuild failed: " << derr << "\n";
                        emitRxStat("error", "msg=decoder_rebuild_failed");
                        break;
                    }
                    skip_until_key = 30;
                }
                else
                {
                    skip_until_key = 0;
                }

                if (tex)
                {
                    SDL_DestroyTexture(tex);
                    tex = nullptr;
                }
                tex_pixfmt = 0;
                tex_w = tex_h = 0;
                full_frame = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * BYTES_PER_PIXEL;
                frame.clear();
                frame.reserve(full_frame);
                std::cout << (ctype == CTRL_RECONFIG ? "Reconfigured: "
                                                     : "Resolution changed: ")
                          << SCREEN_WIDTH << "x" << SCREEN_HEIGHT
                          << "  codec=" << codecToString(sess_codec) << "\n";
                emitRxStat(ctype == CTRL_RECONFIG ? "reconfig" : "resize",
                           "src=" + std::to_string(SCREEN_WIDTH) +
                               "x" + std::to_string(SCREEN_HEIGHT));
                continue;
            }
            if (ctype == CTRL_EOS)
            {
                std::cout << "Sender ended the stream (EOS)\n";
                streaming = false;
                break;
            }
            if (ctype == CTRL_FORCE_IDR)
            {
                continue; /* encoder-side request – nothing to do here */
            }
            std::cerr << "Unknown control type: " << ctype << "\n";
            continue;
        }

        if (frame_sz == 0 || frame_sz > MAX_FRAME_BYTES)
        {
            std::cerr << "Bad frame size: " << frame_sz << "\n";
            break;
        }

        /* Frame data */
        frame.resize(frame_sz);
        size_t got = 0;
        while (got < frame_sz)
        {
            n = (int)recv(client, (char *)frame.data() + got,
                          (int)(frame_sz - got), 0);
            if (n <= 0)
            {
                streaming = false;
                break;
            }
            got += (size_t)n;
        }
        if (!streaming)
            break;

        if (sess_codec >= CODEC_H264)
        {
            /* Coded stream: one access unit per packet */
            std::string derr = decoder.decode(frame.data(), frame.size(), dec);
            if (!derr.empty())
            {
                std::cerr << "Decode failed: " << derr << "\n";
                emitRxStat("error", "msg=decode_failed");
                break;
            }
            if (!dec.valid())
                continue; /* parser buffering – no frame for this AU yet */
            if (skip_until_key > 0 && !dec.keyframe)
            {
                skip_until_key--;
                continue;
            }
            skip_until_key = 0;

            if (dec.fmt == DecodedFrame::FMT_I420)
            {
                if (!ensureTex(SDL_PIXELFORMAT_IYUV, dec.width, dec.height))
                    break;
                SDL_UpdateYUVTexture(tex, nullptr,
                                     dec.plane[0], dec.stride[0],
                                     dec.plane[1], dec.stride[1],
                                     dec.plane[2], dec.stride[2]);
            }
            else if (dec.fmt == DecodedFrame::FMT_NV12)
            {
                if (!ensureTex(SDL_PIXELFORMAT_NV12, dec.width, dec.height))
                    break;
                SDL_UpdateNVTexture(tex, nullptr,
                                    dec.plane[0], dec.stride[0],
                                    dec.plane[1], dec.stride[1]);
            }
            else
            {
                if (!ensureTex(SDL_PIXELFORMAT_RGB24, dec.width, dec.height))
                    break;
                rgb_scratch.resize((size_t)dec.width * dec.height * 3);
                std::string cerr_ =
                    frameToRGB24(dec, rgb_scratch.data(), dec.width * 3);
                if (!cerr_.empty())
                {
                    std::cerr << "RGB convert failed: " << cerr_ << "\n";
                    break;
                }
                SDL_UpdateTexture(tex, nullptr, rgb_scratch.data(),
                                  dec.width * 3);
            }
        }
        else
        {
            /*
             * RLE decompression – per the negotiated codec. A full-size
             * payload is the raw fallback (GPU service unavailable).
             * Decompress: stream of [run r g b] tuples.
             */
            bool was_compressed = (sess_codec == CODEC_RLE) &&
                                  (frame_sz < (uint32_t)full_frame);

            if (was_compressed)
            {
                std::vector<uint8_t> decbuf;
                decbuf.reserve(full_frame);
                const uint8_t *src = frame.data();
                const uint8_t *end = src + frame_sz;
                while (src + 3 < end && decbuf.size() < full_frame)
                {
                    uint8_t run = *src++;
                    uint8_t r = *src++;
                    uint8_t g = *src++;
                    uint8_t b = *src++;
                    for (uint8_t j = 0;
                         j < run && decbuf.size() + 3 <= full_frame; j++)
                    {
                        decbuf.push_back(r);
                        decbuf.push_back(g);
                        decbuf.push_back(b);
                    }
                }
                while (decbuf.size() < full_frame)
                    decbuf.push_back(0);
                frame = std::move(decbuf);
            }

            if (!ensureTex(SDL_PIXELFORMAT_RGB24, SCREEN_WIDTH, SCREEN_HEIGHT))
                break;
            SDL_UpdateTexture(tex, nullptr, frame.data(),
                              SCREEN_WIDTH * BYTES_PER_PIXEL);
        }

        /* Render */
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);

        SDL_RenderPresent(ren);
        frames_rx++;

        if (frames_rx % 100 == 0)
        {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - start_time)
                            .count();
            if (secs > 0)
                std::cout << "Frames: " << frames_rx
                          << "  FPS: " << std::fixed << std::setprecision(1)
                          << (float)frames_rx / (float)secs
                          << "  (" << modeStr << ")\n";
        }
    }

    /* Summary */
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time)
                    .count();
    std::cout << "========================================\n"
              << "  RECEIVER SESSION\n"
              << "========================================\n"
              << "  Mode     : " << modeStr << "\n"
              << "  Src res  : " << SCREEN_WIDTH << "x" << SCREEN_HEIGHT << "\n"
              << "  My res   : " << MY_DISPLAY_WIDTH << "x" << MY_DISPLAY_HEIGHT << "\n"
              << "  Frames   : " << frames_rx << "\n"
              << "  Duration : " << secs << " s\n";
    if (secs > 0)
        std::cout << "  Avg FPS  : " << frames_rx / secs << "\n";
    std::cout << "========================================\n";

    emitRxStat("stop", "frames=" + std::to_string(frames_rx) +
                           "|t=" + std::to_string(secs) +
                           "|codec=" + codecToString(sess_codec) +
                           "|src=" + std::to_string(SCREEN_WIDTH) + "x" +
                           std::to_string(SCREEN_HEIGHT));

    if (tex)
        SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    g_gpu_stats.recordClientDisconnect();
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    /* Enable ANSI colours + UTF-8 output so the banner renders correctly
       on legacy conhost consoles (fixes garbled text at startup). */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD conMode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &conMode))
        SetConsoleMode(hOut, conMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(65001);
#else
    struct sigaction sa = {};
    sa.sa_handler = sigHandler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif

    queryMyDisplaySize();

    std::cout << "========================================\n"
              << "         TwinView RECEIVER v0.9.0     \n"
              << "========================================\n"
              << "  Local IP   : " << getLocalIPAddress() << "\n"
              << "  My display : " << MY_DISPLAY_WIDTH
              << "x" << MY_DISPLAY_HEIGHT << "\n"
              << "  Stream TCP : " << TCP_STREAM_PORT << "\n"
              << "  Compute TCP: " << GPU_ACCEL_PORT << "\n"
              << "  Ports TCP  : " << PORTS_SERVICE_PORT << "\n"
              << "  SSDP UDP   : " << SSDP_ADDR << ":" << SSDP_PORT << "\n"
              << "  GPU Stats  : " << GPU_STATS_FILE << "\n"
              << "  Modes      : extend | mirror\n"
              << "========================================\n";

    if (!initSockets())
    {
        std::cerr << "Socket init failed\n";
        return 1;
    }

    std::thread gpu_thread(gpuServiceThread);
    std::thread port_thread(portServiceThread);
    std::thread ssdp_thread(ssdpAdvertisementThread);
    std::thread stats_thread(gpuStatsWriterThread);

    /* TCP stream server */
    plat_sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (!PLAT_VALID(srv))
    {
        std::cerr << "Server socket failed\n";
        g_running = false;
        ssdp_thread.join();
        gpu_thread.join();
        port_thread.join();
        stats_thread.join();
        cleanupSockets();
        return 1;
    }

    int reuse = 1, sb = SOCK_BUF;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
    setsockopt(srv, SOL_SOCKET, SO_RCVBUF, (char *)&sb, sizeof(sb));

    struct sockaddr_in saddr = {};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(TCP_STREAM_PORT);
    saddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        std::cerr << "Bind failed on " << TCP_STREAM_PORT << "\n";
        PLAT_CLOSE(srv);
        g_running = false;
        ssdp_thread.join();
        gpu_thread.join();
        port_thread.join();
        stats_thread.join();
        cleanupSockets();
        return 1;
    }
    listen(srv, 5);
    std::cout << "Waiting for sender on TCP " << TCP_STREAM_PORT << " ...\n";

    while (g_running)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        struct timeval tv = {1, 0};
        int act = select((int)srv + 1, &fds, nullptr, nullptr, &tv);
        if (act < 0 || !g_running)
            break;
        if (act == 0)
            continue;

        struct sockaddr_in cli = {};
        ACCEPT_LEN_T cli_len = sizeof(cli);
        plat_sock_t client = accept(srv, (struct sockaddr *)&cli,
                                    (ACCEPT_LEN_T *)&cli_len);
        if (!PLAT_VALID(client))
            continue;

        std::cout << "Sender connected: " << inet_ntoa(cli.sin_addr) << "\n";
        handleClientConnection(client);
        PLAT_CLOSE(client);
        if (g_running)
            std::cout << "Waiting for next sender...\n";
    }

    PLAT_CLOSE(srv);
    g_running = false; /* signal threads before joining */
    ssdp_thread.join();
    gpu_thread.join();
    port_thread.join();
    stats_thread.join();
    cleanupSockets();

    // Final stats write
    g_gpu_stats.writeStatsToFile();
    g_gpu_stats.printStats();

    std::cout << "Receiver shut down\n";
    return 0;
}
