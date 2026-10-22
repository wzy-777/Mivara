/**
 * APP.CPP – Mivara GUI LAUNCHER (SDL2 + SDL2_ttf)
 *
 * Layout: fixed left nav sidebar (主页 / 设置) with the active
 * tab's content to its right. Session flows (search / stream / receive) and
 * the connection-history list live inside the 主页 tab and remain reachable
 * via "返回会话" while browsing 设置.
 *
 * Screens:
 *   HOME       – 接收 / 发送 cards + connection-history rows below
 *   RECEIVING  – "正在接收" + local IP / ports; spawns bin/receiver
 *   SEARCHING  – "搜索中" (SSDP discovery on a background thread)
 *   FOUND      – machine list, each row offers 投屏 (mirror) / 扩展 (extend)
 *   NOT_FOUND  – 继续搜索 / 手动输入ip
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
#include <unordered_map>
#include <vector>

#include "config.h"
#include "discover.h"
#include "history.h"
#include "theme.h"
#include "update.h"
#include "vdd.h"
#include "version.h"

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

/* ── metrics ───────────────────────────────────────────────────────────────
 * One fixed canvas: there is no reflow, so every position below is a
 * compile-time integer. Width grew from 820 to fit the settings screen's
 * two-column card layout; height stayed at 700 so the window still fits a
 * 768-px-tall laptop screen together with its title bar and the taskbar.
 */
#define WIN_W 880
#define WIN_H 700
#define LOG_WIN_W 460
#define LOG_WIN_H 540
#define LOG_CAP 2000
#define HIST_VISIBLE 5

/* App shell: a full-width header strip with the nav rail below it. The rest
 * of the geometry — including NAV_W and HEADER_H — is in the shared layout
 * block below, next to the rects that depend on it. */
#define CONTENT_X NAV_W
#define CONTENT_W (WIN_W - NAV_W)
#define CONTENT_CX (CONTENT_X + CONTENT_W / 2)

/* Content padding. */
#define PAD 24

/* Spacing scale (4px base). */
#define SP1 4
#define SP2 8
#define SP3 12
#define SP4 16
#define SP5 20
#define SP6 24

/* Type scale. Bold faces degrade to the regular one when unavailable. */
#define FONT_DISPLAY 30                 /* screen titles                       */
#define FONT_TITLE   22                 /* card titles, nav wordmark           */
#define FONT_BODY    16                 /* row labels, section headers         */
#define FONT_SMALL   13                 /* captions, buttons, chips            */

/* ── shared layout ─────────────────────────────────────────────────────────
 * buildButtons() (hit rects) and renderScreen() (painted rects) both read
 * these. They used to be written out twice as separate literals, which is how
 * the home status text ended up painted underneath the action cards and the
 * settings 编码 label ended up under the codec control. Anything positional
 * belongs here, never inline in either function.
 */
#define HEADER_H 56
#define NAV_W 136
#define NAV_ITEM_X 12
#define NAV_ITEM_W 112
#define NAV_ITEM_H 44
#define NAV_HOME_Y 76
#define NAV_SET_Y 128

#define HDR_GLYPH_CX 36
#define HDR_WORD_X 58

/* Header appearance switcher: three icon cells on a track, right-aligned with
 * the content column. The cells carry sun / moon / monitor glyphs, not text —
 * the labelled version of the same control is the 外观 card on the settings
 * screen, which is where a user who does not read the icons will look. */
#define SEG_Y 13
#define SEG_H 30
#define SEG_PAD 2                       /* track inset around the cells         */
#define SEG_CELL_W 40
#define SEG_CELL_GAP 2
#define SEG_CELL_H (SEG_H - SEG_PAD * 2)
#define SEG_W (SEG_PAD * 2 + SEG_CELL_W * 3 + SEG_CELL_GAP * 2)     /* 128       */
#define SEG_X (BODY_R - SEG_W)          /* flush with the content's right edge  */
#define SEG_CELL_X(i) (SEG_X + SEG_PAD + (i) * (SEG_CELL_W + SEG_CELL_GAP))

/* Header update button: present only while a release is waiting, immediately to
 * the left of the appearance switcher and the same height, so the two read as
 * one row of header controls. It is a ghost hit area — drawHeader() paints it,
 * because its fill depends on the update state (see updateBadgeVisible). */
#define UPD_BTN_W 30
#define UPD_BTN_H SEG_H
#define UPD_BTN_X (SEG_X - 10 - UPD_BTN_W)
#define UPD_BTN_Y SEG_Y

#define BODY_X (CONTENT_X + PAD)        /* 160                                  */
#define BODY_W (CONTENT_W - PAD * 2)    /* 696                                  */
#define BODY_R (BODY_X + BODY_W)        /* 856                                  */

#define CARD_R THEME.radiusCard

/* home */
#define HOME_TITLE_Y 76                 /* tagline under the header             */
#define HOME_BAND_Y 108                 /* update notice band                   */
#define HOME_CAPTION_Y 160
#define HOME_CARD_Y 184
#define HOME_CARD_W 336
#define HOME_CARD_H 156
#define HOME_CARD_X(i) (BODY_X + (i) * (HOME_CARD_W + 24))

#define HIST_CARD_Y 372
#define HIST_CARD_H 288
#define HIST_ROW_X (BODY_X + 16)
#define HIST_ROW_W (BODY_W - 32)
#define HIST_ROW_H 44
#define HIST_ROW_Y(i) (HIST_CARD_Y + 42 + (i) * 48)
#define HIST_BTN_X 636                  /* 投屏 / 扩展 / 删除, right-aligned      */
#define HIST_BTN_W 72
#define HIST_DEL_X (HIST_ROW_X + HIST_ROW_W - 44)
#define HIST_PAGER_Y 674                /* footer caption, clear of the card's
                                         * bottom edge at 660 and its shadow */
#define HIST_CARD_H_EMPTY 132

/* found-devices list: the rows are laid out from the top, so the action row
 * follows the last one instead of floating at a fixed y with a hole above it */
#define FOUND_ROW_Y0 120
#define FOUND_ROW_PITCH 60
#define FOUND_ROW_H 52
#define FOUND_ACTIONS_MAX_Y 512

static int foundActionsY(int n)
{
    if (n > 6) n = 6;
    const int y = FOUND_ROW_Y0 + n * FOUND_ROW_PITCH + 20;
    return y > FOUND_ACTIONS_MAX_Y ? FOUND_ACTIONS_MAX_Y : y;
}

/* These build compound literals, so they must be functions, not macros —
 * SDL_Rect{...}.x does not parse as a macro expansion in an argument list. */
static SDL_Rect rcHomeCard(int i)
{
    return SDL_Rect{HOME_CARD_X(i), HOME_CARD_Y, HOME_CARD_W, HOME_CARD_H};
}
static SDL_Rect rcHomeStop(int i)
{
    return SDL_Rect{HOME_CARD_X(i) + 220, HOME_CARD_Y + 76, 96, 34};
}
static SDL_Rect rcHomeChip(int i)
{
    return SDL_Rect{HOME_CARD_X(i) + 16, HOME_CARD_Y + 98, 96, 24};
}
static SDL_Rect rcHistRow(int i)
{
    return SDL_Rect{HIST_ROW_X, HIST_ROW_Y(i), HIST_ROW_W, HIST_ROW_H};
}
static SDL_Rect rcHistBtn(int i, int which)   /* 0 投屏 / 1 扩展 / 2 删除 */
{
    static const int w[3] = {HIST_BTN_W, HIST_BTN_W, 44};
    int x = (which == 2) ? HIST_DEL_X
                         : HIST_BTN_X + (which == 0 ? 0 : HIST_BTN_W + 8);
    return SDL_Rect{x, HIST_ROW_Y(i) + 6, w[which], 32};
}

/* settings: two columns of cards */
#define SET_COL_L 160
#define SET_COL_R 520
#define SET_COL_W 336
#define SET_CARD_Y 108
#define SET_PAD 20
#define SET_LC_X (SET_COL_L + SET_PAD)  /* 180 */
#define SET_LC_W (SET_COL_W - SET_PAD * 2)  /* 296 */
#define SET_RC_X (SET_COL_R + SET_PAD)  /* 540 */
#define SET_L_H 424
#define SET_R_H 296                     /* 连接与启动                            */
#define SET_APPEAR_Y 420                /* 外观                                  */
#define SET_APPEAR_H 112
#define SET_UPD_Y 548                   /* 更新 (full width)                     */
#define SET_UPD_H 88
#define SET_ACT_Y 648                   /* 确定 / 恢复默认 / 返回会话             */
#define SET_ACT_H 44

#define SET_CODEC_LABEL_Y 168
#define SET_GRID_Y0 196                 /* codec 3x2 grid                        */
#define SET_ROW_Y(i) (288 + (i) * 44)   /* 画质 / 帧率 / 码率 / 缩放 / 色度      */
#define SET_TOGGLE_Y0 158               /* first 连接与启动 toggle row           */
#define SET_STEP_W 36                   /* − / value / + stepper                 */
#define SET_VAL_W 110
#define SET_ROW_R (SET_LC_X + SET_LC_W) /* 476                                   */
#define SET_MINUS_X (SET_ROW_R - 36 - 8 - SET_VAL_W - 8 - SET_STEP_W)  /* 278 */
#define SET_VAL_X   (SET_MINUS_X + SET_STEP_W + 8)                     /* 322 */
#define SET_PLUS_X  (SET_ROW_R - SET_STEP_W)                           /* 440 */

#define TITLE_Y 72

/* ── theme ─────────────────────────────────────────────────────────────────
 * The palette lives in theme.cpp (light / dark / follow-system). THEME is an
 * alias for the live instance so call sites read the same as before, but it is
 * re-read every frame — never cache a Theme:: field across a frame boundary.
 */
#define THEME (theme())

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
    /* update row: one id per ACTION, never per slot - a press and its release
     * must not be dispatched against a state that changed in between */
    BTN_SET_AUTOCHECK = 7055,           /* 自动检查更新 toggle */
    BTN_SET_UPD_CHECK,                  /* 检查更新 (and 重试 after a failed check) */
    BTN_SET_UPD_DL,                     /* 下载更新 (and 重试 after a failed download) */
    BTN_SET_UPD_CANCEL,                 /* 取消下载 */
    BTN_SET_UPD_INSTALL,                /* 立即安装 */
    BTN_SET_UPD_PAGE,                   /* 打开下载页 */
    BTN_SET_UPD_IGNORE,                 /* 忽略此版本 / 取消忽略 */
    BTN_UPD_NOTICE,                     /* home-screen update banner */
    /* history rows (drawn on the home screen under 接收/发送) */
    BTN_HIST_BASE = 5000,               /* row i: +i*10 +0 row / +1 投屏 / +2 扩展 / +3 删除 */
    BTN_HIST_CLEAR = 5300,
    /* device rows on FOUND */
    BTN_DEV_BASE = 1000,                /* +i*10 +1 投屏 / +2 扩展 */
    /* left nav sidebar */
    BTN_NAV_HOME = 8000,
    BTN_NAV_SETTINGS,
    BTN_RETURN_SESSION = 8010,
    /* Header appearance switcher. It shares the 8000 block with the nav ids
     * (8000..8010) — keep any new nav item below BTN_THEME_LIGHT — and it needs
     * its own ids because the 外观 card on the settings screen is a second
     * control for the same setting. See kThemeIds for how they are dispatched. */
    BTN_THEME_LIGHT = 8020,
    BTN_THEME_DARK,
    BTN_THEME_SYSTEM,
    BTN_HDR_UPDATE,                     /* header 更新 button (only while a
                                         * release is waiting); matches against
                                         * nothing else, so it may sit outside
                                         * the kThemeIds table */
    /* The settings-screen appearance cells must sit in the GAP between the
     * settings block (7000..7062, where the update ids are an implicit +1 run
     * from BTN_SET_AUTOCHECK) and the nav block (8000+). Anything inside that
     * run would be swallowed by the theme branch in onButton(), which runs
     * before the switch — that silently turned 「打开下载页」 into "switch to
     * light" once. The assert below keeps the range pinned to the gap. */
    BTN_SET_THEME_BASE = 7070,          /* +0 light / +1 dark / +2 system */
};

static_assert(BTN_SET_THEME_BASE > BTN_UPD_NOTICE && BTN_SET_THEME_BASE + 2 < BTN_NAV_HOME,
              "appearance ids must stay in the gap between the settings and nav ranges");

/* Appearance is the only setting reachable from two places at once — the header
 * switcher (8020..8022) and the 外观 card (7070..7072) — so its ids are matched
 * against a table rather than derived by subtracting a base. The derived form
 * was
 *     (id >= BTN_SET_THEME_BASE) ? id - BTN_SET_THEME_BASE : id - BTN_THEME_LIGHT
 * and that test is true for BOTH ranges (8020 >= 7070), so every header click
 * took the first branch and indexed modes[950] out of bounds. The live mode
 * became garbage, so resolve() fell through to the light palette, no cell drew
 * as selected, and themeModeKey()'s default wrote theme=system to config.ini —
 * i.e. the switcher looked like it always jumped to 亮色 whatever you picked,
 * and a restart then followed the system instead. */
struct ThemeIdEntry { int id; ThemeMode mode; };
static constexpr ThemeIdEntry kThemeIds[] = {
    {BTN_THEME_LIGHT,        ThemeMode::Light},
    {BTN_THEME_DARK,         ThemeMode::Dark},
    {BTN_THEME_SYSTEM,       ThemeMode::System},
    {BTN_SET_THEME_BASE + 0, ThemeMode::Light},
    {BTN_SET_THEME_BASE + 1, ThemeMode::Dark},
    {BTN_SET_THEME_BASE + 2, ThemeMode::System},
};
static constexpr int kThemeIdCount = (int)(sizeof(kThemeIds) / sizeof(kThemeIds[0]));

/* Two ids for the same mode is harmless (there are, deliberately, six entries
 * for three modes); two entries sharing an id is not — that is exactly the
 * collision that once turned 「打开下载页」 into "switch to light". */
constexpr bool themeIdsAreDistinct()
{
    for (int i = 0; i < kThemeIdCount; i++)
        for (int j = i + 1; j < kThemeIdCount; j++)
            if (kThemeIds[i].id == kThemeIds[j].id) return false;
    return true;
}
static_assert(themeIdsAreDistinct(), "two appearance entries claim the same button id");

/* Visual variant. The fill/hover/press colours are resolved from the live
 * palette at draw time, so no colour literal is ever baked into a Button and
 * the old +/-22-channel hover arithmetic (which only looked right on the one
 * dark palette) is gone. */
enum BtnVariant {
    BTN_V_PRIMARY,     /* filled accent, onPrimary label                  */
    BTN_V_SECONDARY,   /* surfaceAlt fill + border, text label            */
    BTN_V_DANGER,      /* solid red, white label (DANGER_SOLID)           */
    BTN_V_LINK,        /* no fill; danger label, tinted wash on hover     */
    BTN_V_CHOICE,      /* selectable cell; .on fills it with the accent   */
    BTN_V_CHIP_OK,     /* successTint chip (投屏)                          */
    BTN_V_CHIP_ACC,    /* primaryTint chip (扩展)                          */
    BTN_V_NAV,         /* nav-rail item; .on marks the active tab         */
    BTN_V_GHOST,       /* hit area only — the caller paints it            */
};

/* Tiny geometric glyphs drawn from the primitive set (see the icon helpers). */
enum GlyphId {
    GLYPH_NONE = 0,
    GLYPH_MONITOR,
    GLYPH_ARROW_UP,
    GLYPH_ARROW_DOWN,
    GLYPH_SLIDERS,
    GLYPH_POWER,
    GLYPH_SUN,          /* appearance: 亮色                       */
    GLYPH_MOON,         /* appearance: 暗色                       */
    GLYPH_UPDATE,       /* header: 有可用更新                     */
};

struct Button {
    SDL_Rect    rect;
    std::string label;
    int         id;
    int         variant = BTN_V_SECONDARY;
    int         glyph   = GLYPH_NONE;
    bool        on      = false;      /* BTN_V_CHOICE: this cell is selected */
    bool        framed  = true;       /* BTN_V_CHOICE: draw the cell's own border */
    TTF_Font   *font    = nullptr;    /* null = the variant's default size   */
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
static TTF_Font     *g_font  = nullptr;   /* FONT_BODY                     */
static TTF_Font     *g_fontL = nullptr;   /* FONT_DISPLAY                  */
static TTF_Font     *g_fontS = nullptr;   /* FONT_SMALL (log / captions)   */
static TTF_Font     *g_fontB = nullptr;   /* FONT_TITLE,  bold             */
static TTF_Font     *g_fontBS = nullptr;  /* FONT_SMALL,  bold (section hd) */

/* Display scale — see the "display scale" block further down. 1.0 until main()
 * resolves it, which is also what keeps the off-screen screenshot harness (it
 * never runs main) rendering at the raw 880x700 layout. */
static float         g_uiScale = 1.0f;

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

/* update check (the worker + state live in update.cpp; the UI polls) */
static bool   g_upd_auto_checked = false;   /* at most one auto check per launch */
static Uint32 g_upd_auto_at = 0;            /* SDL_GetTicks() deadline for it */
static bool   g_upd_installing = false;     /* installer hand-over already begun */
static bool   g_upd_auto_install = false;   /* header button asked for the whole
                                             * download→install run */

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

/* Bold face for titles / selected nav / button labels. Falls back to the
 * regular face, so hierarchy degrades to size+colour but never breaks. */
static TTF_Font *openFontBold(int size)
{
    static const char *candidates[] = {
        "C:/Windows/Fonts/msyhbd.ttc",     /* Microsoft YaHei Bold  */
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",     /* SimHei is already bold-ish */
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        nullptr
    };
    for (int i = 0; candidates[i]; i++) {
        TTF_Font *f = TTF_OpenFont(candidates[i], size);
        if (f) return f;
    }
    return openFont(size);
}

/* ── text / shape drawing ────────────────────────────────────────────────── */
static const double UI_PI = 3.14159265358979323846;

enum TextAlign { ALIGN_LEFT = 0, ALIGN_CENTER, ALIGN_RIGHT };

/* ── display scale ─────────────────────────────────────────────────────────
 * The layout below is authored in a fixed 880x700 grid and every constant in
 * this file is in those LOGICAL units. On a scaled display SDL makes the
 * process per-monitor DPI aware by default, so the OS never bitmap-stretches
 * the window — which is exactly why the raw layout lands 1:1 in physical
 * pixels and, at 125%, draws the whole UI a quarter smaller than every other
 * window on screen. At that size a 13 px CJK glyph gets one pixel per stroke
 * and almost no room to antialias, which is what reads as "jagged".
 *
 * So the UI is drawn through one scale factor:
 *   - the window is created at WIN_* x scale physical pixels;
 *   - SDL_RenderSetScale scales every shape primitive, so the layout constants
 *     above stay untouched;
 *   - fonts are opened at size x scale and text blits itself 1:1 in device
 *     pixels (drawTextPx), because scaling a finished glyph texture — what
 *     SDL's own DPI scaling would do — is precisely what makes text blurry;
 *   - pointer coordinates are divided back into logical units at the event
 *     boundary, so hit rects keep matching what was painted.
 * Resolved once at startup; a window dragged to a monitor with a different
 * scale keeps the scale it was created with (cf. the theme's system poll).
 */
static int uiPx(int v) { return (int)std::lround((double)v * g_uiScale); }

/** Device pixel -> logical UI coordinate (pointer input).
 *
 * floor, not lround: the renderer paints a scaled rect as
 * [floor(x*s), ceil((x+w)*s)), so a device pixel p belongs to the logical unit
 * it is AT or past. Rounding to nearest instead over-rounds every odd device
 * pixel at s = 2 — p = x+w-1 maps to x+w — which shifts the whole hit region a
 * pixel left of the painted one, and on a row of edge-sharing cells (the 缩放 /
 * 色度 chips) that means clicking a cell's rightmost painted column selects its
 * neighbour. Measured over every button on every screen (test/uiprobe
 * `hitcover`): lround misses 1.5 % of painted pixels at 2x and 0.9 % at 1.25x,
 * floor misses 0 % at every integer scale and ~0.7 % at 1.25x. The residue is
 * inherent — at a fractional scale the painted rect spills up to one pixel left
 * of its logical cell, so that pixel is claimed by two controls and no scalar
 * mapping can resolve it. */
static int uiCoord(int px) { return (int)std::floor((double)px / g_uiScale); }

/** Logical edge -> the device pixel the shape layer starts/ends on.
 *
 * floor/ceil, not lround, and that pairing is load-bearing: the renderer paints
 * a scaled rect as [floor(x*s), ceil((x+w)*s)), so two shapes that share a
 * logical edge land on the SAME device pixel instead of each rounding to its
 * own. The rounded-rect bands below rely on it — round-to-nearest leaves a
 * one-pixel gap between the middle band and the corner masks at 1.25x. At
 * scale 1.0 both reduce to the identity. */
static int uiDevLo(int v) { return (int)std::floor((double)v * g_uiScale); }
static int uiDevHi(int v) { return (int)std::ceil((double)v * g_uiScale); }

static SDL_Rect uiDevRect(int x, int y, int w, int h)
{
    SDL_Rect r{uiDevLo(x), uiDevLo(y), 0, 0};
    r.w = uiDevHi(x + w) - r.x;
    r.h = uiDevHi(y + h) - r.y;
    return r;
}

/** Drops the render scale for the lifetime of a shape body.
 *
 * Every shape primitive from here down paints in DEVICE pixels, and has to: a
 * shape rasterised in logical units and then stretched by g_uiScale lands its
 * edges on fractional device pixels, which with no antialiasing turns a regular
 * staircase into an uneven one. Same save / force-1 / restore dance drawTextPx
 * does (:653-657), and the scopes nest, so a shape may call a shape. */
struct ScaleScope {
    SDL_Renderer *ren;
    float sx, sy;
    explicit ScaleScope(SDL_Renderer *r) : ren(r)
    {
        SDL_RenderGetScale(ren, &sx, &sy);
        SDL_RenderSetScale(ren, 1.0f, 1.0f);
    }
    ~ScaleScope() { SDL_RenderSetScale(ren, sx, sy); }
    ScaleScope(const ScaleScope &) = delete;
    ScaleScope &operator=(const ScaleScope &) = delete;
};

/* The colour most recently handed to setColor(). The shape layer needs it
 * because fillCircle/fillRoundRect are documented to draw "in the colour most
 * recently passed to setColor()" and several call sites rely on that (:802,
 * :844-845, :887, :2581, :2761). Carrying the colour through the mask cache
 * instead would have meant changing six signatures and every one of those
 * call sites for no behavioural gain. */
static SDL_Color g_drawColor{0x00, 0x00, 0x00, 0xFF};

static void setColor(SDL_Renderer *ren, SDL_Color c)
{
    g_drawColor = c;
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
}

static SDL_Color withAlpha(SDL_Color c, Uint8 a)
{
    c.a = a;
    return c;
}

/* The only two literal colours the UI may use: a toggle knob, and the wash that
 * shows hover (white over a dark label, black over a light one). Everything
 * else comes from THEME — see AGENTS.md. */
static const SDL_Color WHITE{0xFF, 0xFF, 0xFF, 0xFF};
static const SDL_Color BLACK{0x00, 0x00, 0x00, 0xFF};

/** Physical-pixel blit. The font is already rasterised at the display scale, so
 * the render scale is dropped for the copy — a glyph texture that gets scaled
 * again is exactly the blurry text this whole path exists to avoid. The caller
 * passes device pixels; every other helper in this file takes logical units. */
static void drawTextPx(SDL_Renderer *ren, const std::string &s, int px, int py,
                       SDL_Color c, TTF_Font *font, bool center)
{
    if (!ren || !font || s.empty()) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect dst{center ? px - surf->w / 2 : px, py, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (!tex) return;
    float sx = 1.0f, sy = 1.0f;
    SDL_RenderGetScale(ren, &sx, &sy);
    SDL_RenderSetScale(ren, 1.0f, 1.0f);
    SDL_RenderCopy(ren, tex, nullptr, &dst);
    SDL_RenderSetScale(ren, sx, sy);
    SDL_DestroyTexture(tex);
}

/** (x,y) is the TOP-LEFT of the rendered surface, in LOGICAL units. */
static void drawText(SDL_Renderer *ren, const std::string &s, int x, int y,
                     SDL_Color c, TTF_Font *font, bool center = false)
{
    drawTextPx(ren, s, uiPx(x), uiPx(y), c, font, center);
}

/** Vertically centred on cy, anchored horizontally by 'align'. LOGICAL units:
 * the metrics come back in device pixels (the font is scaled), so the anchor is
 * converted first and the centring happens in device space. */
static void drawTextV(SDL_Renderer *ren, const std::string &s, int x, int cy,
                      SDL_Color c, TTF_Font *font, int align = ALIGN_LEFT)
{
    if (!ren || !font || s.empty()) return;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, s.c_str(), &w, &h) != 0) return;
    const int ax  = uiPx(x);
    const int acy = uiPx(cy);
    const int dx = (align == ALIGN_CENTER) ? ax - w / 2
                 : (align == ALIGN_RIGHT)  ? ax - w
                                           : ax;
    drawTextPx(ren, s, dx, acy - h / 2, c, font, false);
}

/** Vertically centred inside a rect; 'pad' insets the anchored edge. */
static void drawTextIn(SDL_Renderer *ren, const std::string &s, const SDL_Rect &rc,
                       SDL_Color c, TTF_Font *font,
                       int align = ALIGN_LEFT, int pad = 0)
{
    int x = (align == ALIGN_CENTER) ? rc.x + rc.w / 2
          : (align == ALIGN_RIGHT)  ? rc.x + rc.w - pad
                                    : rc.x + pad;
    drawTextV(ren, s, x, rc.y + rc.h / 2, c, font, align);
}

/* ── shape layer: device-native geometry + coverage antialiasing ───────────
 *
 * SDL's shape primitives — RenderDrawLine, RenderFillRect — are hard-edged:
 * no coverage, no antialiasing, anywhere. Every circle, every rounded corner
 * and every glyph in this file is built out of them, so at 1:1 the corners
 * already read as a staircase, and at a fractional display scale
 * (SDL_RenderSetScale(1.25) on a 125 % display) each logical scanline is
 * stretched to 1.25 device pixels, which the rasteriser resolves to one or two
 * rows depending on the fractional offset. Measured on the shipped backend
 * (SDL_RENDERER_ACCELERATED -> direct3d/D3D9): a 1-logical-px hairline that is
 * a uniform 1px at 1:1 comes out as 17 rows of 1px and 5 of 2px at 1.25x, and
 * the rounded-corner edge deviates 0.49 -> 0.82 px RMS from the true arc. Text
 * never had this problem because drawTextPx rasterises FreeType at DEVICE size
 * (:639-659); the shapes now get the same treatment, plus the coverage the
 * primitives never had.
 *
 * Each shape is rasterised ONCE, supersampled, into a coverage-only mask cached
 * per (renderer, kind, size), and drawn as a single tinted blit. Three
 * properties fall out of that and are worth stating because they are what make
 * it safe:
 *   - the mask carries COVERAGE ONLY — RGB is white, alpha is the shape.
 *     Colour arrives at blit time through SetTextureColorMod/AlphaMod, so no
 *     colour literal ever enters a mask and a theme switch needs no
 *     invalidation;
 *   - the mask is built by running the ORIGINAL primitive at MASK_SS x, so a
 *     glyph's hand-tuned maths (the 2px stems, the sun's ray blocks, the moon
 *     bite) is never re-authored — its silhouette cannot drift;
 *   - masks are per-renderer (an SDL texture belongs to the renderer that made
 *     it), so the log window gets its own cache — see closeLogWindow.
 *
 * What this does NOT do: turn the drop shadow's three spread bands into a real
 * gradient (each band's edge is smoothed, the banding stays), or touch the
 * layout, the hit rects, the fonts, or the window size.
 */

/* Supersamples per axis when rasterising a mask. 4 gives 17 coverage levels,
 * which is what a corner arc needs to stop reading as a step; each build is
 * one-off per (kind, size) and the whole set is a few hundred kB. */
static const int MASK_SS = 4;

/* True only while a glyph mask is being rasterised. The public shape helpers
 * then draw their original hard-edged scanlines instead of recursing into the
 * mask cache, which is how a glyph body gets baked without being rewritten. */
static bool g_baking = false;

enum MaskKind { MK_DISC = 1, MK_CORNER, MK_RING, MK_GLYPH, MK_WARN };

/* (renderer, kind, a, b) -> texture. 'a' is a device radius, a glyph id or a
 * warning size; 'b' is a device thickness or size where the kind needs two
 * dimensions. Keyed by renderer because an SDL_Texture is bound to the
 * renderer that created it — blitting the log window's copy from the main
 * renderer (or the reverse) is undefined. */
static std::unordered_map<SDL_Renderer *,
                          std::unordered_map<long long, SDL_Texture *>> g_masks;

static long long maskKey(int kind, int a, int b)
{
    return ((long long)kind << 48) | ((long long)(a & 0xFFFFFF) << 24) |
           (long long)(b & 0xFFFFFF);
}

/** Drop a renderer's masks. Textures must go before their renderer does. */
static void dropMasks(SDL_Renderer *ren)
{
    auto it = g_masks.find(ren);
    if (it == g_masks.end()) return;
    for (auto &kv : it->second)
        if (kv.second) SDL_DestroyTexture(kv.second);
    g_masks.erase(it);
}

/** A throwaway supersampled canvas, cleared to transparent.
 *
 * The shape is painted at MASK_SS x by multiplying the GEOMETRY, not by
 * setting the render scale: the software backend scales a DrawLine's
 * coordinates but not its thickness, so a scaled scanline lands every
 * MASK_SS-th row and combs the shape, while at scale 1 the primitive's own
 * per-row loop covers every supersampled row. */
struct MaskCanvas {
    SDL_Surface  *surf = nullptr;
    SDL_Renderer *ren  = nullptr;
    MaskCanvas(int w, int h)
    {
        surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!surf) return;
        ren = SDL_CreateSoftwareRenderer(surf);
        if (!ren) return;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        /* Ink colour for the bake. The clear above leaves the draw colour at
         * transparent black, and fillCircleRaw takes its colour from the
         * ambient setColor() rather than a parameter — without this the disc
         * bakes as alpha-0 lines and vanishes. */
        SDL_SetRenderDrawColor(ren, 0xFF, 0xFF, 0xFF, 0xFF);
    }
    ~MaskCanvas()
    {
        if (ren) { SDL_RenderFlush(ren); SDL_DestroyRenderer(ren); }
        if (surf) SDL_FreeSurface(surf);
    }
    MaskCanvas(const MaskCanvas &) = delete;
    MaskCanvas &operator=(const MaskCanvas &) = delete;
};

/** Supersampled ARGB -> coverage-only ARGB: RGB white, A = the ss x ss block
 * average at (i,j). The box filter is what turns the hard supersampled edges
 * into the coverage ramp.
 *
 * ss=1 is the "already a 0/255 silhouette" case (the glyph and warning masks,
 * which are baked at LOGICAL resolution and upscaled by the linear filter at
 * blit time). It must still go through here rather than hand the raw surface to
 * uploadMask: MaskCanvas clears to (0,0,0,0), so a transparent pixel would be
 * BLACK, and bilinear filtering at a fractional scale would then mix black into
 * the edge RGB — multiplying the glyph by its own coverage a second time and
 * leaving every antialiased edge muddy instead of a light tint of the ink. */
static SDL_Surface *coverageFrom(SDL_Surface *src, int w, int h, int ss = MASK_SS)
{
    if (!src || w <= 0 || h <= 0 || ss <= 0) return nullptr;
    SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                      SDL_PIXELFORMAT_ARGB8888);
    if (!out) return nullptr;
    SDL_LockSurface(src);
    SDL_LockSurface(out);
    const Uint32 *sp = (const Uint32 *)src->pixels;
    Uint32       *op = (Uint32 *)out->pixels;
    const int sstride = src->pitch / 4, ostride = out->pitch / 4;
    const int n = ss * ss;
    for (int j = 0; j < h; j++) {
        const int by = j * ss;
        for (int i = 0; i < w; i++) {
            const int bx = i * ss;
            int sum = 0;
            for (int sy = 0; sy < ss; sy++) {
                const Uint32 *row = sp + (size_t)(by + sy) * sstride + bx;
                for (int sx = 0; sx < ss; sx++)
                    sum += (int)((row[sx] >> 24) & 0xFF);
            }
            op[(size_t)j * ostride + i] =
                (Uint32)((Uint32)((sum + n / 2) / n) << 24) | 0x00FFFFFFu;
        }
    }
    SDL_UnlockSurface(out);
    SDL_UnlockSurface(src);
    return out;
}

/** Coverage surface -> a texture on 'ren'. The blend mode is set here rather
 * than left at SDL's default (NONE), which would paint the mask's transparent
 * pixels as opaque black. */
static SDL_Texture *uploadMask(SDL_Renderer *ren, SDL_Surface *cov, bool linear)
{
    if (!cov) return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, cov);
    SDL_FreeSurface(cov);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    if (linear) SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    return tex;
}

/** One tinted mask blit, in DEVICE pixels — callers hold a ScaleScope.
 * final alpha = mask coverage x c.a / 255, which is why a translucent fill
 * (the shadow, the tint chips, the hairlines) composites correctly instead of
 * double-darkening where two shapes meet. */
static void blitMask(SDL_Renderer *ren, SDL_Texture *tex, int x, int y, int size,
                     SDL_Color c, SDL_RendererFlip flip = SDL_FLIP_NONE)
{
    if (!tex || size <= 0) return;
    SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(tex, c.a);
    SDL_Rect dst{x, y, size, size};
    SDL_RenderCopyEx(ren, tex, nullptr, &dst, 0.0, nullptr, flip);
}

/** Cached mask lookup, or a null texture if the build failed. */
static SDL_Texture *maskCacheGet(SDL_Renderer *ren, int kind, int a, int b,
                                 long long &key)
{
    key = maskKey(kind, a, b);
    auto &cache = g_masks[ren];
    auto it = cache.find(key);
    return (it == cache.end()) ? nullptr : it->second;
}

static SDL_Texture *maskCachePut(SDL_Renderer *ren, long long key, SDL_Surface *cov,
                                 bool linear = false)
{
    return g_masks[ren][key] = uploadMask(ren, cov, linear);
}

/* ── the shipped rasterisers, verbatim, in whatever units the renderer is in ─
 * Kept as the mask bakers so a shape's silhouette cannot drift from the one
 * that was hand-tuned on screen. */

static void fillCircleRaw(SDL_Renderer *ren, int cx, int cy, int rad)
{
    if (rad <= 0) return;
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = (int)std::sqrt((float)(rad * rad - dy * dy));
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void fillRoundRectRaw(SDL_Renderer *ren, SDL_Rect rc, int rad, SDL_Color c)
{
    if (rc.w <= 0 || rc.h <= 0) return;
    if (rad < 0) rad = 0;
    if (rad > rc.w / 2) rad = rc.w / 2;
    if (rad > rc.h / 2) rad = rc.h / 2;
    setColor(ren, c);
    if (rad == 0) { SDL_RenderFillRect(ren, &rc); return; }

    /* Straight band in the middle — one call, no overlap with the corners. */
    SDL_Rect mid{rc.x, rc.y + rad, rc.w, rc.h - rad * 2};
    if (mid.h > 0) SDL_RenderFillRect(ren, &mid);

    for (int i = 0; i < rad; i++) {
        /* dy = distance from the corner circle's centre, in pixel centres */
        int dy = rad - 1 - i;
        int dx = (int)(std::sqrt((double)(rad * rad - dy * dy)) + 0.5);
        int inset = rad - dx;
        SDL_RenderDrawLine(ren, rc.x + inset, rc.y + i,
                                rc.x + rc.w - 1 - inset, rc.y + i);
        SDL_RenderDrawLine(ren, rc.x + inset, rc.y + rc.h - 1 - i,
                                rc.x + rc.w - 1 - inset, rc.y + rc.h - 1 - i);
    }
}

/* ── mask bakers ─────────────────────────────────────────────────────────── */

/** A filled disc of device radius R. Built from the shipped fillCircle, so the
 * silhouette — including its one-pixel-wide poles, which is what a circle of
 * radius R in integer ROW space actually looks like — is unchanged. */
static SDL_Texture *discMask(SDL_Renderer *ren, int R)
{
    if (R <= 0) return nullptr;
    long long key;
    if (SDL_Texture *hit = maskCacheGet(ren, MK_DISC, R, 0, key)) return hit;

    const int n = 2 * R + 1;
    SDL_Surface *cov = nullptr;
    {
        MaskCanvas cv(n * MASK_SS, n * MASK_SS);
        if (cv.ren) {
            /* Centre the disc on the MIDDLE of output pixel R's supersample
             * block, not on its first canvas pixel. coverageFrom turns block
             * [4R,4R+4) into output pixel R, and blitMask puts that pixel on
             * the device pixel containing (cx,cy) — so the block's centre is
             * canvas 4R+MASK_SS/2. Baking at 4R instead leaves the disc 1.5
             * canvas px (0.375 device px) up-left of centre, which shows up as
             * a lopsided coverage ramp: measured centroid R-0.376 instead of R,
             * against R+0.126 for this placement (0.5 canvas px is the floor
             * for an odd-diameter disc). */
            fillCircleRaw(cv.ren, R * MASK_SS + MASK_SS / 2,
                                  R * MASK_SS + MASK_SS / 2, R * MASK_SS);
            cov = coverageFrom(cv.surf, n, n);
        }
    }
    return maskCachePut(ren, key, cov);
}

/** The TOP-LEFT quarter of a rounded rect of device radius R. The other three
 * corners are this one flipped, so one mask serves all four. */
static SDL_Texture *cornerMask(SDL_Renderer *ren, int R)
{
    if (R <= 0) return nullptr;
    long long key;
    if (SDL_Texture *hit = maskCacheGet(ren, MK_CORNER, R, 0, key)) return hit;

    SDL_Surface *cov = nullptr;
    {
        const int d = 2 * R * MASK_SS;
        MaskCanvas cv(d, d);
        if (cv.ren) {
            fillRoundRectRaw(cv.ren, SDL_Rect{0, 0, d, d}, R * MASK_SS, WHITE);
            cov = coverageFrom(cv.surf, R, R);
        }
    }
    return maskCachePut(ren, key, cov);
}

/** Half-extent of a ring mask, in device pixels — shared by the baker and the
 * blitter so the two can never disagree about the centre. */
static int ringHalf(int R, int thick)
{
    return R + (thick + 1) / 2 + 1;
}

/** A circle outline of device radius R and thickness 'thick'.
 *
 * Analytic rather than baked from the shipped 48-chord polygon: a 48-gon of
 * hard steps is exactly the artefact this layer exists to remove, and the ring
 * is a new shape with no hand-tuned silhouette to stay faithful to. */
static SDL_Texture *ringMask(SDL_Renderer *ren, int R, int thick)
{
    if (R <= 0) return nullptr;
    if (thick < 1) thick = 1;
    long long key;
    if (SDL_Texture *hit = maskCacheGet(ren, MK_RING, R, thick, key)) return hit;

    const int half = ringHalf(R, thick);
    const int n = 2 * half + 1;
    SDL_Surface *cov = SDL_CreateRGBSurfaceWithFormat(0, n, n, 32,
                                                      SDL_PIXELFORMAT_ARGB8888);
    if (!cov) return maskCachePut(ren, key, nullptr);
    SDL_LockSurface(cov);
    Uint32 *op = (Uint32 *)cov->pixels;
    const int stride = cov->pitch / 4;
    const double outer = R + thick * 0.5, inner = R - thick * 0.5;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            const double dx = i - half, dy = j - half;
            const double d  = std::sqrt(dx * dx + dy * dy);
            /* signed distance -> coverage, one pixel wide each side */
            double a = 0.5 + (outer - d);
            if (a > 1.0) a = 1.0;
            if (a < 0.0) a = 0.0;
            double b = 0.5 + (d - inner);
            if (b > 1.0) b = 1.0;
            if (b < 0.0) b = 0.0;
            op[(size_t)j * stride + i] =
                ((Uint32)(a * b * 255.0 + 0.5) << 24) | 0x00FFFFFFu;
        }
    SDL_UnlockSurface(cov);
    return maskCachePut(ren, key, cov);
}

/* ── the public shape primitives ─────────────────────────────────────────── */

/** Fill a device rect at 'k' of the colour's own alpha. */
static void fillScaled(SDL_Renderer *ren, int x, int y, int w, int h,
                       SDL_Color c, double k)
{
    if (w <= 0 || h <= 0 || k <= 0.0) return;
    double a = (double)c.a * (k > 1.0 ? 1.0 : k) + 0.5;
    if (a < 1.0) return;
    SDL_Color s = c;
    s.a = (Uint8)(a > 255.0 ? 255.0 : a);
    SDL_SetRenderDrawColor(ren, s.r, s.g, s.b, s.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

/**
 * An axis-aligned rect with exact fractional-edge coverage, in DEVICE units.
 *
 * A plain RenderFillRect can only land on whole device pixels, so a 1-logical-px
 * hairline at 1.25x would be painted as two full-strength rows where it should
 * be one full row and one quarter-strength — 60 % too heavy, and the divider
 * stack would read as chunky on any fractional-DPI display. (Measured: as
 * shipped the stack came out 17 rows of 1px and 5 of 2px; snapping it to whole
 * pixels made it 22 rows of 2px — even, but wrong in weight. This gives a
 * uniform 1.25px.)
 *
 * Coverage of an axis-aligned rect is separable, and only the outermost device
 * row and column can be partial, so this is at most nine fills, not a mask.
 */
static void fillRectCov(SDL_Renderer *ren, double x0, double y0, double x1, double y1,
                        SDL_Color c)
{
    const int ix0 = (int)std::floor(x0), ix1 = (int)std::ceil(x1);
    const int iy0 = (int)std::floor(y0), iy1 = (int)std::ceil(y1);
    const int w = ix1 - ix0, h = iy1 - iy0;
    if (w <= 0 || h <= 0) return;

    /* Bands along one axis: {origin, length, coverage}. A single-pixel band has
     * no interior, so it carries the whole (<= 1) coverage itself. */
    int    o[3], len[3], n;
    double k[3];
    auto split = [&](int i0, int i1, int extent, double f0, double f1) {
        if (extent == 1) { o[0] = i0; len[0] = 1; k[0] = f1 - f0; return 1; }
        int m = 0;
        o[m] = i0; len[m] = 1; k[m] = i0 + 1 - f0; m++;
        if (extent > 2) { o[m] = i0 + 1; len[m] = extent - 2; k[m] = 1.0; m++; }
        o[m] = i1 - 1; len[m] = 1; k[m] = f1 - (i1 - 1); m++;
        return m;
    };
    const int nx = split(ix0, ix1, w, x0, x1), xo[3] = {o[0], o[1], o[2]};
    const int xl[3] = {len[0], len[1], len[2]};
    const double xk[3] = {k[0], k[1], k[2]};
    n = split(iy0, iy1, h, y0, y1);
    const int yo[3] = {o[0], o[1], o[2]};
    const int yl[3] = {len[0], len[1], len[2]};
    const double yk[3] = {k[0], k[1], k[2]};

    for (int j = 0; j < n; j++)
        for (int i = 0; i < nx; i++)
            fillScaled(ren, xo[i], yo[j], xl[i], yl[j], c, xk[i] * yk[j]);
}

/** Filled disc centred on (cx,cy), radius 'rad' LOGICAL units. Draws in the
 * colour most recently passed to setColor(). */
static void fillCircle(SDL_Renderer *ren, int cx, int cy, int rad)
{
    if (rad <= 0) return;
    if (g_baking) { fillCircleRaw(ren, cx, cy, rad); return; }
    const int R = uiPx(rad);
    if (R <= 0) return;
    ScaleScope scope(ren);
    blitMask(ren, discMask(ren, R), uiPx(cx) - R, uiPx(cy) - R, 2 * R + 1, g_drawColor);
}

/**
 * Rounded rect as three abutting device-aligned bands plus four corner masks.
 *
 * Not mid-rect + 2 side rects + 4 circles: that construction overlapped itself,
 * which is invisible at alpha 255 but paints visible darker seams wherever a
 * translucent fill is used (the tint chips, the shadow, the hairlines). The
 * bands here share their edges exactly — same uiDevLo/uiDevHi pair — so an
 * opaque fill is seamless and a translucent one is uniform. Honours c.a, so the
 * renderer must have SDL_BLENDMODE_BLEND set (done in initRenderer()).
 */
static void fillRoundRect(SDL_Renderer *ren, SDL_Rect rc, int rad, SDL_Color c)
{
    if (rc.w <= 0 || rc.h <= 0) return;
    if (rad < 0) rad = 0;
    if (rad > rc.w / 2) rad = rc.w / 2;
    if (rad > rc.h / 2) rad = rc.h / 2;
    if (g_baking) { fillRoundRectRaw(ren, rc, rad, c); return; }

    const SDL_Rect d = uiDevRect(rc.x, rc.y, rc.w, rc.h);
    if (d.w <= 0 || d.h <= 0) return;
    ScaleScope scope(ren);
    setColor(ren, c);

    int dr = uiPx(rad);
    if (dr > d.w / 2) dr = d.w / 2;
    if (dr > d.h / 2) dr = d.h / 2;
    /* No corners to butt against, so the edges can keep their fractional
     * coverage instead of snapping out to whole device pixels. */
    if (dr <= 0) {
        fillRectCov(ren, rc.x * g_uiScale, rc.y * g_uiScale,
                    (rc.x + rc.w) * g_uiScale, (rc.y + rc.h) * g_uiScale, c);
        return;
    }

    const SDL_Rect mid{d.x, d.y + dr, d.w, d.h - 2 * dr};
    const SDL_Rect top{d.x + dr, d.y, d.w - 2 * dr, dr};
    const SDL_Rect bot{d.x + dr, d.y + d.h - dr, d.w - 2 * dr, dr};
    if (mid.h > 0) SDL_RenderFillRect(ren, &mid);
    if (top.w > 0) {
        SDL_RenderFillRect(ren, &top);
        SDL_RenderFillRect(ren, &bot);
    }

    SDL_Texture *cm = cornerMask(ren, dr);
    if (!cm) return;
    const int x1 = d.x + d.w - dr, y1 = d.y + d.h - dr;
    blitMask(ren, cm, d.x, d.y, dr, c);
    blitMask(ren, cm, x1,  d.y, dr, c, SDL_FLIP_HORIZONTAL);
    blitMask(ren, cm, d.x, y1,  dr, c, SDL_FLIP_VERTICAL);
    blitMask(ren, cm, x1,  y1,  dr, c,
             (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));
}

static void drawRoundRect(SDL_Renderer *ren, SDL_Rect rc, int rad,
                          SDL_Color fill, SDL_Color border)
{
    fillRoundRect(ren, rc, rad, border);
    SDL_Rect in{rc.x + 1, rc.y + 1, rc.w - 2, rc.h - 2};
    if (in.w > 0 && in.h > 0)
        fillRoundRect(ren, in, rad > 0 ? rad - 1 : 0, fill);
}

/** Soft drop shadow: three stacked translucent rounded rects, offset down. */
static void drawShadow(SDL_Renderer *ren, SDL_Rect rc, int rad)
{
    static const int spread[3] = {6, 4, 2};
    static const int weight[3] = {12, 20, 28};   /* out of 64 */
    const SDL_Color base = THEME.shadow;
    for (int i = 0; i < 3; i++) {
        SDL_Color c = base;
        int a = (int)base.a * weight[i] / 64;
        c.a = (Uint8)(a > 255 ? 255 : a);
        if (c.a == 0) continue;
        SDL_Rect r{rc.x - spread[i], rc.y,
                   rc.w + spread[i] * 2, rc.h + spread[i] * 2};
        fillRoundRect(ren, r, rad + spread[i], c);
    }
}

/** Card / panel: soft shadow + surface fill. No border (UU-style flat cards). */
static void drawCard(SDL_Renderer *ren, SDL_Rect rc, int rad)
{
    drawShadow(ren, rc, rad);
    fillRoundRect(ren, rc, rad, THEME.surface);
}

/** A 1px divider at 25% — a full-strength rule would read as a control edge. */
static void drawHairline(SDL_Renderer *ren, int x, int y, int w, int h, SDL_Color c)
{
    SDL_Color d = c;
    d.a = 0x40;
    fillRoundRect(ren, SDL_Rect{x, y, w, h}, 0, d);
}

/** 1px-ish circle outline. Was 48 hard line segments (SDL has no arc
 * primitive); now a cached annulus mask, which is rounder and one draw call. */
static void drawRing(SDL_Renderer *ren, int cx, int cy, int r, SDL_Color c)
{
    if (r <= 0) return;
    const int R = uiPx(r);
    const int thick = uiPx(1);
    if (R <= 0) return;
    ScaleScope scope(ren);
    SDL_Texture *m = ringMask(ren, R, thick);
    if (!m) return;
    const int half = ringHalf(R, thick);
    blitMask(ren, m, uiPx(cx) - half, uiPx(cy) - half, 2 * half + 1, c);
}

/** iOS-style toggle. The OFF track uses the border token: at surfaceAlt or
 * press the white knob had almost no edge against the card behind it. */
static void drawToggle(SDL_Renderer *ren, SDL_Rect rc, bool on)
{
    int rad = rc.h / 2;
    fillRoundRect(ren, rc, rad, on ? SWITCH_ON : THEME.border);
    setColor(ren, WHITE);
    fillCircle(ren, on ? rc.x + rc.w - rad : rc.x + rad, rc.y + rad, rad - 2);
}

/* ── icons ────────────────────────────────────────────────────────────────
 * Every glyph is drawn from the same primitives as the rest of the UI — no
 * image assets, no icon font, no SVG. Each is centred on (cx, cy) in a
 * nominal 'size' box so call sites can place them uniformly.
 */
static void glyphMonitor(SDL_Renderer *ren, int cx, int cy, int size, SDL_Color c)
{
    int w = size, h = size;
    int sh = (int)(h * 0.66);
    int top = cy - h / 2;
    fillRoundRect(ren, SDL_Rect{cx - w / 2, top, w, sh}, 3, c);
    fillRoundRect(ren, SDL_Rect{cx - 1, top + sh, 2, (int)(h * 0.20)}, 1, c);
    fillRoundRect(ren, SDL_Rect{cx - w / 4, cy + h / 2 - 2, w / 2, 2}, 1, c);
}

static void glyphArrow(SDL_Renderer *ren, int cx, int cy, int size, bool up, SDL_Color c)
{
    setColor(ren, c);
    int half = size / 2, head = size / 3;
    int yTip = up ? cy - half : cy + half;
    int yEnd = up ? cy + half : cy - half;
    int yWing = yTip + (up ? head : -head);
    for (int i = 0; i < 2; i++) {          /* 2px stem, readable at 18px */
        SDL_RenderDrawLine(ren, cx + i, yEnd, cx + i, yTip);
        SDL_RenderDrawLine(ren, cx - head, yWing + (up ? i : -i), cx + i, yTip);
        SDL_RenderDrawLine(ren, cx + head, yWing + (up ? i : -i), cx + i, yTip);
    }
}

static void glyphSliders(SDL_Renderer *ren, int cx, int cy, int size, SDL_Color c)
{
    int w = size;
    int x0 = cx - w / 2;
    int y1 = cy - size / 4, y2 = cy + size / 4;
    setColor(ren, c);
    for (int i = 0; i < 2; i++) {
        SDL_RenderDrawLine(ren, x0, y1 + i, x0 + w, y1 + i);
        SDL_RenderDrawLine(ren, x0, y2 + i, x0 + w, y2 + i);
    }
    fillCircle(ren, x0 + w / 3, y1, 3);
    fillCircle(ren, x0 + (w * 2) / 3, y2, 3);
}

static void glyphPower(SDL_Renderer *ren, int cx, int cy, int size, SDL_Color c)
{
    int r = size / 2;
    setColor(ren, c);
    /* Ring with a gap at the top, then the stem through that gap. */
    const double gap   = 0.80;
    const double start = -UI_PI / 2 + gap / 2;
    const double end   = 3 * UI_PI / 2 - gap / 2;
    const int steps = 40;
    int px = cx + (int)std::lround(r * std::cos(start));
    int py = cy + (int)std::lround(r * std::sin(start));
    for (int i = 1; i <= steps; i++) {
        double a = start + (end - start) * i / steps;
        int x = cx + (int)std::lround(r * std::cos(a));
        int y = cy + (int)std::lround(r * std::sin(a));
        SDL_RenderDrawLine(ren, px, py, x, y);
        px = x; py = y;
    }
    for (int i = 0; i < 2; i++)
        SDL_RenderDrawLine(ren, cx + i, cy - r - 2, cx + i, cy + 1);
}

/* ── appearance glyphs ────────────────────────────────────────────────────
 * Sun and moon for the header switcher; 跟随系统 reuses the monitor, which is
 * the desktop convention (Windows Terminal, VS Code, Chrome, Firefox all mark
 * their "system" option with a display) and is a good deal more legible at this
 * size than macOS's half-filled circle. Neither glyph needs a knockout colour —
 * the moon is subtracted geometrically — which matters because they sit on a
 * cell whose fill changes with the selected / hovered / pressed state, so a
 * knockout would have to be told which of six backgrounds to punch through.
 */
static void glyphSun(SDL_Renderer *ren, int cx, int cy, int size, SDL_Color c)
{
    /* The core is deliberately small: at size/4 the rays came out shorter than
     * the disc was wide and the mark read as a dashed circle rather than a sun. */
    const int core = (size / 6 > 2) ? size / 6 : 2;
    const int r0   = core + 1;          /* rays start at the core's edge       */
    const int r1   = size / 2;          /* ...and stop at the nominal box      */
    setColor(ren, c);
    fillCircle(ren, cx, cy, core);
    /* Rays are 2x2 blocks stamped along the direction rather than two parallel
     * 1px lines: on the diagonals the two lines land 1.41 px apart and leave a
     * visibly dashed, lighter ray, so the eight rays did not match in weight. */
    for (int i = 0; i < 8; i++) {
        const double a  = UI_PI * i / 4.0;
        const double ca = std::cos(a), sa = std::sin(a);
        for (int t = r0; t <= r1; t++) {
            SDL_Rect px{cx + (int)std::lround(ca * t), cy + (int)std::lround(sa * t), 2, 2};
            SDL_RenderFillRect(ren, &px);
        }
    }
}

static void glyphMoon(SDL_Renderer *ren, int cx, int cy, int size, SDL_Color c)
{
    const int r  = size / 2;
    const int bx = cx + (r * 5) / 10;   /* the bite, up and to the right        */
    const int by = cy - (r * 15) / 100;
    setColor(ren, c);
    for (int y = -r; y <= r; y++) {
        const int a = (int)std::sqrt((double)(r * r - y * y));
        int lo = cx - a, hi = cx + a;
        const int d = (cy + y) - by;
        if (std::abs(d) <= r) {
            const int b  = (int)std::sqrt((double)(r * r - d * d));
            const int bl = bx - b, bh = bx + b;
            if (bl <= lo && bh >= hi)     continue;        /* row fully eaten  */
            else if (bl <= lo)            lo = bh + 1;     /* eaten from left  */
            else if (bh >= hi)            hi = bl - 1;     /* eaten from right */
            else if (bl - lo >= hi - bh)  hi = bl - 1;     /* split: keep wider*/
            else                          lo = bh + 1;
        }
        if (lo <= hi) SDL_RenderDrawLine(ren, lo, cy + y, hi, cy + y);
    }
}

/* Update: a circular arrow. The obvious alternative — an arrow dropping into a
 * tray — is exactly what 接收 already looks like (a down arrow), and 发送 owns
 * the up arrow, so anything straight would read as a second copy of one of the
 * home-screen cards sitting in the header. Drawn 2px thick to match glyphArrow
 * at 18px, which is the only size the header asks for. */
static void glyphUpdate(SDL_Renderer *ren, int cx, int cy, int size, SDL_Color c)
{
    const double kPi  = 3.14159265358979;
    const double r    = size / 2.0 - 1.0;
    const double gap  = 0.50;                       /* half-opening, radians */
    const double a0   = -kPi / 2.0 + gap;           /* open at the top       */
    const double a1   = a0 + 2.0 * kPi - 2.0 * gap;
    const int    seg  = 28;
    setColor(ren, c);
    double px = 0, py = 0;
    for (int i = 0; i <= seg; i++) {
        const double a = a0 + (a1 - a0) * i / seg;
        const double x = cx + std::cos(a) * r;
        const double y = cy + std::sin(a) * r;
        if (i) {
            for (int t = 0; t < 2; t++)             /* 2px stroke, as elsewhere */
                SDL_RenderDrawLine(ren, (int)std::lround(px) + t, (int)std::lround(py),
                                   (int)std::lround(x) + t, (int)std::lround(y));
        }
        px = x; py = y;
    }
    /* Arrowhead closing the sweep, tip at the end of the arc and pointing along
     * it. The head is a closed triangle, not two open barbs: at 18px a barb is
     * only ~5px long, so the open form reads as a tail hanging off the ring
     * rather than as an arrow. hlen/angle are deliberately tight — a wide head
     * stops looking like an arrowhead and starts looking like a bend. */
    const double hx = cx + std::cos(a1) * r;
    const double hy = cy + std::sin(a1) * r;
    const double tx = -std::sin(a1), ty = std::cos(a1);
    const double hlen = size * 0.30, hrot = 0.52;
    double bx[2], by[2];
    for (int k = 0; k < 2; k++) {
        const double rot = (k ? 1.0 : -1.0) * hrot;
        const double dx = tx * std::cos(rot) - ty * std::sin(rot);
        const double dy = tx * std::sin(rot) + ty * std::cos(rot);
        bx[k] = hx - dx * hlen;
        by[k] = hy - dy * hlen;
        for (int t = 0; t < 2; t++)
            SDL_RenderDrawLine(ren, (int)std::lround(hx) + t, (int)std::lround(hy),
                               (int)std::lround(bx[k]) + t, (int)std::lround(by[k]));
    }
    for (int t = 0; t < 2; t++)
        SDL_RenderDrawLine(ren, (int)std::lround(bx[0]) + t, (int)std::lround(by[0]),
                           (int)std::lround(bx[1]) + t, (int)std::lround(by[1]));
}

/** The warning triangle's ink, as a COVERAGE mask: white where the mark is,
 * transparent where the exclamation punches through. The knockout is written
 * with blending off rather than painted in the surface colour — a mask has no
 * colour to paint it with, and the screen code lays the real knockout rects
 * over the blitted triangle afterwards. 'u' is one logical unit in canvas
 * units, so the 3px bar and dot stay 3 logical px wide at any supersampling. */
static void warningInk(SDL_Renderer *ren, int cx, int cy, int size, int u)
{
    int top = cy - size / 2, bot = cy + size / 2;
    int half = size / 2;
    setColor(ren, WHITE);
    for (int y = top; y <= bot; y++) {
        int w = (y - top) * half / size;
        SDL_RenderDrawLine(ren, cx - w, y, cx + w, y);
    }
    SDL_BlendMode prev;
    SDL_GetRenderDrawBlendMode(ren, &prev);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    SDL_Rect bar{cx - u, cy - size / 4, 3 * u, size / 2};
    SDL_Rect dot{cx - u, cy + size / 3, 3 * u, 3 * u};
    SDL_RenderFillRect(ren, &bar);
    SDL_RenderFillRect(ren, &dot);
    SDL_SetRenderDrawBlendMode(ren, prev);
}

/* ── glyph masks ─────────────────────────────────────────────────────────── */

/** Half-extent, in LOGICAL units, of the square a glyph is baked into.
 *
 * Glyphs are centred on (cx,cy) but several overrun the nominal 'size' box —
 * the power symbol's stem starts 2px above it and the update arrow's head
 * reaches ~0.8x size from the centre — so the box is generous rather than
 * tight, and the wasted transparent margin costs nothing (the mask is cached). */
static int glyphHalf(int size)
{
    return (size * 4) / 5 + 3;
}

/** Ink only: draws a glyph body centred on (cx,cy) in whatever units the
 * renderer is in. Used by the mask baker and never by the screen code, which
 * blits the baked mask instead. */
static void paintGlyph(SDL_Renderer *ren, int glyph, int cx, int cy, int size)
{
    switch (glyph) {
    case GLYPH_MONITOR:    glyphMonitor(ren, cx, cy, size, WHITE); break;
    case GLYPH_ARROW_UP:   glyphArrow(ren, cx, cy, size, true,  WHITE); break;
    case GLYPH_ARROW_DOWN: glyphArrow(ren, cx, cy, size, false, WHITE); break;
    case GLYPH_SLIDERS:    glyphSliders(ren, cx, cy, size, WHITE); break;
    case GLYPH_POWER:      glyphPower(ren, cx, cy, size, WHITE); break;
    case GLYPH_SUN:        glyphSun(ren, cx, cy, size, WHITE); break;
    case GLYPH_MOON:       glyphMoon(ren, cx, cy, size, WHITE); break;
    case GLYPH_UPDATE:     glyphUpdate(ren, cx, cy, size, WHITE); break;
    default: break;
    }
}

/** The mask a glyph is drawn from.
 *
 * A glyph body's STROKE WIDTH is not a parameter — glyphArrow draws its 2px
 * stem by calling RenderDrawLine twice, one pixel apart, and the power ring is
 * one RenderDrawLine per chord. So one canvas pixel must be one LOGICAL pixel:
 * rasterising the body at MASK_SS x its logical size scales its coordinates but
 * leaves every stroke one canvas pixel wide, which comes out of the downsample
 * as a quarter-pixel hairline and turns the power ring almost invisible.
 *
 * There is therefore no supersampling here. The body is baked at the resolution
 * it was designed at — exactly what the old device-scale draw did — and the
 * mask is blitted up to device size with linear filtering, which is what turns
 * the hard staircase into a coverage ramp at a fractional display scale. At
 * scale 1.0 the blit is 1:1 and the result is bit-identical to the old draw.
 */
static SDL_Texture *glyphMask(SDL_Renderer *ren, int glyph, int size, int &halfLog)
{
    const int hl = glyphHalf(size);
    halfLog = hl;
    long long key;
    if (SDL_Texture *hit = maskCacheGet(ren, MK_GLYPH, glyph, size, key)) return hit;

    const int n = 2 * hl + 1;
    SDL_Surface *cov = nullptr;
    {
        MaskCanvas cv(n, n);
        if (cv.ren) {
            g_baking = true;
            paintGlyph(cv.ren, glyph, hl, hl, size);
            g_baking = false;
            /* No downsample (ss=1): the mask is already a 0/255 silhouette, and
             * it is the linear upscale at blit time that turns it into
             * coverage. It still goes through coverageFrom so the transparent
             * pixels carry WHITE rgb — see the note there. */
            cov = coverageFrom(cv.surf, n, n, 1);
        }
    }
    return maskCachePut(ren, key, cov, true);
}

static SDL_Texture *warnMask(SDL_Renderer *ren, int size, int &halfLog)
{
    const int hl = glyphHalf(size);
    halfLog = hl;
    long long key;
    if (SDL_Texture *hit = maskCacheGet(ren, MK_WARN, size, 0, key)) return hit;

    const int n = 2 * hl + 1;
    SDL_Surface *cov = nullptr;
    {
        MaskCanvas cv(n, n);
        if (cv.ren) {
            warningInk(cv.ren, hl, hl, size, 1);
            cov = coverageFrom(cv.surf, n, n, 1);
        }
    }
    return maskCachePut(ren, key, cov, true);
}

static void drawGlyph(SDL_Renderer *ren, int glyph, int cx, int cy, int size, SDL_Color c)
{
    if (glyph == GLYPH_NONE || size <= 0) return;
    ScaleScope scope(ren);
    int hl = 0;
    SDL_Texture *m = glyphMask(ren, glyph, size, hl);
    if (!m) return;
    const int hd = uiPx(hl);
    blitMask(ren, m, uiPx(cx) - hd, uiPx(cy) - hd, 2 * hd + 1, c);
}

static void glyphWarning(SDL_Renderer *ren, int cx, int cy, int size, SDL_Color c,
                         SDL_Color knockout)
{
    if (size <= 0) return;
    ScaleScope scope(ren);
    int hl = 0;
    SDL_Texture *m = warnMask(ren, size, hl);
    if (m) {
        const int hd = uiPx(hl);
        blitMask(ren, m, uiPx(cx) - hd, uiPx(cy) - hd, 2 * hd + 1, c);
    }
    /* The exclamation is two axis-aligned rects, so it needs no mask — but it
     * still has to be snapped to device pixels or its 3px bar lands on 3.75. */
    setColor(ren, knockout);
    SDL_Rect bar = uiDevRect(cx - 1, cy - size / 4, 3, size / 2);
    SDL_Rect dot = uiDevRect(cx - 1, cy + size / 3, 3, 3);
    SDL_RenderFillRect(ren, &bar);
    SDL_RenderFillRect(ren, &dot);
}

/* ── buttons ─────────────────────────────────────────────────────────────── */
static void drawButton(SDL_Renderer *ren, const Button &b)
{
    if (b.variant == BTN_V_GHOST) return;      /* hit area only, caller paints */

    const bool hot   = (g_hot == b.id);
    const bool press = (g_press == b.id);
    const int  r     = THEME.radius;
    /* Buttons use the bold small face; a caller may override for tight cells. */
    TTF_Font *f = b.font ? b.font : g_fontBS;

    auto centred = [&](SDL_Color c) {
        drawTextV(ren, b.label, b.rect.x + b.rect.w / 2, b.rect.y + b.rect.h / 2,
                  c, f, ALIGN_CENTER);
    };
    /* Hover is a translucent wash laid over the variant's own fill. The wash
     * must move that fill AWAY from the label colour — darken under a white
     * label, lighten under a dark one — or hovering would erode the label's
     * contrast (a white wash on the primary fill drops it to 3.72:1, and a
     * dark wash on a tint chip to 4.17:1). */
    auto wash = [&](SDL_Color c, Uint8 a) {
        if (!hot || press) return;
        fillRoundRect(ren, b.rect, r, withAlpha(c, a));
    };

    switch (b.variant) {
    case BTN_V_NAV: {
        if (b.on) {
            fillRoundRect(ren, b.rect, r, THEME.primary);
            SDL_Rect bar{b.rect.x - 9, b.rect.y + 9, 3, b.rect.h - 18};
            fillRoundRect(ren, bar, 2, THEME.accent);
        } else if (press) {
            fillRoundRect(ren, b.rect, r, THEME.press);
        } else if (hot) {
            /* surfaceAlt, not hover: the inactive label is textDim, and
             * textDim on the darker hover fill is only 4.46:1 */
            fillRoundRect(ren, b.rect, r, THEME.surfaceAlt);
        }
        SDL_Color fg = b.on ? THEME.onPrimary : THEME.textDim;
        if (b.glyph != GLYPH_NONE)
            drawGlyph(ren, b.glyph, b.rect.x + 23, b.rect.y + b.rect.h / 2, 18, fg);
        drawTextV(ren, b.label, b.rect.x + 42, b.rect.y + b.rect.h / 2,
                  fg, g_fontBS, ALIGN_LEFT);
        break;
    }

    case BTN_V_CHOICE: {
        /* A standalone selectable cell: it owns its border, so a row of them
         * reads as a set of chips rather than as a track with a thumb.
         * 'framed' = false is the same cell without that border, for the
         * header switcher, whose three cells sit on a track that is already a
         * frame — three borders inside one read as nested boxes. The fill is
         * unchanged either way, and there it matches the track's own, so an
         * unselected cell shows only its glyph. */
        const SDL_Color fg = b.on ? THEME.onPrimary : THEME.text;
        if (b.on) {
            fillRoundRect(ren, b.rect, r - 3, THEME.primary);
            /* Pressing the cell that is ALREADY selected has to respond too —
             * it used to fall through this branch and do nothing at all.
             * Black, not THEME.text: a white wash would lighten the primary
             * fill and take the white label down to 3.2:1. */
            if (press)
                fillRoundRect(ren, b.rect, r - 3, withAlpha(BLACK, 0x26));
        } else if (!b.framed) {
            fillRoundRect(ren, b.rect, r - 3, THEME.surfaceAlt);
            if (press)
                fillRoundRect(ren, b.rect, r - 3, withAlpha(THEME.text, 0x2E));
            else if (hot)
                fillRoundRect(ren, b.rect, r - 3, withAlpha(THEME.text, 0x16));
        } else {
            drawRoundRect(ren, b.rect, r - 3, THEME.surfaceAlt, THEME.border);
            /* Resting fill is surfaceAlt, which is also the colour of the track
             * this cell sits on in the header, so a token-based hover moved the
             * fill by 1.06:1 — invisible. THEME.hover is tuned for a cell on a
             * white card and cannot serve both. A wash of the ink colour works
             * on either background and in either mode (black in light, white in
             * dark) and costs no legibility: the icon starts ~11:1 from the
             * fill, so moving toward it still leaves well over AA. */
            if (press)
                fillRoundRect(ren, b.rect, r - 3, withAlpha(THEME.text, 0x2E));
            else if (hot)
                fillRoundRect(ren, b.rect, r - 3, withAlpha(THEME.text, 0x16));
        }
        if (b.glyph != GLYPH_NONE) {
            /* Icon-only cell (the header switcher): the glyph is sized from the
             * cell so a change to SEG_H carries through to the artwork. */
            int gs = b.rect.h - 6;
            if (gs < 14) gs = 14;
            if (gs > 20) gs = 20;
            drawGlyph(ren, b.glyph, b.rect.x + b.rect.w / 2,
                      b.rect.y + b.rect.h / 2, gs, fg);
        } else {
            centred(fg);
        }
        break;
    }

    case BTN_V_CHIP_OK:
    case BTN_V_CHIP_ACC: {
        const bool ok = (b.variant == BTN_V_CHIP_OK);
        fillRoundRect(ren, b.rect, r - 3, ok ? THEME.successTint : THEME.primaryTint);
        wash(WHITE, 0x1F);                 /* light fill, dark label → lighten */
        centred(ok ? THEME.onSuccessTint : THEME.onPrimaryTint);
        break;
    }

    case BTN_V_PRIMARY: {
        fillRoundRect(ren, b.rect, r,
                      (hot || press) ? THEME.primaryHover : THEME.primary);
        centred(THEME.onPrimary);
        break;
    }

    case BTN_V_DANGER: {
        fillRoundRect(ren, b.rect, r, DANGER_SOLID);
        wash(BLACK, 0x1F);                 /* white label → darken */
        centred(WHITE);
        break;
    }

    case BTN_V_LINK: {
        wash(THEME.danger, 0x14);
        centred(THEME.danger);
        break;
    }

    case BTN_V_SECONDARY:
    default: {
        drawRoundRect(ren, b.rect, r,
                      press ? THEME.press : (hot ? THEME.hover : THEME.surfaceAlt),
                      THEME.border);
        centred(THEME.text);
        break;
    }
    }
}

static void addButton(int x, int y, int w, int h, const std::string &label, int id,
                      int variant = BTN_V_SECONDARY, int glyph = GLYPH_NONE,
                      TTF_Font *font = nullptr)
{
    Button b;
    b.rect    = SDL_Rect{x, y, w, h};
    b.label   = label;
    b.id      = id;
    b.variant = variant;
    b.glyph   = glyph;
    b.font    = font;
    g_buttons.push_back(b);
}

/** A selectable cell; 'on' fills it with the accent. Pass a glyph and leave
 * 'label' empty for an icon-only cell (the header appearance switcher). */
static void addChoice(int x, int y, int w, int h, const std::string &label,
                      int id, bool on, TTF_Font *font = nullptr,
                      int glyph = GLYPH_NONE)
{
    Button b;
    b.rect    = SDL_Rect{x, y, w, h};
    b.label   = label;
    b.id      = id;
    b.variant = BTN_V_CHOICE;
    b.glyph   = glyph;
    b.on      = on;
    b.font    = font;
    g_buttons.push_back(b);
}

/** A cell of the header appearance switcher: a BTN_V_CHOICE that paints no
 * border of its own, because the track underneath already frames the three of
 * them (see the framed flag in drawButton). */
static void addSegment(int x, int y, int w, int h, int id, bool on, int glyph)
{
    addChoice(x, y, w, h, "", id, on, nullptr, glyph);
    g_buttons.back().framed = false;
}

/** A nav-rail item; 'on' marks the active tab. */
static void addNav(int y, const std::string &label, int id, int glyph, bool on)
{
    Button b;
    b.rect    = SDL_Rect{NAV_ITEM_X, y, NAV_ITEM_W, NAV_ITEM_H};
    b.label   = label;
    b.id      = id;
    b.variant = BTN_V_NAV;
    b.glyph   = glyph;
    b.on      = on;
    g_buttons.push_back(b);
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

/* ── update helpers ──────────────────────────────────────────────────────── */
static bool updateIsIgnored(const UpdateInfo &ui)
{
    return ui.state == UpdateState::Available && !g_cfg.ignored_version.empty() &&
           ui.remote_tag == g_cfg.ignored_version;
}

/** True while the header update button has something to say: a release is
 * waiting (or ignored — then it stays away), it is being fetched, or it is on
 * disk ready to install. A failed check/download does NOT resurrect it; that
 * lives in the 更新 card, which is where the retry labels are. */
static bool updateBadgeVisible(const UpdateInfo &ui)
{
    switch (ui.state) {
    case UpdateState::Available:   return !updateIsIgnored(ui);
    case UpdateState::Downloading:
    case UpdateState::Downloaded:  return true;
    default:                       return false;
    }
}

/* One line for the update row; 'col' receives the matching colour. */
static std::string updateStatusText(const UpdateInfo &ui, SDL_Color &col)
{
    switch (ui.state) {
    case UpdateState::Checking:
        col = THEME.textDim;
        return "正在检查更新…";
    case UpdateState::UpToDate:
        col = THEME.success;
        return "已是最新版本";
    case UpdateState::Available:
        col = THEME.success;
        return "发现新版本 " + ui.remote_tag + (updateIsIgnored(ui) ? "·已忽略" : "");
    case UpdateState::Downloading: {
        col = THEME.text;
        int p = updateProgress();
        return (p >= 0) ? ("正在下载更新包… " + std::to_string(p) + "%")
                        : "正在下载更新包…";
    }
    case UpdateState::Downloaded:
        if (!ui.detail.empty()) {            /* the last hand-over failed */
            col = THEME.danger;
            return "启动安装程序失败，请重试";
        }
        col = THEME.success;
        return "更新包已下载，点击「立即安装」";
    case UpdateState::Failed:
        col = THEME.danger;
        return (ui.is_download ? "下载失败：" : "检查失败：") + updateErrorText(ui.detail);
    case UpdateState::Idle:
    default: {
        col = THEME.textDim;
        std::string s = "当前版本 v" MIVARA_VERSION;
        if (g_cfg.last_update_check > 0)
            s += " · 上次检查：" + relTime(g_cfg.last_update_check);
        return s;
    }
    }
}

/* Right edge available to the status text: the leftmost button of the row
 * depends on the state, and the strings are long on purpose. The text starts
 * at BODY_X + 20 (180) and stops 16px short of the leftmost button. */
static int updateTextMaxW(const UpdateInfo &ui)
{
    if (ui.state == UpdateState::Available) return 246;   /* 忽略此版本 at x=442 */
    if (ui.state == UpdateState::Failed)    return 380;   /* 打开下载页 at x=576 */
    return 514;                                           /* primary only (x=710) */
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
    /* 'maxW' is a layout width; the font is rasterised at the display scale, so
     * every measurement below comes back in device pixels — compare like with
     * like or a scaled build clips text that fits. */
    const int limit = uiPx(maxW);
    int w = 0, h = 0;
    TTF_SizeUTF8(font, s.c_str(), &w, &h);
    if (w <= limit) return s;
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
        if (w <= limit) return shown;
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
                                 uiPx(LOG_WIN_W), uiPx(LOG_WIN_H), 0);
    if (!g_log_win) return;
    g_log_ren = SDL_CreateRenderer(g_log_win, -1,
                                   SDL_RENDERER_ACCELERATED |
                                       SDL_RENDERER_PRESENTVSYNC);
    if (!g_log_ren) {
        SDL_DestroyWindow(g_log_win);
        g_log_win = nullptr;
        return;
    }
    SDL_SetRenderDrawBlendMode(g_log_ren, SDL_BLENDMODE_BLEND);
    SDL_RenderSetScale(g_log_ren, g_uiScale, g_uiScale);   /* logical layout */
    g_log_open = true;
    g_log_scroll = 0;
}

static void closeLogWindow()
{
    if (!g_log_open) return;
    /* The log window owns its own renderer, so it owns its own shape masks —
     * an SDL_Texture may not outlive the renderer that made it. */
    dropMasks(g_log_ren);
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
    if (SDL_GetMouseFocus() == g_log_win) {
        SDL_GetMouseState(&mx, &my);
        mx = uiCoord(mx);              /* device pixels -> logical layout */
        my = uiCoord(my);
    }
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

/* Fully-qualified System32 path for a system executable: CreateProcess with a
 * bare name searches the current directory first, so a planted schtasks.exe in
 * a user-writable working directory would run with this process's admin token. */
static std::wstring sysExePath(const wchar_t *name)
{
    wchar_t dir[MAX_PATH] = {0};
    if (!GetSystemDirectoryW(dir, MAX_PATH) || !dir[0])
        return std::wstring();   /* fail closed: never fall back to a bare name */
    return L"\"" + std::wstring(dir) + L"\\" + name + L"\"";
}

/* schtasks with CREATE_NO_WINDOW; waits for the exit code. */
static bool runHiddenW(const std::wstring &cmd)
{
    if (cmd.empty()) return false;
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
        std::wstring cmd = sysExePath(L"schtasks.exe") +
                           L" /create /tn \"Mivara\" /tr \"\\\"" +
                           selfExePathW() +
                           L"\\\"\" /sc onlogon /rl highest /f";
        return runHiddenW(cmd);
    }
    return runHiddenW(sysExePath(L"schtasks.exe") + L" /delete /tn \"Mivara\" /f");
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

    /* ── header: appearance switcher (on every screen) ───────────────────── */
    {
        /* sun / moon / monitor — the third cell is 跟随系统 */
        static const int glyph[3] = {GLYPH_SUN, GLYPH_MOON, GLYPH_MONITOR};
        static const int ids[3]   = {BTN_THEME_LIGHT, BTN_THEME_DARK,
                                     BTN_THEME_SYSTEM};
        static const ThemeMode modes[3] = {ThemeMode::Light, ThemeMode::Dark,
                                           ThemeMode::System};
        const ThemeMode m = themeMode();
        for (int i = 0; i < 3; i++)
            addSegment(SEG_CELL_X(i), SEG_Y + SEG_PAD, SEG_CELL_W, SEG_CELL_H,
                       ids[i], m == modes[i], glyph[i]);
    }

    /* ── header: update button (on every screen, only while one is waiting) ─ */
    {
        UpdateInfo ui = updateGetInfo();
        if (updateBadgeVisible(ui))
            addButton(UPD_BTN_X, UPD_BTN_Y, UPD_BTN_W, UPD_BTN_H, "",
                      BTN_HDR_UPDATE, BTN_V_GHOST);
    }

    /* ── nav rail (on every screen) ──────────────────────────────────────── */
    {
        const NavTab tab = navTabForScreen(g_screen);
        addNav(NAV_HOME_Y, "主页", BTN_NAV_HOME,     GLYPH_MONITOR, tab == NAV_HOME);
        addNav(NAV_SET_Y,  "设置", BTN_NAV_SETTINGS, GLYPH_SLIDERS, tab == NAV_SETTINGS);
    }

    const int cx = CONTENT_CX;
    switch (g_screen) {
    case SCR_HOME: {
        /* Update band first: it is the topmost control and the hit test is
         * first-match-wins. */
        {
            UpdateInfo ui = updateGetInfo();
            const bool av = (ui.state == UpdateState::Available && !updateIsIgnored(ui));
            const bool dn = (ui.state == UpdateState::Downloaded);
            if (av || dn)
                addButton(BODY_X, HOME_BAND_Y, BODY_W, 36, "", BTN_UPD_NOTICE,
                          BTN_V_GHOST);
        }
        /* Stop buttons BEFORE the cards: they are nested inside the card
         * rect, so registering the card first would swallow every click. */
        for (int i = 0; i < 2; i++) {
            const bool active = (i == 0) ? g_receiver.active : g_sender.active;
            if (!active) continue;
            SDL_Rect s = rcHomeStop(i);
            addButton(s.x, s.y, s.w, s.h, i == 0 ? "停止接收" : "停止发送",
                      i == 0 ? BTN_HOME_RECEIVE_STOP : BTN_HOME_SENDER_STOP,
                      BTN_V_DANGER);
        }
        for (int i = 0; i < 2; i++) {
            const bool active = (i == 0) ? g_receiver.active : g_sender.active;
            if (active) continue;          /* the stop button owns this card */
            addButton(HOME_CARD_X(i), HOME_CARD_Y, HOME_CARD_W, HOME_CARD_H, "",
                      i == 0 ? BTN_HOME_RECEIVE : BTN_HOME_SEND, BTN_V_GHOST);
        }
        /* connection history rows */
        for (int i = 0; i < HIST_VISIBLE; i++) {
            const int idx = i + g_hist_scroll;
            if (idx >= (int)g_hist.size()) break;
            SDL_Rect row = rcHistRow(i);
            /* body click area stops short of the trailing buttons */
            addButton(row.x, row.y, HIST_BTN_X - 8 - row.x, row.h, "",
                      BTN_HIST_BASE + idx * 10, BTN_V_GHOST);
            SDL_Rect b1 = rcHistBtn(i, 0);
            SDL_Rect b2 = rcHistBtn(i, 1);
            SDL_Rect b3 = rcHistBtn(i, 2);
            addButton(b1.x, b1.y, b1.w, b1.h, "投屏", BTN_HIST_BASE + idx * 10 + 1,
                      BTN_V_CHIP_OK);
            addButton(b2.x, b2.y, b2.w, b2.h, "扩展", BTN_HIST_BASE + idx * 10 + 2,
                      BTN_V_CHIP_ACC);
            addButton(b3.x, b3.y, b3.w, b3.h, "删除", BTN_HIST_BASE + idx * 10 + 3,
                      BTN_V_LINK);
        }
        if (!g_hist.empty())
            addButton(BODY_R - 88, HIST_CARD_Y + 12, 88, 28, "清空", BTN_HIST_CLEAR,
                      BTN_V_LINK);
        break;
    }

    case SCR_RECEIVING:
        addButton(cx - 178, 518, 170, 48, "停止接收", BTN_RECV_STOP, BTN_V_DANGER);
        addButton(cx + 8,   518, 170, 48, "日志",     BTN_RECV_LOG,  BTN_V_SECONDARY);
        break;

    case SCR_SEARCHING:
        addButton(cx - 90, 480, 180, 48, "取 消", BTN_SEARCH_CANCEL, BTN_V_SECONDARY);
        break;

    case SCR_FOUND: {
        size_t n;
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            n = g_devices.size();
        }
        for (size_t i = 0; i < n && i < 6; i++) {
            const int y = FOUND_ROW_Y0 + (int)i * FOUND_ROW_PITCH;
            addButton(BODY_R - 220, y + 8, 100, 36, "投屏",
                      BTN_DEV_BASE + (int)i * 10 + 1, BTN_V_CHIP_OK);
            addButton(BODY_R - 112, y + 8, 100, 36, "扩展",
                      BTN_DEV_BASE + (int)i * 10 + 2, BTN_V_CHIP_ACC);
        }
        {
            const int y = foundActionsY((int)n);
            addButton(cx - 168, y, 160, 44, "重新搜索",   BTN_FOUND_RESEARCH, BTN_V_SECONDARY);
            addButton(cx + 8,   y, 160, 44, "手动输入 ip", BTN_FOUND_MANUAL,  BTN_V_SECONDARY);
        }
        break;
    }

    case SCR_NOT_FOUND:
        addButton(cx - 280, 484, 180, 52, "继续搜索",   BTN_NF_CONTINUE, BTN_V_PRIMARY);
        addButton(cx - 90,  484, 180, 52, "手动输入 ip", BTN_NF_MANUAL,   BTN_V_SECONDARY);
        addButton(cx + 100, 484, 180, 52, "退 出",      BTN_NF_QUIT,     BTN_V_SECONDARY);
        break;

    case SCR_MANUAL_IP:
        addButton(cx - 168, 440, 160, 48, "连 接", BTN_MANUAL_CONNECT, BTN_V_PRIMARY);
        addButton(cx + 8,   440, 160, 48, "返 回", BTN_MANUAL_BACK,    BTN_V_SECONDARY);
        break;

    case SCR_STREAMING:
        addButton(cx - 178, 510, 170, 48, "停 止", BTN_STREAM_STOP, BTN_V_DANGER);
        addButton(cx + 8,   510, 170, 48, "日志",  BTN_STREAM_LOG,  BTN_V_SECONDARY);
        break;

    case SCR_SETTINGS: {
        /* codec: a 3x2 choice grid, not a segmented control — six cells at
         * 93px do not fit on one row next to their label */
        const char *codecLabels[] = {"自动", "H.264", "H.265", "AV1", "原始", "RLE"};
        const char *codecIds[]    = {"auto", "h264", "h265", "av1", "raw", "rle"};
        for (int i = 0; i < 6; i++) {
            const int col = i % 3, row = i / 3;
            addChoice(SET_LC_X + col * 101, SET_GRID_Y0 + row * 36, 93, 32,
                      codecLabels[i], BTN_SET_CODEC_BASE + i,
                      g_cfg_edit.codec == codecIds[i]);
        }
        /* steppers */
        addButton(SET_MINUS_X, SET_ROW_Y(0), SET_STEP_W, 32, "−",
                  BTN_SET_QUALITY_DEC, BTN_V_SECONDARY);
        addButton(SET_ROW_R - SET_STEP_W, SET_ROW_Y(0), SET_STEP_W, 32, "+",
                  BTN_SET_QUALITY_INC, BTN_V_SECONDARY);
        addButton(SET_MINUS_X, SET_ROW_Y(1), SET_STEP_W, 32, "−",
                  BTN_SET_FPS_DEC, BTN_V_SECONDARY);
        addButton(SET_ROW_R - SET_STEP_W, SET_ROW_Y(1), SET_STEP_W, 32, "+",
                  BTN_SET_FPS_INC, BTN_V_SECONDARY);
        addButton(SET_MINUS_X, SET_ROW_Y(2), SET_STEP_W, 32, "−",
                  BTN_SET_BITRATE_DEC, BTN_V_SECONDARY);
        addButton(SET_ROW_R - SET_STEP_W, SET_ROW_Y(2), SET_STEP_W, 32, "+",
                  BTN_SET_BITRATE_INC, BTN_V_SECONDARY);
        /* scale + chroma choice rows */
        const int scaleVals[] = {100, 75, 50};
        const char *scaleLabels[] = {"100%", "75%", "50%"};
        for (int i = 0; i < 3; i++)
            addChoice(SET_ROW_R - 240 + i * 80, SET_ROW_Y(3), 80, 32,
                      scaleLabels[i], BTN_SET_SCALE_BASE + i,
                      g_cfg_edit.scale_percent == scaleVals[i]);
        const char *chromaIds[] = {"420", "444"};
        const char *chromaLabels[] = {"4:2:0", "4:4:4"};
        for (int i = 0; i < 2; i++)
            addChoice(SET_ROW_R - 160 + i * 80, SET_ROW_Y(4), 80, 32,
                      chromaLabels[i], BTN_SET_CHROMA_BASE + i,
                      g_cfg_edit.chroma == chromaIds[i]);

        /* 连接与启动 toggles */
        const int tog[6] = {BTN_SET_HW, BTN_SET_CONSOLE, BTN_SET_LOGWIN,
                            BTN_SET_AUTOSTART, BTN_SET_AUTORECV, BTN_SET_AUTOCHECK};
        for (int i = 0; i < 6; i++)
            addButton(SET_RC_X, SET_TOGGLE_Y0 + i * 40, SET_COL_W - SET_PAD * 2, 32,
                      "", tog[i], BTN_V_GHOST);

        /* 外观 */
        {
            static const char *lbl[3] = {"亮色", "暗色", "跟随系统"};
            const ThemeMode m = themeMode();
            const ThemeMode modes[3] = {ThemeMode::Light, ThemeMode::Dark,
                                        ThemeMode::System};
            for (int i = 0; i < 3; i++)
                addChoice(SET_RC_X + i * 101, SET_APPEAR_Y + 56, 93, 32,
                          lbl[i], BTN_SET_THEME_BASE + i, m == modes[i]);
        }

        /* update card: three fixed slots so the hit targets never move */
        {
            const bool canDl = updateDownloadSupported();
            UpdateInfo ui = updateGetInfo();
            const int y = SET_UPD_Y + 44;
            if (ui.state == UpdateState::Available)
                addButton(442, y, 126, 32,
                          updateIsIgnored(ui) ? "取消忽略" : "忽略此版本",
                          BTN_SET_UPD_IGNORE, BTN_V_SECONDARY);
            if (ui.state == UpdateState::Failed ||
                (ui.state == UpdateState::Available && canDl))
                addButton(576, y, 126, 32, "打开下载页", BTN_SET_UPD_PAGE,
                          BTN_V_SECONDARY);
            const char *lbl = "检查更新";
            int id = BTN_SET_UPD_CHECK;
            if (ui.state == UpdateState::Available) {
                if (canDl) { lbl = "下载更新"; id = BTN_SET_UPD_DL; }
                else       { lbl = "打开下载页"; id = BTN_SET_UPD_PAGE; }
            } else if (ui.state == UpdateState::Downloading) {
                lbl = "取消下载"; id = BTN_SET_UPD_CANCEL;
            } else if (ui.state == UpdateState::Downloaded) {
                lbl = "立即安装"; id = BTN_SET_UPD_INSTALL;
            } else if (ui.state == UpdateState::Failed) {
                lbl = "重试";
                id = (ui.is_download && !ui.remote_tag.empty()) ? BTN_SET_UPD_DL
                                                               : BTN_SET_UPD_CHECK;
            }
            addButton(710, y, 126, 32, lbl, id, BTN_V_PRIMARY);
        }

        addButton(cx - 168, SET_ACT_Y, 160, SET_ACT_H, "确 定", BTN_SET_OK,
                  BTN_V_PRIMARY);
        addButton(cx + 8,   SET_ACT_Y, 160, SET_ACT_H, "恢复默认", BTN_SET_RESET,
                  BTN_V_SECONDARY);
        if (g_sender.active || g_receiver.active)
            addButton(cx + 184, SET_ACT_Y, 160, SET_ACT_H, "返回会话",
                      BTN_RETURN_SESSION, BTN_V_SECONDARY);
        break;
    }
    }
}

/* ── stat panel text ─────────────────────────────────────────────────────── */
/* The sender reports the concrete encoder ("h264 (nvenc)", "h264_x264").
 * codecDisplay() already put the friendly name on the line, so repeating the
 * raw token reads as "H.264 (h264 (nvenc))" — keep only the distinguishing
 * part. */
static std::string codecDetail(const std::string &impl)
{
    if (impl.empty()) return "";
    const size_t lp = impl.find('(');
    if (lp != std::string::npos) {
        const size_t rp = impl.rfind(')');
        return (rp > lp + 1) ? impl.substr(lp + 1, rp - lp - 1)
                             : impl.substr(lp + 1);
    }
    static const char *kNames[] = {"h264", "h265", "hevc", "av1", "raw", "rle"};
    for (const char *n : kNames) {
        const size_t len = std::strlen(n);
        if (impl.size() < len || impl.compare(0, len, n) != 0) continue;
        /* Bare codec token — raw/rle arrive exactly like this, so there is no
         * separator to strip and nothing left to say: "原始 (raw)" is the very
         * duplication this function exists to remove. */
        if (impl.size() == len) return "";
        if (impl[len] == ' ' || impl[len] == '_' || impl[len] == '-')
            return impl.substr(len + 1);
    }
    return impl;
}

static std::string statLine1(const LiveStats &s)
{
    std::string head = (g_child_label.empty() ? "会话" : g_child_label) + "中";
    std::string out = head + " · " + codecDisplay(s.codec);
    const std::string detail = codecDetail(s.codec);
    if (!detail.empty()) out += " (" + detail + ")";
    if (!s.fps.empty()) out += "   " + s.fps + " fps";
    if (!s.kbps.empty()) out += "   " + fmtMbps(s.kbps);
    return out;
}

static std::string statLine2(const LiveStats &s)
{
    std::string out;
    if (!s.src.empty() && !s.dst.empty()) out = s.src + "→" + s.dst;
    else if (!s.src.empty()) out = s.src;
    if (!s.mode.empty()) {
        /* the wire value is English; the rest of the UI is not */
        const std::string m = (s.mode == "extend") ? "扩展"
                            : (s.mode == "mirror") ? "投屏"
                                                   : s.mode;
        out += (out.empty() ? "" : "   ") + m;
    }
    if (!s.quality.empty()) out += (out.empty() ? "" : "   ") + std::string("Q") + s.quality;
    return out;
}

static void drawStatPanel(SDL_Renderer *ren, SDL_Rect card)
{
    LiveStats s;
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        s = g_stats;
    }
    drawCard(ren, card, CARD_R);
    const int x = card.x + 20;
    const int maxW = card.w - 40;
    drawText(ren, "实时状态", x, card.y + 20, THEME.textDim, g_fontBS);

    if (!s.valid) {
        drawText(ren, "等待统计数据…", x, card.y + 54, THEME.textDim, g_font);
        return;
    }
    if (s.event == "error") {
        drawText(ren, fitText("错误：" + s.msg, g_font, maxW), x, card.y + 54,
                 THEME.danger, g_font);
        return;
    }
    if (s.event == "start" || s.event == "stop") {
        drawText(ren, s.event == "start" ? "会话已开始" : "会话已结束",
                 x, card.y + 54, THEME.text, g_font);
        return;
    }
    drawText(ren, fitText(statLine1(s), g_fontB, maxW), x, card.y + 54,
             THEME.text, g_fontB);
    std::string l2 = statLine2(s);
    if (!l2.empty())
        drawText(ren, fitText(l2, g_fontS, maxW), x, card.y + 94,
                 THEME.textDim, g_fontS);
}

/* ── screen rendering ────────────────────────────────────────────────────── */
/* ── chrome ──────────────────────────────────────────────────────────────── */
static void drawHeader()
{
    fillRoundRect(g_ren, SDL_Rect{0, 0, WIN_W, HEADER_H}, 0, THEME.surface);
    drawHairline(g_ren, 0, HEADER_H - 1, WIN_W, 1, THEME.border);
    drawGlyph(g_ren, GLYPH_MONITOR, HDR_GLYPH_CX, HEADER_H / 2, 22, THEME.primary);
    drawTextV(g_ren, "Mivara 幻屏", HDR_WORD_X, HEADER_H / 2, THEME.text,
              g_fontB, ALIGN_LEFT);
    /* update button — ghost hit area, painted here because its fill encodes the
     * update state: tinted while a release waits, green once it is on disk,
     * with a progress bar under the glyph while it downloads. */
    {
        UpdateInfo ui = updateGetInfo();
        if (updateBadgeVisible(ui)) {
            const SDL_Rect r{UPD_BTN_X, UPD_BTN_Y, UPD_BTN_W, UPD_BTN_H};
            const int rad = THEME.radius - 4;
            const bool done = (ui.state == UpdateState::Downloaded);
            fillRoundRect(g_ren, r, rad,
                          done ? THEME.successTint : THEME.primaryTint);
            if (g_hot == BTN_HDR_UPDATE || g_press == BTN_HDR_UPDATE)
                fillRoundRect(g_ren, r, rad, withAlpha(WHITE, 0x1F));
            drawGlyph(g_ren, GLYPH_UPDATE, r.x + r.w / 2, r.y + r.h / 2 - 2, 18,
                      done ? THEME.onSuccessTint : THEME.onPrimaryTint);
            if (ui.state == UpdateState::Downloading) {
                int p = updateProgress();
                p = (p < 0) ? 0 : (p > 100 ? 100 : p);
                const SDL_Rect track{r.x + 5, r.y + r.h - 6, r.w - 10, 3};
                fillRoundRect(g_ren, track, 1, THEME.surface);
                if (p > 0) {
                    SDL_Rect fill{track.x, track.y, track.w * p / 100, track.h};
                    if (fill.w > 0) fillRoundRect(g_ren, fill, 1, THEME.primary);
                }
            }
        }
    }
    /* appearance switcher: a track with three choice cells on top of it */
    drawRoundRect(g_ren, SDL_Rect{SEG_X, SEG_Y, SEG_W, SEG_H}, THEME.radius,
                  THEME.surfaceAlt, THEME.border);
}

static void drawNav()
{
    fillRoundRect(g_ren, SDL_Rect{0, HEADER_H, NAV_W, WIN_H - HEADER_H}, 0,
                  THEME.surface);
    drawHairline(g_ren, NAV_W - 1, HEADER_H, 1, WIN_H - HEADER_H, THEME.border);
}

static void renderScreen(int mousex, int mousey)
{
    (void)mousex; (void)mousey;
    setColor(g_ren, THEME.bg);
    SDL_RenderClear(g_ren);

    const int cx = CONTENT_CX;
    drawHeader();
    drawNav();

    switch (g_screen) {
    case SCR_HOME: {
        /* update band (the ghost hit-area is added in buildButtons) */
        {
            UpdateInfo ui = updateGetInfo();
            const bool av = (ui.state == UpdateState::Available && !updateIsIgnored(ui));
            const bool dn = (ui.state == UpdateState::Downloaded);
            if (av || dn) {
                SDL_Rect r{BODY_X, HOME_BAND_Y, BODY_W, 36};
                fillRoundRect(g_ren, r, THEME.radius,
                              dn ? THEME.successTint : THEME.primaryTint);
                if (g_hot == BTN_UPD_NOTICE || g_press == BTN_UPD_NOTICE)
                    fillRoundRect(g_ren, r, THEME.radius, withAlpha(WHITE, 0x1F));
                std::string msg = dn ? ("新版本 " + ui.remote_tag + " 已下载 · 前往设置安装")
                                     : ("发现新版本 " + ui.remote_tag + " · 前往设置更新");
                drawTextIn(g_ren, msg, r, dn ? THEME.onSuccessTint : THEME.onPrimaryTint,
                           g_fontBS, ALIGN_CENTER);
            }
        }
        drawText(g_ren, "零配置局域网投屏 · 无线扩展屏", BODY_X, HOME_TITLE_Y,
                 THEME.textDim, g_font);
        if (!g_status_note.empty())
            drawTextV(g_ren, g_status_note, BODY_R, HOME_TITLE_Y + 10, THEME.danger,
                      g_fontS, ALIGN_RIGHT);
        drawText(g_ren, "快捷操作", BODY_X, HOME_CAPTION_Y, THEME.textDim, g_fontBS);

        /* hero cards */
        static const char *kTitle[2]   = {"接收", "发送"};
        static const char *kCaption[2] = {"作为接收端，等待其他设备投屏",
                                          "搜索接收端并开始投屏"};
        static const int   kGlyph[2]   = {GLYPH_ARROW_DOWN, GLYPH_ARROW_UP};
        for (int i = 0; i < 2; i++) {
            const SDL_Rect card = rcHomeCard(i);
            const bool active = (i == 0) ? g_receiver.active : g_sender.active;
            drawCard(g_ren, card, CARD_R);

            SDL_Rect tile{HOME_CARD_X(i) + 16, HOME_CARD_Y + 16, 40, 40};
            fillRoundRect(g_ren, tile, 12,
                          i == 0 ? THEME.successTint : THEME.primaryTint);
            drawGlyph(g_ren, kGlyph[i], tile.x + 20, tile.y + 20, 20,
                      i == 0 ? THEME.onSuccessTint : THEME.onPrimaryTint);

            drawTextV(g_ren, kTitle[i], card.x + 72, card.y + 36, THEME.text,
                      g_fontB, ALIGN_LEFT);
            drawText(g_ren, kCaption[i], card.x + 16, card.y + 72, THEME.textDim,
                     g_fontS);

            const SDL_Rect chip = rcHomeChip(i);
            fillRoundRect(g_ren, chip, chip.h / 2,
                          active ? THEME.successTint : THEME.surfaceAlt);
            drawTextIn(g_ren,
                       active ? (i == 0 ? "接收中" : "发送中") : "未运行",
                       chip, active ? THEME.onSuccessTint : THEME.textDim,
                       g_fontS, ALIGN_CENTER);
        }

        /* connection history — the card shrinks when there is nothing to list,
         * otherwise the empty state is a 288px box with one line in it */
        {
            const SDL_Rect card{BODY_X, HIST_CARD_Y, BODY_W,
                                g_hist.empty() ? HIST_CARD_H_EMPTY : HIST_CARD_H};
            drawCard(g_ren, card, CARD_R);
            drawText(g_ren, "连接历史", card.x + 16, card.y + 16, THEME.text,
                     g_fontBS);
            if (g_hist.empty()) {
                drawTextIn(g_ren, "暂无连接记录 · 成功连接一次后会自动出现在这里",
                           card, THEME.textDim, g_font, ALIGN_CENTER);
            } else {
                for (int i = 0; i < HIST_VISIBLE; i++) {
                    const int idx = i + g_hist_scroll;
                    if (idx >= (int)g_hist.size()) break;
                    const HistEntry &e = g_hist[idx];
                    const SDL_Rect row = rcHistRow(i);
                    if (g_hot >= BTN_HIST_BASE + idx * 10 &&
                        g_hot < BTN_HIST_BASE + idx * 10 + 4)
                        fillRoundRect(g_ren, row, THEME.radius, THEME.hover);
                    if (idx + 1 < (int)g_hist.size())
                        drawHairline(g_ren, row.x, row.y + row.h, row.w, 1,
                                     THEME.border);

                    std::string nm = e.name.empty() ? e.ip : truncateUtf8(e.name, 10);
                    drawText(g_ren, fitText(nm, g_fontS, 440), row.x + 12, row.y + 5,
                             THEME.text, g_fontS);
                    std::string sub = e.ip + ":" + std::to_string(e.port) + " · " +
                                      (e.mode == "extend" ? "扩展" : "投屏") + " · " +
                                      relTime(e.last_used_unix) + " · ×" +
                                      std::to_string(e.use_count);
                    drawText(g_ren, fitText(sub, g_fontS, 440), row.x + 12, row.y + 23,
                             THEME.textDim, g_fontS);
                }
                if ((int)g_hist.size() > HIST_VISIBLE) {
                    std::string pg = "第 " + std::to_string(g_hist_scroll + 1) + "–" +
                                     std::to_string(std::min((int)g_hist.size(),
                                                             g_hist_scroll + HIST_VISIBLE)) +
                                     " 条 / 共 " + std::to_string(g_hist.size()) +
                                     " 条 · 滚轮翻页";
                    drawTextV(g_ren, pg, BODY_R, HIST_PAGER_Y, THEME.textDim,
                              g_fontS, ALIGN_RIGHT);
                }
            }
        }
        break;
    }

    case SCR_RECEIVING: {
        std::string ip = getLocalIPAddress();
        drawText(g_ren, "正在接收", cx, 162, THEME.success, g_fontL, true);
        drawText(g_ren, "其他设备搜索到本机后即可开始投屏", cx, 210, THEME.textDim,
                 g_font, true);

        const SDL_Rect card{BODY_X, 246, BODY_W, 256};
        drawCard(g_ren, card, CARD_R);
        const int lx = card.x + 20;
        drawText(g_ren, "本机 IP", lx, card.y + 24, THEME.textDim, g_fontS);
        drawText(g_ren, ip, lx, card.y + 48, THEME.text, g_fontB);
        drawText(g_ren, "视频端口 8081    ·    计算端口 8082    ·    端口服务 8083    ·    发现服务 UDP 1900",
                 lx, card.y + 96, THEME.textDim, g_fontS);
        drawHairline(g_ren, lx, card.y + 132, card.w - 40, 1, THEME.border);
        drawText(g_ren, "状态：等待发送端连接…", lx, card.y + 150, THEME.text, g_font);
        drawText(g_ren, "也可在发送端执行  sender " + ip, lx, card.y + 186,
                 THEME.textDim, g_fontS);
        break;
    }

    case SCR_SEARCHING: {
        /* radar mark: two rings plus a solid centre */
        drawRing(g_ren, cx, 290, 56, THEME.accentSoft);
        drawRing(g_ren, cx, 290, 38, THEME.accentSoft);
        setColor(g_ren, THEME.accent);
        fillCircle(g_ren, cx, 290, 9);
        const int dots = ((int)(SDL_GetTicks() / 400)) % 4;
        drawText(g_ren, "搜索中" + std::string(dots, '.'), cx, 370, THEME.text,
                 g_fontL, true);
        drawText(g_ren, "正在通过 SSDP 查找局域网内的接收端", cx, 420,
                 THEME.textDim, g_font, true);
        break;
    }

    case SCR_FOUND: {
        std::vector<DiscoveredDevice> devs;
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            devs = g_devices;
        }
        drawText(g_ren, "发现 " + std::to_string(devs.size()) + " 台设备", BODY_X,
                 TITLE_Y, THEME.text, g_fontB);
        drawTextV(g_ren, "选择投屏或扩展", BODY_R, TITLE_Y + 12, THEME.textDim,
                  g_fontS, ALIGN_RIGHT);
        for (size_t i = 0; i < devs.size() && i < 6; i++) {
            const int y = FOUND_ROW_Y0 + (int)i * FOUND_ROW_PITCH;
            const SDL_Rect row{BODY_X, y, BODY_W, FOUND_ROW_H};
            /* border, not a shadow: consecutive rows are only 8px apart and
             * the 12px shadow of one landed on top of the next */
            drawRoundRect(g_ren, row, THEME.radiusCard - 4, THEME.surface,
                          THEME.border);
            std::string label = devs[i].name.empty()
                                    ? devs[i].toString()
                                    : truncateUtf8(devs[i].name, 12) + "  ·  " +
                                          devs[i].toString();
            drawTextIn(g_ren, fitText(label, g_font, BODY_R - 240 - BODY_X - 20),
                       row, THEME.text, g_font, ALIGN_LEFT, 20);
        }
        if (devs.size() > 6)
            drawText(g_ren, "仅显示前 6 台", BODY_X, FOUND_ACTIONS_MAX_Y - 22,
                     THEME.textDim, g_fontS);
        break;
    }

    case SCR_NOT_FOUND: {
        glyphWarning(g_ren, cx, 270, 64, THEME.warning, THEME.bg);
        drawText(g_ren, "未找到可用设备", cx, 328, THEME.text, g_fontL, true);
        drawText(g_ren, "请确认接收端已启动，且两台机器在同一局域网", cx, 376,
                 THEME.textDim, g_font, true);
        if (!g_status_note.empty())
            drawText(g_ren, g_status_note, cx, 428, THEME.danger, g_font, true);
        break;
    }

    case SCR_MANUAL_IP: {
        drawText(g_ren, "手动输入接收端 IP", cx, 230, THEME.text, g_fontL, true);
        drawText(g_ren, "在接收端的主页可以看到它的本机 IP", cx, 278, THEME.textDim,
                 g_font, true);
        const SDL_Rect box{cx - 200, 312, 400, 60};
        /* surface, not inputBg: on the light palette inputBg IS the page colour,
         * so the field had no fill of its own — only a border. */
        drawRoundRect(g_ren, box, THEME.radius, THEME.surface,
                      g_status_note.empty() ? THEME.border : THEME.danger);
        const bool empty = g_manual_ip.empty();
        drawTextIn(g_ren, empty ? "例如 192.168.1.105" : g_manual_ip, box,
                   empty ? THEME.textDim : THEME.text, g_font, ALIGN_CENTER);
        if (!g_status_note.empty())
            drawText(g_ren, g_status_note, cx, 396, THEME.danger, g_font, true);
        break;
    }

    case SCR_STREAMING:
        drawText(g_ren, "正在" + g_child_label, cx, 200, THEME.success, g_fontL, true);
        if (!g_status_note.empty())
            drawText(g_ren, g_status_note, cx, 258, THEME.text, g_font, true);
        drawText(g_ren, "会话进行中 · 关闭窗口不会中断，请点下方按钮结束", cx, 290,
                 THEME.textDim, g_fontS, true);
        drawStatPanel(g_ren, SDL_Rect{BODY_X, 330, BODY_W, 148});
        break;

    case SCR_SETTINGS: {
        drawText(g_ren, "设 置", BODY_X, TITLE_Y, THEME.text, g_fontB);
        drawTextV(g_ren, "修改后点「确定」保存到 config.ini", BODY_R, TITLE_Y + 12,
                  THEME.textDim, g_fontS, ALIGN_RIGHT);

        /* ── left column: 画质与编码 ────────────────────────────────────── */
        {
            const SDL_Rect card{SET_COL_L, SET_CARD_Y, SET_COL_W, SET_L_H};
            drawCard(g_ren, card, CARD_R);
            drawText(g_ren, "画质与编码", SET_LC_X, card.y + 20, THEME.textDim,
                     g_fontBS);
            drawText(g_ren, "编码", SET_LC_X, SET_CODEC_LABEL_Y, THEME.text, g_font);

            std::string vQ = std::to_string(g_cfg_edit.quality);
            std::string vF = std::to_string(g_cfg_edit.fps_limit) + " fps";
            std::string vB = (g_cfg_edit.bitrate_kbps == 0)
                                 ? std::string("质量模式")
                                 : std::to_string(g_cfg_edit.bitrate_kbps) + " kbps";
            static const char *kRow[5] = {"画质 (CRF)", "帧率上限", "码率", "缩放",
                                          "色度"};
            for (int i = 0; i < 5; i++)
                drawTextV(g_ren, kRow[i], SET_LC_X, SET_ROW_Y(i) + 16, THEME.text,
                          g_font, ALIGN_LEFT);
            const std::string *vals[3] = {&vQ, &vF, &vB};
            for (int i = 0; i < 3; i++) {
                const SDL_Rect box{SET_VAL_X, SET_ROW_Y(i), SET_VAL_W, 32};
                fillRoundRect(g_ren, box, THEME.radius - 3, THEME.surfaceAlt);
                drawTextIn(g_ren, *vals[i], box, THEME.text, g_fontS, ALIGN_CENTER);
            }
        }

        /* ── right column: 连接与启动 ───────────────────────────────────── */
        {
            const SDL_Rect card{SET_COL_R, SET_CARD_Y, SET_COL_W, SET_R_H};
            drawCard(g_ren, card, CARD_R);
            drawText(g_ren, "连接与启动", SET_RC_X, card.y + 20, THEME.textDim,
                     g_fontBS);
            static const char *kToggle[6] = {"硬件加速", "状态控制台", "日志窗口",
                                             "开机自启动", "自动接收", "自动检查更新"};
            const bool on[6] = {g_cfg_edit.hw_accel != 0,
                                g_cfg_edit.show_status_console != 0,
                                g_cfg_edit.show_log_window != 0,
                                g_cfg_edit.auto_start != 0,
                                g_cfg_edit.auto_receive != 0,
                                g_cfg_edit.auto_check_update != 0};
            const int id[6] = {BTN_SET_HW, BTN_SET_CONSOLE, BTN_SET_LOGWIN,
                               BTN_SET_AUTOSTART, BTN_SET_AUTORECV,
                               BTN_SET_AUTOCHECK};
            for (int i = 0; i < 6; i++) {
                const int y = SET_TOGGLE_Y0 + i * 40;
                const SDL_Rect row{SET_RC_X, y, SET_COL_W - SET_PAD * 2, 32};
                if (g_hot == id[i] || g_press == id[i])
                    fillRoundRect(g_ren, row, THEME.radius - 3, THEME.hover);
                drawTextV(g_ren, kToggle[i], row.x, y + 16, THEME.text, g_font,
                          ALIGN_LEFT);
                drawToggle(g_ren, SDL_Rect{row.x + row.w - 44, y + 4, 44, 24}, on[i]);
            }
        }

        /* ── right column: 外观 ─────────────────────────────────────────── */
        {
            const SDL_Rect card{SET_COL_R, SET_APPEAR_Y, SET_COL_W, SET_APPEAR_H};
            drawCard(g_ren, card, CARD_R);
            drawText(g_ren, "外观", SET_RC_X, card.y + 20, THEME.textDim, g_fontBS);
            /* the three cells are painted by drawButton (BTN_V_CHOICE) */
        }

        /* ── 更新 (full width) ──────────────────────────────────────────── */
        {
            const SDL_Rect card{BODY_X, SET_UPD_Y, BODY_W, SET_UPD_H};
            drawCard(g_ren, card, CARD_R);
            drawText(g_ren, "软件更新", card.x + 20, card.y + 14, THEME.textDim,
                     g_fontBS);
            UpdateInfo ui = updateGetInfo();
            SDL_Color col = THEME.textDim;
            std::string txt = updateStatusText(ui, col);
            drawText(g_ren, fitText(txt, g_fontS, updateTextMaxW(ui)),
                     card.x + 20, card.y + 40, col, g_fontS);
            if (ui.state == UpdateState::Downloading) {
                int p = updateProgress();
                p = p < 0 ? 0 : (p > 100 ? 100 : p);
                const SDL_Rect bar{card.x + 20, card.y + 66, 240, 5};
                fillRoundRect(g_ren, bar, 2, THEME.surfaceAlt);
                if (p > 0) {
                    const SDL_Rect fill{bar.x, bar.y, bar.w * p / 100, bar.h};
                    fillRoundRect(g_ren, fill, 2, THEME.primary);
                }
            }
        }

        /* ── 确定 / 恢复默认 / 返回会话 ─────────────────────────────────── */
        break;
    }
    }

    for (auto &b : g_buttons) drawButton(g_ren, b);

    /* dot on the 设置 nav item while an update is pending */
    {
        UpdateInfo ui = updateGetInfo();
        const bool dot = (ui.state == UpdateState::Downloaded) ||
                         (ui.state == UpdateState::Available && !updateIsIgnored(ui));
        if (dot) {
            setColor(g_ren, ui.state == UpdateState::Downloaded ? THEME.success
                                                                : THEME.danger);
            fillCircle(g_ren, NAV_ITEM_X + NAV_ITEM_W - 8, NAV_SET_Y + 22, 5);
        }
    }
    SDL_RenderPresent(g_ren);
}

/* ── button actions ──────────────────────────────────────────────────────── */
/** Hand the verified installer over to Setup and quit on success.
 * Launch FIRST and quit only on success: if the hand-over fails the user keeps
 * the running session and simply clicks 立即安装 again. On success the normal
 * exit path is what stops the children (the Ctrl+C path detaches the virtual
 * display) while Setup shows its wizard, so app.exe is released before Setup
 * replaces it. */
static void startInstall()
{
    UpdateInfo ui = updateGetInfo();
    if (ui.state != UpdateState::Downloaded) return;
    if (g_upd_installing) return;                /* one hand-over, ever */
    g_upd_installing = true;
    if (updateLaunchInstaller()) {
        logPush("[gui] 已启动安装程序，Mivara 退出");
        SDL_Event q;
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    } else {
        logPush("[gui] 启动安装程序失败");
        g_upd_installing = false;                /* allow a retry */
    }
}

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
    /* appearance: the header switcher and the 外观 card both land here */
    for (int i = 0; i < kThemeIdCount; i++) {
        if (kThemeIds[i].id != id) continue;
        const ThemeMode picked = kThemeIds[i].mode;
        themeSetMode(picked);
        /* Persist immediately: the header switcher is available on every
         * screen, so requiring 确定 would make it look broken on the others.
         * This mirrors 日志窗口 / 忽略此版本, the other instant side effects. */
        g_cfg.theme = themeModeKey(picked);
        g_cfg_edit.theme = g_cfg.theme;
        configSave(g_cfg);
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
        /* Only re-snapshot when arriving from another screen. openSettings()
         * does g_cfg_edit = g_cfg, so re-clicking the tab that is already open
         * used to throw away every pending edit with no visible cause — the
         * 画质 box just snapped back to its saved value. */
        if (g_screen != SCR_SETTINGS) openSettings();
        break;
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
    case BTN_SET_AUTOCHECK:
        g_cfg_edit.auto_check_update = !g_cfg_edit.auto_check_update;
        break;
    case BTN_SET_UPD_IGNORE: {
        UpdateInfo ui = updateGetInfo();
        if (ui.state == UpdateState::Available) {
            if (updateIsIgnored(ui)) {
                g_cfg.ignored_version.clear();
                logPush("[gui] 已取消忽略 " + ui.remote_tag);
            } else {
                g_cfg.ignored_version = ui.remote_tag;
                logPush("[gui] 已忽略版本 " + ui.remote_tag);
            }
            configSave(g_cfg);   /* immediate side effect (cf. BTN_SET_LOGWIN) */
        }
        break;
    }
    case BTN_SET_UPD_CHECK:
        updateCheckStart();          /* no-op while a check/download is running */
        break;
    case BTN_SET_UPD_DL:
        updateDownloadStart();       /* Available, or 重试 after a failed download */
        break;
    case BTN_SET_UPD_CANCEL:
        updateDownloadCancel();
        break;
    case BTN_SET_UPD_INSTALL:
        startInstall();
        break;
    case BTN_SET_UPD_PAGE:
        if (!updateOpenReleasePage()) logPush("[gui] 打开浏览器失败");
        break;
    case BTN_UPD_NOTICE:
        openSettings();
        break;
    case BTN_HDR_UPDATE: {
        UpdateInfo ui = updateGetInfo();
        if (ui.state == UpdateState::Available) {
            if (!updateDownloadSupported()) {    /* POSIX: browser hand-off only */
                if (!updateOpenReleasePage()) logPush("[gui] 打开浏览器失败");
                break;
            }
            /* One click buys the whole run: download, then hand over to Setup
             * as soon as it verifies. Only a live session stops it — quitting
             * would kill that session — and then the button just turns green
             * and waits for a second click. */
            if (updateDownloadStart()) {
                g_upd_auto_install = true;
                logPush("[gui] 开始下载更新 " + ui.remote_tag);
            }
        } else if (ui.state == UpdateState::Downloaded) {
            startInstall();
        }
        /* Downloading: deliberately inert — a stray click must not cancel a
         * download that is already running (取消下载 lives in the 更新 card). */
        break;
    }
    case BTN_SET_OK: {
        const int want = g_cfg_edit.auto_start ? 1 : 0;
        const int had = g_cfg.auto_start ? 1 : 0;
        /* Update metadata is runtime state, not a user setting: preserve it
         * across the working-copy commit (else the 24h throttle and the
         * ignored-version filter would be silently reset on every 确定). */
        const long long   keep_last = g_cfg.last_update_check;
        const std::string keep_ign  = g_cfg.ignored_version;
        g_cfg = g_cfg_edit;
        g_cfg.last_update_check = keep_last;
        g_cfg.ignored_version   = keep_ign;
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
        g_cfg_edit = AppConfig{};          /* theme defaults to "system" */
        /* The appearance applies instantly everywhere else (header switcher),
         * so applying it here too keeps 恢复默认 from looking half-done. */
        themeSetMode(ThemeMode::System);
        g_cfg.theme = "system";
        configSave(g_cfg);
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

    /* Resolve the display scale before anything is measured or created — see
     * the "display scale" block for why the UI is drawn through it. */
    {
        float ddpi = 0, hdpi = 0, vdpi = 0;
        if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0 && ddpi > 0)
            g_uiScale = ddpi / 96.0f;
        if (g_uiScale < 1.0f) g_uiScale = 1.0f;
        if (g_uiScale > 3.0f) g_uiScale = 3.0f;
        /* Snap to quarter steps — Windows only offers 100/125/150/175/200%, and
         * an arbitrary factor would land every rounded edge on its own
         * sub-pixel offset. */
        g_uiScale = (float)std::lround((double)g_uiScale * 4.0) / 4.0f;
        /* The layout is fixed, so scaling must never push the window past the
         * screen: at 125% on a 768-tall panel the bottom row (确定 / 退出)
         * would land off-screen with no way to reach it. Round DOWN to the
         * previous quarter step, and never below 1:1 — an unscaled 880x700
         * window is the most this layout can promise. */
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0 &&
            usable.w > 0 && usable.h > 0) {
            double fit = (double)usable.w / WIN_W;
            const double fitH = (double)usable.h / WIN_H;
            if (fitH < fit) fit = fitH;
            const float capped = (float)(std::floor(fit * 4.0) / 4.0);
            if (capped < 1.0f) g_uiScale = 1.0f;
            else if (g_uiScale > capped) g_uiScale = capped;
        }
    }

    g_font   = openFont(uiPx(FONT_BODY));       /* 16 — row labels, values  */
    g_fontL  = openFont(uiPx(FONT_DISPLAY));    /* 30 — screen titles       */
    g_fontS  = openFont(uiPx(FONT_SMALL));      /* 13 — captions, chips     */
    g_fontB  = openFontBold(uiPx(FONT_TITLE));  /* 22 — card titles         */
    g_fontBS = openFontBold(uiPx(FONT_SMALL + 2));  /* 15 — buttons         */
    if (!g_font || !g_fontL || !g_fontS || !g_fontB || !g_fontBS) {
        std::cerr << "找不到中文字体（msyh/simhei/NotoSansCJK）\n";
        TTF_Quit(); SDL_Quit();
        return 1;
    }

    configLoad(g_cfg);
    g_hist = historyLoad();

    /* The stored appearance must be applied before the first frame. An
     * unparsable value leaves the default (follow system) in place. */
    {
        ThemeMode m = ThemeMode::System;
        themeModeFromKey(g_cfg.theme.c_str(), m);
        themeSetMode(m);
    }

    g_win = SDL_CreateWindow("Mivara v" MIVARA_VERSION, SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, uiPx(WIN_W), uiPx(WIN_H), 0);
    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_win || !g_ren) {
        std::cerr << "SDL_CreateWindow/Renderer: " << SDL_GetError() << "\n";
        return 1;
    }
    /* Required by fillRoundRect: the tint chips, hairlines and the stacked
     * drop shadow all rely on SDL honouring the alpha channel. */
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    /* Every shape primitive below is in logical units; this is what maps them
     * onto the larger back buffer. */
    SDL_RenderSetScale(g_ren, g_uiScale, g_uiScale);

    if (g_cfg.show_log_window) openLogWindow();

#ifndef _WIN32
    /* Writing SETP lines to a sender that already exited must fail the write,
     * not terminate the GUI: ignore SIGPIPE (see sendToChild). */
    signal(SIGPIPE, SIG_IGN);
#endif

    initSockets();
    logPush("[gui] app start");
    {
        char sbuf[64];
        std::snprintf(sbuf, sizeof(sbuf), "[gui] 显示缩放 %.2fx（窗口 %dx%d）",
                      g_uiScale, uiPx(WIN_W), uiPx(WIN_H));
        logPush(sbuf);
    }
    updateStartupSweep();       /* drop installers left by an earlier run */

    if (g_cfg.auto_receive) launchReceiver();

    /* first automatic update check ~3 s in, after startup has settled; the
     * thread is only ever started from inside the loop, so an early return
     * above can never leave a joinable worker behind */
    g_upd_auto_at = SDL_GetTicks() + 3000;

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
                    handleLogClick(uiCoord(ev.button.x), uiCoord(ev.button.y));
                } else if (ev.button.windowID == SDL_GetWindowID(g_win)) {
                    g_press = BTN_NONE;
                    SDL_Point p{uiCoord(ev.button.x), uiCoord(ev.button.y)};
                    for (auto &b : g_buttons) {
                        if (SDL_PointInRect(&p, &b.rect)) { g_press = b.id; break; }
                    }
                }
            }
            else if (ev.type == SDL_MOUSEBUTTONUP &&
                     ev.button.button == SDL_BUTTON_LEFT) {
                if (ev.button.windowID == SDL_GetWindowID(g_win)) {
                    SDL_Point p{uiCoord(ev.button.x), uiCoord(ev.button.y)};
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

        /* async update check / download completion (main thread only) */
        {
            UpdateInfo upd;
            if (updateTake(upd)) {
                if (upd.is_download) {
                    if (upd.state == UpdateState::Downloaded) {
                        logPush("[gui] 更新包已下载：" + upd.remote_tag);
                        /* the header button asked for download→install in one
                         * go; a live session is the one thing that defers it */
                        if (g_upd_auto_install) {
                            g_upd_auto_install = false;
                            if (g_sender.active || g_receiver.active)
                                logPush("[gui] 会话进行中：更新已就绪，结束会话后点右上角安装");
                            else
                                startInstall();
                        }
                    }
                    else if (upd.state == UpdateState::Available && upd.detail == "cancelled") {
                        g_upd_auto_install = false;
                        logPush("[gui] 已取消下载");
                    }
                    else if (upd.state == UpdateState::Failed) {
                        g_upd_auto_install = false;
                        logPush("[gui] 更新下载失败：" + updateErrorText(upd.detail));
                    }
                } else {
                    /* every finished check refreshes the 24 h throttle */
                    g_cfg.last_update_check = (long long)std::time(nullptr);
                    configSave(g_cfg);
                    if (upd.state == UpdateState::Available)
                        logPush("[gui] 更新检查：发现新版本 " + upd.remote_tag);
                    else if (upd.state == UpdateState::UpToDate)
                        logPush(std::string("[gui] 更新检查：已是最新版本（v") +
                                MIVARA_VERSION + "）");
                    else
                        logPush("[gui] 更新检查失败：" + updateErrorText(upd.detail));
                }
            }
            /* one delayed automatic check per launch (default on, 24 h throttle;
             * manual checks are never throttled) */
            if (!g_upd_auto_checked && g_cfg.auto_check_update &&
                SDL_GetTicks() >= g_upd_auto_at) {
                g_upd_auto_checked = true;
                if ((long long)std::time(nullptr) - g_cfg.last_update_check >= 86400)
                    updateCheckStart();
            }
        }

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

        /* Rebuild BEFORE the hover test, not after: g_buttons must be the list
         * this frame is about to paint. The event loop above deliberately still
         * hit-tests the list that was on screen when the click happened (that is
         * what the user aimed at), but a hover computed from it would be a frame
         * stale whenever the screen changed inside the loop. */
        buildButtons();
        int mx = -1, my = -1;
        if (SDL_GetMouseFocus() == g_win) {
            SDL_GetMouseState(&mx, &my);
            mx = uiCoord(mx);          /* device pixels -> logical layout */
            my = uiCoord(my);
        }
        g_hot = BTN_NONE;
        for (auto &b : g_buttons) {
            if (mx >= b.rect.x && mx < b.rect.x + b.rect.w &&
                my >= b.rect.y && my < b.rect.y + b.rect.h) {
                g_hot = b.id;
                break;
            }
        }
        /* Re-resolve "follow system" before painting; the call rate-limits
         * itself, and it is a no-op unless the mode is System. */
        themeTick(SDL_GetTicks());
        renderScreen(mx, my);
        if (g_log_open) renderLogWindow();
    }

    if (g_search_thread.joinable()) {
        ++g_search_generation;
        g_search_thread.join();
    }
    updateJoin();               /* abort + join any in-flight check/download */
    stopChild(g_sender, g_child_extend);
    stopChild(g_receiver);
    closeLogWindow();
    cleanupSockets();
    if (g_font)  TTF_CloseFont(g_font);
    if (g_fontL) TTF_CloseFont(g_fontL);
    if (g_fontS) TTF_CloseFont(g_fontS);
    TTF_Quit();
    dropMasks(g_ren);           /* a mask texture may not outlive its renderer */
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
