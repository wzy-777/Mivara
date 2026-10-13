/**
 * APP.CPP – Mivara GUI LAUNCHER (SDL2 + SDL2_ttf)
 *
 * Layout: fixed left nav sidebar (主页 / 设置 + 退出) with the active
 * tab's content to its right. Session flows (search / stream / receive) and
 * the connection-history list live inside the 主页 tab and remain reachable
 * via "返回会话" while browsing 设置.
 *
 * Screens:
 *   HOME       – 接收 / 发送 cards + connection-history rows below
 *   RECEIVING  – "正在接收" + local IP / ports; spawns bin/receiver
 *   SEARCHING  – "搜索中" (SSDP discovery on a background thread)
 *   FOUND      – machine list, each row offers 投屏 (mirror) / 扩展 (extend)
 *   NOT_FOUND  – 继续搜索 / 手动输入ip / 退出
 *   MANUAL_IP  – type a receiver IP, then the same mode choice
 *   STREAMING  – session running (bin/sender …) + live STAT panel
 *   SETTINGS   – AppConfig editor (codec / hw / quality / fps / bitrate / …)
 *
 * Children run either with their own console (show_status_console=1) or
 * hidden with stdout+stderr captured into a log ring that feeds a second
 * "Mivara 日志" window; STAT| lines feed the in-app status panel.
 *
 * Stopping the sender is always graceful (Ctrl+C / SIGINT) so vddShutdown()
 * detaches the virtual display – never TerminateProcess as the first resort.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "discover.h"
#include "history.h"
#include "vdd.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <limits.h>
#endif

#define WIN_W 820
#define WIN_H 700
#define LOG_WIN_W 420
#define LOG_WIN_H 520
#define LOG_CAP 2000
#define HIST_VISIBLE 5
#define HEADER_H 64
#define NAV_W 140
#define CONTENT_X NAV_W
#define CONTENT_W (WIN_W - NAV_W)
#define CONTENT_CX (CONTENT_X + CONTENT_W / 2)

/* ── theme ───────────────────────────────────────────────────────────────── */
struct Theme {
    SDL_Color bg, surface, hover, press, primary, success, danger;
    SDL_Color text, textDim, inputBg, border;
    int gap, rowH, radius;
};

static const Theme THEME = {
    /* bg      */ {22, 22, 34, 255},
    /* surface */ {32, 34, 52, 255},
    /* hover   */ {48, 52, 76, 255},
    /* press   */ {36, 40, 60, 255},
    /* primary */ {46, 91, 171, 255},
    /* success */ {34, 139, 84, 255},
    /* danger  */ {160, 54, 60, 255},
    /* text    */ {230, 232, 240, 255},
    /* textDim */ {150, 155, 175, 255},
    /* inputBg */ {18, 18, 28, 255},
    /* border  */ {58, 62, 88, 255},
    /* gap/rowH/radius */ 12, 52, 8
};

/* ── screens ─────────────────────────────────────────────────────────────── */
enum Screen {
    SCR_HOME,
    SCR_RECEIVING,
    SCR_SEARCHING,
    SCR_FOUND,
    SCR_NOT_FOUND,
    SCR_MANUAL_IP,
    SCR_STREAMING,
    SCR_SETTINGS,
};

/* Left-nav tabs. SCR_SETTINGS maps to its tab; every other screen belongs
 * to NAV_HOME (session flow + connection history live inside the home tab). */
enum NavTab { NAV_HOME, NAV_SETTINGS };

/* ── button ids ──────────────────────────────────────────────────────────── */
enum BtnId {
    BTN_NONE = -1,
    /* home */
    BTN_HOME_RECEIVE = 100,
    BTN_HOME_SEND,
    BTN_HOME_RECEIVE_STOP,
    BTN_HOME_SENDER_STOP,
    /* receiving */
    BTN_RECV_STOP = 200,
    BTN_RECV_LOG,
    /* searching */
    BTN_SEARCH_CANCEL = 300,
    /* found */
    BTN_FOUND_RESEARCH = 400,
    BTN_FOUND_MANUAL,
    /* not found */
    BTN_NF_CONTINUE = 500,
    BTN_NF_MANUAL,
    BTN_NF_QUIT,
    /* manual ip */
    BTN_MANUAL_CONNECT = 600,
    BTN_MANUAL_BACK,
    /* streaming */
    BTN_STREAM_STOP = 700,
    BTN_STREAM_LOG,
    /* settings */
    BTN_SET_CODEC_BASE = 7000,          /* +0..5 */
    BTN_SET_HW = 7010,
    BTN_SET_QUALITY_DEC,
    BTN_SET_QUALITY_INC,
    BTN_SET_FPS_DEC,
    BTN_SET_FPS_INC,
    BTN_SET_BITRATE_DEC,
    BTN_SET_BITRATE_INC,
    BTN_SET_SCALE_BASE = 7020,          /* +0..2 */
    BTN_SET_CHROMA_BASE = 7030,         /* +0..1 */
    BTN_SET_CONSOLE = 7040,
    BTN_SET_LOGWIN,
    BTN_SET_OK = 7050,
    BTN_SET_RESET,
    BTN_SET_AUTOSTART = 7053,
    BTN_SET_AUTORECV,
    /* history rows (drawn on the home screen under 接收/发送) */
    BTN_HIST_BASE = 5000,               /* row i: +i*10 +0 row / +1 投屏 / +2 扩展 / +3 删除 */
    BTN_HIST_CLEAR = 5300,
    /* device rows on FOUND */
    BTN_DEV_BASE = 1000,                /* +i*10 +1 投屏 / +2 扩展 */
    /* left nav sidebar */
    BTN_NAV_HOME = 8000,
    BTN_NAV_SETTINGS,
    BTN_NAV_QUIT,
    BTN_RETURN_SESSION = 8010,
};

struct Button {
    SDL_Rect rect;
    std::string label;
    int id;
    SDL_Color color;
};

struct ChildProc {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    HANDLE stdout_read = nullptr;
    HANDLE stdin_write = nullptr;   /* SETP|… runtime reconfigure channel */
#else
    pid_t pid = -1;
    int stdout_fd = -1;
    int stdin_fd = -1;
#endif
    bool active = false;
    std::thread reader;
};

/* Last-wins decoded STAT| payload. */
struct LiveStats {
    bool valid = false;
    std::string event, msg;
    std::string fps, kbps, codec, hw, mode, src, dst, quality, frames, t;
};

/* ── globals ─────────────────────────────────────────────────────────────── */
static SDL_Window   *g_win   = nullptr;
static SDL_Renderer *g_ren   = nullptr;
static TTF_Font     *g_font  = nullptr;   /* body / buttons */
static TTF_Font     *g_fontL = nullptr;   /* headings */
static TTF_Font     *g_fontS = nullptr;   /* log lines / stat panel */

static SDL_Window   *g_log_win = nullptr;
static SDL_Renderer *g_log_ren = nullptr;
static bool          g_log_open = false;
static std::atomic<int> g_log_scroll{0};  /* lines scrolled up from bottom */

static Screen                g_screen = SCR_HOME;
static Screen                g_session_screen = SCR_HOME; /* home-flow screen when browsing other tabs */
static std::vector<Button>   g_buttons;
static std::string           g_manual_ip;
static std::string           g_status_note;
    static std::string           g_child_label;          /* active sender label */
    static bool                  g_child_extend = false;

static std::vector<DiscoveredDevice> g_devices;
static std::mutex                  g_dev_mtx;
static std::atomic<bool>           g_searching{false};
static std::atomic<bool>           g_search_done{false};
static std::atomic<int>            g_found_count{0};
static std::atomic<unsigned long>  g_search_generation{0};
static std::thread                  g_search_thread;

static AppConfig g_cfg;                 /* live config */
static AppConfig g_cfg_edit;            /* SCR_SETTINGS working copy */
static AppConfig g_live_params;         /* params the running sender holds */
static std::string g_last_ip, g_last_name;
static int  g_last_port = 8081;
static std::vector<HistEntry> g_hist;
static int g_hist_scroll = 0;

static std::deque<std::string> g_log_lines;
static std::mutex              g_log_mtx;
static LiveStats               g_stats;
static std::mutex              g_stats_mtx;

static int g_hot = BTN_NONE;
static int g_press = BTN_NONE;

static ChildProc g_sender;
static ChildProc g_receiver;

/* ── paths & fonts ───────────────────────────────────────────────────────── */
static std::string exeDir()
{
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
#else
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        std::string p(buf);
        size_t s = p.find_last_of('/');
        return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
    }
    return ".";
#endif
}

static TTF_Font *openFont(int size)
{
    static const char *candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",       /* Microsoft YaHei       */
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",     /* SimHei                */
        "C:/Windows/Fonts/simsun.ttc",     /* SimSun                */
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        nullptr
    };
    for (int i = 0; candidates[i]; i++) {
        TTF_Font *f = TTF_OpenFont(candidates[i], size);
        if (f) return f;
    }
    return nullptr;
}

/* ── text / shape drawing ────────────────────────────────────────────────── */
static void drawText(SDL_Renderer *ren, const std::string &s, int x, int y,
                     SDL_Color c, TTF_Font *font, bool center = false)
{
    if (!ren || !font || s.empty()) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst{x, y, surf->w, surf->h};
    if (center) dst.x = x - surf->w / 2;
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_RenderCopy(ren, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
}

static void fillCircle(SDL_Renderer *ren, int cx, int cy, int rad)
{
    if (rad <= 0) return;
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = (int)std::sqrt((float)(rad * rad - dy * dy));
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void fillRoundRect(SDL_Renderer *ren, SDL_Rect rc, int rad, SDL_Color c)
{
    if (rc.w <= 0 || rc.h <= 0) return;
    if (rad > rc.w / 2) rad = rc.w / 2;
    if (rad > rc.h / 2) rad = rc.h / 2;
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_Rect mid = rc;
    mid.x += rad;
    mid.w -= rad * 2;
    SDL_RenderFillRect(ren, &mid);
    SDL_Rect left{rc.x, rc.y + rad, rad, rc.h - rad * 2};
    SDL_Rect right{rc.x + rc.w - rad, rc.y + rad, rad, rc.h - rad * 2};
    SDL_RenderFillRect(ren, &left);
    SDL_RenderFillRect(ren, &right);
    fillCircle(ren, rc.x + rad, rc.y + rad, rad);
    fillCircle(ren, rc.x + rc.w - 1 - rad, rc.y + rad, rad);
    fillCircle(ren, rc.x + rad, rc.y + rc.h - 1 - rad, rad);
    fillCircle(ren, rc.x + rc.w - 1 - rad, rc.y + rc.h - 1 - rad, rad);
}

static void drawRoundRect(SDL_Renderer *ren, SDL_Rect rc, int rad,
                          SDL_Color fill, SDL_Color border)
{
    fillRoundRect(ren, rc, rad, border);
    SDL_Rect in{rc.x + 1, rc.y + 1, rc.w - 2, rc.h - 2};
    if (in.w > 0 && in.h > 0)
        fillRoundRect(ren, in, rad > 0 ? rad - 1 : 0, fill);
}

static void drawButton(SDL_Renderer *ren, const Button &b)
{
    if (b.label.empty()) return;           /* ghost hit-area, custom-drawn */
    SDL_Color c = b.color;
    if (g_press == b.id) {
        c.r = (Uint8)std::max(0, (int)c.r - 26);
        c.g = (Uint8)std::max(0, (int)c.g - 26);
        c.b = (Uint8)std::max(0, (int)c.b - 26);
    } else if (g_hot == b.id) {
        c.r = (Uint8)std::min(255, (int)c.r + 22);
        c.g = (Uint8)std::min(255, (int)c.g + 22);
        c.b = (Uint8)std::min(255, (int)c.b + 22);
    }
    drawRoundRect(ren, b.rect, THEME.radius, c, THEME.border);
    int tw = 0, th = 0;
    if (g_font) TTF_SizeUTF8(g_font, b.label.c_str(), &tw, &th);
    drawText(ren, b.label, b.rect.x + b.rect.w / 2,
             b.rect.y + (b.rect.h - th) / 2, THEME.text, g_font, true);
}

static void addButton(int x, int y, int w, int h,
                      const std::string &label, int id, SDL_Color color)
{
    g_buttons.push_back({{x, y, w, h}, label, id, color});
}

/* ── format helpers ──────────────────────────────────────────────────────── */
static std::string relTime(long long ts)
{
    if (ts <= 0) return "未知";
    long long d = (long long)std::time(nullptr) - ts;
    if (d < 0) d = 0;
    if (d < 60) return "刚刚";
    if (d < 3600) return std::to_string(d / 60) + "分钟前";
    if (d < 86400) return std::to_string(d / 3600) + "小时前";
    if (d < 86400 * 30) return std::to_string(d / 86400) + "天前";
    return "很久以前";
}

static std::string codecDisplay(const std::string &impl)
{
    if (impl.empty()) return "—";
    if (impl.find("h264") != std::string::npos ||
        impl.find("avc") != std::string::npos) return "H.264";
    if (impl.find("h265") != std::string::npos ||
        impl.find("hevc") != std::string::npos) return "H.265";
    if (impl.find("av1") != std::string::npos) return "AV1";
    if (impl.find("rle") != std::string::npos) return "RLE";
    if (impl.find("raw") != std::string::npos) return "原始";
    return impl;
}

static std::string fmtMbps(const std::string &kbps)
{
    if (kbps.empty()) return "—";
    double kb = std::atof(kbps.c_str());
    char buf[32];
    if (kb >= 1000.0) {
        std::snprintf(buf, sizeof(buf), "%.1f Mbps", kb / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f kbps", kb);
    }
    return buf;
}

static std::string truncateUtf8(const std::string &s, size_t maxChars)
{
    size_t n = 0, i = 0;
    while (i < s.size() && n < maxChars) {
        unsigned char c = (unsigned char)s[i];
        size_t adv = 1;
        if (c >= 0xF0) adv = 4;
        else if (c >= 0xE0) adv = 3;
        else if (c >= 0xC0) adv = 2;
        i += adv;
        n++;
    }
    if (i < s.size()) return s.substr(0, i) + "…";
    return s;
}

/** Clip to a pixel width by dropping trailing chars (with ellipsis). */
static std::string fitText(const std::string &s, TTF_Font *font, int maxW)
{
    if (!font || s.empty() || maxW <= 0) return s;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, s.c_str(), &w, &h);
    if (w <= maxW) return s;
    /* Shrink the plain string, measure "…" only for display. An earlier
     * version re-appended "…" to an already-ellipsized string each pass, so
     * t never shrank once it ended with "…" — an infinite loop (frozen GUI)
     * whenever a label needed more than one char of clipping. */
    std::string t = s;
    while (t.size() > 1) {
        size_t end = t.size() - 1;                 /* drop the last char */
        while (end > 0 && ((unsigned char)t[end - 1] & 0xC0) == 0x80) end--;
        if (end > 0) end--;
        t = t.substr(0, end);
        std::string shown = t + "…";
        TTF_SizeUTF8(font, shown.c_str(), &w, &h);
        if (w <= maxW) return shown;
    }
    return "…";
}

/* ── log ring + STAT parsing ─────────────────────────────────────────────── */
static std::string persistLogPath()
{
#ifdef _WIN32
    return configDir() + "\\app.log";
#else
    return configDir() + "/app.log";
#endif
}

static std::string logStamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000);
    std::tm tmv = {};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return buf;
}

/* Persistent child/GUI diagnostics: %LOCALAPPDATA%\Mivara\app.log. */
static void persistLogLine(const std::string &line)
{
#ifdef _WIN32
    CreateDirectoryA(configDir().c_str(), nullptr);
#else
    mkdir(configDir().c_str(), 0755);
#endif
    FILE *f = std::fopen(persistLogPath().c_str(), "ab");
    if (!f) return;
    std::string stamped = logStamp() + line;
    if (stamped.empty() || stamped.back() != '\n') stamped += "\n";
    std::fwrite(stamped.data(), 1, stamped.size(), f);
    std::fclose(f);
}

static void logPush(const std::string &line)
{
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_log_lines.push_back(line);
    while ((int)g_log_lines.size() > LOG_CAP) g_log_lines.pop_front();
    if (g_log_scroll > 0) g_log_scroll++;   /* keep the user's viewport */
    persistLogLine(line);
}

static void parseStatLine(const std::string &line)
{
    /* STAT|k=v|k=v|… */
    LiveStats s;
    s.valid = true;
    std::string body = line.substr(5);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t bar = body.find('|', pos);
        std::string tok = (bar == std::string::npos)
                              ? body.substr(pos)
                              : body.substr(pos, bar - pos);
        size_t eq = tok.find('=');
        if (eq != std::string::npos) {
            std::string k = tok.substr(0, eq);
            std::string v = tok.substr(eq + 1);
            if (k == "t") s.t = v;
            else if (k == "frames") s.frames = v;
            else if (k == "fps") s.fps = v;
            else if (k == "kbps") s.kbps = v;
            else if (k == "codec") s.codec = v;
            else if (k == "hw") s.hw = v;
            else if (k == "mode") s.mode = v;
            else if (k == "src") s.src = v;
            else if (k == "dst") s.dst = v;
            else if (k == "quality") s.quality = v;
            else if (k == "event") s.event = v;
            else if (k == "msg") s.msg = v;
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    std::lock_guard<std::mutex> lk(g_stats_mtx);
    g_stats = s;
}

static void handleChildLine(const std::string &line)
{
    if (line.compare(0, 5, "STAT|") == 0) parseStatLine(line);
    else logPush(line);
    if (line.compare(0, 5, "STAT|") == 0) persistLogLine(line);
}

static void stopReader(ChildProc &child)
{
    if (child.reader.joinable()) child.reader.join();
#ifdef _WIN32
    if (child.stdout_read) {
        CloseHandle(child.stdout_read);
        child.stdout_read = nullptr;
    }
    if (child.stdin_write) {
        CloseHandle(child.stdin_write);
        child.stdin_write = nullptr;
    }
#else
    if (child.stdout_fd >= 0) {
        close(child.stdout_fd);
        child.stdout_fd = -1;
    }
    if (child.stdin_fd >= 0) {
        close(child.stdin_fd);
        child.stdin_fd = -1;
    }
#endif
}

/* Write one SETP|… line to the child's stdin (runtime reconfigure). */
static bool sendToChild(ChildProc &child, const std::string &line)
{
    const std::string s = line + "\n";
#ifdef _WIN32
    if (!child.stdin_write) return false;
    DWORD n = 0;
    return WriteFile(child.stdin_write, s.data(), (DWORD)s.size(), &n, nullptr) &&
           n == s.size();
#else
    if (child.stdin_fd < 0) return false;
    /* A pipe writes partially for buffers > PIPE_BUF, so loop. EPIPE means the
     * child is gone: report failure instead of writing on — SIGPIPE is ignored
     * in main() so a dead child can never kill the GUI. */
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = write(child.stdin_fd, s.data() + sent, s.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return true;
#endif
}

/* Diff the 7 transfer params into a SETP line; returns "SETP" if unchanged. */
static std::string buildSetpLine(const AppConfig &want, const AppConfig &have)
{
    std::string want_codec = (want.codec == "auto" || want.codec.empty())
                                 ? "h264" : want.codec;
    std::string have_codec = (have.codec == "auto" || have.codec.empty())
                                 ? "h264" : have.codec;
    std::string s = "SETP";
    auto add = [&](const char *k, const std::string &v) {
        s += "|"; s += k; s += "="; s += v;
    };
    if (want_codec != have_codec)                add("codec", want_codec);
    if ((want.hw_accel != 0) != (have.hw_accel != 0))
        add("hw", want.hw_accel ? "1" : "0");
    if (want.bitrate_kbps != have.bitrate_kbps)
        add("bitrate", std::to_string(want.bitrate_kbps));
    if (want.quality != have.quality)
        add("quality", std::to_string(want.quality));
    if (want.fps_limit != have.fps_limit)
        add("fps", std::to_string(want.fps_limit));
    if (want.scale_percent != have.scale_percent)
        add("scale", std::to_string(want.scale_percent));
    if (want.chroma != have.chroma)              add("chroma", want.chroma);
    return s;
}

/* ── log window ──────────────────────────────────────────────────────────── */
static void openLogWindow()
{
    if (g_log_open) return;
    g_log_win = SDL_CreateWindow("Mivara 日志",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 LOG_WIN_W, LOG_WIN_H, 0);
    if (!g_log_win) return;
    g_log_ren = SDL_CreateRenderer(g_log_win, -1,
                                   SDL_RENDERER_ACCELERATED |
                                       SDL_RENDERER_PRESENTVSYNC);
    if (!g_log_ren) {
        SDL_DestroyWindow(g_log_win);
        g_log_win = nullptr;
        return;
    }
    g_log_open = true;
    g_log_scroll = 0;
}

static void closeLogWindow()
{
    if (!g_log_open) return;
    if (g_log_ren) SDL_DestroyRenderer(g_log_ren);
    if (g_log_win) SDL_DestroyWindow(g_log_win);
    g_log_ren = nullptr;
    g_log_win = nullptr;
    g_log_open = false;
}

static void toggleLogWindow()
{
    if (g_log_open) closeLogWindow();
    else openLogWindow();
}

static void renderLogWindow()
{
    if (!g_log_open || !g_log_ren) return;
    SDL_SetRenderDrawColor(g_log_ren, THEME.bg.r, THEME.bg.g, THEME.bg.b, 255);
    SDL_RenderClear(g_log_ren);

    /* toolbar */
    SDL_Rect bar{0, 0, LOG_WIN_W, 44};
    SDL_SetRenderDrawColor(g_log_ren, THEME.surface.r, THEME.surface.g,
                           THEME.surface.b, 255);
    SDL_RenderFillRect(g_log_ren, &bar);
    drawText(g_log_ren, "日志", 16, 10, THEME.text, g_font);

    SDL_Rect bClear{LOG_WIN_W - 150, 8, 64, 28};
    SDL_Rect bClose{LOG_WIN_W - 78, 8, 64, 28};
    int mx = -1, my = -1;
    if (SDL_GetMouseFocus() == g_log_win) SDL_GetMouseState(&mx, &my);
    auto hover = [&](const SDL_Rect &r) {
        return mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
    };
    SDL_Color c1 = hover(bClear) ? THEME.hover : THEME.press;
    SDL_Color c2 = hover(bClose) ? THEME.hover : THEME.press;
    drawRoundRect(g_log_ren, bClear, 6, c1, THEME.border);
    drawRoundRect(g_log_ren, bClose, 6, c2, THEME.border);
    drawText(g_log_ren, "清空", bClear.x + bClear.w / 2, bClear.y + 5,
             THEME.text, g_fontS, true);
    drawText(g_log_ren, "关闭", bClose.x + bClose.w / 2, bClose.y + 5,
             THEME.text, g_fontS, true);

    /* lines */
    std::vector<std::string> snap;
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        snap.assign(g_log_lines.begin(), g_log_lines.end());
    }
    const int lineH = 20;
    const int top = 52;
    const int visible = (LOG_WIN_H - top - 8) / lineH;
    int total = (int)snap.size();
    if (g_log_scroll > total - visible) g_log_scroll = std::max(0, total - visible);
    if (g_log_scroll < 0) g_log_scroll = 0;
    int end = total - g_log_scroll;
    int start = std::max(0, end - visible);
    for (int i = start; i < end; i++) {
        int y = top + (i - start) * lineH;
        drawText(g_log_ren, snap[i], 10, y, THEME.textDim, g_fontS);
    }
    if (snap.empty())
        drawText(g_log_ren, "（暂无输出）", 16, top + 8, THEME.textDim, g_fontS);

    SDL_RenderPresent(g_log_ren);
}

static void handleLogClick(int x, int y)
{
    SDL_Rect bClear{LOG_WIN_W - 150, 8, 64, 28};
    SDL_Rect bClose{LOG_WIN_W - 78, 8, 64, 28};
    SDL_Point p{x, y};
    if (SDL_PointInRect(&p, &bClear)) {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        g_log_lines.clear();
        g_log_scroll = 0;
    } else if (SDL_PointInRect(&p, &bClose)) {
        closeLogWindow();
    }
}

/* ── child process helpers ───────────────────────────────────────────────── */
static std::string sibling(const char *name)
{
    return exeDir() + "/" + name;
}

static bool spawnChild(ChildProc &child, const std::string &path, const std::vector<std::string> &args)
{
    if (child.active) return false;
    if (child.reader.joinable()) child.reader.join();

    const bool redirect = (g_cfg.show_status_console == 0);

#ifdef _WIN32
    std::string cmd = "\"" + path + "\"";
    for (auto &a : args) cmd += " " + a;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    ZeroMemory(&child.pi, sizeof(child.pi));

    HANDLE hWrite = nullptr;
    HANDLE hInRead = nullptr;
    DWORD flags = CREATE_NEW_CONSOLE;
    if (redirect) {
        /* Hidden console + anonymous pipes: stdout/stderr → reader thread,
         * stdin ← SETP lines for runtime reconfigure. CREATE_NO_WINDOW still
         * allocates a console so AttachConsole() in stopChild() keeps working. */
        flags = CREATE_NO_WINDOW;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&child.stdout_read, &hWrite, &sa, 0)) return false;
        SetHandleInformation(child.stdout_read, HANDLE_FLAG_INHERIT, 0);
        if (!CreatePipe(&hInRead, &child.stdin_write, &sa, 0)) {
            CloseHandle(hWrite);
            CloseHandle(child.stdout_read);
            child.stdout_read = nullptr;
            return false;
        }
        SetHandleInformation(child.stdin_write, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = hInRead;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
    }

    BOOL ok = CreateProcessA(path.c_str(), buf.data(), nullptr, nullptr,
                             redirect ? TRUE : FALSE, flags,
                             nullptr, nullptr, &si, &child.pi);
    if (hWrite) CloseHandle(hWrite);        /* parent copy: EOF when child dies */
    if (hInRead) CloseHandle(hInRead);
    if (!ok) {
        if (child.stdout_read) {
            CloseHandle(child.stdout_read);
            child.stdout_read = nullptr;
        }
        if (child.stdin_write) {
            CloseHandle(child.stdin_write);
            child.stdin_write = nullptr;
        }
        return false;
    }
    child.active = true;
    if (redirect && child.stdout_read) {
        HANDLE hRead = child.stdout_read;
        child.reader = std::thread([hRead] {
            char chunk[4096];
            std::string partial;
            for (;;) {
                DWORD n = 0;
                BOOL rd = ReadFile(hRead, chunk, sizeof(chunk), &n, nullptr);
                if (!rd || n == 0) break;
                partial.append(chunk, n);
                size_t pos;
                while ((pos = partial.find('\n')) != std::string::npos) {
                    std::string line = partial.substr(0, pos);
                    partial.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    handleChildLine(line);
                }
            }
            if (!partial.empty()) handleChildLine(partial);
        });
    }
    return true;
#else
    int fds[2] = {-1, -1};
    int in_fds[2] = {-1, -1};
    if (redirect) {
        if (pipe(fds) != 0) return false;
        if (pipe(in_fds) != 0) { close(fds[0]); close(fds[1]); return false; }
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (redirect) {
            close(fds[0]);
            dup2(fds[1], 1);
            dup2(fds[1], 2);
            close(fds[1]);
            close(in_fds[1]);
            dup2(in_fds[0], 0);
            close(in_fds[0]);
        }
        std::vector<char *> argv;
        std::vector<std::string> store;
        store.push_back(path);
        for (auto &a : args) store.push_back(a);
        for (auto &s : store) argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);
        execv(path.c_str(), argv.data());
        _exit(127);
    }
    if (pid < 0) {
        if (redirect) {
            close(fds[0]); close(fds[1]);
            close(in_fds[0]); close(in_fds[1]);
        }
        return false;
    }
    if (redirect) {
        close(fds[1]);
        child.stdout_fd = fds[0];
        close(in_fds[0]);
        child.stdin_fd = in_fds[1];
        int fd = fds[0];
        child.reader = std::thread([fd] {
            char chunk[4096];
            std::string partial;
            for (;;) {
                ssize_t n = read(fd, chunk, sizeof(chunk));
                if (n <= 0) break;
                partial.append(chunk, (size_t)n);
                size_t pos;
                while ((pos = partial.find('\n')) != std::string::npos) {
                    std::string line = partial.substr(0, pos);
                    partial.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    handleChildLine(line);
                }
            }
            if (!partial.empty()) handleChildLine(partial);
        });
    }
    child.pid = pid;
    child.active = true;
    return true;
#endif
}

/* Graceful stop: Ctrl+C (Windows) / SIGINT (POSIX) so the sender runs its
 * cleanup path including vddShutdown(). Falls back to hard kill after 8 s
 * (vddShutdown + SetDisplayConfig can take a few seconds), then force-
 * detaches the virtual display so it can never linger after a stop.
 * DO NOT reorder this sequence — CREATE_NO_WINDOW still has a hidden
 * console, so AttachConsole keeps working. */
static void stopChild(ChildProc &child, bool extend = false)
{
    if (!child.active) return;
#ifdef _WIN32
    FreeConsole();
    SetConsoleCtrlHandler(nullptr, TRUE);          /* GUI ignores Ctrl+C */
    bool hard_kill = false;
    if (AttachConsole(child.pi.dwProcessId)) {
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        if (WaitForSingleObject(child.pi.hProcess, 8000) == WAIT_TIMEOUT) {
            TerminateProcess(child.pi.hProcess, 1);
            hard_kill = true;
        }
        FreeConsole();
    } else {
        TerminateProcess(child.pi.hProcess, 1);
        hard_kill = true;
    }
    CloseHandle(child.pi.hProcess);
    CloseHandle(child.pi.hThread);
    child.active = false;
    /* Safety net: a hard-killed extend sender never reached its own
     * vddShutdown() – detach the virtual display from here instead. */
    if (hard_kill && extend) {
        std::cout << "强制停止：正在移除虚拟显示器...\n";
        if (vddForceDetach())
            std::cout << "虚拟显示器已移除\n";
        else
            std::cout << "虚拟显示器移除失败（可能本就未激活）\n";
    }
    stopReader(child);
#else
    kill(child.pid, SIGINT);
    for (int i = 0; i < 30; i++) {
        int st = 0;
        if (waitpid(child.pid, &st, WNOHANG) == child.pid) {
            child.active = false;
            stopReader(child);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(child.pid, SIGKILL);
    waitpid(child.pid, nullptr, 0);
    child.active = false;
    stopReader(child);
#endif
}

static bool childExited(ChildProc &child, const char *name)
{
    if (!child.active) return true;
#ifdef _WIN32
    if (WaitForSingleObject(child.pi.hProcess, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(child.pi.hProcess, &code);
        logPush(std::string("[gui] ") + name + " exited code=" +
                std::to_string((unsigned long)code));
        CloseHandle(child.pi.hProcess);
        CloseHandle(child.pi.hThread);
        child.active = false;
        stopReader(child);
        return true;
    }
    return false;
#else
    int st = 0;
    if (waitpid(child.pid, &st, WNOHANG) == child.pid) {
        logPush(std::string("[gui] ") + name + " exited");
        child.active = false;
        stopReader(child);
        return true;
    }
    return false;
#endif
}

static bool binaryExists(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

/* ── auto-start (Windows Task Scheduler) ─────────────────────────────────── */
#ifdef _WIN32
static std::wstring selfExePathW()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

/* schtasks with CREATE_NO_WINDOW; waits for the exit code. */
static bool runHiddenW(const std::wstring &cmd)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}
#endif

/* Task name must match the [UninstallRun] cleanup in installer/Mivara.iss. */
static bool applyAutoStart(int enable)
{
#ifdef _WIN32
    if (enable) {
        /* /tr value must contain an embedded quoted path (spaces in
         * "Program Files"), and /rl highest avoids a UAC prompt at logon. */
        std::wstring cmd = L"schtasks.exe /create /tn \"Mivara\" /tr \"\\\"" +
                           selfExePathW() +
                           L"\\\"\" /sc onlogon /rl highest /f";
        return runHiddenW(cmd);
    }
    return runHiddenW(L"schtasks.exe /delete /tn \"Mivara\" /f");
#else
    (void)enable;
    return true; /* no-op outside Windows */
#endif
}

/* ── nav helpers ─────────────────────────────────────────────────────────── */
static NavTab navTabForScreen(Screen s)
{
    return (s == SCR_SETTINGS) ? NAV_SETTINGS : NAV_HOME;
}

/* Session screens track g_session_screen so "主页" can return to the live
 * session after browsing 设置. The settings tab doesn't. */
static void setScreen(Screen s)
{
    g_screen = s;
    if (s != SCR_SETTINGS)
        g_session_screen = s;
}

static Screen resolveHomeScreen()
{
    if (g_sender.active) return SCR_STREAMING;
    if (g_receiver.active) return SCR_RECEIVING;
    return g_session_screen;
}

/* ── actions ─────────────────────────────────────────────────────────────── */
static void startSearch()
{
    if (g_searching.exchange(true)) return;      /* already running */
    if (g_search_thread.joinable()) g_search_thread.join();
    const unsigned long generation = ++g_search_generation;
    g_search_done = false;
    g_search_thread = std::thread([generation] {
        auto found = discoverReceivers(5);
        if (generation != g_search_generation.load()) {
            g_searching = false;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            g_devices = found;
        }
        g_found_count = (int)found.size();
        g_searching = false;
        g_search_done = true;
    });
    setScreen(SCR_SEARCHING);
}

/* Take ip/name by value: history callers pass references into g_hist, and
 * this function reloads g_hist below — keeping those references would leave
 * ip/name dangling before the final status/last-used assignments. */
static void launchStream(std::string ip, int port, bool extend, std::string name)
{
    std::string path = sibling("sender.exe");
#ifndef _WIN32
    path = sibling("sender");
#endif
    if (!binaryExists(path)) {
        g_status_note = "找不到 sender 程序，请先运行 make";
        setScreen(SCR_NOT_FOUND);
        return;
    }
    std::string codec = (g_cfg.codec == "auto" || g_cfg.codec.empty())
                            ? "h264" : g_cfg.codec;
    std::vector<std::string> args = {
        ip, std::to_string(port),
        "--mode", extend ? "extend" : "mirror",
        "--codec", codec,
        "--hw", g_cfg.hw_accel ? "1" : "0",
        "--bitrate", std::to_string(g_cfg.bitrate_kbps),
        "--quality", std::to_string(g_cfg.quality),
        "--fps", std::to_string(g_cfg.fps_limit),
        "--scale", std::to_string(g_cfg.scale_percent),
        "--chroma", g_cfg.chroma
    };
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        g_stats = LiveStats{};
    }
    logPush("[gui] launch sender " + ip + ":" + std::to_string(port) +
            " mode=" + (extend ? "extend" : "mirror") +
            " codec=" + codec);
    if (!testTcpConnection(ip, port, 1500)) {
        logPush("[gui] TCP probe failed for " + ip + ":" + std::to_string(port));
        g_status_note = "无法连接 " + ip + ":" + std::to_string(port);
        setScreen(SCR_HOME);
        return;
    }
    if (!spawnChild(g_sender, path, args)) {
        logPush("[gui] spawn sender failed");
        g_status_note = "启动 sender 失败";
        setScreen(SCR_NOT_FOUND);
        return;
    }
    historyUpsert(ip, port, name.empty() ? ip : name,
                  extend ? "extend" : "mirror");
    g_hist = historyLoad();           /* keep the home-screen list in sync */
    g_hist_scroll = 0;
    g_child_extend = extend;
    g_child_label = extend ? "扩展" : "投屏";
    g_status_note = "目标: " + ip + ":" + std::to_string(port);
    g_live_params = g_cfg;
    g_last_ip = ip;
    g_last_port = port;
    g_last_name = name;
    setScreen(SCR_STREAMING);
}

static void launchReceiver()
{
    std::string path = sibling("receiver.exe");
#ifndef _WIN32
    path = sibling("receiver");
#endif
    if (!binaryExists(path)) {
        g_status_note = "找不到 receiver 程序，请先运行 make";
        setScreen(SCR_HOME);
        return;
    }
    if (!spawnChild(g_receiver, path, {})) {
        g_status_note = "启动 receiver 失败";
        setScreen(SCR_HOME);
        return;
    }
    g_status_note.clear();
    g_session_screen = SCR_RECEIVING;
    setScreen(SCR_RECEIVING);
}

static void openSettings()
{
    g_cfg_edit = g_cfg;
    g_screen = SCR_SETTINGS;
}

/* ── screen building ─────────────────────────────────────────────────────── */
static void buildButtons()
{
    g_buttons.clear();

    /* left nav sidebar — present on every screen */
    const NavTab tab = navTabForScreen(g_screen);
    addButton(16, HEADER_H + 26, NAV_W - 32, 48, "主  页", BTN_NAV_HOME,
              tab == NAV_HOME ? THEME.primary : THEME.press);
    addButton(16, HEADER_H + 86, NAV_W - 32, 48, "设  置", BTN_NAV_SETTINGS,
              tab == NAV_SETTINGS ? THEME.primary : THEME.press);
    addButton(16, WIN_H - 76, NAV_W - 32, 48, "退  出", BTN_NAV_QUIT, THEME.press);

    const int cx = CONTENT_CX;
    switch (g_screen) {
    case SCR_HOME: {
        addButton(CONTENT_X + 40, 150, 280, 115, "接 收", BTN_HOME_RECEIVE, THEME.success);
        addButton(CONTENT_X + 360, 150, 280, 115, "发 送", BTN_HOME_SEND, THEME.primary);
        if (g_receiver.active)
            addButton(CONTENT_X + 40, 270, 280, 38, "停止接收", BTN_HOME_RECEIVE_STOP, THEME.danger);
        if (g_sender.active)
            addButton(CONTENT_X + 360, 270, 280, 38, "停止发送", BTN_HOME_SENDER_STOP, THEME.danger);
        /* connection history rows under the two cards */
        int y = 376;
        for (int i = 0; i < HIST_VISIBLE; i++) {
            int idx = i + g_hist_scroll;
            if (idx >= (int)g_hist.size()) break;
            int rowX = CONTENT_X + 30, rowW = CONTENT_W - 60;
            int x3 = rowX + rowW - 72;          /* 删除 */
            int x2 = x3 - 84;                   /* 扩展 */
            int x1 = x2 - 84;                   /* 投屏 */
            addButton(rowX, y, x1 - rowX - 10, 46, "", BTN_HIST_BASE + idx * 10, THEME.surface);
            addButton(x1, y + 5, 76, 36, "投屏",
                      BTN_HIST_BASE + idx * 10 + 1, THEME.success);
            addButton(x2, y + 5, 76, 36, "扩展",
                      BTN_HIST_BASE + idx * 10 + 2, THEME.primary);
            addButton(x3, y + 5, 72, 36, "删除",
                      BTN_HIST_BASE + idx * 10 + 3, THEME.danger);
            y += 52;
        }
        if (!g_hist.empty())
            addButton(CONTENT_X + CONTENT_W - 150, 340, 130, 34, "清空历史",
                      BTN_HIST_CLEAR, THEME.danger);
        break;
    }

    case SCR_RECEIVING:
        addButton(cx - 190, 490, 170, 52, "停止接收", BTN_RECV_STOP, THEME.danger);
        addButton(cx + 20,  490, 170, 52, "日志", BTN_RECV_LOG, THEME.press);
        break;

    case SCR_SEARCHING:
        addButton(cx - 90, 470, 180, 52, "取 消", BTN_SEARCH_CANCEL, THEME.press);
        break;

    case SCR_FOUND: {
        size_t n;
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            n = g_devices.size();
        }
        int y = 150;
        for (size_t i = 0; i < n && i < 6; i++, y += 56) {
            addButton(CONTENT_X + CONTENT_W - 230, y + 6, 100, 40, "投屏",
                      BTN_DEV_BASE + (int)i * 10 + 1, THEME.success);
            addButton(CONTENT_X + CONTENT_W - 120, y + 6, 100, 40, "扩展",
                      BTN_DEV_BASE + (int)i * 10 + 2, THEME.primary);
        }
        addButton(cx - 170, 510, 160, 48, "重新搜索", BTN_FOUND_RESEARCH, THEME.press);
        addButton(cx + 10,  510, 160, 48, "手动输入ip", BTN_FOUND_MANUAL, THEME.press);
        break;
    }

    case SCR_NOT_FOUND:
        addButton(cx - 320, 300, 200, 56, "继续搜索", BTN_NF_CONTINUE, THEME.success);
        addButton(cx - 100, 300, 200, 56, "手动输入ip", BTN_NF_MANUAL, THEME.primary);
        addButton(cx + 120, 300, 200, 56, "退 出", BTN_NF_QUIT, THEME.danger);
        break;

    case SCR_MANUAL_IP:
        addButton(cx - 180, 330, 160, 52, "连 接", BTN_MANUAL_CONNECT, THEME.success);
        addButton(cx + 20,  330, 160, 52, "返 回", BTN_MANUAL_BACK, THEME.press);
        break;

    case SCR_STREAMING:
        addButton(cx - 190, 455, 170, 52, "停 止", BTN_STREAM_STOP, THEME.danger);
        addButton(cx + 20,  455, 170, 52, "日志", BTN_STREAM_LOG, THEME.press);
        break;

    case SCR_SETTINGS: {
        const char *codecLabels[] = {"自动", "H.264", "H.265", "AV1", "原始", "RLE"};
        const char *codecIds[]    = {"auto", "h264", "h265", "av1", "raw", "rle"};
        int segW = 90, segH = 32;
        int right = CONTENT_X + CONTENT_W - 70;
        int segX = right - segW * 6 - 4 * 5;
        int y0 = 118, pitch = 40;
        for (int i = 0; i < 6; i++) {
            bool on = (g_cfg_edit.codec == codecIds[i]);
            addButton(segX + i * (segW + 4), y0, segW, segH,
                      codecLabels[i], BTN_SET_CODEC_BASE + i,
                      on ? THEME.primary : THEME.press);
        }
        int y1 = y0 + pitch;
        addButton(CONTENT_X + 20, y1, CONTENT_W - 40, 34, "", BTN_SET_HW, THEME.surface);
        /* quality stepper */
        int y2 = y1 + pitch;
        addButton(right - 190, y2, 36, 32, "−", BTN_SET_QUALITY_DEC, THEME.press);
        addButton(right - 36,  y2, 36, 32, "+", BTN_SET_QUALITY_INC, THEME.press);
        /* fps stepper */
        int y3 = y2 + pitch;
        addButton(right - 190, y3, 36, 32, "−", BTN_SET_FPS_DEC, THEME.press);
        addButton(right - 36,  y3, 36, 32, "+", BTN_SET_FPS_INC, THEME.press);
        /* bitrate stepper */
        int y4 = y3 + pitch;
        addButton(right - 190, y4, 36, 32, "−", BTN_SET_BITRATE_DEC, THEME.press);
        addButton(right - 36,  y4, 36, 32, "+", BTN_SET_BITRATE_INC, THEME.press);
        /* scale segmented */
        int y5 = y4 + pitch;
        const char *scaleLabels[] = {"100%", "75%", "50%"};
        const int scaleVals[] = {100, 75, 50};
        int scW = 88, scX = right - scW * 3 - 4 * 2;
        for (int i = 0; i < 3; i++) {
            bool on = (g_cfg_edit.scale_percent == scaleVals[i]);
            addButton(scX + i * (scW + 4), y5, scW, 32,
                      scaleLabels[i], BTN_SET_SCALE_BASE + i,
                      on ? THEME.primary : THEME.press);
        }
        /* chroma segmented */
        int y6 = y5 + pitch;
        const char *chromaLabels[] = {"4:2:0", "4:4:4"};
        const char *chromaIds[] = {"420", "444"};
        int chW = 88, chX = right - chW * 2 - 4;
        for (int i = 0; i < 2; i++) {
            bool on = (g_cfg_edit.chroma == chromaIds[i]);
            addButton(chX + i * (chW + 4), y6, chW, 32,
                      chromaLabels[i], BTN_SET_CHROMA_BASE + i,
                      on ? THEME.primary : THEME.press);
        }
        int y7 = y6 + pitch;
        addButton(CONTENT_X + 20, y7, CONTENT_W - 40, 34, "", BTN_SET_CONSOLE, THEME.surface);
        int y8 = y7 + pitch;
        addButton(CONTENT_X + 20, y8, CONTENT_W - 40, 34, "", BTN_SET_LOGWIN, THEME.surface);
        int y9 = y8 + pitch;
        addButton(CONTENT_X + 20, y9, CONTENT_W - 40, 34, "", BTN_SET_AUTOSTART, THEME.surface);
        int y10 = y9 + pitch;
        addButton(CONTENT_X + 20, y10, CONTENT_W - 40, 34, "", BTN_SET_AUTORECV, THEME.surface);

        addButton(cx - 155, 630, 140, 48, "确 定", BTN_SET_OK, THEME.success);
        addButton(cx + 15,  630, 140, 48, "恢复默认", BTN_SET_RESET, THEME.press);
        if (g_sender.active || g_receiver.active)
            addButton(cx + 185, 630, 140, 48, "返回会话", BTN_RETURN_SESSION, THEME.press);
        break;
    }
    }
}

/* ── stat panel text ─────────────────────────────────────────────────────── */
static std::string statLine1(const LiveStats &s)
{
    std::string head = (g_child_label.empty() ? "会话" : g_child_label) + "中";
    std::string cd = codecDisplay(s.codec);
    std::string out = head + " · " + cd;
    if (!s.codec.empty() && s.codec != "raw" && s.codec != "rle")
        out += " (" + s.codec + ")";
    if (!s.fps.empty()) out += "   " + s.fps + " fps";
    if (!s.kbps.empty()) out += "   " + fmtMbps(s.kbps);
    return out;
}

static std::string statLine2(const LiveStats &s)
{
    std::string out;
    if (!s.src.empty() && !s.dst.empty()) out = s.src + "→" + s.dst;
    else if (!s.src.empty()) out = s.src;
    if (!s.mode.empty()) out += (out.empty() ? "" : "   ") + s.mode;
    if (!s.quality.empty()) out += (out.empty() ? "" : "   ") + std::string("Q") + s.quality;
    return out;
}

static void drawStatPanel(SDL_Renderer *ren, int x, int y, int w, int h)
{
    LiveStats s;
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        s = g_stats;
    }
    SDL_Rect panel{x, y, w, h};
    drawRoundRect(ren, panel, THEME.radius, THEME.surface, THEME.border);

    if (!s.valid) {
        drawText(ren, "等待统计数据…", x + 24, y + h / 2 - 10, THEME.textDim, g_fontS);
        return;
    }
    if (s.event == "error") {
        drawText(ren, "错误" + (s.msg.empty() ? std::string("") : ": " + s.msg),
                 x + 24, y + 18, THEME.danger, g_fontS);
        return;
    }
    if (s.event == "start" || s.event == "stop") {
        drawText(ren, s.event == "start" ? "会话开始" : "会话结束",
                 x + 24, y + 18, THEME.textDim, g_fontS);
        return;
    }
    drawText(ren, fitText(statLine1(s), g_fontS, w - 48), x + 24, y + 18,
             THEME.text, g_fontS);
    std::string l2 = statLine2(s);
    if (!l2.empty())
        drawText(ren, fitText(l2, g_fontS, w - 48), x + 24, y + 52,
                 THEME.textDim, g_fontS);
}

/* ── screen rendering ────────────────────────────────────────────────────── */
static void renderScreen(int mousex, int mousey)
{
    (void)mousex; (void)mousey;
    SDL_SetRenderDrawColor(g_ren, THEME.bg.r, THEME.bg.g, THEME.bg.b, 255);
    SDL_RenderClear(g_ren);

    const int cx = CONTENT_CX;

    /* header (full width) */
    SDL_Rect hdr{0, 0, WIN_W, HEADER_H};
    SDL_SetRenderDrawColor(g_ren, THEME.surface.r, THEME.surface.g,
                           THEME.surface.b, 255);
    SDL_RenderFillRect(g_ren, &hdr);
    drawText(g_ren, "Mivara  幻屏", WIN_W / 2, 18, THEME.text, g_fontL, true);

    /* left nav sidebar */
    SDL_Rect nav{0, HEADER_H, NAV_W, WIN_H - HEADER_H};
    SDL_SetRenderDrawColor(g_ren, THEME.surface.r, THEME.surface.g,
                           THEME.surface.b, 255);
    SDL_RenderFillRect(g_ren, &nav);
    SDL_SetRenderDrawColor(g_ren, THEME.border.r, THEME.border.g,
                           THEME.border.b, 255);
    SDL_RenderDrawLine(g_ren, NAV_W - 1, HEADER_H, NAV_W - 1, WIN_H);

    switch (g_screen) {
    case SCR_HOME: {
        drawText(g_ren, "零配置局域网投屏 / 无线扩展屏", cx, 112, THEME.textDim,
                 g_font, true);
        drawText(g_ren, "作为接收端，等待其他设备投屏", CONTENT_X + 180, 292,
                 THEME.textDim, g_fontS, true);
        drawText(g_ren, "搜索接收端并开始投屏", CONTENT_X + 500, 292,
                 THEME.textDim, g_fontS, true);
        drawText(g_ren, g_receiver.active ? "接收运行中" : "接收未运行",
                 CONTENT_X + 180, 220, g_receiver.active ? THEME.success : THEME.textDim, g_fontS, true);
        drawText(g_ren, g_sender.active ? (g_child_label + "运行中") : "发送未运行",
                 CONTENT_X + 500, 220, g_sender.active ? THEME.success : THEME.textDim, g_fontS, true);
        if (!g_status_note.empty())
            drawText(g_ren, g_status_note, cx, 318, THEME.danger, g_font, true);
        /* connection history under the two cards */
        drawText(g_ren, "连接历史", CONTENT_X + 30, 344, THEME.text, g_font);
        if (g_hist.empty()) {
            drawText(g_ren, "暂无连接记录，成功连接一次后会自动出现在这里",
                     cx, 440, THEME.textDim, g_font, true);
        } else {
            int y = 376;
            for (int i = 0; i < HIST_VISIBLE; i++) {
                int idx = i + g_hist_scroll;
                if (idx >= (int)g_hist.size()) break;
                const HistEntry &e = g_hist[idx];
                SDL_Rect row{CONTENT_X + 30, y, CONTENT_W - 60, 46};
                bool hov = (g_hot >= BTN_HIST_BASE + idx * 10 &&
                            g_hot < BTN_HIST_BASE + idx * 10 + 4);
                drawRoundRect(g_ren, row, THEME.radius,
                              hov ? THEME.hover : THEME.surface, THEME.border);
                std::string nm = e.name.empty() ? e.ip : truncateUtf8(e.name, 12);
                std::string line = nm + " · " + e.ip + ":" +
                                   std::to_string(e.port) + " · " +
                                   (e.mode == "extend" ? "扩展" : "投屏") +
                                   " · " + relTime(e.last_used_unix) +
                                   " · ×" + std::to_string(e.use_count);
                drawText(g_ren, fitText(line, g_fontS, 350), CONTENT_X + 50, y + 14,
                         THEME.text, g_fontS);
                y += 52;
            }
            if ((int)g_hist.size() > HIST_VISIBLE) {
                drawText(g_ren,
                         "第 " + std::to_string(g_hist_scroll + 1) + "-" +
                             std::to_string(std::min((int)g_hist.size(),
                                                     g_hist_scroll + HIST_VISIBLE)) +
                             " 条 / 共 " + std::to_string(g_hist.size()) + " 条  （滚轮翻页）",
                         cx, 640, THEME.textDim, g_fontS, true);
            }
        }
        break;
    }

    case SCR_RECEIVING: {
        drawText(g_ren, "正在接收", cx, 110, THEME.success, g_fontL, true);
        std::string ip = getLocalIPAddress();
        drawText(g_ren, "本机 IP：  " + ip, cx, 190, THEME.text, g_font, true);
        drawText(g_ren, "视频端口：8081      计算端口：8082      端口服务：8083",
                 cx, 236, THEME.textDim, g_font, true);
        drawText(g_ren, "发现服务：UDP 1900", cx, 274, THEME.textDim, g_font, true);
        drawText(g_ren, "状态：等待发送端连接…", cx, 330, THEME.text, g_font, true);
        drawText(g_ren, "发送端可运行  sender " + ip, cx, 376, THEME.textDim,
                 g_font, true);
        break;
    }

    case SCR_SEARCHING: {
        int dots = ((int)(SDL_GetTicks() / 400)) % 4;
        drawText(g_ren, "搜索中" + std::string(dots, '.'), cx, 220, THEME.text,
                 g_fontL, true);
        drawText(g_ren, "正在通过 SSDP 查找局域网内的接收端", cx, 290,
                 THEME.textDim, g_font, true);
        break;
    }

    case SCR_FOUND: {
        std::vector<DiscoveredDevice> devs;
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            devs = g_devices;
        }
        drawText(g_ren, "发现 " + std::to_string(devs.size()) + " 台设备",
                 cx, 88, THEME.text, g_fontL, true);
        int y = 150;
        for (size_t i = 0; i < devs.size() && i < 6; i++, y += 56) {
            SDL_Rect row{CONTENT_X + 30, y, CONTENT_W - 60, 52};
            drawRoundRect(g_ren, row, THEME.radius, THEME.surface, THEME.border);
            std::string label = devs[i].name.empty()
                                    ? devs[i].toString()
                                    : truncateUtf8(devs[i].name, 12) + "  ·  " +
                                          devs[i].toString();
            drawText(g_ren, fitText(label, g_fontS, 360), CONTENT_X + 55, y + 17,
                     THEME.text, g_fontS);
        }
        if (devs.size() > 6)
            drawText(g_ren, "仅显示前 6 台", cx, y + 8, THEME.textDim, g_font, true);
        break;
    }

    case SCR_NOT_FOUND:
        drawText(g_ren, "未找到可用设备", cx, 180, THEME.text, g_fontL, true);
        drawText(g_ren, "请确认接收端已启动，且两台机器在同一局域网",
                 cx, 240, THEME.textDim, g_font, true);
        if (!g_status_note.empty())
            drawText(g_ren, g_status_note, cx, 410, THEME.danger, g_font, true);
        break;

    case SCR_MANUAL_IP:
        drawText(g_ren, "手动输入接收端 IP", cx, 140, THEME.text, g_fontL, true);
        {
            SDL_Rect box{cx - 200, 220, 400, 60};
            drawRoundRect(g_ren, box, THEME.radius, THEME.inputBg, THEME.primary);
            std::string shown = g_manual_ip.empty() ? "例如 192.168.1.105" : g_manual_ip;
            SDL_Color c = g_manual_ip.empty() ? THEME.textDim : THEME.text;
            drawText(g_ren, shown, cx, 238, c, g_font, true);
        }
        if (!g_status_note.empty())
            drawText(g_ren, g_status_note, cx, 420, THEME.danger, g_font, true);
        break;

    case SCR_STREAMING:
        drawText(g_ren, "正在" + g_child_label, cx, 110, THEME.success, g_fontL, true);
        drawText(g_ren, g_status_note, cx, 175, THEME.text, g_font, true);
        drawText(g_ren, "推流中… 关闭请点下方按钮", cx, 215, THEME.textDim,
                 g_font, true);
        drawText(g_ren, "实时状态", CONTENT_X + 50, 268, THEME.textDim, g_fontS);
        drawStatPanel(g_ren, CONTENT_X + 30, 292, CONTENT_W - 60, 120);
        break;

    case SCR_SETTINGS: {
        drawText(g_ren, "设 置", cx, 76, THEME.text, g_fontL, true);
        const int y0 = 118, pitch = 40;
        const int lblX = CONTENT_X + 30;
        const int right = CONTENT_X + CONTENT_W - 70;
        /* labels */
        drawText(g_ren, "编码", lblX, y0 + 6, THEME.text, g_font);
        drawText(g_ren, "画质 (CRF)", lblX, y0 + pitch * 2 + 6, THEME.text, g_font);
        drawText(g_ren, "帧率上限", lblX, y0 + pitch * 3 + 6, THEME.text, g_font);
        drawText(g_ren, "码率", lblX, y0 + pitch * 4 + 6, THEME.text, g_font);
        drawText(g_ren, "缩放", lblX, y0 + pitch * 5 + 6, THEME.text, g_font);
        drawText(g_ren, "色度", lblX, y0 + pitch * 6 + 6, THEME.text, g_font);

        /* value boxes between stepper buttons */
        auto valueBox = [&](int y, const std::string &val) {
            SDL_Rect box{right - 150, y, 110, 32};
            drawRoundRect(g_ren, box, 6, THEME.inputBg, THEME.border);
            drawText(g_ren, val, box.x + box.w / 2, box.y + 7, THEME.text,
                     g_fontS, true);
        };
        valueBox(y0 + pitch * 2, std::to_string(g_cfg_edit.quality));
        valueBox(y0 + pitch * 3, std::to_string(g_cfg_edit.fps_limit) + " fps");
        valueBox(y0 + pitch * 4,
                 g_cfg_edit.bitrate_kbps == 0
                     ? "质量模式"
                     : std::to_string(g_cfg_edit.bitrate_kbps) + " kbps");

        /* toggle rows (ghost hit-areas) */
        auto toggleRow = [&](int id, int y, const char *label, bool on) {
            SDL_Rect row{CONTENT_X + 20, y, CONTENT_W - 40, 34};
            bool hov = (g_hot == id || g_press == id);
            drawRoundRect(g_ren, row, THEME.radius,
                          hov ? THEME.hover : THEME.surface, THEME.border);
            drawText(g_ren, label, row.x + 18, y + 6, THEME.text, g_font);
            SDL_Rect pill{row.x + row.w - 70, y + 6, 56, 22};
            SDL_Color pc = on ? THEME.success : THEME.inputBg;
            fillRoundRect(g_ren, pill, 11, pc);
            fillCircle(g_ren, on ? (pill.x + pill.w - 11) : (pill.x + 11),
                       pill.y + 11, 9);
        };
        toggleRow(BTN_SET_HW, y0 + pitch, "硬件加速", g_cfg_edit.hw_accel != 0);
        toggleRow(BTN_SET_CONSOLE, y0 + pitch * 7, "状态控制台",
                  g_cfg_edit.show_status_console != 0);
        toggleRow(BTN_SET_LOGWIN, y0 + pitch * 8, "日志窗口",
                  g_cfg_edit.show_log_window != 0);
        toggleRow(BTN_SET_AUTOSTART, y0 + pitch * 9, "开机自启动",
                  g_cfg_edit.auto_start != 0);
        toggleRow(BTN_SET_AUTORECV, y0 + pitch * 10, "自动接收",
                  g_cfg_edit.auto_receive != 0);

        drawText(g_ren, "所有设置保存到 config.ini，通过命令行传给子进程",
                 cx, 580, THEME.textDim, g_fontS, true);
        break;
    }
    }

    for (auto &b : g_buttons) drawButton(g_ren, b);
    SDL_RenderPresent(g_ren);
}

/* ── button actions ──────────────────────────────────────────────────────── */
static void onButton(int id)
{
    if (id >= BTN_SET_CODEC_BASE && id < BTN_SET_CODEC_BASE + 6) {
        const char *ids[] = {"auto", "h264", "h265", "av1", "raw", "rle"};
        g_cfg_edit.codec = ids[id - BTN_SET_CODEC_BASE];
        return;
    }
    if (id >= BTN_SET_SCALE_BASE && id < BTN_SET_SCALE_BASE + 3) {
        const int vals[] = {100, 75, 50};
        g_cfg_edit.scale_percent = vals[id - BTN_SET_SCALE_BASE];
        return;
    }
    if (id >= BTN_SET_CHROMA_BASE && id < BTN_SET_CHROMA_BASE + 2) {
        const char *vals[] = {"420", "444"};
        g_cfg_edit.chroma = vals[id - BTN_SET_CHROMA_BASE];
        return;
    }

    switch (id) {
    case BTN_NAV_HOME:
        setScreen(SCR_HOME);
        break;
    case BTN_RETURN_SESSION:
        g_screen = resolveHomeScreen();
        break;
    case BTN_NAV_SETTINGS:
        openSettings();
        break;
    case BTN_NAV_QUIT: {
        SDL_Event e; e.type = SDL_QUIT; SDL_PushEvent(&e);
        break;
    }

    case BTN_HOME_RECEIVE:
        launchReceiver();
        break;
    case BTN_HOME_SEND:
        g_status_note.clear();
        startSearch();
        break;
    case BTN_HOME_RECEIVE_STOP:
        stopChild(g_receiver);
        break;
    case BTN_HOME_SENDER_STOP:
        stopChild(g_sender, g_child_extend);
        g_child_extend = false;
        break;

    case BTN_RECV_STOP:
        stopChild(g_receiver);
        setScreen(SCR_HOME);
        break;
    case BTN_RECV_LOG:
        toggleLogWindow();
        break;

    case BTN_SEARCH_CANCEL:
        ++g_search_generation;
        g_searching = false;
        setScreen(SCR_HOME);
        break;

    case BTN_FOUND_RESEARCH:
        g_status_note.clear();
        startSearch();
        break;
    case BTN_FOUND_MANUAL:
    case BTN_NF_MANUAL:
        g_manual_ip.clear();
        g_status_note.clear();
        SDL_StartTextInput();
        setScreen(SCR_MANUAL_IP);
        break;

    case BTN_NF_CONTINUE:
        g_status_note.clear();
        startSearch();
        break;
    case BTN_NF_QUIT: {
        SDL_Event e2; e2.type = SDL_QUIT; SDL_PushEvent(&e2);
        break;
    }

    case BTN_MANUAL_CONNECT: {
        std::string ip = g_manual_ip;
        if (ip.empty()) { g_status_note = "请输入 IP 地址"; break; }
        if (!testTcpConnection(ip, 8081, 1500)) {
            g_status_note = "无法连接 " + ip + ":8081";
            break;
        }
        /* reuse the found-list screen with this single device */
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            g_devices.clear();
            g_devices.emplace_back(ip, 8081);
        }
        SDL_StopTextInput();
        g_status_note.clear();
        setScreen(SCR_FOUND);
        break;
    }
    case BTN_MANUAL_BACK:
        SDL_StopTextInput();
        g_status_note.clear();
        setScreen(SCR_HOME);
        break;

    case BTN_STREAM_STOP:
        stopChild(g_sender, g_child_extend);
        g_child_extend = false;
        setScreen(SCR_HOME);
        break;
    case BTN_STREAM_LOG:
        toggleLogWindow();
        break;

    /* settings */
    case BTN_SET_HW:
        g_cfg_edit.hw_accel = !g_cfg_edit.hw_accel;
        break;
    case BTN_SET_QUALITY_DEC:
        g_cfg_edit.quality = std::max(0, g_cfg_edit.quality - 1);
        break;
    case BTN_SET_QUALITY_INC:
        g_cfg_edit.quality = std::min(51, g_cfg_edit.quality + 1);
        break;
    case BTN_SET_FPS_DEC:
        g_cfg_edit.fps_limit = std::max(1, g_cfg_edit.fps_limit - 5);
        break;
    case BTN_SET_FPS_INC:
        g_cfg_edit.fps_limit = std::min(240, g_cfg_edit.fps_limit + 5);
        break;
    case BTN_SET_BITRATE_DEC:
        g_cfg_edit.bitrate_kbps = std::max(0, g_cfg_edit.bitrate_kbps - 1000);
        break;
    case BTN_SET_BITRATE_INC:
        g_cfg_edit.bitrate_kbps = std::min(100000, g_cfg_edit.bitrate_kbps + 1000);
        break;
    case BTN_SET_CONSOLE:
        g_cfg_edit.show_status_console = !g_cfg_edit.show_status_console;
        break;
    case BTN_SET_LOGWIN:
        g_cfg_edit.show_log_window = !g_cfg_edit.show_log_window;
        if (g_cfg_edit.show_log_window) openLogWindow();
        else closeLogWindow();
        break;
    case BTN_SET_AUTOSTART:
        g_cfg_edit.auto_start = !g_cfg_edit.auto_start;
        break;
    case BTN_SET_AUTORECV:
        g_cfg_edit.auto_receive = !g_cfg_edit.auto_receive;
        break;
    case BTN_SET_OK: {
        const int want = g_cfg_edit.auto_start ? 1 : 0;
        const int had = g_cfg.auto_start ? 1 : 0;
        g_cfg = g_cfg_edit;
        configSave(g_cfg);
        /* Re-register whenever auto_start is on so a moved exe path stays
         * current; drop the task when it was just switched off. */
        if (want != had || want == 1) {
            if (!applyAutoStart(want))
                g_status_note = "计划任务写入失败（需管理员）";
            else
                g_status_note.clear();
        }
        /* Live-apply transfer params to a running sender session. */
        if (g_sender.active) {
            std::string setp = buildSetpLine(g_cfg, g_live_params);
            if (setp.size() > 4) {           /* something actually changed */
#ifdef _WIN32
                const bool have_stdin = (g_sender.stdin_write != nullptr);
#else
                const bool have_stdin = (g_sender.stdin_fd >= 0);
#endif
                if (have_stdin) {
                    if (sendToChild(g_sender, setp)) {
                        g_live_params = g_cfg;
                        g_status_note = "参数已更新";
                    } else {
                        g_status_note = "参数未生效：发送进程无响应";
                    }
                } else {
                    /* Console-mode child has no stdin channel – restart the
                     * session with the new params instead. */
                    const bool ext = g_child_extend;
                    const std::string ip = g_last_ip, nm = g_last_name;
                    const int port = g_last_port;
                    stopChild(g_sender, g_child_extend);
                    g_status_note = "控制台模式：已按新参数重启会话";
                    launchStream(ip, port, ext, nm);
                    return;
                }
            }
        }
        g_screen = resolveHomeScreen();
        break;
    }
    case BTN_SET_RESET:
        g_cfg_edit = AppConfig{};
        break;

    case BTN_HIST_CLEAR:
        historyClear();
        g_hist.clear();
        g_hist_scroll = 0;
        break;

    default:
        /* device rows: BTN_DEV_BASE + idx*10 + 1(投屏) / +2(扩展) */
        if (id >= BTN_DEV_BASE && id < BTN_HIST_BASE) {
            int rel = id - BTN_DEV_BASE;
            int idx = rel / 10;
            int kind = rel % 10;
            std::vector<DiscoveredDevice> devs;
            {
                std::lock_guard<std::mutex> lk(g_dev_mtx);
                devs = g_devices;
            }
            if (idx >= 0 && idx < (int)devs.size() && (kind == 1 || kind == 2))
                launchStream(devs[idx].ip_address, devs[idx].tcp_port,
                             kind == 2, devs[idx].name);
            break;
        }
        /* history rows: BTN_HIST_BASE + idx*10 + 0(row) / 1 / 2 / 3 */
        if (id >= BTN_HIST_BASE && id < BTN_HIST_CLEAR) {
            int rel = id - BTN_HIST_BASE;
            int idx = rel / 10;
            int kind = rel % 10;
            if (idx >= 0 && idx < (int)g_hist.size()) {
                const HistEntry &e = g_hist[idx];
                if (kind == 3) {
                    historyDelete(e.ip, e.port);
                    g_hist = historyLoad();
                    /* Clamp to the last full page, not to the entry count —
                     * otherwise deleting a middle entry leaves a blank list. */
                    const int maxScroll =
                        std::max(0, (int)g_hist.size() - HIST_VISIBLE);
                    if (g_hist_scroll > maxScroll) g_hist_scroll = maxScroll;
                } else if (kind == 1 || kind == 2) {
                    launchStream(e.ip, e.port, kind == 2, e.name);
                } else {
                    /* row body click: reconnect with the entry's last mode */
                    launchStream(e.ip, e.port, e.mode == "extend", e.name);
                }
            }
            break;
        }
        break;
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init: " << SDL_GetError() << "\n";
        return 1;
    }
    if (TTF_Init() < 0) {
        std::cerr << "TTF_Init: " << TTF_GetError() << "\n";
        SDL_Quit();
        return 1;
    }
    g_font  = openFont(22);
    g_fontL = openFont(34);
    g_fontS = openFont(16);
    if (!g_font || !g_fontL || !g_fontS) {
        std::cerr << "找不到中文字体（msyh/simhei/NotoSansCJK）\n";
        TTF_Quit(); SDL_Quit();
        return 1;
    }

    configLoad(g_cfg);
    g_hist = historyLoad();

    g_win = SDL_CreateWindow("Mivara", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, 0);
    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_win || !g_ren) {
        std::cerr << "SDL_CreateWindow/Renderer: " << SDL_GetError() << "\n";
        return 1;
    }

    if (g_cfg.show_log_window) openLogWindow();

#ifndef _WIN32
    /* Writing SETP lines to a sender that already exited must fail the write,
     * not terminate the GUI: ignore SIGPIPE (see sendToChild). */
    signal(SIGPIPE, SIG_IGN);
#endif

    initSockets();
    logPush("[gui] app start");

    if (g_cfg.auto_receive) launchReceiver();

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            }
            else if (ev.type == SDL_WINDOWEVENT &&
                     ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                if (g_log_win && ev.window.windowID == SDL_GetWindowID(g_log_win))
                    closeLogWindow();
                else
                    running = false;
            }
            else if (ev.type == SDL_MOUSEWHEEL) {
                if (g_log_win && ev.wheel.windowID == SDL_GetWindowID(g_log_win)) {
                    int dir = ev.wheel.y;
                    if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dir = -dir;
                    g_log_scroll -= dir * 3;
                    if (g_log_scroll < 0) g_log_scroll = 0;
                } else if (g_screen == SCR_HOME && !g_hist.empty()) {
                    int dir = ev.wheel.y;
                    if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dir = -dir;
                    g_hist_scroll -= dir;
                    int maxScroll = std::max(0, (int)g_hist.size() - HIST_VISIBLE);
                    if (g_hist_scroll < 0) g_hist_scroll = 0;
                    if (g_hist_scroll > maxScroll) g_hist_scroll = maxScroll;
                }
            }
            else if (ev.type == SDL_MOUSEBUTTONDOWN &&
                     ev.button.button == SDL_BUTTON_LEFT) {
                if (g_log_win && ev.button.windowID == SDL_GetWindowID(g_log_win)) {
                    handleLogClick(ev.button.x, ev.button.y);
                } else if (ev.button.windowID == SDL_GetWindowID(g_win)) {
                    g_press = BTN_NONE;
                    SDL_Point p{ev.button.x, ev.button.y};
                    for (auto &b : g_buttons) {
                        if (SDL_PointInRect(&p, &b.rect)) { g_press = b.id; break; }
                    }
                }
            }
            else if (ev.type == SDL_MOUSEBUTTONUP &&
                     ev.button.button == SDL_BUTTON_LEFT) {
                if (ev.button.windowID == SDL_GetWindowID(g_win)) {
                    SDL_Point p{ev.button.x, ev.button.y};
                    int hit = BTN_NONE;
                    for (auto &b : g_buttons) {
                        if (SDL_PointInRect(&p, &b.rect)) { hit = b.id; break; }
                    }
                    if (g_press != BTN_NONE && hit == g_press) onButton(g_press);
                    g_press = BTN_NONE;
                }
            }
            else if (ev.type == SDL_TEXTINPUT) {
                if (g_screen == SCR_MANUAL_IP) {
                    for (const char *c = ev.text.text; *c; c++) {
                        if ((isdigit((unsigned char)*c) || *c == '.') &&
                            g_manual_ip.size() < 15)
                            g_manual_ip += *c;
                    }
                }
            }
            else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (g_screen == SCR_MANUAL_IP) {
                        SDL_StopTextInput();
                        setScreen(SCR_HOME);
                    } else if (g_screen == SCR_SETTINGS) {
                        g_screen = resolveHomeScreen();
                    } else if (g_screen == SCR_STREAMING) {
                        stopChild(g_sender, g_child_extend);
                        g_child_extend = false;
                        setScreen(SCR_HOME);
                    } else if (g_screen == SCR_RECEIVING) {
                        stopChild(g_receiver);
                        setScreen(SCR_HOME);
                    } else if (g_screen != SCR_HOME) {
                        setScreen(SCR_HOME);
                    } else running = false;
                }
                else if (g_screen == SCR_MANUAL_IP &&
                         ev.key.keysym.sym == SDLK_BACKSPACE) {
                    if (!g_manual_ip.empty()) g_manual_ip.pop_back();
                }
                else if (g_screen == SCR_MANUAL_IP &&
                         ev.key.keysym.sym == SDLK_RETURN) {
                    onButton(BTN_MANUAL_CONNECT);
                }
            }
        }

        /* async search completion */
        if (g_search_done.exchange(false))
            setScreen((g_found_count > 0) ? SCR_FOUND : SCR_NOT_FOUND);

        /* child exit tracking */
        if (g_sender.active && childExited(g_sender, "sender")) {
            g_child_extend = false;
            if (g_screen == SCR_STREAMING) {
                g_status_note = "会话已结束";
                setScreen(SCR_HOME);
            }
        }
        if (g_receiver.active && childExited(g_receiver, "receiver")) {
            if (g_screen == SCR_RECEIVING) {
                g_status_note = "接收会话已结束";
                setScreen(SCR_HOME);
            }
        }

        int mx = -1, my = -1;
        if (SDL_GetMouseFocus() == g_win) SDL_GetMouseState(&mx, &my);
        g_hot = BTN_NONE;
        for (auto &b : g_buttons) {
            if (mx >= b.rect.x && mx < b.rect.x + b.rect.w &&
                my >= b.rect.y && my < b.rect.y + b.rect.h) {
                g_hot = b.id;
                break;
            }
        }
        buildButtons();
        renderScreen(mx, my);
        if (g_log_open) renderLogWindow();
    }

    if (g_search_thread.joinable()) {
        ++g_search_generation;
        g_search_thread.join();
    }
    stopChild(g_sender, g_child_extend);
    stopChild(g_receiver);
    closeLogWindow();
    cleanupSockets();
    if (g_font)  TTF_CloseFont(g_font);
    if (g_fontL) TTF_CloseFont(g_fontL);
    if (g_fontS) TTF_CloseFont(g_fontS);
    TTF_Quit();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
