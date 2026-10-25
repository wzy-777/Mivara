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
 * Mirror mode also opens borderless fullscreen by default; F11 in either
 * mode drops back to a normal resizable window (original behaviour).
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
#include <cstdlib>
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
#include <condition_variable>
#include <deque>
#include <memory>
#include <map>

#include <SDL2/SDL.h>
#include "auth.h"
#include "config.h"
#include "discover.h"
#include "gpu_accelerate.h"
#include "kvm.h"
#include "ports.h"
#include "proto.h"
#include "codec.h"
#include "version.h"

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

/* Display size cache readable from the handshake workers. SDL may only be
 * touched on the main thread, so queryMyDisplaySize() (main thread) updates
 * this cache and the workers only read it. Width and height are published as
 * one 64-bit value so a worker can never pair a new width with an old height. */
static std::atomic<uint64_t> g_disp_cache{((uint64_t)1920 << 32) | 1080u};

static void dispCacheSet(int w, int h)
{
    g_disp_cache.store(((uint64_t)(uint32_t)w << 32) | (uint32_t)h,
                       std::memory_order_relaxed);
}

static void dispCacheGet(int &w, int &h)
{
    const uint64_t v = g_disp_cache.load(std::memory_order_relaxed);
    w = (int)(uint32_t)(v >> 32);
    h = (int)(uint32_t)v;
}

/* ══════════════════════════════════════════════════════════════════════════
 * CONNECTION AUTHORIZATION (PRD: 接收与安全)
 * ══════════════════════════════════════════════════════════════════════════ */

/* The policy is read from config.ini, not from argv: the password record must
 * never appear on a command line or in the process list. The file is the same
 * one the GUI writes, and the children run with the same environment. */
struct RxPolicy
{
    MivaraAuthMode mode = MIVARA_AUTH_MANUAL;
    uint8_t  salt[16] = {};
    uint8_t  hash[32] = {};
    uint32_t iter = 0;
    bool     record_ok = false;   /* password mode: record present + valid */
    std::string why;              /* diagnostic when !record_ok */
};

static std::mutex g_policy_mtx;
static RxPolicy g_policy;

/* Serializes STAT writes: handshake workers, the session thread and the
 * service threads all print. */
static std::mutex g_stat_mtx;

/* Rejects handshakes beyond this many concurrent workers (fixed pool). */
#define RX_WORKERS 4

/* Sessions authorized by a handshake worker and waiting for the main thread
 * (SDL must run on the main thread). */
struct GrantedSession
{
    plat_sock_t sock = PLAT_INVALID;
    std::string ip;
    std::string device;
    HsRequestV2 req = {};
    std::unique_ptr<Decoder> decoder;
    CodecId codec = CODEC_RAW;
    uint8_t token[HS_TOKEN_BYTES] = {};
    /* Cross-mode slot (8081 + 8084) owned by this session; the main loop
     * releases it when the session ends. */
    MivaraSessionLease lease = {};
};

/* Releases a claimed cross-mode slot on every early handshake return. The
 * claim is handed over to GrantedSession only once the session is queued. */
struct SessionLeaseGuard
{
    MivaraSessionLease lease;
    SessionLeaseGuard() = default;
    explicit SessionLeaseGuard(const MivaraSessionLease &l) : lease(l) {}
    SessionLeaseGuard(const SessionLeaseGuard &) = delete;
    SessionLeaseGuard &operator=(const SessionLeaseGuard &) = delete;
    ~SessionLeaseGuard()
    {
        if (lease.epoch)
            mivaraSessionRelease(lease);
    }
    /** Claim once; a second call is a no-op. */
    bool acquire(MivaraSessionKind kind, int ttl_ms)
    {
        if (lease.epoch) return true;
        return mivaraSessionClaim(kind, ttl_ms, &lease);
    }
    void dismiss() { lease = MivaraSessionLease{}; }
};

static std::mutex g_sess_mtx;
static std::condition_variable g_sess_cv;
static std::deque<GrantedSession> g_sess_q;
static std::deque<plat_sock_t> g_handshake_q;

/* Disconnect request from the GUI (RX|disconnect). */
static std::mutex g_active_mtx;
static plat_sock_t g_active_client = PLAT_INVALID;
static std::atomic<bool> g_disconnect_req{false};

/* Manual approval bookkeeping. Entries are keyed by (ip, device): a second
 * connection from the same sender while its prompt is open waits on the same
 * decision instead of opening a second prompt (PRD: 重复请求合并). */
struct Approval
{
    uint32_t id = 0;
    std::string ip;
    std::string device;
    std::string purpose;       /* "video" (8081) | "stitch" (8084) */
    int waiters = 1;
    /* 0 open, 1 allow, -1 deny, -2 timeout, -3 receiver shutting down,
     * -4 peer hung up / sent before approval. */
    int decision = 0;
    bool granted = false;      /* allow consumed by one waiter */
    int64_t deadline_ms = 0;
    bool announced = false;    /* STAT pending emitted */
    bool done_emitted = false; /* exactly one terminal STAT done per id */
};

static std::mutex g_appr_mtx;
static std::condition_variable g_appr_cv;
static std::vector<std::unique_ptr<Approval>> g_apprs;
static std::atomic<uint32_t> g_next_appr_id{1};

/* Password failures are counted by the process-wide throttle in auth.cpp: the
 * 8081 and 8084 handshakes share one connection password, so alternating ports
 * must not grant two independent budgets (issue 14). */

/* GUI mode: stdout/stdin are pipes owned by the launcher (app). */
static bool g_gui_mode = false;

static int64_t steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

/* Stop callback handed to the C sidecar services (8082 / 8083).  It reads
   the one existing shutdown flag, so both services stay in sync with the
   SDL loop, the console handler and the POSIX signal handler. */
extern "C" int mivara_service_should_stop(void)
{
    return g_running ? 0 : 1;
}

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

/* ── Machine-readable status lines (same contract as the sender) ─────────── */
static void emitRxStat(const char *event, const std::string &extra)
{
    std::lock_guard<std::mutex> lk(g_stat_mtx);
    std::cout << "STAT|event=" << event;
    if (!extra.empty())
        std::cout << "|" << extra;
    std::cout << "\n" << std::flush;
}

/* ── authorization policy ────────────────────────────────────────────────── */

/* Load config.ini and derive the policy. A missing/unknown mode is manual
 * (fail closed); a malformed password record keeps password mode but marks it
 * unusable so every handshake is refused with HS_ERR_AUTH_CONFIG — never
 * silently downgraded to "open". */
static RxPolicy loadPolicyFromConfig()
{
    RxPolicy p;
    AppConfig cfg;
    configLoad(cfg);

    MivaraAuthMode mode = MIVARA_AUTH_MANUAL;
    if (!mivaraAuthModeFromKey(cfg.auth_mode, mode))
        mode = MIVARA_AUTH_MANUAL;          /* unknown key → manual */
    p.mode = mode;
    p.iter = (uint32_t)cfg.auth_iter;

    if (mode == MIVARA_AUTH_PASSWORD) {
        uint8_t salt[16] = {}, hash[32] = {};
        std::string why;
        if (!mivaraHexDecode(cfg.auth_salt, salt, sizeof(salt)))
            why = "密码记录缺失或损坏";
        else if (!mivaraHexDecode(cfg.auth_hash, hash, sizeof(hash)))
            why = "密码记录缺失或损坏";
        else if (!mivaraAuthRecordUsable(hash, salt, p.iter, &why))
            /* why set by the helper */;
        if (why.empty()) {
            std::memcpy(p.salt, salt, sizeof(salt));
            std::memcpy(p.hash, hash, sizeof(hash));
            p.record_ok = true;
        } else {
            p.record_ok = false;
            p.why = why;
        }
    }
    return p;
}

static const char *policyKey(const RxPolicy &p)
{
    return mivaraAuthModeKey(p.mode);
}

/* Replace the policy atomically and tell the GUI. `announce_error` is false on
 * the startup path: the auth_config error is emitted after the ready line so a
 * corrupt record ends as a fatal state instead of being covered by "ready"
 * (issue 8). A reload always announces it, because there is no ready to follow. */
static void applyPolicy(const RxPolicy &p, bool announce_error = true)
{
    {
        std::lock_guard<std::mutex> lk(g_policy_mtx);
        g_policy = p;
    }
    emitRxStat("policy", std::string("mode=") + policyKey(p) +
                             (p.mode == MIVARA_AUTH_PASSWORD
                                  ? (p.record_ok ? "|record=ok" : "|record=bad")
                                  : ""));
    if (announce_error && p.mode == MIVARA_AUTH_PASSWORD && !p.record_ok)
        emitRxStat("error", "msg=auth_config");
}

static void reloadPolicyFromConfig()
{
    applyPolicy(loadPolicyFromConfig(), true);
}

static RxPolicy currentPolicy()
{
    std::lock_guard<std::mutex> lk(g_policy_mtx);
    return g_policy;
}

/* Copy of the current policy after the handshake response is built. */
static bool policyRefusesAll(const RxPolicy &p)
{
    return p.mode == MIVARA_AUTH_PASSWORD && !p.record_ok;
}

/* ── session token (shared with the 8082/8083 services) ────────────────────
 * The grant path installs the token through mivaraSessionTokenInstall() before
 * it is disclosed, and clears it with mivaraSessionTokenClearIf() so an ending
 * session cannot wipe a newer one. Only the final shutdown clears blindly. */
static void clearSessionToken()
{
    static const uint8_t zero[HS_TOKEN_BYTES] = {};
    mivaraSessionTokenSet(zero);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MANUAL APPROVAL (每次手动授权)
 * ══════════════════════════════════════════════════════════════════════════ */

#define APPROVAL_TIMEOUT_MS 30000

enum ApprovalResult
{
    APPROVAL_ALLOW = 0,
    APPROVAL_DENY,
    APPROVAL_TIMEOUT,
    APPROVAL_CANCELLED,  /* peer vanished, receiver shutting down */
    APPROVAL_FAILED,     /* no human was asked (no interactive desktop) */
    APPROVAL_BUSY        /* another device's prompt is already open */
};

/* Only one native prompt may exist at a time (one MessageBox / one stdin
 * prompt). A second request is answered "busy" instead of stacking dialogs. */
static std::atomic<bool> g_native_prompt_open{false};

/* Ask the local user without the GUI. Only used when the receiver was started
 * by hand; the app path uses STAT/stdin below. */
static ApprovalResult nativeManualPrompt(const std::string &ip,
                                         const std::string &device,
                                         plat_sock_t client,
                                         const char *purpose = "video")
{
    const bool stitch = purpose && std::strcmp(purpose, "stitch") == 0;
    if (g_native_prompt_open.exchange(true)) return APPROVAL_BUSY;
    struct PromptGuard {
        ~PromptGuard() { g_native_prompt_open = false; }
    } prompt_guard;
    /* Nothing may already be queued: a peer that sends before approval is
     * either broken or hostile. */
    char sink[1];
#ifdef _WIN32
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(client, &rd);
    struct timeval zero = {0, 0};
    if (select(0, &rd, nullptr, nullptr, &zero) > 0) {
        if (recv(client, sink, sizeof(sink), MSG_PEEK) == 0)
            return APPROVAL_CANCELLED;
    }

    std::wstring wdev(device.begin(), device.end());
    std::wstring wip(ip.begin(), ip.end());
    std::wstring msg = L"设备「" + wdev + L"」(" + wip + L") 请求" +
                       (stitch ? L"拼接控制（键盘与鼠标）"
                               : L"投屏 / 扩展连接") +
                       L"到此电脑。\n\n"
                       L"仅在你信任这台设备时选择「是」。30 秒内未选择将自动拒绝。";
    const DWORD owner_tid = GetCurrentThreadId();
    std::atomic<bool> done{false};
    std::atomic<int> reason{0};      /* 1 timeout, 2 peer, 3 shutdown */
    /* One monotonic deadline for the watcher and for the decision below. A
     * click that arrives after it must never create a session (issue 21). */
    const int64_t start = steadyMs();
    std::thread watcher([&] {
        int why = 0;
        while (!done.load()) {
            if (!g_running.load()) { why = 3; break; }
            if (steadyMs() - start >= APPROVAL_TIMEOUT_MS) { why = 1; break; }
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(client, &fds);
            struct timeval tv = {0, 100000};
            if (select(0, &fds, nullptr, nullptr, &tv) < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (FD_ISSET(client, &fds)) {
                const int n = recv(client, sink, sizeof(sink), 0);
                if (n == 0) { why = 2; break; }
                if (n < 0) {
                    const int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK && err != WSAETIMEDOUT &&
                        err != WSAEINTR) { why = 2; break; }
                }
            }
        }
        if (done.load()) return;
        reason = why ? why : 1;
        const int64_t give_up = steadyMs() + 3000;
        for (;;) {
            struct RxDismiss { DWORD tid; int posted; } ctx{owner_tid, 0};
            EnumThreadWindows(owner_tid, [](HWND h, LPARAM lp) -> BOOL {
                RxDismiss *c = (RxDismiss *)lp;
                wchar_t cls[64] = L"";
                if (GetClassNameW(h, cls, 64) && wcscmp(cls, L"#32770") == 0) {
                    PostMessageW(h, WM_COMMAND,
                                 MAKEWPARAM(IDNO, BN_CLICKED), 0);
                    ++c->posted;
                }
                return TRUE;
            }, (LPARAM)&ctx);
            if (ctx.posted || steadyMs() >= give_up) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    const int r = MessageBoxW(nullptr, msg.c_str(), L"Mivara - 连接请求",
                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                                  MB_TOPMOST | MB_SETFOREGROUND);
    done = true;
    if (watcher.joinable()) watcher.join();
    /* The deadline wins over IDYES: the watcher may have already expired but
     * not yet dismissed the dialog when the user clicked "是". */
    if (reason.load() == 1 || steadyMs() - start >= APPROVAL_TIMEOUT_MS)
        return APPROVAL_TIMEOUT;
    if (reason.load() == 2 || reason.load() == 3) return APPROVAL_CANCELLED;
    if (r == IDYES) return APPROVAL_ALLOW;
    if (r == IDNO) return APPROVAL_DENY;
    return APPROVAL_FAILED;
#else
    /* POSIX: prompt on the terminal. select() gives the 30 s auto-deny and
     * also notices a closed stdin (EOF -> fail closed). */
    std::fprintf(stderr,
                 "\nMivara: 设备「%s」(%s) 请求%s。允许? [y/N] (30 秒后自动拒绝) ",
                 device.c_str(), ip.c_str(),
                 stitch ? "拼接控制（键盘与鼠标）" : "投屏 / 扩展连接");
    std::fflush(stderr);

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(0, &rd);
    struct timeval tv = {APPROVAL_TIMEOUT_MS / 1000, 0};
    const int64_t start = steadyMs();
    const int sel = select(1, &rd, nullptr, nullptr, &tv);
    if (!g_running) return APPROVAL_CANCELLED;
    /* Same monotonic deadline as the GUI/native paths: a late "y" is a timeout
     * and never opens a session (issue 21). */
    if (sel == 0 || steadyMs() - start >= APPROVAL_TIMEOUT_MS) {
        std::fprintf(stderr, "\n已超时，拒绝连接\n");
        return APPROVAL_TIMEOUT;
    }
    if (sel < 0) return APPROVAL_FAILED;
    std::string line;
    char c;
    for (;;) {
        const ssize_t n = read(0, &c, 1);
        if (n <= 0) return APPROVAL_DENY;      /* EOF: fail closed */
        if (c == '\n' || c == '\r') break;
        if (line.size() < 16) line.push_back(c);
    }
    if (steadyMs() - start >= APPROVAL_TIMEOUT_MS) {
        std::fprintf(stderr, "\n已超时，拒绝连接\n");
        return APPROVAL_TIMEOUT;
    }
    if (line == "y" || line == "Y" || line == "yes" || line == "YES")
        return APPROVAL_ALLOW;
    return APPROVAL_DENY;
#endif
}

/* Non-destructive liveness probe for a peer waiting on manual approval.
 * A zero-timeout select gates recv(MSG_PEEK), so this never blocks and never
 * consumes a byte. A closed socket, a hard error or any unsolicited byte (the
 * sender must stay silent until we answer) means the request is over. */
static bool peerGone(plat_sock_t s)
{
    if (!PLAT_VALID(s)) return false;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    struct timeval zero = {0, 0};
#ifdef _WIN32
    const int sel = select(0, &rd, nullptr, nullptr, &zero);
#else
    const int sel = select((int)s + 1, &rd, nullptr, nullptr, &zero);
#endif
    if (sel < 0) {
#ifdef _WIN32
        const int err = WSAGetLastError();
        return err != WSAEWOULDBLOCK && err != WSAEINTR;
#else
        return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
#endif
    }
    if (sel == 0) return false;              /* no data: still connected */
    char c = 0;
    const int n = (int)recv(s, &c, 1, MSG_PEEK);
    if (n == 0) return true;                 /* orderly shutdown */
    if (n > 0) return true;                  /* unsolicited data: violation */
#ifdef _WIN32
    const int err = WSAGetLastError();
    return err != WSAEWOULDBLOCK && err != WSAETIMEDOUT && err != WSAEINTR;
#else
    return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
#endif
}

/* GUI path: queue a prompt for the app and wait for AUTH|id=N|allow/deny.
 * Requests from the same device merge into one prompt; only one merged request
 * is granted the session, the rest report "busy". `client` lets the waiter
 * notice that the sender hung up, so a cancelled request releases the approval
 * slot immediately instead of holding it for the full 30 s (issue 11). */
static ApprovalResult waitManualApproval(const std::string &ip,
                                         const std::string &device,
                                         plat_sock_t client,
                                         const char *purpose = "video")
{
    Approval *a = nullptr;
    bool announce = false;
    {
        std::lock_guard<std::mutex> lk(g_appr_mtx);
        for (auto &e : g_apprs)
            if (e->decision == 0 && e->ip == ip && e->device == device &&
                e->purpose == purpose) {
                a = e.get();
                a->waiters++;
                break;
            }
        if (!a) {
            /* One prompt at a time across 8081 and 8084: the GUI has a single
             * approval dialog and the native path a single MessageBox. A
             * second device must be answered "busy" instead of silently
             * replacing the request the user is looking at. Requests from the
             * same device still merge above. */
            for (auto &e : g_apprs)
                if (e->decision == 0) return APPROVAL_BUSY;
            auto up = std::make_unique<Approval>();
            up->id = g_next_appr_id++;
            up->ip = ip;
            up->device = device;
            up->purpose = purpose;
            up->deadline_ms = steadyMs() + APPROVAL_TIMEOUT_MS;
            a = up.get();
            g_apprs.push_back(std::move(up));
            announce = true;
        }
    }

    if (announce)
        emitRxStat("auth", "state=pending|id=" + std::to_string(a->id) +
                               "|dev=" + device + "|ip=" + ip +
                               "|mode=manual|req=" + purpose);

    int decision = 0;
    bool take = false;
    bool emitTimeout = false;
    bool emitCancelled = false;
    uint32_t doneId = 0;
    {
        std::unique_lock<std::mutex> lk(g_appr_mtx);
        for (;;) {
            if (a->decision != 0) break;
            if (!g_running) { a->decision = -3; break; }
            const int64_t left = a->deadline_ms - steadyMs();
            if (left <= 0) {
                a->decision = -2;
                a->done_emitted = true;
                emitTimeout = true;
                doneId = a->id;
                break;
            }
            /* The peer hung up while the user was deciding: close the prompt
             * now instead of after the 30 s deadline. The deadline check above
             * runs first, so "closed exactly at 30 s" is a timeout. */
            if (peerGone(client)) {
                a->decision = -4;
                a->done_emitted = true;
                emitCancelled = true;
                doneId = a->id;
                break;
            }
            g_appr_cv.wait_for(lk, std::chrono::milliseconds(
                                       std::min<int64_t>(left, 250)));
        }
        decision = a->decision;
        /* A late allow is a timeout, never a grant. resolveApproval may
         * already have reported result=allow for a click that was in time;
         * then only the local verdict changes (one done line per id). */
        if (decision == 1 && steadyMs() >= a->deadline_ms) {
            a->decision = -2;
            decision = -2;
            if (!a->done_emitted) {
                a->done_emitted = true;
                emitTimeout = true;
            }
            doneId = a->id;
        }
        if (decision == 1 && !a->granted) {
            a->granted = true;
            take = true;
        }
        if (a->waiters > 0) a->waiters--;
        if (a->waiters <= 0 && a->decision != 0) {
            for (size_t i = 0; i < g_apprs.size(); i++)
                if (g_apprs[i].get() == a) {
                    g_apprs.erase(g_apprs.begin() + (long)i);
                    break;
                }
        }
    }
    if (emitTimeout)
        emitRxStat("auth", "state=done|id=" + std::to_string(doneId) +
                               "|result=timeout");
    else if (emitCancelled)
        emitRxStat("auth", "state=done|id=" + std::to_string(doneId) +
                               "|result=cancelled");

    switch (decision) {
    case 1:  return take ? APPROVAL_ALLOW : APPROVAL_DENY;
    case -1: return APPROVAL_DENY;
    case -2: return APPROVAL_TIMEOUT;
    case -3:
    case -4: return APPROVAL_CANCELLED;
    default: return APPROVAL_FAILED;
    }
}

/* Resolve a pending prompt from the GUI (stdin thread). The monotonic deadline
 * is checked under the same lock as the decision, so an "allow" that arrives
 * after 30 s is recorded as a timeout and never grants a session. Exactly one
 * terminal done line is emitted per id (the waiter stays silent when it sees
 * the -2 that this function just wrote). */
static void resolveApproval(uint32_t id, bool allow)
{
    bool found = false;
    bool late = false;
    {
        std::lock_guard<std::mutex> lk(g_appr_mtx);
        for (auto &e : g_apprs)
            if (e->id == id && e->decision == 0) {
                if (steadyMs() >= e->deadline_ms) {
                    e->decision = -2;
                    late = true;
                } else {
                    e->decision = allow ? 1 : -1;
                }
                e->done_emitted = true;
                found = true;
                break;
            }
    }
    if (found) {
        g_appr_cv.notify_all();
        emitRxStat("auth", "state=done|id=" + std::to_string(id) +
                               std::string("|result=") +
                               (late ? "timeout" : (allow ? "allow" : "deny")));
    }
    /* An unknown id (already timed out) is ignored: the prompt is gone and
     * nothing is granted. */
}

/* The app can withdraw every pending prompt when it exits. */
static void cancelAllApprovals()
{
    {
        std::lock_guard<std::mutex> lk(g_appr_mtx);
        for (auto &e : g_apprs)
            if (e->decision == 0) e->decision = -3;
    }
    g_appr_cv.notify_all();
}

/* ── 8084 (拼接) authorization hooks ─────────────────────────────────────── */
/* 拼接 must not be a way around 接收与安全: the input channel asks for the
 * same policy snapshot and reuses the same approval path (app popup in GUI
 * mode, native prompt when the receiver runs alone). There is deliberately no
 * second, weaker consent dialog — manual mode is the only prompt, and it is
 * the one the user configured. */
static bool kvmPolicyHook(MivaraAuthMode *mode, uint8_t salt[16],
                          uint8_t hash[32], uint32_t *iterations,
                          bool *record_ok, void *ctx)
{
    (void)ctx;
    const RxPolicy p = currentPolicy();
    *mode = p.mode;
    std::memcpy(salt, p.salt, sizeof(p.salt));
    std::memcpy(hash, p.hash, sizeof(p.hash));
    *iterations = p.iter;
    *record_ok = p.record_ok;
    return true;
}

static KvmApprovalResult kvmApproveHook(const char *ip, const char *device,
                                        intptr_t client, void *ctx)
{
    (void)ctx;
    const std::string peer_ip = ip ? ip : "";
    const std::string peer_dev =
        (device && *device) ? std::string(device) : peer_ip;
    const ApprovalResult ar =
        g_gui_mode ? waitManualApproval(peer_ip, peer_dev,
                                        (plat_sock_t)client, "stitch")
                   : nativeManualPrompt(peer_ip, peer_dev,
                                        (plat_sock_t)client, "stitch");
    /* Every verdict maps to its own value: kvm.cpp must only arm the deny
     * cooldown for an explicit human refusal (issue 11). */
    switch (ar) {
    case APPROVAL_ALLOW:     return KVM_APPROVAL_ALLOW;
    case APPROVAL_DENY:      return KVM_APPROVAL_DENY;
    case APPROVAL_TIMEOUT:   return KVM_APPROVAL_TIMEOUT;
    case APPROVAL_CANCELLED: return KVM_APPROVAL_CANCELLED;
    case APPROVAL_BUSY:      return KVM_APPROVAL_BUSY;
    default:                 return KVM_APPROVAL_FAILED;
    }
}

static const KvmTargetAuth g_kvm_auth = {kvmPolicyHook, kvmApproveHook,
                                         nullptr};


/* SSDP friendly name: this machine's hostname (fallback "Mivara"). */
static std::string localHostName()
{
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0')
        return "Mivara";
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

/* A legacy (pre-v3) peer must NOT end as a silent black screen. This runs on
 * a handshake worker, where SDL must not be touched, so the report is a STAT
 * line + console message; the GUI turns it into a visible notification. */
static void showLegacyPeerError()
{
    const char *msg =
        "Handshake from legacy Mivara build \xe2\x80\x94 please update both ends";
    {
        std::lock_guard<std::mutex> lk(g_stat_mtx);
        std::cerr << COLOR_RED << msg << COLOR_RESET << "\n" << std::flush;
    }
    emitRxStat("error", "msg=legacy_peer");
}

/* ══════════════════════════════════════════════════════════════════════════
 * QUERY THIS RECEIVER'S DISPLAY SIZE via SDL2
 * ══════════════════════════════════════════════════════════════════════════ */
static void queryMyDisplaySize()
{
    /* Ask SDL for physical pixels. Without this the process stays DPI
     * virtualised (the receiver has no dpiAware manifest), so at 150 %
     * scaling SDL_GetCurrentDisplayMode reports 1280x720 for a 1920x1080
     * panel. The sender would then match the virtual display to the wrong
     * size and the stream would be upscaled. The hint must be set before
     * SDL_Init; queryMyDisplaySize() is the first SDL call in the process. */
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    /* Temporarily init SDL just to get display bounds */
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return;
    SDL_DisplayMode dm = {};
    if (SDL_GetCurrentDisplayMode(0, &dm) != 0)
        SDL_GetDesktopDisplayMode(0, &dm);   /* fallback */
    if (dm.w > 0 && dm.h > 0)
    {
        MY_DISPLAY_WIDTH = dm.w;
        MY_DISPLAY_HEIGHT = dm.h;
        dispCacheSet(dm.w, dm.h);
    }
    SDL_Quit();
}

/* Thread-safe IPv4 text form. inet_ntoa() returns a per-process static buffer
 * on POSIX, which the SSDP thread and the handshake workers would share. */
static std::string ipv4Text(const struct in_addr &a)
{
    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &a, buf, sizeof(buf))) return std::string();
    return std::string(buf);
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
            std::string lip = getLocalIPForPeer(ipv4Text(from.sin_addr));
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
            std::cout << "SSDP: replied to " << ipv4Text(from.sin_addr)
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
    gpu_service_run(mivara_service_should_stop);
}

/* ══════════════════════════════════════════════════════════════════════════
 * PORT SERVICE THREAD
 * ══════════════════════════════════════════════════════════════════════════ */
static void portServiceThread()
{
    ports_service_run(mivara_service_should_stop);
}

/* ══════════════════════════════════════════════════════════════════════════
 * GPU STATS WRITER THREAD
 * ══════════════════════════════════════════════════════════════════════════ */
static void gpuStatsWriterThread()
{
    auto last_print = std::chrono::steady_clock::now();

    while (g_running)
    {
        /* Sleep in 1 s chunks so shutdown is not delayed by a full 10 s. */
        for (int i = 0; i < 10 && g_running; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_running)
            break;

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
/* Read exactly len bytes, polling so shutdown and the handshake deadline are
 * honoured even while a peer is silent (e.g. its user is typing a password). */
static int recvAllTimed(plat_sock_t s, void *buf, size_t len, int64_t deadline_ms)
{
    char *p = (char *)buf;
    size_t got = 0;
#ifdef _WIN32
    DWORD to = 250;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
#else
    struct timeval tv = {0, 250000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    while (got < len) {
        if (!g_running || steadyMs() > deadline_ms)
            return -1;
        const int n = (int)recv(s, p + got, (int)(len - got), 0);
        if (n > 0) { got += (size_t)n; continue; }
        if (n == 0) return -1;                    /* peer closed */
#ifdef _WIN32
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK || err == WSAEINTR)
            continue;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
#endif
        return -1;
    }
    return 0;
}

/* The device name is attacker-controlled: keep it printable and free of the
 * '|' that separates STAT fields (the GUI logs it verbatim). */
static std::string deviceNameFromWire(const HsRequestExtV3 &ext)
{
    std::string d;
    for (size_t i = 0; i < sizeof(ext.device); i++) {
        const unsigned char c = (unsigned char)ext.device[i];
        if (!c) break;
        d.push_back((c < 0x20 || c == 0x7f || c == '|') ? '?' : (char)c);
    }
    if (d.size() > 32) d.resize(32);
    return d;
}

/* Refusal path: HsResponseV2 only (the extension is sent on success).
 *
 * Invariant: every HsResponseV2 emitted BEFORE HsAuthStart must carry
 * TVHS_MAGIC + TVHS_VERSION. The sender classifies a first frame by magic and
 * version, so a current-version refusal is reported as its real status instead
 * of "old peer". Statuses used before the challenge: HS_ERR_DIMS (display-size
 * probe — a successful size answer, not an error), HS_ERR_VERSION,
 * HS_ERR_BUSY and HS_ERR_AUTH_CONFIG. Do not hand-build these responses. */
static int sendHsStatus(plat_sock_t s, uint32_t status, CodecId codec)
{
    HsResponseV2 r = {};
    int dw = 0, dh = 0;
    dispCacheGet(dw, dh);
    r.magic        = htonl(TVHS_MAGIC);
    r.version      = htonl(TVHS_VERSION);
    r.recv_width   = htonl((uint32_t)dw);
    r.recv_height  = htonl((uint32_t)dh);
    r.status       = htonl(status);
    r.codec        = htonl((uint32_t)codec);
    return send_all_p(s, &r, sizeof(r));
}

/* ══════════════════════════════════════════════════════════════════════════
 * HANDSHAKE WORKER  –  v3 authorization (runs off the main thread)
 * ══════════════════════════════════════════════════════════════════════════ */
static bool handshakeOne(plat_sock_t client)
{
    int sb = SOCK_BUF;
    setsockopt(client, SOL_SOCKET, SO_RCVBUF, (char *)&sb, sizeof(sb));

    struct sockaddr_in cli_addr = {};
    ACCEPT_LEN_T addr_len = sizeof(cli_addr);
    getpeername(client, (struct sockaddr *)&cli_addr, &addr_len);
    const std::string client_ip = ipv4Text(cli_addr.sin_addr);

    /* ── 1. HsRequestV2 (48 B) ── */
    HsRequestV2 wire = {};
    if (recvAllTimed(client, &wire, sizeof(wire), steadyMs() + 10000) < 0)
        return false;
    if (ntohl(wire.magic) != TVHS_MAGIC) {
        showLegacyPeerError();
        return false;
    }
    HsRequestV2 req = wire;       /* keep the wire bytes: the proof binds them */
    hsReqFromWire(req);

    /* ── 2. Display-size probe: no session, no authorization ──
     * It reveals only the display size and starts nothing. Accepted for any
     * protocol version so an older sender can still size its virtual display;
     * it can never reach the session path. */
    if (req.width == 0 && req.height == 0) {
        int dw = 0, dh = 0;
        dispCacheGet(dw, dh);
        /* Same status and dimensions as before, but built by the single helper
         * so the magic/version invariant cannot drift (issue 7). */
        if (sendHsStatus(client, HS_ERR_DIMS, CODEC_RAW) >= 0) {
            std::lock_guard<std::mutex> lk(g_stat_mtx);
            std::cout << "Display probe -> " << dw << "x" << dh << "\n";
        }
        return false;
    }

    /* A pre-v3 peer never reaches the authorization exchange. */
    if (req.version != TVHS_VERSION) {
        emitRxStat("error", "msg=version_mismatch");
        sendHsStatus(client, HS_ERR_VERSION, CODEC_RAW);
        return false;
    }

    const RxPolicy pol = currentPolicy();
    if (policyRefusesAll(pol)) {
        emitRxStat("error", "msg=auth_config");
        sendHsStatus(client, HS_ERR_AUTH_CONFIG, CODEC_RAW);
        return false;
    }

    /* A throttled source is refused BEFORE the challenge, so it cannot make
     * the receiver spend PBKDF2 work or learn the salt/iterations (issue 6).
     * The check is repeated after the identity frame as defence in depth. */
    if (pol.mode == MIVARA_AUTH_PASSWORD &&
        mivaraAuthThrottleRetryAfter(client_ip) > 0) {
        emitRxStat("error", "msg=auth_throttled");
        sendHsStatus(client, HS_ERR_AUTH, CODEC_RAW);
        return false;
    }

    /* ── 3. Cross-mode session slot (8081 video + 8084 stitch) ──
     * A cheap pre-check answers BUSY before the challenge. The slot itself is
     * claimed only after authorization succeeds — or just before a manual
     * prompt — so an unauthenticated peer cannot pin it for the whole
     * identity/proof budget and starve legitimate senders. The guard releases
     * the claim on every early return; it is handed to GrantedSession only on
     * success. */
    if (mivaraSessionOwner() != MIVARA_SESSION_NONE) {
        sendHsStatus(client, HS_ERR_BUSY, CODEC_RAW);
        return false;
    }
    SessionLeaseGuard lease_guard;

    /* ── 4. Challenge ──
     * The KDF parameters are only meaningful (and only disclosed) in password
     * mode; open/manual peers cannot use them. */
    HsAuthStart as = {};
    as.magic    = htonl(TVHA_MAGIC);
    as.version  = htonl(TVHA_VERSION);
    as.mode     = htonl((uint32_t)pol.mode);
    if (pol.mode == MIVARA_AUTH_PASSWORD) {
        as.kdf_iter = htonl(pol.iter);
        std::memcpy(as.salt, pol.salt, sizeof(as.salt));
    }
    if (!mivaraRandomBytes(as.nonce, sizeof(as.nonce))) {
        emitRxStat("error", "msg=auth_config");
        sendHsStatus(client, HS_ERR_AUTH_CONFIG, CODEC_RAW);
        return false;
    }
    if (send_all_p(client, &as, sizeof(as)) < 0)
        return false;

    /* ── 5. Sender identity ── */
    HsRequestExtV3 ext = {};
    if (recvAllTimed(client, &ext, sizeof(ext), steadyMs() + 15000) < 0) {
        /* A peer that opens a handshake and stalls is abuse; counting it here
         * (all modes) keeps repeated slot-grabbing from being free. */
        mivaraAuthThrottleNoteAbuse(client_ip);
        return false;
    }
    if (ntohl(ext.magic) != TVHX_MAGIC || ntohl(ext.version) != TVHA_VERSION) {
        mivaraAuthThrottleNoteAbuse(client_ip);
        sendHsStatus(client, HS_ERR_AUTH, CODEC_RAW);
        return false;
    }
    const std::string device = deviceNameFromWire(ext);

    /* ── 6. Policy enforcement ── */
    if (pol.mode == MIVARA_AUTH_PASSWORD) {
        /* Defence in depth: the pre-challenge check may have aged while the
         * sender was typing the password. */
        if (mivaraAuthThrottleRetryAfter(client_ip) > 0) {
            emitRxStat("error", "msg=auth_throttled");
            sendHsStatus(client, HS_ERR_AUTH, CODEC_RAW);
            return false;
        }
        HsAuthProof proof = {};
        /* Must stay below the 180 s claim TTL above: the sender's password
         * prompt allows 120 s, and the claim starts before the challenge. */
        if (recvAllTimed(client, &proof, sizeof(proof), steadyMs() + 150000) < 0) {
            mivaraAuthThrottleNoteAbuse(client_ip);
            return false;
        }
        if (ntohl(proof.magic) != TVHA_MAGIC ||
            ntohl(proof.version) != TVHA_VERSION) {
            mivaraAuthThrottleNoteAbuse(client_ip);
            sendHsStatus(client, HS_ERR_AUTH, CODEC_RAW);
            return false;
        }
        uint8_t bound[sizeof(HsRequestV2) + sizeof(HsRequestExtV3)];
        std::memcpy(bound, &wire, sizeof(wire));
        std::memcpy(bound + sizeof(wire), &ext, sizeof(ext));
        uint8_t expect[32];
        /* auth.h derives a purpose-separated key internally; the stored record
         * is never used directly as the MAC key. */
        mivaraAuthProof(pol.hash, MIVARA_AUTH_PURPOSE_VIDEO, as.nonce, bound,
                        sizeof(bound), expect);
        const bool proof_ok = mivaraConstEq(expect, proof.proof, 32);
        mivaraSecureZero(expect, sizeof(expect));
        if (!proof_ok) {
            mivaraAuthThrottleNoteFailure(client_ip);
            emitRxStat("error", "msg=auth_failed");
            sendHsStatus(client, HS_ERR_AUTH, CODEC_RAW);
            return false;
        }
        mivaraAuthThrottleNoteSuccess(client_ip);
    } else if (pol.mode == MIVARA_AUTH_MANUAL) {
        /* A source that keeps raising prompts (timeouts, hangups) is cooled
         * down before it can flash another dialog. */
        if (mivaraAuthThrottleRetryAfter(client_ip) > 0) {
            emitRxStat("error", "msg=auth_throttled");
            sendHsStatus(client, HS_ERR_DENIED, CODEC_RAW);
            return false;
        }
        /* Hold the slot across the prompt: a human decision must not be lost
         * when the other port finishes authorizing first. The 30 s dialog is
         * the only unauthenticated hold, and repeat offenders are throttled
         * above. */
        if (!lease_guard.acquire(MIVARA_SESSION_VIDEO, APPROVAL_TIMEOUT_MS + 15000)) {
            sendHsStatus(client, HS_ERR_BUSY, CODEC_RAW);
            return false;
        }
        const ApprovalResult ar =
            g_gui_mode ? waitManualApproval(client_ip, device, client)
                       : nativeManualPrompt(client_ip, device, client);
        if (ar != APPROVAL_ALLOW) {
            if (ar == APPROVAL_TIMEOUT) {
                mivaraAuthThrottleNoteAbuse(client_ip);
                emitRxStat("error", "msg=auth_timeout");
                sendHsStatus(client, HS_ERR_AUTH_TIMEOUT, CODEC_RAW);
            } else if (ar == APPROVAL_DENY) {
                mivaraAuthThrottleNoteFailure(client_ip);
                emitRxStat("error", "msg=auth_denied");
                sendHsStatus(client, HS_ERR_DENIED, CODEC_RAW);
            } else if (ar == APPROVAL_BUSY) {
                emitRxStat("error", "msg=peer_busy");
                sendHsStatus(client, HS_ERR_BUSY, CODEC_RAW);
            } else if (ar == APPROVAL_CANCELLED) {
                /* The peer hung up (or we are shutting down): do not write to
                 * a socket that is already gone (SIGPIPE on POSIX). */
                mivaraAuthThrottleNoteAbuse(client_ip);
                emitRxStat("error", "msg=auth_cancelled");
            } else {
                emitRxStat("error", "msg=auth_unavailable");
                if (!peerGone(client))
                    sendHsStatus(client, HS_ERR_AUTH, CODEC_RAW);
            }
            return false;
        }
    }

    /* Password/open have no human decision to protect: claim now, right before
     * the grant. Manual already claimed before its prompt. */
    if (!lease_guard.acquire(MIVARA_SESSION_VIDEO, 30000)) {
        sendHsStatus(client, HS_ERR_BUSY, CODEC_RAW);
        return false;
    }
    if (!mivaraSessionPromote(lease_guard.lease)) {
        sendHsStatus(client, HS_ERR_BUSY, CODEC_RAW);
        return false;
    }

    /* ── 7. Stream parameters + decoder (reject, never downgrade) ── */
    uint32_t hs_status = HS_OK;
    const int w = (int)req.width, h = (int)req.height, fps = (int)req.fps;
    if (w <= 0 || w > 7680 || h <= 0 || h > 4320 || fps <= 0 || fps > 240)
        hs_status = HS_ERR_DIMS;
    else if (req.codec > (uint32_t)CODEC_AV1)
        hs_status = HS_ERR_CODEC;

    CodecId sess_codec = CODEC_RAW;
    std::unique_ptr<Decoder> decoder(new Decoder());
    if (hs_status == HS_OK) {
        sess_codec = (CodecId)req.codec;
        if (req.codec >= (uint32_t)CODEC_H264) {
            const std::string derr = decoder->open(sess_codec, w, h);
            if (!derr.empty()) {
                std::lock_guard<std::mutex> lk(g_stat_mtx);
                std::cerr << "Decoder open failed: " << derr << "\n";
                hs_status = HS_ERR_DECODER;
            }
        }
    }
    if (hs_status != HS_OK) {
        emitRxStat("error",
                   hs_status == HS_ERR_DIMS    ? "msg=dims_rejected"
                   : hs_status == HS_ERR_CODEC ? "msg=codec_rejected"
                                               : "msg=decoder_unavailable");
        sendHsStatus(client, hs_status, sess_codec);
        return false;
    }

    /* ── 8. Grant: install the token, then answer ──
     * The slot was claimed and promoted right after authorization (see 6b/7).
     * The token is installed BEFORE the first success byte is sent, so a
     * sender that dials 8082/8083 immediately after copying it cannot race the
     * gate (issue 9). Any failure rolls the token back; the lease guard
     * releases the cross-mode slot. */
    uint8_t token[HS_TOKEN_BYTES] = {};
    if (!mivaraRandomBytes(token, sizeof(token))) {
        emitRxStat("error", "msg=auth_config");
        sendHsStatus(client, HS_ERR_AUTH_CONFIG, sess_codec);
        return false;
    }
    mivaraSessionTokenInstall(token);

    bool sent = (sendHsStatus(client, HS_OK, sess_codec) == 0);
    if (sent) {
        HsResponseExtV3 ok = {};
        ok.magic   = htonl(TVHR_MAGIC);
        ok.version = htonl(TVHA_VERSION);
        std::memcpy(ok.token, token, sizeof(ok.token));
        sent = (send_all_p(client, &ok, sizeof(ok)) == 0);
    }
    if (!sent) {
        mivaraSessionTokenClearIf(token);
        mivaraSecureZero(token, sizeof(token));
        return false;
    }

    g_gpu_stats.recordClient(client_ip);

    {
        std::lock_guard<std::mutex> lk(g_stat_mtx);
        std::cout << "Sender: " << w << "x" << h << " @ " << fps
                  << " FPS  mode="
                  << (req.mode == MODE_TRUE_EXTEND ? "extend" : "mirror")
                  << "  codec=" << codecToString(sess_codec)
                  << "  device=" << device << "  ip=" << client_ip << "\n";
    }

    GrantedSession s;
    s.sock    = client;
    s.ip      = client_ip;
    s.device  = device;
    s.req     = req;
    s.decoder = std::move(decoder);
    s.codec   = sess_codec;
    s.lease   = lease_guard.lease;
    std::memcpy(s.token, token, sizeof(s.token));

    {
        std::lock_guard<std::mutex> lk(g_sess_mtx);
        if (g_sess_q.size() >= 4 || !g_running) {
            mivaraSessionTokenClearIf(token);
            mivaraSecureZero(token, sizeof(token));
            return false;
        }
        g_sess_q.push_back(std::move(s));
        /* Ownership of the slot moved into the queued session. */
        lease_guard.dismiss();
    }
    mivaraSecureZero(token, sizeof(token));
    g_sess_cv.notify_all();
    return true;
}

/* Fixed worker pool: handshakes may block on a human decision, but the
 * listener must keep accepting so a second sender immediately learns the
 * receiver is busy instead of waiting in the backlog. */
static void handshakeWorkerLoop()
{
    for (;;) {
        plat_sock_t c = PLAT_INVALID;
        {
            std::unique_lock<std::mutex> lk(g_sess_mtx);
            g_sess_cv.wait(lk, [] {
                return !g_running || !g_handshake_q.empty();
            });
            if (g_handshake_q.empty()) {
                if (!g_running) return;
                continue;
            }
            c = g_handshake_q.front();
            g_handshake_q.pop_front();
        }
        if (!handshakeOne(c))
            PLAT_CLOSE(c);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * SESSION  –  runs on the main thread (SDL requirement)
 * ══════════════════════════════════════════════════════════════════════════ */
static void setActiveClient(plat_sock_t s)
{
    std::lock_guard<std::mutex> lk(g_active_mtx);
    g_active_client = s;
    g_disconnect_req = false;
}

static void clearActiveClient()
{
    std::lock_guard<std::mutex> lk(g_active_mtx);
    g_active_client = PLAT_INVALID;
    g_disconnect_req = false;
}

/* Called from the stdin thread: unblock the session's recv(). Only shutdown()
 * is used (never close) so the main thread keeps sole ownership of the
 * handle and a reused descriptor can never be affected. */
static void requestSessionDisconnect()
{
    std::lock_guard<std::mutex> lk(g_active_mtx);
    g_disconnect_req = true;
    if (PLAT_VALID(g_active_client)) {
#ifdef _WIN32
        shutdown(g_active_client, SD_BOTH);
#else
        shutdown(g_active_client, SHUT_RDWR);
#endif
    }
}

static bool runSession(GrantedSession &sess)
{
    plat_sock_t client = sess.sock;
    const std::string client_ip = sess.ip;
    Decoder &decoder = *sess.decoder;
    CodecId sess_codec = sess.codec;

    int sb = SOCK_BUF;
    setsockopt(client, SOL_SOCKET, SO_RCVBUF, (char *)&sb, sizeof(sb));
#ifdef _WIN32
    DWORD to = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
#else
    struct timeval tv = {10, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    SCREEN_WIDTH = (int)sess.req.width;
    SCREEN_HEIGHT = (int)sess.req.height;
    TARGET_FPS = (int)sess.req.fps;
    /* 拼接 is a separate KVM channel (port 8084, kvm.cpp) and never travels
     * on 8081; anything but the documented extend value is a mirror session. */
    DISPLAY_MODE = (sess.req.mode == MODE_TRUE_EXTEND) ? MODE_TRUE_EXTEND
                                                       : MODE_MIRROR;
    const char *modeStr =
        DISPLAY_MODE == MODE_TRUE_EXTEND ? "extend" : "mirror";

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
     * Both modes default to borderless fullscreen on the receiver's own
     * display. SDL_WINDOW_FULLSCREEN_DESKTOP covers the entire display
     * without changing the video mode — cross-platform and safe — and F11
     * drops either mode back to a normal resizable window.
     *
     * EXTEND: the window looks like a real second monitor.
     * MIRROR: created windowed at a display-fitting size first, then made
     *   fullscreen, so F11 has a sensible window size to restore to.
     */
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;

    if (DISPLAY_MODE == MODE_TRUE_EXTEND)
    {
        /* Borderless fullscreen — looks like a real second monitor */
        win = SDL_CreateWindow(
            "Mivara Extended Display",
            SDL_WINDOWPOS_UNDEFINED_DISPLAY(0),
            SDL_WINDOWPOS_UNDEFINED_DISPLAY(0),
            MY_DISPLAY_WIDTH, MY_DISPLAY_HEIGHT,
            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    else
    {
        /* Mirror: create at a fitted window size, then cover the display */
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
            "Mivara Mirror",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            ww, wh,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                SDL_WINDOW_FULLSCREEN_DESKTOP);
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

    /* Note: no SDL_RenderSetLogicalSize(). The render loop below aspect-fits
     * the texture into the real output rectangle, so a stream whose aspect
     * ratio differs from this display gets black bars instead of being
     * stretched. */

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

    setActiveClient(client);
    emitRxStat("start",
               std::string("codec=") + codecToString(sess_codec) +
                   "|mode=" + modeStr +
                   "|src=" + std::to_string(SCREEN_WIDTH) + "x" +
                   std::to_string(SCREEN_HEIGHT) +
                   "|dst=" + std::to_string(MY_DISPLAY_WIDTH) + "x" +
                   std::to_string(MY_DISPLAY_HEIGHT) +
                   "|dev=" + sess.device + "|ip=" + client_ip);

    while (streaming && g_running)
    {
        /* The GUI can end this session (RX|disconnect) while the receiver
         * keeps waiting for the next sender. */
        if (g_disconnect_req) {
            streaming = false;
            break;
        }

        /* Event pump */
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
            {
                /* Closing the preview window ends the session, not the
                 * receive service: the receiver keeps listening. */
                streaming = false;
            }
            else if (ev.type == SDL_KEYDOWN)
            {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE || k == SDLK_q)
                    streaming = false;
                /* F11: toggle fullscreen (both modes; mirror is fullscreen
                 * by default but can be dropped back to a window) */
                if (k == SDLK_F11)
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

        /* TCP may return a partial header; never parse uninitialised bytes. */
        uint32_t net_sz = 0;
        int n = 0;
        if (recv_all(client, &net_sz, sizeof(net_sz)) < 0)
        {
            std::cout << "Sender disconnected or sent an incomplete frame header\n";
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
            const bool was_compressed = sess_codec == CODEC_RLE && frame_sz < full_frame;
            if ((sess_codec == CODEC_RAW && frame_sz != full_frame) ||
                (sess_codec == CODEC_RLE && frame_sz > full_frame))
            {
                std::cerr << "Raw frame length does not match the negotiated dimensions\n";
                emitRxStat("error", "msg=bad_raw_frame");
                break;
            }
            if (was_compressed)
            {
                std::vector<uint8_t> decbuf;
                decbuf.reserve(full_frame);
                bool valid = frame_sz % 4 == 0;
                size_t off = 0;
                while (valid && off < frame.size())
                {
                    const uint8_t run = frame[off];
                    if (run == 0 || decbuf.size() + (size_t)run * 3 > full_frame)
                    {
                        valid = false;
                        break;
                    }
                    for (uint8_t j = 0; j < run; j++)
                    {
                        decbuf.push_back(frame[off + 1]);
                        decbuf.push_back(frame[off + 2]);
                        decbuf.push_back(frame[off + 3]);
                    }
                    off += 4;
                }
                if (!valid || decbuf.size() != full_frame)
                {
                    std::cerr << "Invalid or incomplete RLE frame\n";
                    emitRxStat("error", "msg=bad_rle_frame");
                    break;
                }
                frame = std::move(decbuf);
            }

            if (!ensureTex(SDL_PIXELFORMAT_RGB24, SCREEN_WIDTH, SCREEN_HEIGHT))
                break;
            SDL_UpdateTexture(tex, nullptr, frame.data(),
                              SCREEN_WIDTH * BYTES_PER_PIXEL);
        }

        /* Render: preserve the stream's aspect ratio. A full-output blit
         * stretches the picture whenever the sender's virtual-display mode
         * and this panel differ (e.g. a 16:9 mode on a 16:10 laptop), which
         * is exactly the reported "stretched extended screen". Fit the
         * stream inside the output and centre it; the black clear supplies
         * the bars. When the aspects match, this fills the screen as before.
         * Use the stream's logical size (SCREEN_*) rather than the texture
         * size so decoder row padding cannot skew the ratio. */
        int out_w = 0, out_h = 0;
        if (SDL_GetRendererOutputSize(ren, &out_w, &out_h) != 0 ||
            out_w <= 0 || out_h <= 0)
        {
            out_w = MY_DISPLAY_WIDTH;
            out_h = MY_DISPLAY_HEIGHT;
        }
        int src_w = SCREEN_WIDTH, src_h = SCREEN_HEIGHT;
        if (src_w <= 0 || src_h <= 0) { src_w = tex_w; src_h = tex_h; }

        SDL_Rect dst = { 0, 0, out_w, out_h };
        if (src_w > 0 && src_h > 0)
        {
            double sc = std::min((double)out_w / (double)src_w,
                                 (double)out_h / (double)src_h);
            int dw = (int)((double)src_w * sc + 0.5);
            int dh = (int)((double)src_h * sc + 0.5);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            if (dw > out_w) dw = out_w;
            if (dh > out_h) dh = out_h;
            dst.x = (out_w - dw) / 2;
            dst.y = (out_h - dh) / 2;
            dst.w = dw;
            dst.h = dh;
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (tex)
            SDL_RenderCopy(ren, tex, nullptr, &dst);

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

    clearActiveClient();
    emitRxStat("stop", "frames=" + std::to_string(frames_rx) +
                           "|t=" + std::to_string(secs) +
                           "|codec=" + codecToString(sess_codec) +
                           "|src=" + std::to_string(SCREEN_WIDTH) + "x" +
                           std::to_string(SCREEN_HEIGHT) +
                           "|dev=" + sess.device + "|ip=" + client_ip);

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
/* Token gate for the sidecar services (8082/8083). They run in this process,
 * so a plain C callback is enough — no extra networking or shared file. */
extern "C" int rxTokenCheck(const uint8_t *token, uint32_t len)
{
    return len == HS_TOKEN_BYTES && mivaraSessionTokenCheck(token);
}

/* GUI protocol on stdin (the app owns the pipe). Kept in one place so a
 * malformed line can never fall through to another parser. */
static void receiverStdinLoop()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("AUTH|", 0) == 0) {
            /* AUTH|id=N|allow  /  AUTH|id=N|deny */
            uint32_t id = 0;
            size_t p = 5;
            if (line.compare(p, 3, "id=") == 0) {
                p += 3;
                size_t bar = line.find('|', p);
                const std::string ids = line.substr(p, bar == std::string::npos
                                                           ? std::string::npos
                                                           : bar - p);
                char *end = nullptr;
                const unsigned long v = std::strtoul(ids.c_str(), &end, 10);
                if (!ids.empty() && end && *end == '\0' && v <= 0xFFFFFFFFul)
                    id = (uint32_t)v;
                if (bar != std::string::npos) {
                    const std::string act = line.substr(bar + 1);
                    if (act == "allow") resolveApproval(id, true);
                    else if (act == "deny") resolveApproval(id, false);
                }
            }
        } else if (line == "RX|disconnect") {
            requestSessionDisconnect();
        } else if (line == "RX|reload") {
            /* Settings were saved: reload 接收与安全 without dropping a
             * running session. */
            reloadPolicyFromConfig();
        } else if (line == "RX|quit" || line == "STOP|mivara") {
            /* STOP|mivara is the shared owner-stop line: the app uses it for
             * both children so a stop never depends on console control
             * events (Ctrl+C is not delivered to these child consoles). */
            cancelAllApprovals();
            g_running = false;
            g_sess_cv.notify_all();
        } else if (line == "PING") {
            emitRxStat("pong", "");
        }
    }
    /* EOF: the GUI is gone. Never linger as an orphan with the ports open. */
    cancelAllApprovals();
    g_running = false;
    g_sess_cv.notify_all();
}

static void dispatcherLoop(plat_sock_t srv)
{
    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        struct timeval tv = {1, 0};
        const int act = select((int)srv + 1, &fds, nullptr, nullptr, &tv);
        if (act <= 0 || !g_running)
            continue;

        struct sockaddr_in cli = {};
        ACCEPT_LEN_T cli_len = sizeof(cli);
        plat_sock_t client = accept(srv, (struct sockaddr *)&cli,
                                    (ACCEPT_LEN_T *)&cli_len);
        if (!PLAT_VALID(client))
            continue;

        {
            std::lock_guard<std::mutex> lk(g_sess_mtx);
            if (g_handshake_q.size() >= 16) {
                PLAT_CLOSE(client);     /* overload: drop, never queue forever */
                continue;
            }
            g_handshake_q.push_back(client);
        }
        g_sess_cv.notify_all();
    }
}

/* ── sidecar service startup report (8082/8083/8084) ───────────────────────
 * Each service thread publishes its real listener state (ports.h /
 * gpu_accelerate.h / kvm.h). `state` 0 means it never finished starting while
 * we waited; report that as fail instead of pretending it is available. */
static const char *svcStateKey(int state)
{
    if (state == 1) return "ok";
    if (state == 2) return "na";
    return "fail";      /* -1 failed, 0 still starting when we gave up */
}

static const char *svcStageKey(int state, int stage)
{
    if (state == 2) return "unsupported";
    switch (stage) {
    case 1: return "socket";
    case 2: return "winsock";
    case 3: return "bind";
    case 4: return "listen";
    default: return "none";
    }
}

static void emitServiceStatus(const char *svc, uint16_t port, int state,
                              int stage, int err)
{
    emitRxStat("svc", std::string("svc=") + svc + "|port=" +
                           std::to_string((int)port) + "|state=" +
                           svcStateKey(state) + "|stage=" +
                           svcStageKey(state, stage) + "|err=" +
                           std::to_string(err));
}

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

    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--gui") == 0)
            g_gui_mode = true;

    queryMyDisplaySize();

    std::cout << "========================================\n"
              << "         Mivara RECEIVER v" MIVARA_VERSION "     \n"
              << "========================================\n"
              << "  Local IP   : " << getLocalIPAddress() << "\n"
              << "  My display : " << MY_DISPLAY_WIDTH
              << "x" << MY_DISPLAY_HEIGHT << "\n"
              << "  Stream TCP : " << TCP_STREAM_PORT << "\n"
              << "  Compute TCP: " << GPU_ACCEL_PORT << "\n"
              << "  Ports TCP  : " << PORTS_SERVICE_PORT << "\n"
              << "  Stitch TCP : " << KVM_PORT << "\n"
              << "  SSDP UDP   : " << SSDP_ADDR << ":" << SSDP_PORT << "\n"
              << "  GPU Stats  : " << GPU_STATS_FILE << "\n"
              << "  Modes      : extend | mirror | stitch (TCP " << KVM_PORT << ")\n"
              << "========================================\n";

    /* Connection policy from config.ini (never argv: see auth.h). The
     * auth_config error is deferred until after ready (issue 8). */
    applyPolicy(loadPolicyFromConfig(), false);

    if (!initSockets())
    {
        std::cerr << "Socket init failed\n";
        emitRxStat("error", "msg=socket_init_failed");
        return 1;
    }

    /* 8082/8083 only serve a session that completed the 8081 handshake. */
    gpu_service_set_token_check(rxTokenCheck);
    ports_service_set_token_check(rxTokenCheck);

    std::thread gpu_thread(gpuServiceThread);
    std::thread port_thread(portServiceThread);
    std::thread ssdp_thread(ssdpAdvertisementThread);
    std::thread stats_thread(gpuStatsWriterThread);
    /* 拼接 (stitch) input channel. It is independent of the video listener:
     * the receiver can be idle (no window) and still hand control to a peer.
     * The thread polls g_running, so it joins on every shutdown path below. */
    std::thread kvm_thread(kvmServiceRun, std::ref(g_running), &g_kvm_auth);

    /* TCP stream server */
    plat_sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (!PLAT_VALID(srv))
    {
        std::cerr << "Server socket failed\n";
        emitRxStat("error", "msg=socket_init_failed");
        g_running = false;
        g_sess_cv.notify_all();
        ssdp_thread.join();
        gpu_thread.join();
        port_thread.join();
        stats_thread.join();
        kvm_thread.join();
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

    bool bound = (bind(srv, (struct sockaddr *)&saddr, sizeof(saddr)) >= 0);
    if (bound) bound = (listen(srv, 8) >= 0);
    if (!bound)
    {
        std::cerr << "Bind/listen failed on " << TCP_STREAM_PORT << "\n";
        emitRxStat("error", "msg=bind_failed");
        PLAT_CLOSE(srv);
        g_running = false;
        g_sess_cv.notify_all();
        ssdp_thread.join();
        gpu_thread.join();
        port_thread.join();
        stats_thread.join();
        kvm_thread.join();
        cleanupSockets();
        return 1;
    }
    /* Report the real 8082/8083/8084 listener state before ready: a missing
     * sidecar must not be hidden behind a generic "waiting for connection"
     * (issue 13). The threads were started above; give them a bounded time to
     * settle instead of polling forever. */
    {
        gpu_startup_status gpu_st = {};
        ports_startup_status ports_st = {};
        KvmStartupStatus kvm_st = {};
        const int64_t svc_deadline = steadyMs() + 1500;
        for (;;) {
            gpu_st = gpu_service_status();
            ports_st = ports_service_status();
            kvm_st = kvmServiceStatus();
            const bool settled = gpu_st.state != 0 && ports_st.state != 0 &&
                                 kvm_st.state != 0;
            if (settled || steadyMs() >= svc_deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        emitServiceStatus("gpu", GPU_ACCEL_PORT, gpu_st.state,
                          gpu_st.stage, gpu_st.error_code);
        emitServiceStatus("ports", PORTS_SERVICE_PORT, ports_st.state,
                          ports_st.stage, ports_st.error_code);
        emitServiceStatus("kvm", KVM_PORT, kvm_st.state,
                          kvm_st.stage, kvm_st.error_code);
    }

    std::cout << "Waiting for sender on TCP " << TCP_STREAM_PORT << " ...\n";
    {
        const RxPolicy pol = currentPolicy();
        const bool bad_record =
            pol.mode == MIVARA_AUTH_PASSWORD && !pol.record_ok;
        emitRxStat("ready", std::string("port=") +
                                std::to_string(TCP_STREAM_PORT) +
                                "|policy=" + policyKey(pol) +
                                (bad_record ? "|record=bad|state=error" : ""));
        /* Keep the fatal error as the LAST line: a corrupt record leaves the
         * service listening but every password connection fails closed. */
        if (bad_record)
            emitRxStat("error", "msg=auth_config");
    }

    std::thread dispatcher(dispatcherLoop, srv);
    std::vector<std::thread> workers;
    for (int i = 0; i < RX_WORKERS; i++)
        workers.emplace_back(handshakeWorkerLoop);

    if (g_gui_mode)
        std::thread(receiverStdinLoop).detach();

    /* Session loop (main thread — SDL video must live here). A granted
     * session is handed over by a handshake worker through g_sess_q. */
    while (g_running) {
        GrantedSession sess;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(g_sess_mtx);
            g_sess_cv.wait_for(lk, std::chrono::milliseconds(250), [] {
                return !g_running || !g_sess_q.empty();
            });
            if (!g_sess_q.empty()) {
                sess = std::move(g_sess_q.front());
                g_sess_q.pop_front();
                have = true;
            }
        }
        if (!have) {
            /* Keep the display-size probe honest while the receiver is idle:
             * a monitor change or dock replug must not leave the sender
             * sizing its virtual display from a stale cache. SDL stays on
             * this thread; once per second is enough and cheap. */
            static int64_t last_disp = 0;
            const int64_t now = steadyMs();
            if (now - last_disp >= 1000) {
                queryMyDisplaySize();
                last_disp = now;
            }
            continue;
        }

        queryMyDisplaySize();       /* hot-plug / mode change between sessions */
        runSession(sess);
        PLAT_CLOSE(sess.sock);
        /* Clear only this session's token: an ending session must not wipe a
         * token that a newly granted session already installed. The slot is
         * released after the token so a fresh claim cannot interleave. */
        mivaraSessionTokenClearIf(sess.token);
        mivaraSessionRelease(sess.lease);
        /* The cache must reflect the mode after the session's SDL_Quit. */
        queryMyDisplaySize();
        emitRxStat("waiting", "policy=" + std::string(policyKey(currentPolicy())));
    }

    cancelAllApprovals();
    PLAT_CLOSE(srv);
    g_running = false; /* signal threads before joining */
    g_sess_cv.notify_all();
    dispatcher.join();
    for (auto &w : workers) w.join();
    {
        /* Sessions granted but never picked up by the main loop: close their
         * sockets and release the cross-mode slots before shutdown. */
        std::lock_guard<std::mutex> lk(g_sess_mtx);
        while (!g_sess_q.empty()) {
            GrantedSession &qs = g_sess_q.front();
            if (PLAT_VALID(qs.sock)) PLAT_CLOSE(qs.sock);
            mivaraSessionRelease(qs.lease);
            g_sess_q.pop_front();
        }
    }
    /* All handshake workers have joined, so no installation can race this. */
    clearSessionToken();
    ssdp_thread.join();
    gpu_thread.join();
    port_thread.join();
    stats_thread.join();
    kvm_thread.join();
    cleanupSockets();

    // Final stats write
    g_gpu_stats.writeStatsToFile();
    g_gpu_stats.printStats();

    std::cout << "Receiver shut down\n";
    return 0;
}
