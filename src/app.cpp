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

#include "auth.h"
#include "config.h"
#include "discover.h"
#include "history.h"
#include "kvm.h"
#include "logo_icon.h"
#include "theme.h"
#include "tray.h"
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
#define FONT_SMALL   14                 /* captions, buttons, chips            */

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
 * the content column. The cells carry sun / moon / monitor glyphs, not text;
 * this is the only appearance control — Settings does not repeat it. */
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

/* home — one primary entry (连接设备) plus a low-weight receive status bar.
 * 接收 is no longer a hero card: it is a setting that runs in the background,
 * so it gets a status row with one contextual action instead. */
#define HOME_TITLE_Y 76                 /* tagline under the header             */
#define HOME_BAND_Y 108                 /* update notice band                   */
#define HOME_CAPTION_Y 160
#define HOME_CARD_Y 184
#define HOME_CARD_W BODY_W              /* full-width 连接设备 card              */
#define HOME_CARD_H 104
#define HOME_CARD_X(i) (BODY_X + (i) * (HOME_CARD_W + 24))
/* While a sender session is active the hero card becomes a session card with
 * these two buttons; they sit inside the same card rect, right-aligned. */
#define HOME_SESSION_BTN_H 34
#define HOME_SESSION_BTN_Y (HOME_CARD_Y + 56)
#define HOME_SESSION_STOP_W 96
#define HOME_SESSION_BACK_W 116
#define HOME_SESSION_STOP_X (HOME_CARD_X(0) + HOME_CARD_W - 16 - HOME_SESSION_STOP_W)
#define HOME_SESSION_BACK_X (HOME_SESSION_STOP_X - 8 - HOME_SESSION_BACK_W)

#define RECV_BAR_Y 300
#define RECV_BAR_H 68
#define RECV_BAR_ACT_W 104
#define RECV_BAR_ACT_H 32
#define RECV_BAR_ACT_X (BODY_R - 16 - RECV_BAR_ACT_W)
#define RECV_BAR_ACT_Y (RECV_BAR_Y + (RECV_BAR_H - RECV_BAR_ACT_H) / 2)

#define HIST_CARD_Y 376
#define HIST_CARD_H 284
#define HIST_ROW_X (BODY_X + 16)
#define HIST_ROW_W (BODY_W - 32)
#define HIST_ROW_H 44
#define HIST_ROW_Y(i) (HIST_CARD_Y + 42 + (i) * 48)
#define HIST_BTN_X 584                  /* 投屏 / 扩展 / 拼接 / 删除, right-aligned */
#define HIST_BTN_W 64
#define HIST_BTN_GAP 6
#define HIST_DEL_X (HIST_ROW_X + HIST_ROW_W - 44)
#define HIST_TEXT_W (HIST_BTN_X - 8 - (HIST_ROW_X + 12))   /* name/subtitle fit */
#define HIST_PAGER_Y 674                /* footer caption, clear of the card's
                                         * bottom edge at 660 and its shadow */
#define HIST_CARD_H_EMPTY 132

/* found-devices list: the rows are laid out from the top, so the action row
 * follows the last one instead of floating at a fixed y with a hole above it */
#define FOUND_ROW_Y0 154
#define FOUND_ROW_PITCH 60
#define FOUND_ROW_H 54
#define FOUND_ACTIONS_MAX_Y 512
/* Three per-device actions (投屏 / 扩展 / 拼接), right-aligned in the row. */
#define DEV_BTN_W 84
#define DEV_BTN_GAP 8
#define DEV_BTN_X(k) (BODY_R - DEV_BTN_W - (2 - (k)) * (DEV_BTN_W + DEV_BTN_GAP))

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
static SDL_Rect rcHistRow(int i)
{
    return SDL_Rect{HIST_ROW_X, HIST_ROW_Y(i), HIST_ROW_W, HIST_ROW_H};
}
static SDL_Rect rcHistBtn(int i, int which)   /* 0 投屏 / 1 扩展 / 2 拼接 / 3 删除 */
{
    static const int w[4] = {HIST_BTN_W, HIST_BTN_W, HIST_BTN_W, 44};
    int x = (which == 3) ? HIST_DEL_X
                         : HIST_BTN_X + which * (HIST_BTN_W + HIST_BTN_GAP);
    return SDL_Rect{x, HIST_ROW_Y(i) + 6, w[which], 32};
}

/* 拼接 direction chooser (modal over whatever screen opened it). The pads are
 * laid out spatially — up / left · 本机 · right / down — so the question reads
 * as "where is the other PC", which is exactly the side the mouse must leave
 * from. */
#define SIDE_DLG_W 420
#define SIDE_DLG_H 320
#define SIDE_DLG_X (CONTENT_X + (CONTENT_W - SIDE_DLG_W) / 2)
#define SIDE_DLG_Y 170
#define SIDE_DLG_CX (SIDE_DLG_X + SIDE_DLG_W / 2)
#define SIDE_PAD_W 104
#define SIDE_PAD_H 44
#define SIDE_ROW1_Y (SIDE_DLG_Y + 104)
#define SIDE_ROW2_Y (SIDE_ROW1_Y + 52)
#define SIDE_ROW3_Y (SIDE_ROW2_Y + 52)

static SDL_Rect rcSidePad(int side)
{
    const int top = SIDE_DLG_CX - SIDE_PAD_W / 2;
    switch (side) {
    case KVM_SIDE_LEFT:
        return SDL_Rect{SIDE_DLG_CX - 58 - SIDE_PAD_W, SIDE_ROW2_Y,
                        SIDE_PAD_W, SIDE_PAD_H};
    case KVM_SIDE_RIGHT:
        return SDL_Rect{SIDE_DLG_CX + 58, SIDE_ROW2_Y, SIDE_PAD_W, SIDE_PAD_H};
    case KVM_SIDE_TOP:
        return SDL_Rect{top, SIDE_ROW1_Y, SIDE_PAD_W, SIDE_PAD_H};
    default:
        return SDL_Rect{top, SIDE_ROW3_Y, SIDE_PAD_W, SIDE_PAD_H};
    }
}
static SDL_Rect rcSideCenter()
{
    return SDL_Rect{SIDE_DLG_CX - 40, SIDE_ROW2_Y, 80, SIDE_PAD_H};
}
static SDL_Rect rcSideCancel()
{
    return SDL_Rect{SIDE_DLG_CX - 60, SIDE_DLG_Y + 264, 120, 34};
}

/* In-session four-way switch, drawn inside the stitch status panel. */
#define ST_PANEL_X BODY_X
#define ST_PANEL_Y 330
#define ST_PANEL_W BODY_W
#define ST_PANEL_H 148
#define ST_DIR_CAPTION_W 36
#define ST_DIR_CHIP_W 70
#define ST_DIR_CHIP_H 26
#define ST_DIR_CHIP_Y (ST_PANEL_Y + 112)
#define ST_DIR_CHIP_X(i) \
    (ST_PANEL_X + 20 + ST_DIR_CAPTION_W + (i) * (ST_DIR_CHIP_W + 8))

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
#define SET_R_H 296                     /* right-hand settings card              */
#define SET_UPD_Y 548                   /* 更新 (full width)                     */
#define SET_UPD_H 88
#define SET_ACT_Y 648                   /* 恢复默认 / 取消 / 保存 / 返回会话      */
#define SET_ACT_H 44

/* Three settings groups share the same two-column card geometry. */
#define SET_TAB_Y 58
#define SET_TAB_H 34
#define SET_TAB_W 118
#define SET_TAB_X(i) (BODY_R - (3 - (i)) * (SET_TAB_W + 8))

/* 接收与安全 tab */
#define SEC_SWITCH_Y 108
#define SEC_SWITCH_H 228
#define SEC_AUTH_Y 348
#define SEC_AUTH_H 264
#define SEC_ROW_X (BODY_X + 20)
#define SEC_ROW_W (BODY_W - 40)
#define SEC_TOGGLE_Y(i) (SEC_SWITCH_Y + 48 + (i) * 38)
#define SEC_POLICY_Y(i) (SEC_AUTH_Y + 50 + (i) * 42)
#define SEC_POLICY_X (BODY_X + 32)
#define SEC_PW_ROW_Y (SEC_AUTH_Y + 190)

/* Authorization modals: the receiver's approval request and the sender's
 * password prompt. Both are overlays drawn above the screen but below the
 * buttons, which buildButtons() limits to the dialog's own cells. */
#define AUTH_DLG_W 460
#define AUTH_DLG_H 250
#define AUTH_DLG_X (CONTENT_X + (CONTENT_W - AUTH_DLG_W) / 2)
#define AUTH_DLG_Y 220

#define PW_DLG_W 460
#define PW_DLG_H 286
#define PW_DLG_X (CONTENT_X + (CONTENT_W - PW_DLG_W) / 2)
#define PW_DLG_Y 200
#define PW_FIELD_X (PW_DLG_X + 24)
#define PW_FIELD_Y (PW_DLG_Y + 78)
#define PW_FIELD_W (PW_DLG_W - 48)
#define PW_FIELD_H 44
#define PW_SHOW_X (PW_FIELD_X + PW_FIELD_W - 116)
#define PW_SHOW_Y (PW_FIELD_Y + PW_FIELD_H + 12)

/* 设置 / 修改连接密码: one masked field. The 显示密码 link is the only
 * safeguard against a typo now that there is no confirmation field. */
#define SETPW_DLG_W 460
#define SETPW_DLG_H 250
#define SETPW_DLG_X (CONTENT_X + (CONTENT_W - SETPW_DLG_W) / 2)
#define SETPW_DLG_Y 190
#define SETPW_FIELD_X (SETPW_DLG_X + 24)
#define SETPW_FIELD_W (SETPW_DLG_W - 48)
#define SETPW_FIELD_H 38
#define SETPW_A_Y (SETPW_DLG_Y + 84)
#define SETPW_SHOW_X (SETPW_FIELD_X + SETPW_FIELD_W - 116)
#define SETPW_SHOW_Y (SETPW_A_Y + SETPW_FIELD_H + 10)

#define SET_CODEC_LABEL_Y 168
#define SET_GRID_Y0 196                 /* codec 3x2 grid                        */
#define SET_ROW_Y(i) (288 + (i) * 44)   /* 画质 / 帧率 / 码率 / 缩放 / 色度      */
#define SET_TOGGLE_Y0 158               /* first 启动 toggle row                 */
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

/* Send-side mode. The keys are exactly the sender's --mode values and the
 * strings stored in history.ini, so a row can be replayed without a table. */
enum SendMode { SEND_MIRROR = 0, SEND_EXTEND, SEND_STITCH };

static const char *sendModeKey(int mode)
{
    switch (mode) {
    case SEND_EXTEND: return "extend";
    case SEND_STITCH: return "stitch";
    default:          return "mirror";
    }
}
static int sendModeFromKey(const std::string &key)
{
    if (key == "extend") return SEND_EXTEND;
    if (key == "stitch") return SEND_STITCH;
    return SEND_MIRROR;
}
static const char *sendModeLabel(int mode)
{
    switch (mode) {
    case SEND_EXTEND: return "扩展";
    case SEND_STITCH: return "拼接";
    default:          return "投屏";
    }
}

/* Sender connection phase. The sender used to fold 连接中 / 等待密码 / 等待授权 /
 * 验证中 / 已授权未开流 into one "会话进行中" screen; the phase is now the only
 * source for that screen's copy, so waiting is never shown as an active
 * session. STAT|event=start (video) or event=stitch (拼接) is the only way into
 * SP_LIVE. */
enum SendPhase {
    SP_IDLE = 0,
    SP_CONNECTING,
    SP_PASSWORD,
    SP_WAIT_AUTH,
    SP_VERIFY,
    SP_GRANTED,
    SP_LIVE,
    SP_FAILED
};
static SendPhase g_send_phase = SP_IDLE;

/* STAT lines are last-wins and can arrive out of order on a slow link: once the
 * session is live, an older auth line must not drag the UI back to 等待授权. */
static void setSendPhase(SendPhase p)
{
    if (g_send_phase == SP_LIVE &&
        (p == SP_CONNECTING || p == SP_PASSWORD || p == SP_WAIT_AUTH ||
         p == SP_VERIFY || p == SP_GRANTED))
        return;
    g_send_phase = p;
}

/* ── button ids ──────────────────────────────────────────────────────────── */
enum BtnId {
    BTN_NONE = -1,
    /* home */
    BTN_HOME_RECEIVE = 100,
    BTN_HOME_SEND,
    BTN_HOME_RECEIVE_STOP,
    BTN_HOME_SENDER_STOP,
    BTN_HOME_CONNECT,                   /* 连接设备 hero card (102) */
    BTN_HOME_RECV_START,                /* 接收状态栏 开启 / 重试   */
    BTN_HOME_RECV_DETAIL,               /* 接收状态栏 → 接收详情    */
    BTN_HOME_RECV_SETTINGS,             /* 接收状态栏 → 接收与安全  */
    /* receiving */
    BTN_RECV_STOP = 200,
    BTN_RECV_LOG,
    BTN_RECV_DETAILS,
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
    BTN_STREAM_DETAILS,
    BTN_STREAM_RETRY,
    BTN_STREAM_BACK,
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
    BTN_SET_CANCEL = 7052,
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
    BTN_HIST_BASE = 5000,               /* row i: +i*10 +0 row / +1 投屏 / +2 扩展
                                         * / +3 拼接 / +4 删除 */
    BTN_HIST_CLEAR = 5300,
    /* device rows on FOUND */
    BTN_DEV_BASE = 1000,                /* +i*10 +1 投屏 / +2 扩展 / +3 拼接 */
    /* left nav sidebar */
    BTN_NAV_HOME = 8000,
    BTN_NAV_SETTINGS,
    BTN_RETURN_SESSION = 8010,
    /* Header appearance switcher. It shares the 8000 block with the nav ids
     * (8000..8010) — keep any new nav item below BTN_THEME_LIGHT. See
     * kThemeIds for how the three ids are dispatched. */
    BTN_THEME_LIGHT = 8020,
    BTN_THEME_DARK,
    BTN_THEME_SYSTEM,
    BTN_HDR_UPDATE,                     /* header 更新 button (only while a
                                         * release is waiting); matches against
                                         * nothing else, so it may sit outside
                                         * the kThemeIds table */
    /* 拼接 direction: the modal chooser shown right after clicking 拼接, and
     * the live four-way switch on the session screen. Same order as KvmSide
     * (left/right/top/bottom), so id - base indexes the side tables. */
    BTN_STITCH_SIDE_BASE = 9000,        /* +0..3, plus cancel */
    BTN_STITCH_SIDE_CANCEL = 9004,
    BTN_STITCH_SWITCH_BASE = 9010,      /* +0..3 */

    /* authorization modals (receiver approval / sender password) */
    BTN_AUTH_ALLOW  = 9100,
    BTN_AUTH_DENY,
    BTN_PW_CONNECT  = 9110,
    BTN_PW_CANCEL,
    BTN_PW_SHOW,

    /* settings tabs + 接收与安全 controls (below the nav block, above the
     * stitch block; exact-match ids, so no range arithmetic) */
    BTN_SET_TAB_BASE  = 8030,           /* +0 常规 / +1 画质 / +2 接收与安全 */
    BTN_SET_ALLOW_RECV = 8040,          /* 允许其他设备连接到此电脑 */
    BTN_SET_POLICY_OPEN,                /* 允许任意连接             */
    BTN_SET_POLICY_PASSWORD,            /* 仅允许密码连接           */
    BTN_SET_POLICY_MANUAL,              /* 每次手动授权             */
    BTN_SET_PW_EDIT,                    /* 设置 / 修改连接密码      */
    BTN_SETPW_OK      = 9120,           /* password editor modal */
    BTN_SETPW_CANCEL,
    BTN_SETPW_SHOW,
    BTN_SETPW_FIELD_A,
    BTN_DISCARD_KEEP = 9130,
    BTN_DISCARD_CONFIRM,
};

/* 接收与安全 policy radios — kept outside the enum arithmetic so the three
 * policies cannot drift from auth.h's MivaraAuthMode values. */
static const int kAuthModeBtn[3] = {BTN_SET_POLICY_OPEN,
                                    BTN_SET_POLICY_PASSWORD,
                                    BTN_SET_POLICY_MANUAL};
static const char *kAuthModeKey[3] = {"open", "password", "manual"};

/* Appearance has a single control (the header switcher, 8020..8022), but its
 * ids are still matched against a table rather than derived by subtracting a
 * base. The derived form was written for two ranges (the header and the since-
 * removed settings 外观 card):
 *     (id >= BTN_SET_THEME_BASE) ? id - BTN_SET_THEME_BASE : id - BTN_THEME_LIGHT
 * Because that test was true for both ranges, every header click took the first
 * branch and indexed out of bounds, so the switcher appeared to jump to 亮色.
 * A table makes that class of bug impossible; keep it even with one range. */
struct ThemeIdEntry { int id; ThemeMode mode; };
static constexpr ThemeIdEntry kThemeIds[] = {
    {BTN_THEME_LIGHT,   ThemeMode::Light},
    {BTN_THEME_DARK,    ThemeMode::Dark},
    {BTN_THEME_SYSTEM,  ThemeMode::System},
};
static constexpr int kThemeIdCount = (int)(sizeof(kThemeIds) / sizeof(kThemeIds[0]));

/* Two entries sharing an id would be a collision — the kind that once turned
 * 「打开下载页」 into "switch to light". */
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
    BTN_V_CHIP_STITCH, /* warningTint chip (拼接)                          */
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
    GLYPH_LOGO,         /* original overlapping-frame Mivara mark   */
};

struct Button {
    SDL_Rect    rect;
    std::string label;
    int         id;
    int         variant = BTN_V_SECONDARY;
    int         glyph   = GLYPH_NONE;
    bool        on      = false;      /* BTN_V_CHOICE: this cell is selected */
    bool        framed  = true;       /* BTN_V_CHOICE: draw the cell's own border */
    bool        enabled = true;
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

static bool childExited(ChildProc &child, const char *name);
static void setScreen(Screen s);

/* spawnChild flags: the control pipe and the visible console are independent.
 * A receiver ALWAYS gets the pipe — it speaks the --gui protocol over
 * stdin/stdout — so "no pipe + --gui" can no longer be constructed. The console
 * bit only decides whether a second, visible console window is allocated. */
enum { SPAWN_PIPES = 1u, SPAWN_CONSOLE = 2u };

/* Last-wins decoded STAT| payload. */
struct LiveStats {
    bool valid = false;
    std::string event, msg;
    std::string fps, kbps, codec, hw, mode, src, dst, quality, frames, t;
    std::string state;      /* event=stitch: waiting | local | remote */
    std::string side;       /* event=stitch: left | right | top | bottom */
    std::string id;         /* event=auth: approval request id            */
    std::string dev, ip;    /* event=auth|start|stop: peer identity      */
    std::string req;        /* event=auth: video (8081) | stitch (8084)  */
    std::string policy;     /* receiver: open | password | manual        */
    std::string record;     /* receiver: ok | bad (password record)      */
    std::string svc;        /* receiver: gpu | ports | kvm (event=svc)   */
    std::string stage;      /* receiver: service startup stage           */
    std::string err;        /* receiver: service error token             */
    int         port = 0;   /* receiver: service port                    */
};

/* The 拼接 chooser holds the device it was opened for; only valid while
 * `open`. The modal is an overlay, not a Screen, so backing out returns to
 * the list it was opened from. */
struct StitchAsk {
    bool open = false;
    std::string ip;
    std::string name;
    int port = KVM_PORT;
};

/* ── receive state (from the receiver's STAT lines) ───────────────────────
 * The home screen shows a low-weight status bar instead of a hero card, so
 * every state the receiver can be in needs a name here. RSV_OFF means the
 * receiver child is not running; RSV_FAILED keeps the error token until the
 * user retries. */
enum RecvState {
    RSV_OFF = 0,      /* 接收已关闭            */
    RSV_STARTING,     /* 正在开启              */
    RSV_WAITING,      /* 等待连接              */
    RSV_AUTH,         /* 等待授权              */
    RSV_STREAMING,    /* 正在接收              */
    RSV_FAILED        /* 开启失败              */
};

/* A manual-approval request from the receiver (event=auth, state=pending).
 * The receiver auto-denies after 30 s; the deadline here only stops the app
 * from showing a dialog the receiver has already given up on. */
struct AuthAsk {
    bool   open = false;
    long   id = 0;
    std::string dev, ip;
    std::string req;              /* "video" | "stitch" (empty = video) */
    Uint32 deadline = 0;
};

/* Sender-side password prompt (event=auth, state=need_password). */
struct PwAsk {
    bool   open = false;
    bool   show = false;          /* 显示密码 toggle */
    std::string text;
    std::string error;
    Uint32 deadline = 0;          /* sender gives up 120 s after asking */
};

/* 设置 → 接收与安全: the single-field password editor. Only the PBKDF2
 * record is kept; the plaintext is dropped as soon as it is hashed. */
struct SetPwAsk {
    bool   open = false;
    bool   show = false;
    std::string a, err;
};

/* Startup result of the auxiliary receivers (8082 计算 / 8083 端口 / 8084 拼接).
 * The primary video listener (8081) is tracked by RecvState: only its failure
 * is 开启失败. A failed side service degrades the receive state instead. */
struct RecvServiceState {
    int         state = -1;       /* -1 unknown · 0 ok · 1 fail · 2 na */
    int         port = 0;
    std::string err;              /* stable token (bind_failed, …) */
};
struct RecvServices {
    RecvServiceState gpu, ports, kvm;
    RecvServices() { gpu.port = 8082; ports.port = 8083; kvm.port = 8084; }
};

/* A queued STAT line plus the receiver generation that produced it. Lines from
 * a previous receiver process must never resurrect 等待连接 after a stop. */
struct StatMsg {
    std::string line;
    bool        from_receiver = false;
    unsigned long epoch = 0;
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

enum class StatusTone { Info, Success, Warning, Error };

static Screen                g_screen = SCR_HOME;
static Screen                g_session_screen = SCR_HOME; /* home-flow screen when browsing other tabs */
static std::vector<Button>   g_buttons;
static size_t                g_modal_button_begin = 0;
static std::string           g_manual_ip;
static bool                  g_manual_field_error = false;
static std::string           g_status_note;
static StatusTone            g_status_tone = StatusTone::Error;
static std::string           g_child_label;
static bool                  g_child_extend = false;
static int                   g_child_mode = SEND_MIRROR;
static bool                  g_history_recorded = false;
static bool                  g_sender_stop_requested = false;
static bool                  g_details_open = false;
static bool                  g_recv_details_open = false;

static std::vector<DiscoveredDevice> g_devices;
static std::mutex                  g_dev_mtx;
static std::atomic<bool>           g_searching{false};
static std::atomic<bool>           g_search_cancel{false};
static std::atomic<int>            g_found_count{0};
static std::atomic<unsigned long>  g_search_generation{0};
static std::thread                 g_search_thread;
struct SearchUpdate {
    unsigned long generation = 0;
    DiscoveredDevice device{"", 0};
    bool finished = false;
};
static std::vector<SearchUpdate>   g_search_updates;

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
static StitchAsk g_stitch_ask;          /* 拼接 direction modal (if open) */
static std::atomic<int> g_recv_state{RSV_OFF};
static std::atomic<unsigned long> g_recv_epoch{0}; /* incremented per launch/stop */
static std::mutex       g_recv_mtx;
static std::string      g_recv_policy = "manual";  /* open | password | manual */
static std::string      g_recv_peer;               /* device · ip of the session/request */
static std::string      g_recv_error;              /* last receiver error token */
static RecvServices     g_recv_services;           /* guarded by g_recv_mtx */
static Uint32           g_recv_started_at = 0;     /* SDL_GetTicks() of STARTING */
static AuthAsk          g_auth_ask;
static PwAsk            g_pw_ask;
static SetPwAsk         g_set_pw;
static bool             g_discard_open = false;
static bool             g_discard_to_home = false;
static int              g_set_tab = 0;      /* 0 常规 / 1 画质与编码 / 2 接收与安全 */
static std::string      g_auth_wait_note;  /* sender: 等待授权 / 正在验证 */

/* Windows tray state. `g_hidden` means the main window is hidden but the
 * process (and its children) keep running; `g_tray_ok` is false on POSIX or
 * when the shell refused the icon, in which case closing the window exits as
 * before. */
static bool             g_tray_ok = false;
static bool             g_hidden = false;
static std::string      g_tray_tip_last;
static bool             g_exit_requested = false;  /* UI asked to quit      */
static bool             g_exit_no_confirm = false; /* installer hand-over  */

/* STAT lines arrive on the child readers' threads; only the GUI thread may
 * touch SDL / the dialogs, so the readers enqueue and the loop drains. */
static std::mutex g_stat_q_mtx;
static std::deque<StatMsg> g_stat_q;
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

/* ── 拼接 sides ──────────────────────────────────────────────────────────── */
/* Order matches KvmSide and the BTN_STITCH_*_BASE offsets. "左侧" = the peer
 * sits to the left of this machine, so that is also the edge the cursor must
 * leave through. */
static const char *kStitchSideKey[4]   = {"left", "right", "top", "bottom"};
static const char *kStitchSideLabel[4] = {"左侧", "右侧", "上方", "下方"};
static const char *kStitchEnterEdge[4] = {"左缘", "右缘", "上缘", "下缘"};
static const char *kStitchBackEdge[4]  = {"右缘", "左缘", "下缘", "上缘"};

static int stitchSideFromConfig()
{
    const int s = kvmSideFromKey(g_cfg.stitch_side.c_str());
    return s < 0 ? (int)KVM_SIDE_RIGHT : s;
}

/* The side the running controller reports; the saved preference is the
 * fallback before the first STAT line (and in console mode, which has no
 * stdin channel and therefore never switches). */
static int stitchSideCurrent()
{
    std::lock_guard<std::mutex> lk(g_stats_mtx);
    const int s = kvmSideFromKey(g_stats.side.c_str());
    return s >= 0 ? s : stitchSideFromConfig();
}

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
 * Glyphs are centred on (cx, cy) in a nominal size box and painted through
 * the same coverage-mask path as the rest of the interface.
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

static void glyphLogo(SDL_Renderer *ren, int cx, int cy, int size, SDL_Color c)
{
    const int left = cx - size / 2, top = cy - size / 2;
    auto s = [&](int v) { return (int)std::lround(v * size / 256.0); };
    auto rc = [&](int x, int y, int w, int h) {
        return SDL_Rect{left + s(x), top + s(y), s(w), s(h)};
    };
    auto erase = [&](SDL_Rect r, int rad) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        fillRoundRectRaw(ren, r, rad, SDL_Color{0, 0, 0, 0});
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    };
    auto frame = [&](int x, int y, int w, int h, int rad) {
        const SDL_Rect outer = rc(x, y, w, h);
        const int stroke = s(16);
        fillRoundRect(ren, outer, s(rad), c);
        erase(SDL_Rect{outer.x + stroke, outer.y + stroke,
                       outer.w - 2 * stroke, outer.h - 2 * stroke},
              std::max(1, s(rad) - stroke));
    };
    frame(32, 32, 136, 136, 22);
    erase(rc(80, 80, 152, 152), s(30));
    frame(88, 88, 136, 136, 22);
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
    case GLYPH_LOGO:       glyphLogo(ren, cx, cy, size, WHITE); break;
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

    const bool hot   = b.enabled && (g_hot == b.id);
    const bool press = b.enabled && (g_press == b.id);
    const int  r     = THEME.radius;
    /* Buttons use the bold small face; a caller may override for tight cells. */
    TTF_Font *f = b.font ? b.font : g_fontBS;

    auto centred = [&](SDL_Color c) {
        drawTextV(ren, b.label, b.rect.x + b.rect.w / 2, b.rect.y + b.rect.h / 2,
                  c, f, ALIGN_CENTER);
    };
    if (!b.enabled) {
        drawRoundRect(ren, b.rect, r, THEME.surfaceAlt, THEME.border);
        centred(THEME.textDim);
        return;
    }
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
    case BTN_V_CHIP_ACC:
    case BTN_V_CHIP_STITCH: {
        const bool ok  = (b.variant == BTN_V_CHIP_OK);
        const bool acc = (b.variant == BTN_V_CHIP_ACC);
        fillRoundRect(ren, b.rect, r - 3,
                      ok ? THEME.successTint
                         : (acc ? THEME.primaryTint : THEME.warningTint));
        wash(WHITE, 0x1F);                 /* light fill, dark label → lighten */
        centred(ok ? THEME.onSuccessTint
                   : (acc ? THEME.onPrimaryTint : THEME.onWarningTint));
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
                      TTF_Font *font = nullptr, bool enabled = true)
{
    Button b;
    b.rect    = SDL_Rect{x, y, w, h};
    b.label   = label;
    b.id      = id;
    b.variant = variant;
    b.glyph   = glyph;
    b.font    = font;
    b.enabled = enabled;
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

static SDL_Color statusToneColor()
{
    switch (g_status_tone) {
    case StatusTone::Success: return THEME.success;
    case StatusTone::Warning: return THEME.warning;
    case StatusTone::Info:    return THEME.textDim;
    case StatusTone::Error:
    default:                  return THEME.danger;
    }
}

static std::string sendPhaseLabel()
{
    switch (g_send_phase) {
    case SP_CONNECTING: return "正在连接";
    case SP_PASSWORD:   return "需要连接密码";
    case SP_WAIT_AUTH:  return "等待对方授权";
    case SP_VERIFY:     return "正在验证密码";
    case SP_GRANTED:    return "对方已允许连接";
    case SP_LIVE:       return "正在" + g_child_label;
    case SP_FAILED:     return "连接失败";
    case SP_IDLE:
    default:            return "会话已结束";
    }
}

static SDL_Color sendPhaseColor()
{
    if (g_send_phase == SP_LIVE) return THEME.success;
    if (g_send_phase == SP_FAILED) return THEME.danger;
    if (g_send_phase == SP_PASSWORD || g_send_phase == SP_WAIT_AUTH ||
        g_send_phase == SP_VERIFY || g_send_phase == SP_GRANTED)
        return THEME.warning;
    if (g_send_phase == SP_IDLE) return THEME.textDim;
    return THEME.primary;
}

static std::string sendTargetName()
{
    return g_last_name.empty() ? g_last_ip : g_last_name;
}

static std::string sendTargetEndpoint()
{
    return g_last_ip + ":" +
           std::to_string(g_child_mode == SEND_STITCH ? KVM_PORT : g_last_port);
}

static void recordStartedSession()
{
    if (g_history_recorded || g_last_ip.empty()) return;
    g_history_recorded = true;
    historyUpsert(g_last_ip, g_last_port,
                  g_last_name.empty() ? g_last_ip : g_last_name,
                  sendModeKey(g_child_mode));
    g_hist = historyLoad();
    g_hist_scroll = 0;
}

static bool validIPv4Address(const std::string &ip)
{
    if (ip.empty()) return false;
    int parts = 0;
    size_t begin = 0;
    while (begin < ip.size()) {
        const size_t end = ip.find('.', begin);
        if (end == begin) return false;
        int value = 0, digits = 0;
        for (size_t i = begin; i < end && i < ip.size(); i++) {
            const char c = ip[i];
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
            if (++digits > 3 || value > 255) return false;
        }
        if (digits == 0) return false;
        parts++;
        if (end == std::string::npos) return parts == 4;
        begin = end + 1;
    }
    return false;
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

static void closeAuthAsk(long id);
static void openPasswordAsk(const std::string &error);
static void showFromTray();

/* Short, non-leaking description of a receiver error token. */
static std::string receiverErrorText(const std::string &msg)
{
    if (msg == "bind_failed")         return "端口 8081 被占用或被防火墙拦截";
    if (msg == "socket_init_failed")  return "网络初始化失败";
    if (msg == "auth_failed")         return "连接密码错误";
    if (msg == "auth_throttled")      return "密码错误次数过多，请稍后再试";
    if (msg == "auth_timeout")        return "授权超时，已自动拒绝";
    if (msg == "auth_denied")         return "已拒绝连接请求";
    if (msg == "auth_config")         return "连接密码记录无效，请在设置中重新设置";
    if (msg == "spawn_failed")        return "接收程序启动失败";
    if (msg == "not_installed")       return "找不到 receiver 程序，请重新安装或运行 make";
    if (msg == "start_timeout")       return "接收服务启动超时，请重试";
    if (msg == "receiver_exited")     return "接收服务意外退出，请重试";
    if (msg == "receiver_unresponsive") return "接收进程无响应，请重新开启接收";
    if (msg == "version_mismatch")    return "对方版本不兼容，请将两端更新到同一版本";
    if (msg.empty())                  return "接收服务出现异常";
    return msg;
}

/* Sender-side failure tokens → user-facing text. The sender reports stable
 * tokens on STAT|event=error, and the PRD requires the user to be able to tell
 * a wrong password from a refusal, a timeout, a busy peer or a version skew.
 * Unknown values are shown unchanged: kvm.cpp already emits Chinese text. */
static std::string senderErrorText(const std::string &msg)
{
    if (msg == "bad_password")    return "连接密码错误";
    if (msg == "auth_timeout")    return "对方未在 30 秒内确认，已自动拒绝";
    if (msg == "auth_denied")     return "对方拒绝了连接请求";
    if (msg == "peer_busy")       return "对方正在接收其他连接，请稍后再试";
    if (msg == "auth_config")     return "对方接收授权配置异常，请在对方设置中重新设置连接密码";
    if (msg == "version_mismatch") return "两端版本不一致，请将软件更新到同一版本";
    if (msg == "legacy_peer")     return "对方版本过旧，不支持连接授权，请更新两端";
    if (msg == "auth_failed")     return "授权过程中连接已断开";
    if (msg == "codec_rejected")  return "对方不支持所选编码，请更换编码方式";
    if (msg == "dims_rejected")   return "分辨率不被对方接受，请调整缩放";
    if (msg == "decoder_unavailable") return "对方解码器不可用，请更换编码方式";
    if (msg == "handshake_rejected")  return "对方拒绝了握手";
    if (msg == "connect_failed")      return "无法连接对方（接收服务未开启或网络不可达）";
    if (msg == "no_response")         return "对方未响应，请检查接收服务与网络连接";
    if (msg == "protocol_error")      return "连接协议异常，请确认两端使用同一版本";
    if (msg == "encode_failed")       return "画面编码失败，请尝试更换编码方式";
    if (msg == "send_failed")         return "网络连接中断";
    if (msg == "encoder_open_failed" || msg == "encoder_rebuild_failed")
        return "编码器初始化失败，请更换编码方式";
    if (msg == "capture_failed_or_display_changed")
        return "屏幕采集失败，或显示配置已改变";
    if (msg == "vdd_startup_failed")  return "虚拟显示器启动失败";
    if (msg == "vdd_lost")            return "虚拟显示器已断开";
    if (msg == "resize_notify_failed" || msg == "reconfig_notify_failed")
        return "通知对方调整参数失败";
    return msg;
}

/* Reason text for an auxiliary service error token (never a password). */
static std::string recvServiceErrorText(const std::string &err)
{
    if (err == "bind_failed")        return "端口被占用或被防火墙拦截";
    if (err == "socket_init_failed") return "网络初始化失败";
    if (err == "unsupported")        return "当前平台不支持";
    return err;
}

/* "部分服务不可用：计算 8082（端口被占用…）、拼接 8084（…）"
 * 8081 is the primary listener and is represented by RSV_FAILED, not here.
 * state=na (e.g. 拼接 on POSIX) is "not applicable", not a problem. */
static std::string recvServicesIssue(const RecvServices &svc)
{
    struct Item { const char *label; const RecvServiceState *st; };
    const Item items[3] = {{"计算", &svc.gpu},
                           {"端口", &svc.ports},
                           {"拼接", &svc.kvm}};
    std::string out;
    for (const Item &it : items) {
        if (it.st->state != 1) continue;     /* only fail() is a problem */
        out += out.empty() ? "部分服务不可用：" : "、";
        out += it.label;
        out += " " + std::to_string(it.st->port);
        const std::string why = recvServiceErrorText(it.st->err);
        if (!why.empty()) out += "（" + why + "）";
    }
    return out;
}

/* Home-screen status bar state, driven by the receiver's STAT lines. */
static void applyReceiverStat(const LiveStats &s)
{
    if (s.event == "policy") {
        bool recovered = false;
        bool bad = false;
        {
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            if (!s.mode.empty()) g_recv_policy = s.mode;
            bad = (s.mode == "password" && s.record == "bad");
            /* A usable record — or a non-password policy, which has no record
             * at all — resolves 授权配置无效. Nothing else may clear it. */
            if (g_recv_error == "auth_config" &&
                (s.record == "ok" ||
                 (!s.mode.empty() && s.mode != "password"))) {
                g_recv_error.clear();
                recovered = true;
            }
        }
        if (bad) {
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            g_recv_error = "auth_config";
            /* An active session is real; the new policy only affects new
             * connections, so do not paint the session as failed. */
            if (g_recv_state.load() != RSV_STREAMING) g_recv_state = RSV_FAILED;
            return;
        }
        if (recovered && g_receiver.active) g_recv_state = RSV_WAITING;
        return;
    }
    if (s.event == "ready" || s.event == "waiting") {
        /* A corrupt password record leaves the service listening but rejects
         * every connection. A later ready/waiting must never paint that as
         * 等待连接 — only a policy line with record=ok clears it. */
        if (s.record == "bad" || s.state == "error") {
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            g_recv_error = "auth_config";
            if (!s.policy.empty()) g_recv_policy = s.policy;
            if (g_recv_state.load() != RSV_STREAMING) g_recv_state = RSV_FAILED;
            if (g_hidden)
                trayNotify("接收服务异常", receiverErrorText("auth_config"));
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            if (!s.policy.empty()) g_recv_policy = s.policy;
            if (g_recv_error == "auth_config") {
                if (g_recv_state.load() != RSV_STREAMING) g_recv_state = RSV_FAILED;
                return;                     /* sticky until record=ok */
            }
            g_recv_error.clear();
        }
        const int cur = g_recv_state.load();
        if (cur != RSV_STREAMING && cur != RSV_AUTH)
            g_recv_state = RSV_WAITING;
        return;
    }
    if (s.event == "auth") {
        if (s.state == "pending") {
            g_auth_ask.open = true;
            g_auth_ask.id = std::atol(s.id.c_str());
            g_auth_ask.dev = s.dev;
            g_auth_ask.ip = s.ip;
            g_auth_ask.req = s.req;
            g_auth_ask.deadline = SDL_GetTicks() + 30000;
            g_recv_state = RSV_AUTH;
            {
                std::lock_guard<std::mutex> lk(g_recv_mtx);
                g_recv_peer = (s.dev.empty() ? s.ip : s.dev) +
                              (s.ip.empty() ? "" : " · " + s.ip);
            }
            /* The prompt has a 30 s deadline, so a hidden window must come
             * back immediately — a balloon alone would not let the user answer
             * in time. */
            if (g_hidden) {
                showFromTray();
                trayNotify("有设备请求连接",
                           (s.dev.empty() ? s.ip : s.dev) +
                               (s.req == "stitch"
                                    ? " 请求拼接控制，请在 30 秒内选择"
                                    : " 请求投屏/扩展，请在 30 秒内选择"));
            }
        } else if (s.state == "done") {
            const long id = std::atol(s.id.c_str());
            /* Only the request that owns the visible prompt may return the
             * state to "waiting"; a late done for an older id must not clear
             * a prompt the user is still looking at. */
            const bool current = g_auth_ask.open && g_auth_ask.id == id;
            closeAuthAsk(id);
            if (current && g_receiver.active) g_recv_state = RSV_WAITING;
        }
        return;
    }
    if (s.event == "start") {
        g_recv_state = RSV_STREAMING;
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        g_recv_peer = (s.dev.empty() ? s.ip : s.dev) +
                      (s.ip.empty() ? "" : " · " + s.ip);
        /* A corrupt record cannot produce a real session; keep the token. */
        if (g_recv_error != "auth_config") g_recv_error.clear();
        return;
    }
    if (s.event == "stop") {
        if (g_receiver.active) g_recv_state = RSV_WAITING;
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        g_recv_peer.clear();
        return;
    }
    if (s.event == "svc") {
        int val;
        if (s.state == "ok")       val = 0;
        else if (s.state == "fail") val = 1;
        else if (s.state == "na")   val = 2;
        else return;
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        RecvServiceState *dst = nullptr;
        if (s.svc == "gpu")        dst = &g_recv_services.gpu;
        else if (s.svc == "ports") dst = &g_recv_services.ports;
        else if (s.svc == "kvm")   dst = &g_recv_services.kvm;
        if (!dst) return;
        dst->state = val;
        if (s.port > 0) dst->port = s.port;
        dst->err = s.err;
        logPush(std::string("[gui] 接收服务 ") + s.svc + "(" +
                std::to_string(dst->port) + "): " + s.state +
                (s.stage.empty() ? "" : " stage=" + s.stage) +
                (s.err.empty() ? "" : " err=" + s.err));
        return;
    }
    if (s.event == "error") {
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        g_recv_error = s.msg;
        /* Per-connection failures leave the service listening; a bind/socket
         * failure or an unusable password record makes the service a real
         * 开启失败. auth_config may also arrive for a second peer while a
         * session is live — that session is still real. */
        const bool fatal = (s.msg == "bind_failed" ||
                            s.msg == "socket_init_failed" ||
                            s.msg == "auth_config");
        if (fatal) {
            if (!(s.msg == "auth_config" &&
                  g_recv_state.load() == RSV_STREAMING))
                g_recv_state = RSV_FAILED;
            /* Errors must stay discoverable while the window is hidden. */
            if (g_hidden) trayNotify("接收服务异常", receiverErrorText(s.msg));
        }
        return;
    }
}

static void parseStatLine(const std::string &line, bool from_receiver)
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
            else if (k == "state") s.state = v;
            else if (k == "side") s.side = v;
            else if (k == "id") s.id = v;
            else if (k == "dev") s.dev = v;
            else if (k == "ip") s.ip = v;
            else if (k == "req") s.req = v;
            else if (k == "policy") s.policy = v;
            else if (k == "record") s.record = v;
            else if (k == "svc") s.svc = v;
            else if (k == "stage") s.stage = v;
            else if (k == "err") s.err = v;
            else if (k == "port") s.port = std::atoi(v.c_str());
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    if (from_receiver) {
        /* The receiver owns the home status bar; its stats never drive the
         * sender's stat panel. */
        applyReceiverStat(s);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        g_stats = s;
    }
    /* Sender authorization progress: the connect screen (and the password
     * modal) follow these lines. The phase — not the last STAT payload — is
     * what renderScreen() reads, so an out-of-order line cannot leave the UI
     * claiming an active session while a password is still being checked. */
    if (s.event == "auth") {
        if (s.state == "need_password") {
            setSendPhase(SP_PASSWORD);
            openPasswordAsk(std::string());
        } else if (s.state == "authenticating") {
            g_pw_ask.open = false;
            setSendPhase(SP_VERIFY);
            g_auth_wait_note = "正在验证连接密码…";
        } else if (s.state == "waiting") {
            g_pw_ask.open = false;
            setSendPhase(SP_WAIT_AUTH);
            g_auth_wait_note = "已发送连接请求，等待对方授权（最多 30 秒）…";
        } else if (s.state == "granted") {
            setSendPhase(SP_GRANTED);
            g_auth_wait_note = "对方已允许连接";
        } else if (s.state == "cancelled") {
            g_pw_ask.open = false;
            setSendPhase(SP_CONNECTING);
            g_auth_wait_note.clear();
        }
    } else if (s.event == "error") {
        if (!(g_sender_stop_requested && s.msg == "send_failed")) {
            setSendPhase(SP_FAILED);
            g_status_tone = StatusTone::Error;
        }
        g_auth_wait_note.clear();
    } else if (s.event == "start") {
        setSendPhase(SP_LIVE);
        recordStartedSession();
        g_auth_wait_note.clear();
    } else if (s.event == "stitch") {
        /* state=waiting is emitted before the 8084 connect and before the peer
         * authorizes, so it is not a live session yet. Only local/remote are. */
        if (s.state != "waiting") {
            setSendPhase(SP_LIVE);
            recordStartedSession();
            g_auth_wait_note.clear();
        }
    } else if (s.event == "stop") {
        setSendPhase(SP_IDLE);
        g_auth_wait_note.clear();
    }
}

static void handleChildLine(const std::string &line, bool from_receiver,
                            unsigned long epoch)
{
    if (line.compare(0, 5, "STAT|") == 0) {
        /* Reader thread: queue it for the GUI thread. Only the GUI thread may
         * open dialogs or touch SDL (SDL_StartTextInput in particular). */
        {
            std::lock_guard<std::mutex> lk(g_stat_q_mtx);
            if (g_stat_q.size() > 512) g_stat_q.pop_front();
            g_stat_q.push_back(StatMsg{line, from_receiver, epoch});
        }
        persistLogLine(line);
    } else {
        logPush(line);
    }
}

/** Apply every queued STAT line. GUI thread only, once per frame.
 *  Receiver lines carry the generation that produced them: a stop/restart
 *  bumps g_recv_epoch, so a teardown ready/waiting/stop line from the previous
 *  process can never put the UI back into 等待连接. */
static void drainChildStats()
{
    std::deque<StatMsg> batch;
    {
        std::lock_guard<std::mutex> lk(g_stat_q_mtx);
        batch.swap(g_stat_q);
    }
    const unsigned long epoch = g_recv_epoch.load();
    for (auto &p : batch) {
        if (p.from_receiver && p.epoch != epoch) continue;
        parseStatLine(p.line, p.from_receiver);
    }
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

/* ── authorization dialogs ───────────────────────────────────────────────── */
/* Receiver side: a peer asks to connect and the policy is 每次手动授权. The
 * dialog mirrors the receiver's own 30 s deadline; when it lapses the
 * receiver denies on its own, so the app only stops showing the prompt. */
static void closeAuthAsk(long id)
{
    if (!g_auth_ask.open) return;
    if (id >= 0 && g_auth_ask.id != id) return;   /* a newer request owns it */
    g_auth_ask.open = false;
}

static void answerAuthAsk(bool allow)
{
    if (!g_auth_ask.open) return;
    const bool ok = sendToChild(g_receiver,
                                "AUTH|id=" + std::to_string(g_auth_ask.id) + "|" +
                                    (allow ? "allow" : "deny"));
    g_auth_ask.open = false;
    if (!ok) {
        /* The decision never reached the receiver. Report the real reason
         * instead of leaving the user believing the peer was answered. */
        if (childExited(g_receiver, "receiver")) {
            drainChildStats();
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            g_recv_peer.clear();
            if (g_recv_error.empty()) g_recv_error = "receiver_exited";
            g_recv_state = RSV_FAILED;
            logPush("[gui] 授权答复失败：接收进程已退出");
        } else {
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            g_recv_error = "receiver_unresponsive";
            g_recv_state = RSV_FAILED;
            logPush("[gui] 授权答复失败：接收进程无响应");
        }
        return;
    }
    if (g_receiver.active) g_recv_state = RSV_WAITING;
}

/* Sender side: the receiver's policy is 仅允许密码连接. The password is typed
 * here, sent over stdin (never argv), and cleared as soon as it is handed
 * over. */
static void openPasswordAsk(const std::string &error)
{
    if (g_pw_ask.open) {
        if (!error.empty()) g_pw_ask.error = error;
        return;
    }
    g_pw_ask.open = true;
    g_pw_ask.show = false;
    g_pw_ask.text.clear();
    g_pw_ask.error = error;
    g_pw_ask.deadline = SDL_GetTicks() + 120000;
    SDL_StartTextInput();
    /* A password prompt is time-limited and must be answerable: a hidden
     * window cannot accept text, and a balloon alone would let the 120 s
     * deadline expire unanswered. */
    if (g_hidden) {
        showFromTray();
        trayNotify("对方要求输入连接密码",
                   "请输入该接收端设置的 4～8 个字符连接密码（字母不区分大小写）；120 秒内未输入将取消连接");
    }
}

static void closePasswordAsk(bool cancel)
{
    if (!g_pw_ask.open) return;
    const bool was_open = g_pw_ask.open;
    g_pw_ask.open = false;
    g_pw_ask.text.assign(g_pw_ask.text.size(), '\0');
    g_pw_ask.text.clear();
    SDL_StopTextInput();
    if (cancel && was_open) {
        sendToChild(g_sender, "AUTH|cancel");
        setSendPhase(SP_CONNECTING);
        g_auth_wait_note.clear();
    }
}

static void submitPassword()
{
    if (!mivaraPasswordValid(g_pw_ask.text)) {
        g_pw_ask.error = mivaraPasswordRule();
        return;
    }
    sendToChild(g_sender, "AUTH|password=" + g_pw_ask.text);
    g_pw_ask.open = false;
    g_pw_ask.text.assign(g_pw_ask.text.size(), '\0');
    g_pw_ask.text.clear();
    SDL_StopTextInput();
    setSendPhase(SP_VERIFY);
    g_auth_wait_note = "正在验证连接密码…";
}

/* Single source of truth for the 4–8 character password field. A too-long
 * input is rejected as a whole and the original text is kept: the value that
 * is later saved/submitted must never be a silently truncated different
 * password. Non-ASCII / invisible bytes are rejected outright instead of
 * being dropped — with no confirmation field left, silently shrinking a
 * pasted password would save something the user never intended. */
static const int kPasswordMaxLen = 8;   /* mirrors the 4–8 rule in auth.h */

static bool appendPasswordInput(std::string &dst, const char *text,
                                std::string &err)
{
    std::string add;
    bool bad = false;
    if (text) {
        for (const char *c = text; *c; c++) {
            const unsigned char u = (unsigned char)*c;
            if (u >= 0x20 && u < 0x7F) add.push_back((char)u);
            else bad = true;
        }
    }
    if (bad) {
        err = "连接密码仅支持 ASCII 可见字符";
        return false;
    }
    if ((int)(dst.size() + add.size()) > kPasswordMaxLen) {
        err = "密码长度为 4～8 个字符";
        return false;
    }
    if (add.empty()) return false;      /* nothing visible: keep old text+err */
    dst += add;
    err.clear();
    return true;
}

/* Receiving-side password editor (设置 → 接收与安全). The plaintext lives only
 * in this field and is wiped on close; config.ini keeps salt + PBKDF2 hash
 * (see auth.h). The 显示密码 link replaces the old confirmation field. */
static void closeSetPwAsk()
{
    if (!g_set_pw.open) return;
    g_set_pw.open = false;
    g_set_pw.a.assign(g_set_pw.a.size(), '\0');
    g_set_pw.a.clear();
    g_set_pw.err.clear();
    SDL_StopTextInput();
}

static void openSetPwAsk()
{
    /* The local 拼接 direction chooser and the password editor are two steps of
     * different flows; keeping both open would stack two modals for no reason. */
    g_stitch_ask.open = false;
    g_set_pw.open = true;
    g_set_pw.show = false;
    g_set_pw.a.clear();
    g_set_pw.err.clear();
    SDL_StartTextInput();
}

static void submitSetPassword()
{
    if (!mivaraPasswordValid(g_set_pw.a)) {
        g_set_pw.err = mivaraPasswordRule();
        return;
    }
    uint8_t salt[16], hash[32];
    uint32_t iter = (g_cfg_edit.auth_iter > 0)
                        ? (uint32_t)g_cfg_edit.auth_iter
                        : 0;
    if (!mivaraAuthMakeRecord(g_set_pw.a, salt, hash, iter)) {
        g_set_pw.err = "无法生成密码记录";
        return;
    }
    g_cfg_edit.auth_salt = mivaraHexEncode(salt, sizeof(salt));
    g_cfg_edit.auth_hash = mivaraHexEncode(hash, sizeof(hash));
    g_cfg_edit.auth_iter = (int)iter;
    closeSetPwAsk();
    g_status_note = "密码已设置，点「保存设置」保存";
}

/* Drop dialogs whose deadline the child owns (receiver: 30 s auto-deny,
 * sender: 120 s password prompt). */
static void tickAuthDialogs()
{
    const Uint32 now = SDL_GetTicks();
    if (g_auth_ask.open && (Sint32)(now - g_auth_ask.deadline) >= 0) {
        closeAuthAsk(-1);
        if (g_receiver.active && g_recv_state.load() == RSV_AUTH)
            g_recv_state = RSV_WAITING;
    }
    if (g_pw_ask.open && (Sint32)(now - g_pw_ask.deadline) >= 0)
        closePasswordAsk(true);
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

static void applyMivaraWindowIcon(SDL_Window *window)
{
    SDL_Surface *icon = mivaraCreateIconSurface();
    if (!icon) return;
    SDL_SetWindowIcon(window, icon);
    SDL_FreeSurface(icon);
}

/* ── log window ──────────────────────────────────────────────────────────── */
static void openLogWindow()
{
    if (g_log_open) return;
    g_log_win = SDL_CreateWindow("Mivara 日志",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 uiPx(LOG_WIN_W), uiPx(LOG_WIN_H), 0);
    if (!g_log_win) return;
    applyMivaraWindowIcon(g_log_win);
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

static bool spawnChild(ChildProc &child, const std::string &path,
                       const std::vector<std::string> &args,
                       unsigned spawn_flags, bool is_receiver = false,
                       unsigned long epoch = 0)
{
    if (child.active) return false;
    if (child.reader.joinable()) child.reader.join();

    /* Defence in depth: every receiver is launched with --gui and therefore
     * needs the stdin/stdout channel, whatever the caller passed. */
    if (is_receiver) spawn_flags |= SPAWN_PIPES;
    const bool pipes = (spawn_flags & SPAWN_PIPES) != 0;

#ifdef _WIN32
    const bool console = (spawn_flags & SPAWN_CONSOLE) != 0;
    std::string cmd = "\"" + path + "\"";
    for (auto &a : args) cmd += " " + a;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    ZeroMemory(&child.pi, sizeof(child.pi));

    HANDLE hWrite = nullptr;
    HANDLE hInRead = nullptr;
    /* Console and pipe are independent: the console bit only chooses whether a
     * visible cmd window is allocated. CREATE_NO_WINDOW still allocates a
     * (hidden) console, so AttachConsole() in stopChild() keeps working either
     * way; with both bits set the child gets a visible console while its
     * stdout/stderr go to the pipe. */
    DWORD flags = console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    if (pipes) {
        /* Anonymous pipes: stdout/stderr → reader thread, stdin ← AUTH/RX/SETP
         * lines. This is the channel the --gui receiver protocol depends on. */
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
                             pipes ? TRUE : FALSE, flags,
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
    if (pipes && child.stdout_read) {
        HANDLE hRead = child.stdout_read;
        child.reader = std::thread([hRead, is_receiver, epoch] {
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
                    handleChildLine(line, is_receiver, epoch);
                }
            }
            if (!partial.empty()) handleChildLine(partial, is_receiver, epoch);
        });
    }
    return true;
#else
    int fds[2] = {-1, -1};
    int in_fds[2] = {-1, -1};
    if (pipes) {
        if (pipe(fds) != 0) return false;
        if (pipe(in_fds) != 0) { close(fds[0]); close(fds[1]); return false; }
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (pipes) {
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
        if (pipes) {
            close(fds[0]); close(fds[1]);
            close(in_fds[0]); close(in_fds[1]);
        }
        return false;
    }
    if (pipes) {
        close(fds[1]);
        child.stdout_fd = fds[0];
        close(in_fds[0]);
        child.stdin_fd = in_fds[1];
        int fd = fds[0];
        child.reader = std::thread([fd, is_receiver, epoch] {
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
                    handleChildLine(line, is_receiver, epoch);
                }
            }
            if (!partial.empty()) handleChildLine(partial, is_receiver, epoch);
        });
    }
    child.pid = pid;
    child.active = true;
    return true;
#endif
}

#ifdef _WIN32
/* A NULL handler (SetConsoleCtrlHandler(nullptr, TRUE)) only swallows Ctrl+C,
 * not Ctrl+Break. The GUI attaches to the child's console before broadcasting
 * a break, so it needs a handler that also covers CTRL_BREAK_EVENT — otherwise
 * the stop signal would kill this process. Everything else stays default. */
static BOOL WINAPI stopIgnoreCtrlHandler(DWORD type)
{
    return (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) ? TRUE : FALSE;
}
#endif

/* Graceful stop: ask the child over its stdin pipe, falling back to a console
 * event, then to a hard kill. The pipe line (shared with the receiver) is the
 * primary path: on current Windows (verified on 11 26200) a GUI parent cannot
 * deliver Ctrl+C to a child console at all — GenerateConsoleCtrlEvent returns
 * success but no handler ever runs — so the old Ctrl+C-only sequence always
 * sat out the full 8 s timeout before TerminateProcess. Ctrl+Break *is*
 * delivered and remains the fallback for the visible-status-console launch
 * (no stdin pipe). Either way cleanup (stats flush, vddShutdown) still runs
 * before the 8 s cap, and a hard kill then force-detaches the virtual display
 * so it can never linger after a stop. */
static void stopChild(ChildProc &child, bool extend = false)
{
    if (!child.active) return;
#ifdef _WIN32
    bool hard_kill = false;
    bool exited = false;
    const DWORD deadline = GetTickCount() + 8000;

    /* 1) stdin command. The line is longer than the 8-character password
     * rule, so a child waiting at the interactive prompt cannot mistake it
     * for a password. */
    const bool asked = sendToChild(child, "STOP|mivara");
    if (asked)
        exited = WaitForSingleObject(child.pi.hProcess, 8000) == WAIT_OBJECT_0;

    /* 2) console fallback: no pipe (visible status console), or a child that
     * ignored the command. Never broadcast without the guard handler. */
    if (!exited) {
        const DWORD left = deadline > GetTickCount()
                               ? deadline - GetTickCount() : 0;
        FreeConsole();
        SetConsoleCtrlHandler(stopIgnoreCtrlHandler, TRUE);
        if (AttachConsole(child.pi.dwProcessId)) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
            /* Detach BEFORE waiting: when the child exits the visible console
             * is torn down and an attached process is terminated by the
             * resulting close event (verified — it survives only if this
             * FreeConsole happens first). */
            FreeConsole();
            exited = WaitForSingleObject(child.pi.hProcess, left) == WAIT_OBJECT_0;
        }
        SetConsoleCtrlHandler(stopIgnoreCtrlHandler, FALSE);
    }
    if (!exited) {
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

/* A control line (AUTH reply, RX|reload) did not reach the receiver. Report
 * the real reason: a dead child and a live-but-stuck child are different
 * states and must not both look like "saved". */
static void noteReceiverControlFailure(const char *what)
{
    if (childExited(g_receiver, "receiver")) {
        drainChildStats();
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        g_recv_peer.clear();
        if (g_recv_error.empty()) g_recv_error = "receiver_exited";
        g_recv_state = RSV_FAILED;
        g_status_note = "接收进程已退出，操作未生效";
        logPush(std::string("[gui] ") + what + "失败：接收进程已退出");
    } else {
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        g_recv_error = "receiver_unresponsive";
        g_recv_state = RSV_FAILED;
        g_status_note = std::string(what) + "未生效：接收进程无响应";
        logPush(std::string("[gui] ") + what + "失败：接收进程无响应");
    }
}

/* Every sender stop path must also leave the connection phase: a dead child
 * must never keep the home screen showing 等待授权/会话进行中. */
static void stopSender()
{
    g_sender_stop_requested = true;
    stopChild(g_sender, g_child_extend);
    g_child_extend = false;
    g_send_phase = SP_IDLE;
    g_auth_wait_note.clear();
    g_status_tone = StatusTone::Info;
}

/* A receiver that never reports ready is not "正在开启" forever. Ten seconds
 * covers a cold start plus service-thread startup; the child is killed so a
 * retry can bind the ports. */
static void tickRecvState()
{
    if (g_recv_state.load() != RSV_STARTING || g_recv_started_at == 0) return;
    if (SDL_GetTicks() - g_recv_started_at <= 10000) return;
    ++g_recv_epoch;
    if (g_receiver.active) stopChild(g_receiver);
    closeAuthAsk(-1);
    {
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        g_recv_peer.clear();
        g_recv_error = "start_timeout";
    }
    g_recv_started_at = 0;
    g_recv_state = RSV_FAILED;
    g_status_note = "启动接收超时，请重试";
    logPush("[gui] 启动接收超时（10 秒未就绪）");
    if (g_screen == SCR_RECEIVING) setScreen(SCR_HOME);
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
#ifdef _WIN32
static const wchar_t *kAutoStartTask = L"Mivara";

/* Does the logon task exist? schtasks exits non-zero when it does not, which
 * makes this the read-back used to verify a registration. */
static bool autoStartRegistered()
{
    return runHiddenW(sysExePath(L"schtasks.exe") + L" /query /tn \"" +
                      kAutoStartTask + L"\"");
}
#endif

/* Enable or disable the logon task, then return the state Task Scheduler
 * reports — never the requested one. A config flag that says "on" while the
 * task is absent would make the switch lie and launch nothing at logon. */
static bool applyAutoStart(int enable)
{
#ifdef _WIN32
    const std::wstring schtasks = sysExePath(L"schtasks.exe");
    if (enable) {
        /* /tr value must contain an embedded quoted path (spaces in
         * "Program Files"); /rl highest is what lets this requireAdministrator
         * app start at logon without a UAC prompt. */
        runHiddenW(schtasks + L" /create /tn \"" + kAutoStartTask +
                   L"\" /tr \"\\\"" + selfExePathW() +
                   L"\\\"\" /sc onlogon /rl highest /f");
    } else if (autoStartRegistered()) {
        /* Deleting an absent task exits non-zero; only ask when it is there. */
        runHiddenW(schtasks + L" /delete /tn \"" + kAutoStartTask + L"\" /f");
    }
    return autoStartRegistered();
#else
    (void)enable;
    return false;   /* auto-start is Windows-only */
#endif
}

/* Bring config.ini in line with what Task Scheduler actually holds. When the
 * preference is on, re-register on every start: this refreshes a moved exe
 * path and re-enables a task the user disabled by hand. When it is off, drop a
 * leftover task. A failure rewrites the config to the real state so the
 * settings switch never claims something logon will not do. */
static void reconcileAutoStart(AppConfig &cfg)
{
#ifdef _WIN32
    const bool want = (cfg.auto_start != 0);
    if (want) {
        if (!applyAutoStart(1)) {
            cfg.auto_start = 0;
            configSave(cfg);
            logPush("[gui] 开机自启动注册失败，已关闭该设置（计划任务不可写）");
        }
        return;
    }
    if (autoStartRegistered() && !applyAutoStart(0)) {
        cfg.auto_start = 1;
        configSave(cfg);
        logPush("[gui] 残留的开机自启动任务无法删除，已保持该设置为开");
    }
#else
    /* Auto-start is Windows-only: clear a preference stored by an older build
       or a copied config so the switch does not claim it works here. */
    if (cfg.auto_start != 0) {
        cfg.auto_start = 0;
        configSave(cfg);
    }
#endif
}

/* ── nav helpers ─────────────────────────────────────────────────────────── */
static NavTab navTabForScreen(Screen s)
{
    return (s == SCR_SETTINGS) ? NAV_SETTINGS : NAV_HOME;
}

/* Remember the last non-settings page for 取消 / ESC / 返回会话. */
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
    g_search_cancel = true;
    if (g_search_thread.joinable()) g_search_thread.join();
    g_search_cancel = false;
    g_status_note.clear();
    g_searching = true;
    const unsigned long generation = ++g_search_generation;
    {
        std::lock_guard<std::mutex> lk(g_dev_mtx);
        g_devices.clear();
        g_search_updates.clear();
        g_found_count = 0;
    }
    g_search_thread = std::thread([generation] {
        DiscoveryOptions options;
        options.cancelled = &g_search_cancel;
        options.onDevice = [generation](const DiscoveredDevice &device) {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            g_search_updates.push_back({generation, device, false});
        };
        discoverReceivers(5, options);
        std::lock_guard<std::mutex> lk(g_dev_mtx);
        g_search_updates.push_back({generation, DiscoveredDevice("", 0), true});
    });
    setScreen(SCR_SEARCHING);
}

static void processSearchUpdates()
{
    std::vector<SearchUpdate> updates;
    {
        std::lock_guard<std::mutex> lk(g_dev_mtx);
        updates.swap(g_search_updates);
    }
    for (const SearchUpdate &update : updates) {
        if (update.generation != g_search_generation.load()) continue;
        if (!update.finished) {
            {
                std::lock_guard<std::mutex> lk(g_dev_mtx);
                const bool duplicate = std::any_of(g_devices.begin(), g_devices.end(),
                    [&](const DiscoveredDevice &d) {
                        return d.ip_address == update.device.ip_address &&
                               d.tcp_port == update.device.tcp_port;
                    });
                if (!duplicate) g_devices.push_back(update.device);
                g_found_count = (int)g_devices.size();
            }
            if (g_screen == SCR_SEARCHING) setScreen(SCR_FOUND);
            continue;
        }
        g_searching = false;
        if (g_screen == SCR_SEARCHING)
            setScreen(g_found_count > 0 ? SCR_FOUND : SCR_NOT_FOUND);
    }
}

/* Own the target strings: history callers pass references into g_hist, and
 * recordStartedSession() reloads that vector when the session actually starts. */
static void launchStream(std::string ip, int port, int mode, std::string name,
                         const char *stitch_side = nullptr)
{
    /* One sender per process: a second start would fail in spawnChild() and
     * leave the user staring at an error while the real session runs unseen. */
    if (g_sender.active) {
        g_status_note = "已有发送会话";
        g_status_tone = StatusTone::Warning;
        setScreen(SCR_STREAMING);
        return;
    }
    const bool extend = (mode == SEND_EXTEND);
    const bool stitch = (mode == SEND_STITCH);
    g_child_extend = extend;
    g_child_mode = mode;
    g_child_label = sendModeLabel(mode);
    g_last_ip = ip;
    g_last_port = port;
    g_last_name = name;
    g_history_recorded = false;
    g_sender_stop_requested = false;
    g_details_open = false;
    g_send_phase = SP_CONNECTING;
    g_status_note.clear();
    g_status_tone = StatusTone::Info;
    g_auth_wait_note.clear();
    auto fail = [&](const std::string &message) {
        g_status_note = message;
        g_status_tone = StatusTone::Error;
        g_send_phase = SP_FAILED;
        setScreen(SCR_STREAMING);
    };
    std::string path = sibling("sender.exe");
#ifndef _WIN32
    path = sibling("sender");
#endif
    if (!binaryExists(path)) {
        fail("找不到发送程序，请先运行 make");
        return;
    }
    if (stitch && !kvmSupported()) {
        logPush("[gui] stitch is unavailable on this platform");
        fail("拼接模式目前仅支持 Windows");
        return;
    }
    std::string codec = (g_cfg.codec == "auto" || g_cfg.codec.empty())
                            ? "h264" : g_cfg.codec;
    std::vector<std::string> args = {
        ip, std::to_string(port),
        "--mode", sendModeKey(mode)
    };
    /* A stitch session has no capture/encoder: passing codec/quality flags
     * would only suggest they do something. */
    if (stitch) {
        /* The chooser's answer, or the saved preference for a caller that
         * has no chooser (row-body replay). Always one of the four keys. */
        const int s = kvmSideFromKey(stitch_side ? stitch_side
                                                 : g_cfg.stitch_side.c_str());
        args.push_back("--stitch-side");
        args.push_back(kStitchSideKey[s < 0 ? (int)KVM_SIDE_RIGHT : s]);
    } else {
        args.insert(args.end(), {
            "--codec", codec,
            "--hw", g_cfg.hw_accel ? "1" : "0",
            "--bitrate", std::to_string(g_cfg.bitrate_kbps),
            "--quality", std::to_string(g_cfg.quality),
            "--fps", std::to_string(g_cfg.fps_limit),
            "--scale", std::to_string(g_cfg.scale_percent),
            "--chroma", g_cfg.chroma
        });
        if (extend && g_cfg.extend_res != "auto") {
            args.push_back("--extend-res");
            args.push_back(g_cfg.extend_res);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        g_stats = LiveStats{};
    }
    logPush("[gui] launch sender " + ip + ":" + std::to_string(port) +
            " mode=" + sendModeKey(mode) + (stitch ? "" : " codec=" + codec));
    const int probe_port = stitch ? KVM_PORT : port;
    if (!testTcpConnection(ip, probe_port, 1500)) {
        logPush("[gui] TCP probe failed for " + ip + ":" + std::to_string(probe_port));
        fail(stitch ? "无法连接 " + ip + ":" + std::to_string(probe_port) +
                          "（对方需先启动接收）"
                    : "无法连接 " + ip + ":" + std::to_string(port));
        return;
    }
#ifdef _WIN32
    /* Extend mode: the sender may append the receiver's exact resolution to
     * the virtual display driver's vdd_settings.xml. Enabled by default;
     * `vdd_mode_write=never` in config.ini opts out (legacy "ask" is treated
     * as always — the GUI no longer prompts). */
    if (extend) {
        const bool allow = g_cfg.vdd_mode_write != "never";
        args.push_back("--vdd-add-modes");
        args.push_back(allow ? "1" : "0");
        logPush(std::string("[gui] vdd_settings.xml write: ") +
                (allow ? "enabled" : "disabled by config"));
    }
#endif
    /* Sender keeps its old console semantics: 状态控制台 on = its own visible
     * console (no pipe), off = hidden child with the SETP/STAT pipes. */
    const unsigned spawn_flags =
        g_cfg.show_status_console ? SPAWN_CONSOLE : SPAWN_PIPES;
    if (!spawnChild(g_sender, path, args, spawn_flags)) {
        logPush("[gui] spawn sender failed");
        fail("无法启动发送程序，请查看日志");
        return;
    }
    g_status_note = "目标: " + sendTargetEndpoint();
    g_status_tone = StatusTone::Info;
    g_live_params = g_cfg;
    setScreen(SCR_STREAMING);
}

/* 拼接 always asks which side the peer is on before spawning the controller:
 * the answer is what tells the sender which local edge hands control over. */
static void openStitchDialog(const std::string &ip, int port,
                             const std::string &name)
{
    /* 授权 has the higher modal priority; stacking the direction chooser under
     * it would leave a dialog whose clicks cannot reach it. */
    if (g_auth_ask.open) {
        g_status_note = "请先处理连接授权请求";
        return;
    }
    if (!kvmSupported()) {           /* launchStream reports the error */
        launchStream(ip, port, SEND_STITCH, name);
        return;
    }
    g_stitch_ask.open = true;
    g_stitch_ask.ip = ip;
    g_stitch_ask.port = port;
    g_stitch_ask.name = name;
}

static void chooseStitchSide(int side)
{
    if (!g_stitch_ask.open || side < 0 || side > 3) return;
    const std::string ip = g_stitch_ask.ip;
    const std::string name = g_stitch_ask.name;
    const int port = g_stitch_ask.port;
    g_stitch_ask.open = false;
    /* Remember the answer: it is the default the next chooser opens on. */
    g_cfg.stitch_side = kStitchSideKey[side];
    g_cfg_edit.stitch_side = g_cfg.stitch_side;
    configSave(g_cfg);
    launchStream(ip, port, SEND_STITCH, name, kStitchSideKey[side]);
}

/* Single entry point for "start this mode": 拼接 always asks for its side
 * first, mirror/extend go straight to the sender. */
static void startSendMode(const std::string &ip, int port, int mode,
                          const std::string &name)
{
    if (g_sender.active) {           /* launchStream() also guards, defensively */
        g_status_note = "已有发送会话";
        setScreen(SCR_STREAMING);
        return;
    }
    if (mode == SEND_STITCH) openStitchDialog(ip, port, name);
    else launchStream(ip, port, mode, name);
}

/* True while the receive service is actually up. Derived exclusively from the
 * state machine — never from a persisted preference (auto_receive is only the
 * next-launch preference; the old allow_receive key is ignored at startup). */
static bool receiveRunning()
{
    const int st = g_recv_state.load();
    return st == RSV_STARTING || st == RSV_WAITING || st == RSV_AUTH ||
           st == RSV_STREAMING;
}

static void stopReceiver();

/* Receiving is a background state: starting it keeps the user on the home
 * screen, where the status bar tracks it. Callers that must stay on their
 * screen (设置 → 接收开关) save/restore g_screen around this call. */
static void launchReceiver()
{
    std::string path = sibling("receiver.exe");
#ifndef _WIN32
    path = sibling("receiver");
#endif
    if (g_receiver.active) {
        if (g_recv_state.load() != RSV_FAILED) {
            g_status_note = "接收服务已在运行";
            return;
        }
        /* A failed child (corrupt record / unresponsive) may still own 8081
         * and the SSDP socket. Take the single stop path first, then start a
         * fresh process; spawning on top would only fail or double-bind. */
        stopReceiver();
    }
    if (!binaryExists(path)) {
        g_status_note = "找不到 receiver 程序，请先运行 make";
        {
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            g_recv_error = "not_installed";
            g_recv_peer.clear();
            g_recv_services = RecvServices{};
        }
        g_recv_state = RSV_FAILED;
        g_recv_started_at = 0;
        setScreen(SCR_HOME);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        g_recv_error.clear();
        g_recv_peer.clear();
        g_recv_policy = g_cfg.auth_mode;
        g_recv_services = RecvServices{};
    }
    g_recv_state = RSV_STARTING;
    g_recv_started_at = SDL_GetTicks();
    const unsigned long epoch = ++g_recv_epoch;
    /* --gui: approval requests travel over stdout/stdin (STAT event=auth ↔
     * AUTH|id=…|allow). The pipe is therefore mandatory; 状态控制台 only adds a
     * visible console window (whose stdout is still the pipe, which is why the
     * in-app log window is opened below). */
    const unsigned flags =
        (unsigned)SPAWN_PIPES |
        (g_cfg.show_status_console ? (unsigned)SPAWN_CONSOLE : 0u);
    if (!spawnChild(g_receiver, path, {"--gui"}, flags, /*is_receiver=*/true,
                    epoch)) {
        g_status_note = "启动 receiver 失败";
        {
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            g_recv_error = "spawn_failed";
        }
        g_recv_state = RSV_FAILED;
        g_recv_started_at = 0;
        setScreen(SCR_HOME);
        return;
    }
    g_status_note.clear();
    g_recv_started_at = SDL_GetTicks();
    if (g_cfg.show_status_console) {
        if (!g_hidden) {
            openLogWindow();
            if (g_log_win) SDL_RaiseWindow(g_log_win);
        }
        logPush("[gui] 状态控制台模式：receiver 输出显示在应用内日志窗口");
    }
    setScreen(SCR_HOME);
}

/* The single stop path for the receiver. Bumping the generation BEFORE the
 * join invalidates every teardown STAT line the reader may still enqueue, so
 * a queued ready/waiting/stop can never resurrect 等待连接. */
static void stopReceiver()
{
    ++g_recv_epoch;
    if (g_receiver.active) stopChild(g_receiver);
    closeAuthAsk(-1);
    g_recv_started_at = 0;
    g_recv_state = RSV_OFF;
    {
        std::lock_guard<std::mutex> lk(g_recv_mtx);
        g_recv_peer.clear();
        g_recv_error.clear();
        g_recv_services = RecvServices{};
    }
    if (g_screen == SCR_RECEIVING) setScreen(SCR_HOME);
}

static bool settingsDirty()
{
    return g_cfg_edit.codec != g_cfg.codec ||
           g_cfg_edit.hw_accel != g_cfg.hw_accel ||
           g_cfg_edit.bitrate_kbps != g_cfg.bitrate_kbps ||
           g_cfg_edit.quality != g_cfg.quality ||
           g_cfg_edit.fps_limit != g_cfg.fps_limit ||
           g_cfg_edit.scale_percent != g_cfg.scale_percent ||
           g_cfg_edit.chroma != g_cfg.chroma ||
           g_cfg_edit.show_status_console != g_cfg.show_status_console ||
           g_cfg_edit.show_log_window != g_cfg.show_log_window ||
           g_cfg_edit.auto_start != g_cfg.auto_start ||
           g_cfg_edit.auto_receive != g_cfg.auto_receive ||
           g_cfg_edit.auto_check_update != g_cfg.auto_check_update ||
           g_cfg_edit.theme != g_cfg.theme ||
           g_cfg_edit.extend_res != g_cfg.extend_res ||
           g_cfg_edit.vdd_mode_write != g_cfg.vdd_mode_write ||
           g_cfg_edit.stitch_side != g_cfg.stitch_side ||
           g_cfg_edit.auth_mode != g_cfg.auth_mode ||
           g_cfg_edit.auth_salt != g_cfg.auth_salt ||
           g_cfg_edit.auth_hash != g_cfg.auth_hash ||
           g_cfg_edit.auth_iter != g_cfg.auth_iter;
}

static void restoreSettingsPreview()
{
    ThemeMode mode = ThemeMode::System;
    themeModeFromKey(g_cfg.theme.c_str(), mode);
    themeSetMode(mode);
    if (g_cfg.show_log_window && !g_log_open) openLogWindow();
    if (!g_cfg.show_log_window && g_log_open) closeLogWindow();
}

static void cancelSettings(bool to_home)
{
    g_cfg_edit = g_cfg;
    restoreSettingsPreview();
    g_discard_open = false;
    g_status_note.clear();
    g_status_tone = StatusTone::Info;
    if (to_home) setScreen(SCR_HOME);
    else         g_screen = resolveHomeScreen();
}

static void requestLeaveSettings(bool to_home)
{
    if (settingsDirty()) {
        g_discard_to_home = to_home;
        g_discard_open = true;
        return;
    }
    cancelSettings(to_home);
}

static void openSettings()
{
    g_cfg_edit = g_cfg;
    g_discard_open = false;
    g_status_note.clear();
    g_status_tone = StatusTone::Info;
    g_screen = SCR_SETTINGS;
}

/* ── receive status bar ──────────────────────────────────────────────────── */
struct RecvView {
    int         state = RSV_OFF;
    std::string policy = "manual";
    std::string peer;
    std::string error;
    RecvServices services;
};

static RecvView recvView()
{
    RecvView v;
    v.state = g_recv_state.load();
    std::lock_guard<std::mutex> lk(g_recv_mtx);
    v.policy = g_recv_policy;
    v.peer = g_recv_peer;
    v.error = g_recv_error;
    v.services = g_recv_services;
    return v;
}

static const char *recvStateLabel(int st)
{
    switch (st) {
    case RSV_STARTING:  return "正在开启";
    case RSV_WAITING:   return "等待连接";
    case RSV_AUTH:      return "等待授权";
    case RSV_STREAMING: return "正在接收";
    case RSV_FAILED:    return "开启失败";
    default:            return "已关闭";
    }
}

static std::string recvStateDetail(const RecvView &v)
{
    std::string base;
    switch (v.state) {
    case RSV_STARTING:
        base = "正在启动接收服务…";
        break;
    case RSV_WAITING:
        base = v.policy == "open"
                   ? "任何设备都可连接（无验证）"
                   : (v.policy == "password" ? "仅允许密码连接"
                                             : "每次连接都需要你在本机确认");
        break;
    case RSV_AUTH:
        base = v.peer.empty() ? "有一台设备正在请求连接"
                              : v.peer + " 请求连接";
        break;
    case RSV_STREAMING:
        base = v.peer.empty() ? "会话进行中" : v.peer;
        break;
    case RSV_FAILED:
        return v.error.empty() ? "接收服务无法启动" : receiverErrorText(v.error);
    default:
        return "未开启，其他设备无法连接到此电脑";
    }
    /* A side service that failed to bind is not a dead receiver, but the user
     * must still see that something is missing (and on which port). */
    if (v.state == RSV_WAITING || v.state == RSV_AUTH ||
        v.state == RSV_STREAMING) {
        const std::string issue = recvServicesIssue(v.services);
        if (!issue.empty()) base += " · " + issue;
    }
    return base;
}

/* One contextual action per state — the single source for the home status
 * bar's right-hand button. Derived from RecvView only, never from config. */
enum RecvAction { RA_NONE, RA_START, RA_STOP, RA_RETRY, RA_SETTINGS };

static RecvAction recvActionFor(const RecvView &v)
{
    switch (v.state) {
    case RSV_OFF:       return RA_START;
    case RSV_STARTING:  return RA_NONE;
    case RSV_WAITING:
    case RSV_AUTH:
    case RSV_STREAMING: return RA_STOP;
    case RSV_FAILED:
        /* A corrupt password record can only be fixed in 接收与安全. */
        return (v.error == "auth_config") ? RA_SETTINGS : RA_RETRY;
    default:            return RA_NONE;
    }
}

static const char *recvActionLabel(const RecvView &v)
{
    switch (recvActionFor(v)) {
    case RA_START:    return "开启接收";
    case RA_STOP:     return "停止接收";
    case RA_RETRY:    return "重试";
    case RA_SETTINGS: return "去设置";
    default:          return "";
    }
}

static int recvActionButton(const RecvView &v)
{
    switch (recvActionFor(v)) {
    case RA_START:
    case RA_RETRY:    return BTN_HOME_RECV_START;
    case RA_STOP:     return BTN_HOME_RECEIVE_STOP;
    case RA_SETTINGS: return BTN_HOME_RECV_SETTINGS;
    default:          return BTN_NONE;
    }
}

static SDL_Color recvStateColor(int st)
{
    switch (st) {
    case RSV_WAITING:
    case RSV_STREAMING: return THEME.success;
    case RSV_AUTH:      return THEME.warning;
    case RSV_FAILED:    return THEME.danger;
    case RSV_STARTING:  return THEME.primary;
    default:            return THEME.textDim;
    }
}

/* ── tray / exit ─────────────────────────────────────────────────────────── */
/* Only an established session (or a pending approval) counts. A receiver that
 * is merely listening — automatic receive with no peer yet — is not a session
 * and must not add a confirmation to 退出. */
static bool hasActiveSession()
{
    if (g_sender.active) return true;
    const int st = g_recv_state.load();
    return st == RSV_AUTH || st == RSV_STREAMING;
}

static bool confirmExit()
{
#ifdef _WIN32
    if (!hasActiveSession()) return true;
    return MessageBoxW(nullptr,
                       L"当前有进行中的会话。退出 Mivara 将结束会话、关闭接收服务"
                       L"并移除虚拟显示器。\n\n确定要退出吗？",
                       L"Mivara 幻屏",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                           MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
#else
    /* No tray on POSIX: closing the window keeps its old meaning and the
     * session screen still offers a deliberate stop. */
    return true;
#endif
}

/** Exit for real (tray 退出 / 退出 buttons). The SDL_QUIT handler in main()
 *  performs the confirmation exactly once. */
static void requestQuit(bool no_confirm = false)
{
    g_exit_no_confirm = no_confirm;
    g_exit_requested = true;
    SDL_Event q;
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
}

static void hideToTray()
{
    if (!g_tray_ok || g_hidden) return;
    /* A pending prompt (approval / password) is time-limited; hiding the
     * window would make it unanswerable while the deadline keeps running. */
    if (g_auth_ask.open || g_pw_ask.open || g_stitch_ask.open || g_set_pw.open)
        return;
    SDL_HideWindow(g_win);
    if (g_log_win && g_log_open) SDL_HideWindow(g_log_win);
    g_hidden = true;
    if (!g_cfg.tray_hint_shown) {
        /* One time only; the flag lives in config.ini and changes nothing
         * else about the user's settings. */
        trayNotify("Mivara 仍在运行",
                   "关闭窗口不会中断会话。单击托盘图标可恢复，右键可退出。");
        g_cfg.tray_hint_shown = 1;
        configSave(g_cfg);
    }
}

static void showFromTray()
{
    if (!g_hidden) {
        SDL_RaiseWindow(g_win);
        return;
    }
    g_hidden = false;
    SDL_ShowWindow(g_win);
    SDL_RestoreWindow(g_win);
    SDL_RaiseWindow(g_win);
    if (g_log_win && g_log_open) SDL_ShowWindow(g_log_win);
}

/* State summary for the tray tooltip (≤127 chars after conversion). */
static std::string trayTooltip()
{
    const RecvView rv = recvView();
    std::string s = "Mivara · ";
    if (g_sender.active) {
        s += sendPhaseLabel();
    } else {
        /* Derived from the receive state machine only. g_cfg.allow_receive is
         * deliberately not consulted: it is no longer a truth source. */
        switch (rv.state) {
        case RSV_STARTING:  s += "正在开启接收";   break;
        case RSV_WAITING:   s += "接收：等待连接"; break;
        case RSV_AUTH:      s += "接收：等待授权"; break;
        case RSV_STREAMING: s += "接收：正在接收"; break;
        case RSV_FAILED:    s += "接收开启失败";   break;
        default:            s += "接收已关闭";     break;
        }
    }
    return s;
}

/* ── screen building ─────────────────────────────────────────────────────── */
/* One modal stack shared by buildButtons(), renderScreen() and onButton(). */
enum Modal {
    MODAL_NONE = 0,
    MODAL_DISCARD,
    MODAL_SETPW,
    MODAL_PW,
    MODAL_STITCH,
    MODAL_AUTH
};

static Modal topModal()
{
    if (g_auth_ask.open)   return MODAL_AUTH;
    if (g_stitch_ask.open) return MODAL_STITCH;
    if (g_pw_ask.open)     return MODAL_PW;
    if (g_set_pw.open)     return MODAL_SETPW;
    if (g_discard_open)    return MODAL_DISCARD;
    return MODAL_NONE;
}

static void appendModalButtons()
{
    switch (topModal()) {
    case MODAL_AUTH: {
        const int y = AUTH_DLG_Y + AUTH_DLG_H - 58;
        addButton(AUTH_DLG_X + 200, y, 110, 38, "拒绝", BTN_AUTH_DENY,
                  BTN_V_SECONDARY);
        addButton(AUTH_DLG_X + 322, y, 114, 38, "允许一次", BTN_AUTH_ALLOW,
                  BTN_V_PRIMARY);
        break;
    }
    case MODAL_PW: {
        const SDL_Rect show{PW_SHOW_X, PW_SHOW_Y, 116, 26};
        addButton(show.x, show.y, show.w, show.h,
                  g_pw_ask.show ? "隐藏密码" : "显示密码", BTN_PW_SHOW,
                  BTN_V_SECONDARY);
        const int y = PW_DLG_Y + PW_DLG_H - 58;
        addButton(PW_DLG_X + 200, y, 110, 38, "取消", BTN_PW_CANCEL,
                  BTN_V_SECONDARY);
        addButton(PW_DLG_X + 322, y, 114, 38, "连 接", BTN_PW_CONNECT,
                  BTN_V_PRIMARY);
        break;
    }
    case MODAL_SETPW: {
        addButton(SETPW_FIELD_X, SETPW_A_Y, SETPW_FIELD_W, SETPW_FIELD_H, "",
                  BTN_SETPW_FIELD_A, BTN_V_GHOST);
        addButton(SETPW_SHOW_X, SETPW_SHOW_Y - 6, 116, 24, "", BTN_SETPW_SHOW,
                  BTN_V_GHOST);
        const int y = SETPW_DLG_Y + SETPW_DLG_H - 58;
        addButton(SETPW_DLG_X + 200, y, 110, 38, "取消", BTN_SETPW_CANCEL,
                  BTN_V_SECONDARY);
        addButton(SETPW_DLG_X + 322, y, 114, 38, "应 用", BTN_SETPW_OK,
                  BTN_V_PRIMARY);
        break;
    }
    case MODAL_STITCH: {
        const int cur = stitchSideFromConfig();
        for (int i = 0; i < 4; i++) {
            const SDL_Rect r = rcSidePad(i);
            addChoice(r.x, r.y, r.w, r.h, kStitchSideLabel[i],
                      BTN_STITCH_SIDE_BASE + i, i == cur);
        }
        const SDL_Rect c = rcSideCancel();
        addButton(c.x, c.y, c.w, c.h, "取 消", BTN_STITCH_SIDE_CANCEL,
                  BTN_V_SECONDARY);
        break;
    }
    case MODAL_DISCARD: {
        const int y = AUTH_DLG_Y + AUTH_DLG_H - 58;
        addButton(AUTH_DLG_X + 200, y, 110, 38, "继续编辑", BTN_DISCARD_KEEP,
                  BTN_V_SECONDARY);
        addButton(AUTH_DLG_X + 322, y, 114, 38, "放弃修改", BTN_DISCARD_CONFIRM,
                  BTN_V_DANGER);
        break;
    }
    default:
        break;
    }
}

static void buildButtons()
{
    g_buttons.clear();
    g_modal_button_begin = 0;

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
        if (g_sender.active) {
            /* Returning to 主页 must never strand a running session: the hero
             * card becomes the session card with real 返回会话 / 停止 buttons,
             * and the connect entry is not registered (a second sender cannot
             * start anyway). */
            addButton(HOME_SESSION_BACK_X, HOME_SESSION_BTN_Y,
                      HOME_SESSION_BACK_W, HOME_SESSION_BTN_H, "返回会话",
                      BTN_RETURN_SESSION, BTN_V_SECONDARY);
            addButton(HOME_SESSION_STOP_X, HOME_SESSION_BTN_Y,
                      HOME_SESSION_STOP_W, HOME_SESSION_BTN_H,
                      std::string("停止") + g_child_label,
                      BTN_HOME_SENDER_STOP, BTN_V_DANGER);
        } else {
            addButton(HOME_CARD_X(0), HOME_CARD_Y, HOME_CARD_W, HOME_CARD_H, "",
                      BTN_HOME_CONNECT, BTN_V_GHOST);
        }

        /* 接收状态栏：右侧动作按钮必须先注册。命中测试是 first-match-wins，
         * 而整条详情热区覆盖动作按钮；绘制顺序则让动作按钮盖在上面，所以
         * “先动作、后详情”是唯一同时满足视觉与点击的顺序。 */
        {
            const RecvView rv = recvView();
            const RecvAction act = recvActionFor(rv);
            const int actId = recvActionButton(rv);
            if (actId != BTN_NONE)
                addButton(RECV_BAR_ACT_X, RECV_BAR_ACT_Y, RECV_BAR_ACT_W,
                          RECV_BAR_ACT_H, recvActionLabel(rv),
                          actId, (act == RA_STOP) ? BTN_V_DANGER
                                                  : BTN_V_SECONDARY);
            if (rv.state != RSV_OFF)
                addButton(BODY_X, RECV_BAR_Y, BODY_W, RECV_BAR_H, "",
                          BTN_HOME_RECV_DETAIL, BTN_V_GHOST);
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
            SDL_Rect b4 = rcHistBtn(i, 3);
            addButton(b1.x, b1.y, b1.w, b1.h, "投屏", BTN_HIST_BASE + idx * 10 + 1,
                      BTN_V_SECONDARY);
            addButton(b2.x, b2.y, b2.w, b2.h, "扩展", BTN_HIST_BASE + idx * 10 + 2,
                      BTN_V_SECONDARY);
            addButton(b3.x, b3.y, b3.w, b3.h, "拼接", BTN_HIST_BASE + idx * 10 + 3,
                      BTN_V_SECONDARY);
            addButton(b4.x, b4.y, b4.w, b4.h, "删除", BTN_HIST_BASE + idx * 10 + 4,
                      BTN_V_LINK);
        }
        if (!g_hist.empty())
            addButton(BODY_R - 88, HIST_CARD_Y + 12, 88, 28, "清空", BTN_HIST_CLEAR,
                      BTN_V_LINK);
        break;
    }

    case SCR_RECEIVING: {
        addButton(BODY_X + 16, 326, BODY_W - 32, 36, "",
                  BTN_RECV_DETAILS, BTN_V_GHOST);
        addButton(cx - 178, 518, 170, 48, "停止接收", BTN_RECV_STOP, BTN_V_DANGER);
        addButton(cx + 8,   518, 170, 48, "日志",     BTN_RECV_LOG,  BTN_V_SECONDARY);
        break;
    }

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
            addButton(DEV_BTN_X(0), y + 8, DEV_BTN_W, 36, "投屏",
                      BTN_DEV_BASE + (int)i * 10 + 1, BTN_V_SECONDARY);
            addButton(DEV_BTN_X(1), y + 8, DEV_BTN_W, 36, "扩展",
                      BTN_DEV_BASE + (int)i * 10 + 2, BTN_V_SECONDARY);
            addButton(DEV_BTN_X(2), y + 8, DEV_BTN_W, 36, "拼接",
                      BTN_DEV_BASE + (int)i * 10 + 3, BTN_V_SECONDARY);
        }
        {
            const int y = foundActionsY((int)n);
            addButton(cx - 168, y, 160, 44, "重新搜索", BTN_FOUND_RESEARCH, BTN_V_SECONDARY);
            addButton(cx + 8, y, 160, 44, "手动输入 IPv4", BTN_FOUND_MANUAL, BTN_V_SECONDARY);
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

    case SCR_STREAMING: {
        if (g_sender.active && g_child_mode == SEND_STITCH &&
            g_send_phase == SP_LIVE) {
            const int cur = stitchSideCurrent();
            for (int i = 0; i < 4; i++)
                addChoice(ST_DIR_CHIP_X(i), ST_DIR_CHIP_Y, ST_DIR_CHIP_W,
                          ST_DIR_CHIP_H, kStitchSideLabel[i],
                          BTN_STITCH_SWITCH_BASE + i, i == cur, g_fontS);
        }
        if (g_send_phase == SP_LIVE)
            addButton(ST_PANEL_X, ST_PANEL_Y, ST_PANEL_W, 48, "",
                      BTN_STREAM_DETAILS, BTN_V_GHOST);
        if (g_send_phase == SP_FAILED) {
            addButton(cx - 178, 510, 170, 48, "返回", BTN_STREAM_BACK, BTN_V_SECONDARY);
            addButton(cx + 8, 510, 170, 48,
                      std::string("重试") + g_child_label,
                      BTN_STREAM_RETRY, BTN_V_PRIMARY);
            addButton(cx + 184, 510, 160, 48, "日志", BTN_STREAM_LOG, BTN_V_SECONDARY);
        } else {
            addButton(cx - 178, 510, 170, 48,
                      std::string("停止") + g_child_label,
                      BTN_STREAM_STOP, BTN_V_DANGER, GLYPH_NONE, nullptr,
                      g_sender.active);
            addButton(cx + 8, 510, 170, 48, "日志", BTN_STREAM_LOG, BTN_V_SECONDARY);
        }
        break;
    }

    case SCR_SETTINGS: {
        static const char *kTab[3] = {"常规", "画质与编码", "接收与安全"};
        for (int i = 0; i < 3; i++)
            addChoice(SET_TAB_X(i), SET_TAB_Y, SET_TAB_W, SET_TAB_H,
                      kTab[i], BTN_SET_TAB_BASE + i, g_set_tab == i);

        if (g_set_tab == 1) {
            const char *codecLabels[] = {"自动", "H.264", "H.265", "AV1", "原始", "RLE"};
            const char *codecIds[]    = {"auto", "h264", "h265", "av1", "raw", "rle"};
            for (int i = 0; i < 6; i++) {
                const int col = i % 3, row = i / 3;
                addChoice(SET_LC_X + col * 101, SET_GRID_Y0 + row * 36, 93, 32,
                          codecLabels[i], BTN_SET_CODEC_BASE + i,
                          g_cfg_edit.codec == codecIds[i]);
            }
            addButton(SET_MINUS_X, SET_ROW_Y(0), SET_STEP_W, 32, "−",
                      BTN_SET_QUALITY_DEC, BTN_V_SECONDARY, GLYPH_NONE, nullptr,
                      g_cfg_edit.quality > 0);
            addButton(SET_PLUS_X, SET_ROW_Y(0), SET_STEP_W, 32, "+",
                      BTN_SET_QUALITY_INC, BTN_V_SECONDARY, GLYPH_NONE, nullptr,
                      g_cfg_edit.quality < 51);
            addButton(SET_MINUS_X, SET_ROW_Y(1), SET_STEP_W, 32, "−",
                      BTN_SET_FPS_DEC, BTN_V_SECONDARY, GLYPH_NONE, nullptr,
                      g_cfg_edit.fps_limit > 1);
            addButton(SET_PLUS_X, SET_ROW_Y(1), SET_STEP_W, 32, "+",
                      BTN_SET_FPS_INC, BTN_V_SECONDARY, GLYPH_NONE, nullptr,
                      g_cfg_edit.fps_limit < 240);
            addButton(SET_MINUS_X, SET_ROW_Y(2), SET_STEP_W, 32, "−",
                      BTN_SET_BITRATE_DEC, BTN_V_SECONDARY, GLYPH_NONE, nullptr,
                      g_cfg_edit.bitrate_kbps > 0);
            addButton(SET_PLUS_X, SET_ROW_Y(2), SET_STEP_W, 32, "+",
                      BTN_SET_BITRATE_INC, BTN_V_SECONDARY, GLYPH_NONE, nullptr,
                      g_cfg_edit.bitrate_kbps < 100000);
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
            addButton(SET_RC_X, SET_TOGGLE_Y0, SET_COL_W - SET_PAD * 2, 32, "",
                      BTN_SET_HW, BTN_V_GHOST);
        } else if (g_set_tab == 0) {
            const int startup[2] = {BTN_SET_AUTORECV, BTN_SET_AUTOSTART};
            for (int i = 0; i < 2; i++)
                addButton(SET_LC_X, SET_TOGGLE_Y0 + i * 40, SET_LC_W, 32, "",
                          startup[i], BTN_V_GHOST);
            const int diagnostics[3] = {BTN_SET_CONSOLE, BTN_SET_LOGWIN,
                                        BTN_SET_AUTOCHECK};
            for (int i = 0; i < 3; i++)
                addButton(SET_RC_X, SET_TOGGLE_Y0 + i * 40, SET_COL_W - SET_PAD * 2,
                          32, "", diagnostics[i], BTN_V_GHOST);
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
        } else {
            addButton(SEC_ROW_X, SEC_TOGGLE_Y(0), SEC_ROW_W, 32, "",
                      BTN_SET_ALLOW_RECV, BTN_V_GHOST);
            for (int i = 0; i < 3; i++)
                addButton(SEC_ROW_X, SEC_POLICY_Y(i), SEC_ROW_W, 38, "",
                          kAuthModeBtn[i], BTN_V_GHOST);
            addButton(SEC_ROW_X + SEC_ROW_W - 96, SEC_PW_ROW_Y - 8, 96, 28,
                      g_cfg_edit.auth_hash.empty() ? "设置" : "修改",
                      BTN_SET_PW_EDIT, BTN_V_SECONDARY);
        }

        addButton(BODY_X, SET_ACT_Y, 140, SET_ACT_H, "恢复默认", BTN_SET_RESET,
                  BTN_V_SECONDARY);
        addButton(cx - 170, SET_ACT_Y, 150, SET_ACT_H, "取消", BTN_SET_CANCEL,
                  BTN_V_SECONDARY);
        addButton(cx - 10, SET_ACT_Y, 150, SET_ACT_H, "保存设置", BTN_SET_OK,
                  BTN_V_PRIMARY);
        if (g_sender.active || g_receiver.active)
            addButton(cx + 156, SET_ACT_Y, 172, SET_ACT_H, "返回会话",
                      BTN_RETURN_SESSION, BTN_V_SECONDARY);
        break;
    }
    }

    g_modal_button_begin = g_buttons.size();
    appendModalButtons();
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
    std::string out = codecDisplay(s.codec);
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
                            : (s.mode == "stitch") ? "拼接"
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
    const bool stitch_session = g_sender.active && g_child_mode == SEND_STITCH;
    drawText(ren, g_send_phase == SP_LIVE ? "连接详情" : "连接状态",
             x, card.y + 16, THEME.textDim, g_fontBS);
    if (g_send_phase == SP_LIVE && !stitch_session)
        drawTextV(ren, g_details_open ? "收起 ▲" : "展开 ▼", card.x + card.w - 20,
                  card.y + 26, THEME.textDim, g_fontS, ALIGN_RIGHT);

    if (g_send_phase != SP_LIVE) {
        drawText(ren, sendPhaseLabel(), x, card.y + 48, sendPhaseColor(), g_font);
        return;
    }
    if (!stitch_session && !g_details_open) return;

    if (stitch_session)
        drawText(ren, "方向", x, ST_DIR_CHIP_Y + 5, THEME.textDim, g_fontS);

    if (!s.valid) {
        drawText(ren, "等待统计数据…", x, card.y + 54, THEME.textDim, g_font);
        return;
    }
    if (s.event == "error") {
        drawText(ren, fitText("错误：" + senderErrorText(s.msg), g_font, maxW),
                 x, card.y + 54, THEME.danger, g_font);
        return;
    }
    if (s.event == "start" || s.event == "stop") {
        drawText(ren, s.event == "start" ? "会话已开始" : "会话已结束",
                 x, card.y + 54, THEME.text, g_font);
        return;
    }
    if (s.event == "stitch") {
        int side = kvmSideFromKey(s.side.c_str());
        if (side < 0) side = stitchSideFromConfig();
        std::string hint;
        if (s.state == "waiting") {
            hint = "等待对方确认…";
        } else if (s.state == "remote") {
            hint = std::string("已接管对方键鼠 · Ctrl+Alt+Shift+Q 或移出对方") +
                   kStitchBackEdge[side] + "返回";
        } else {
            hint = std::string("光标移到屏幕") + kStitchEnterEdge[side] +
                   "即可控制对方";
        }
        drawText(ren, "控制状态", x, card.y + 50, THEME.text, g_fontB);
        drawText(ren, fitText(hint, g_fontS, maxW), x, card.y + 86,
                 THEME.textDim, g_fontS);
        return;
    }
    drawText(ren, fitText(statLine1(s), g_fontB, maxW), x, card.y + 54,
             THEME.text, g_fontB);
    std::string l2 = statLine2(s);
    if (!l2.empty())
        drawText(ren, fitText(l2, g_fontS, maxW), x, card.y + 94,
                 THEME.textDim, g_fontS);
}

/* Modal scrim + card for the 拼接 direction question. The four pads are
 * painted by the common button pass (they are BTN_V_CHOICE cells), so this
 * only paints what has no hit area of its own: the backdrop, the title and
 * the 本机 marker that makes the spatial arrangement readable. */
static void drawStitchSideDialog(SDL_Renderer *ren)
{
    fillRoundRect(ren, SDL_Rect{0, 0, WIN_W, WIN_H}, 0, withAlpha(BLACK, 0x66));
    const SDL_Rect card{SIDE_DLG_X, SIDE_DLG_Y, SIDE_DLG_W, SIDE_DLG_H};
    drawCard(ren, card, CARD_R);
    const std::string target = g_stitch_ask.name.empty()
                                   ? g_stitch_ask.ip : g_stitch_ask.name;
    drawText(ren, fitText(target + " 在本机的哪一侧？", g_fontB, SIDE_DLG_W - 48),
             SIDE_DLG_X + 24, SIDE_DLG_Y + 22, THEME.text, g_fontB);
    drawText(ren, fitText(g_stitch_ask.ip + " · 共享键鼠，不传输画面",
                          g_fontS, SIDE_DLG_W - 48),
             SIDE_DLG_X + 24, SIDE_DLG_Y + 58, THEME.textDim, g_fontS);
    drawText(ren, "从该侧移出鼠标即可控制对方", SIDE_DLG_X + 24, SIDE_DLG_Y + 78,
             THEME.textDim, g_fontS);
    const SDL_Rect me = rcSideCenter();
    fillRoundRect(ren, me, THEME.radius - 3, THEME.surfaceAlt);
    drawTextIn(ren, "本机", me, THEME.textDim, g_fontS, ALIGN_CENTER);
}

/* ── authorization and settings modals ──────────────────────────────────────
 * Only topModal() is painted here, matching buildButtons() and onButton(). */
static void drawAuthDialogs(SDL_Renderer *ren)
{
    if (topModal() == MODAL_DISCARD) {
        fillRoundRect(ren, SDL_Rect{0, 0, WIN_W, WIN_H}, 0, withAlpha(BLACK, 0x66));
        const SDL_Rect card{AUTH_DLG_X, AUTH_DLG_Y, AUTH_DLG_W, AUTH_DLG_H};
        drawCard(ren, card, CARD_R);
        const int x = AUTH_DLG_X + 24;
        drawText(ren, "放弃未保存修改？", x, AUTH_DLG_Y + 22,
                 THEME.text, g_fontB);
        drawText(ren, "当前设置尚未保存，离开后这些修改将丢失。", x,
                 AUTH_DLG_Y + 72, THEME.textDim, g_fontS);
        drawText(ren, "选择继续编辑，或明确放弃本次修改。", x,
                 AUTH_DLG_Y + 102, THEME.textDim, g_fontS);
        return;
    }
    if (topModal() == MODAL_AUTH) {
        fillRoundRect(ren, SDL_Rect{0, 0, WIN_W, WIN_H}, 0, withAlpha(BLACK, 0x66));
        const SDL_Rect card{AUTH_DLG_X, AUTH_DLG_Y, AUTH_DLG_W, AUTH_DLG_H};
        drawCard(ren, card, CARD_R);
        const int x = AUTH_DLG_X + 24;
        drawText(ren, "有一台设备请求连接", x, AUTH_DLG_Y + 22, THEME.text,
                 g_fontB);
        const std::string name = g_auth_ask.dev.empty() ? "未知设备"
                                                        : g_auth_ask.dev;
        drawText(ren, fitText(name, g_font, AUTH_DLG_W - 48), x,
                 AUTH_DLG_Y + 62, THEME.text, g_font);
        drawText(ren, g_auth_ask.ip.empty() ? "地址未知" : g_auth_ask.ip, x,
                 AUTH_DLG_Y + 90, THEME.textDim, g_fontS);
        drawText(ren,
                 g_auth_ask.req == "stitch"
                     ? "请求：拼接控制（键盘与鼠标）"
                     : "请求：视频连接（投屏或扩展）",
                 x, AUTH_DLG_Y + 116, THEME.textDim, g_fontS);
        const long leftMs = (long)g_auth_ask.deadline - (long)SDL_GetTicks();
        const int secs = leftMs > 0 ? (int)(leftMs / 1000) + 1 : 0;
        drawText(ren, secs > 0 ? "若 " + std::to_string(secs) +
                                     " 秒内未选择，将自动拒绝"
                               : "正在自动拒绝…",
                 x, AUTH_DLG_Y + 142, THEME.textDim, g_fontS);
        return;
    }
    if (topModal() == MODAL_PW) {
        fillRoundRect(ren, SDL_Rect{0, 0, WIN_W, WIN_H}, 0, withAlpha(BLACK, 0x66));
        const SDL_Rect card{PW_DLG_X, PW_DLG_Y, PW_DLG_W, PW_DLG_H};
        drawCard(ren, card, CARD_R);
        const int x = PW_DLG_X + 24;
        drawText(ren, "对方要求连接密码", x, PW_DLG_Y + 22, THEME.text, g_fontB);
        drawText(ren, "请输入接收端设置的 4–8 个字符连接密码（字母不区分大小写）", x, PW_DLG_Y + 54,
                 THEME.textDim, g_fontS);

        const SDL_Rect field{PW_FIELD_X, PW_FIELD_Y, PW_FIELD_W, PW_FIELD_H};
        drawRoundRect(ren, field, THEME.radius, THEME.surfaceAlt,
                      g_pw_ask.error.empty() ? THEME.border : THEME.danger);
        std::string shown = g_pw_ask.show
                                ? g_pw_ask.text
                                : std::string(g_pw_ask.text.size(), '*');
        drawTextIn(ren, shown.empty() ? "密码" : shown, field,
                   shown.empty() ? THEME.textDim : THEME.text, g_font,
                   ALIGN_LEFT, 14);
        int textW = 0, textH = 0;
        TTF_SizeUTF8(g_font, shown.c_str(), &textW, &textH);
        const int caretX = field.x + 14 + (shown.empty() ? 0 : uiCoord(textW));
        fillRoundRect(ren, SDL_Rect{caretX, field.y + 10, 1, field.h - 20}, 0,
                      THEME.border);

        drawText(ren, "支持 Ctrl+V 粘贴", x, PW_SHOW_Y + 2, THEME.textDim,
                 g_fontS);
        if (!g_pw_ask.error.empty())
            drawText(ren, g_pw_ask.error, x, PW_SHOW_Y + 26, THEME.danger,
                     g_fontS);
        return;
    }
    if (topModal() == MODAL_SETPW) {
        fillRoundRect(ren, SDL_Rect{0, 0, WIN_W, WIN_H}, 0, withAlpha(BLACK, 0x66));
        const SDL_Rect card{SETPW_DLG_X, SETPW_DLG_Y, SETPW_DLG_W, SETPW_DLG_H};
        drawCard(ren, card, CARD_R);
        const int x = SETPW_DLG_X + 24;
        const bool changing = !g_cfg_edit.auth_hash.empty();
        drawText(ren, changing ? "修改连接密码" : "设置连接密码", x,
                 SETPW_DLG_Y + 18, THEME.text, g_fontB);
        drawText(ren, "4–8 个字符，字母不区分大小写；仅用于连接准入，不加密画面", x,
                 SETPW_DLG_Y + 52, THEME.textDim, g_fontS);

        auto pwField = [&](const std::string &val, int y) {
            const SDL_Rect rc{x, y, SETPW_FIELD_W, SETPW_FIELD_H};
            drawRoundRect(ren, rc, THEME.radius, THEME.surfaceAlt,
                          g_set_pw.err.empty() ? THEME.primary
                                               : THEME.danger);
            std::string shown = g_set_pw.show ? val
                                             : std::string(val.size(), '*');
            drawTextIn(ren, shown.empty() ? "请输入" : shown, rc,
                       shown.empty() ? THEME.textDim : THEME.text, g_font,
                       ALIGN_LEFT, 12);
        };
        pwField(g_set_pw.a, SETPW_A_Y);

        drawTextV(ren, g_set_pw.show ? "隐藏密码" : "显示密码", SETPW_SHOW_X,
                  SETPW_SHOW_Y + 8, THEME.primary, g_fontS, ALIGN_LEFT);
        drawText(ren, "支持 Ctrl+V 粘贴", x, SETPW_SHOW_Y,
                 THEME.textDim, g_fontS);
        if (!g_set_pw.err.empty())
            drawText(ren, fitText(g_set_pw.err, g_fontS, SETPW_FIELD_W), x,
                     SETPW_SHOW_Y + 24, THEME.danger, g_fontS);
    }
}

/* ── screen rendering ────────────────────────────────────────────────────── */
/* ── chrome ──────────────────────────────────────────────────────────────── */
static void drawHeader()
{
    fillRoundRect(g_ren, SDL_Rect{0, 0, WIN_W, HEADER_H}, 0, THEME.surface);
    drawHairline(g_ren, 0, HEADER_H - 1, WIN_W, 1, THEME.border);
    drawGlyph(g_ren, GLYPH_LOGO, HDR_GLYPH_CX, HEADER_H / 2, 24, THEME.primary);
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
        drawText(g_ren, "发送到其他电脑", BODY_X, HOME_TITLE_Y,
                 THEME.text, g_fontB);
        if (!g_status_note.empty())
            drawTextV(g_ren, fitText(g_status_note, g_fontS, BODY_W - 220),
                      BODY_R, HOME_TITLE_Y + 10, statusToneColor(),
                      g_fontS, ALIGN_RIGHT);
        drawText(g_ren, "快捷操作", BODY_X, HOME_CAPTION_Y, THEME.textDim, g_fontBS);

        /* 连接设备 — the one primary entry. 接收 used to sit next to it as an
         * equal-weight card, which read as "start a second app"; it is now a
         * background state with its own status bar below. While a sender
         * session runs the same rect becomes a session card. */
        {
            const SDL_Rect card = rcHomeCard(0);
            drawCard(g_ren, card, CARD_R);

            SDL_Rect tile{card.x + 18, card.y + 26, 46, 46};
            const bool live = (g_send_phase == SP_LIVE);
            fillRoundRect(g_ren, tile, 14,
                          live ? THEME.successTint : THEME.primaryTint);
            drawGlyph(g_ren, g_sender.active ? GLYPH_MONITOR : GLYPH_ARROW_UP,
                      tile.x + 23, tile.y + 23, 22,
                      live ? THEME.onSuccessTint : THEME.onPrimaryTint);

            if (g_sender.active) {
                drawTextV(g_ren, sendPhaseLabel(), card.x + 82,
                          card.y + 42, sendPhaseColor(), g_fontB, ALIGN_LEFT);
                const std::string sub = sendTargetName() + " · " + sendTargetEndpoint();
                drawText(g_ren,
                         fitText(sub, g_fontS,
                                 HOME_SESSION_BACK_X - 12 - (card.x + 82)),
                         card.x + 82, card.y + 66, THEME.textDim, g_fontS);
            } else {
                if (g_hot == BTN_HOME_CONNECT || g_press == BTN_HOME_CONNECT)
                    fillRoundRect(g_ren, card, CARD_R, THEME.hover);
                drawTextV(g_ren, "选择目标设备", card.x + 82, card.y + 42,
                          THEME.text, g_fontB, ALIGN_LEFT);
                drawText(g_ren, "搜索后直接选择投屏、扩展或拼接",
                         card.x + 82, card.y + 66, THEME.textDim, g_fontS);
                drawTextV(g_ren, "›", card.x + card.w - 22, card.y + card.h / 2,
                          THEME.textDim, g_fontL, ALIGN_RIGHT);
            }
        }

        /* 接收状态栏 —低权重，但各种状态都可见、可操作。 */
        {
            const RecvView rv = recvView();
            const SDL_Rect bar{BODY_X, RECV_BAR_Y, BODY_W, RECV_BAR_H};
            drawRoundRect(g_ren, bar, THEME.radiusCard - 6, THEME.surface,
                          THEME.border);

            setColor(g_ren, recvStateColor(rv.state));
            fillCircle(g_ren, bar.x + 20, bar.y + 24, 5);

            const int textW = RECV_BAR_ACT_X - 12 - (bar.x + 36);
            drawText(g_ren, fitText(std::string("本机接收 · ") +
                                        recvStateLabel(rv.state),
                                    g_fontBS, textW),
                     bar.x + 36, bar.y + 14, THEME.text, g_fontBS);
            drawText(g_ren, fitText(recvStateDetail(rv), g_fontS, textW),
                     bar.x + 36, bar.y + 38, THEME.textDim, g_fontS);
        }

        /* connection history — the card shrinks when there is nothing to list,
         * otherwise the empty state is a 288px box with one line in it */
        {
            const SDL_Rect card{BODY_X, HIST_CARD_Y, BODY_W,
                                g_hist.empty() ? HIST_CARD_H_EMPTY : HIST_CARD_H};
            drawCard(g_ren, card, CARD_R);
            drawText(g_ren, "最近连接", card.x + 16, card.y + 16, THEME.text,
                     g_fontBS);
            drawText(g_ren, "仅记录已开始的会话", card.x + 100, card.y + 18,
                     THEME.textDim, g_fontS);
            if (g_hist.empty()) {
                drawTextIn(g_ren, "暂无连接记录 · 真正开始一次会话后会自动出现在这里",
                           card, THEME.textDim, g_font, ALIGN_CENTER);
            } else {
                for (int i = 0; i < HIST_VISIBLE; i++) {
                    const int idx = i + g_hist_scroll;
                    if (idx >= (int)g_hist.size()) break;
                    const HistEntry &e = g_hist[idx];
                    const SDL_Rect row = rcHistRow(i);
                    if (g_hot >= BTN_HIST_BASE + idx * 10 &&
                        g_hot < BTN_HIST_BASE + idx * 10 + 5)
                        fillRoundRect(g_ren, row, THEME.radius, THEME.hover);
                    if (idx + 1 < (int)g_hist.size())
                        drawHairline(g_ren, row.x, row.y + row.h, row.w, 1,
                                     THEME.border);

                    std::string nm = e.name.empty() ? e.ip : truncateUtf8(e.name, 14);
                    drawText(g_ren, fitText(nm, g_fontS, HIST_TEXT_W),
                             row.x + 12, row.y + 5,
                             THEME.text, g_fontS);
                    std::string sub = e.ip + ":" + std::to_string(e.port) + " · " +
                                      sendModeLabel(sendModeFromKey(e.mode)) + " · " +
                                      relTime(e.last_used_unix) + " · " +
                                      std::to_string(e.use_count) + " 次";
                    drawText(g_ren, fitText(sub, g_fontS, HIST_TEXT_W), row.x + 12,
                             row.y + 23,
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
        const RecvView rv = recvView();
        std::string ip = getLocalIPAddress();
        drawText(g_ren, recvStateLabel(rv.state), cx, 162,
                 recvStateColor(rv.state), g_fontL, true);
        drawText(g_ren, recvStateDetail(rv), cx, 210, THEME.textDim, g_font, true);

        const SDL_Rect card{BODY_X, 246, BODY_W, 256};
        drawCard(g_ren, card, CARD_R);
        const int lx = card.x + 20;
        drawText(g_ren, "本机 IP", lx, card.y + 24, THEME.textDim, g_fontS);
        drawText(g_ren, ip, lx, card.y + 48, THEME.text, g_fontB);
        const SDL_Rect detail{lx, 326, card.w - 40, 36};
        if (g_hot == BTN_RECV_DETAILS || g_press == BTN_RECV_DETAILS)
            fillRoundRect(g_ren, detail, THEME.radius - 3, THEME.hover);
        drawText(g_ren, "服务详情", lx, 336, THEME.text, g_fontBS);
        drawTextV(g_ren, g_recv_details_open ? "收起 ▲" : "展开 ▼",
                  lx + detail.w, 344, THEME.textDim, g_fontS, ALIGN_RIGHT);
        if (g_recv_details_open) {
            auto svcText = [](const char *label, const RecvServiceState &st) {
                std::string t = std::string(label) + " " + std::to_string(st.port);
                if (st.state == 0)      t += " 正常";
                else if (st.state == 2) t += " 不适用";
                else if (st.state == 1) {
                    t += " 未启动";
                    const std::string why = recvServiceErrorText(st.err);
                    if (!why.empty()) t += "（" + why + "）";
                } else                  t += " 未确认";
                return t;
            };
            const bool videoUp = (rv.state == RSV_WAITING || rv.state == RSV_AUTH ||
                                  rv.state == RSV_STREAMING);
            const std::string svcLine =
                std::string("视频 8081 ") + (videoUp ? "已就绪" : "未就绪") + " · " +
                svcText("计算", rv.services.gpu) + " · " +
                svcText("端口", rv.services.ports) + " · " +
                svcText("拼接", rv.services.kvm) + " · 发现 UDP 1900";
            drawText(g_ren, fitText(svcLine, g_fontS, card.w - 40), lx,
                     376, THEME.textDim, g_fontS);
            const std::string issue = recvServicesIssue(rv.services);
            if (!issue.empty())
                drawText(g_ren, fitText(issue, g_fontS, card.w - 40), lx,
                         398, THEME.warning, g_fontS);
        }
        const int dividerY = g_recv_details_open ? 420 : 386;
        drawHairline(g_ren, lx, dividerY, card.w - 40, 1, THEME.border);
        drawText(g_ren, "授权方式：" +
                            std::string(rv.policy == "open" ? "允许任意连接"
                                        : rv.policy == "password" ? "仅允许密码连接"
                                                                  : "每次手动授权"),
                 lx, dividerY + 24, THEME.text, g_font);
        drawText(g_ren, "密码验证只用于连接准入，画面数据并未加密",
                 lx, dividerY + 58, THEME.textDim, g_fontS);
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
        drawText(g_ren, "正在搜索同一局域网中的设备", cx, 420,
                 THEME.textDim, g_font, true);
        break;
    }

    case SCR_FOUND: {
        std::vector<DiscoveredDevice> devs;
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            devs = g_devices;
        }
        drawText(g_ren, "选择目标设备", BODY_X, TITLE_Y, THEME.text, g_fontB);
        drawTextV(g_ren, std::to_string(devs.size()) + " 个可用地址 · 点击模式即连接",
                  BODY_R, TITLE_Y + 12, THEME.textDim, g_fontS, ALIGN_RIGHT);
        drawText(g_ren, "投屏 · 镜像画面", BODY_X, 116, THEME.textDim, g_fontS);
        drawText(g_ren, "扩展 · 新增桌面", BODY_X + 218, 116, THEME.textDim, g_fontS);
        drawText(g_ren, "拼接 · 共享键鼠，不传画面", BODY_X + 436, 116,
                 THEME.textDim, g_fontS);
        for (size_t i = 0; i < devs.size() && i < 6; i++) {
            const int y = FOUND_ROW_Y0 + (int)i * FOUND_ROW_PITCH;
            const SDL_Rect row{BODY_X, y, BODY_W, FOUND_ROW_H};
            drawRoundRect(g_ren, row, THEME.radiusCard - 4, THEME.surface,
                          THEME.border);
            const std::string name = devs[i].name.empty()
                                         ? "未命名设备" : truncateUtf8(devs[i].name, 14);
            const int textW = DEV_BTN_X(0) - (row.x + 20) - 20;
            drawText(g_ren, fitText(name, g_font, textW), row.x + 20, row.y + 7,
                     THEME.text, g_font);
            drawText(g_ren, fitText(devs[i].toString(), g_fontS, textW),
                     row.x + 20, row.y + 31, THEME.textDim, g_fontS);
        }
        if (devs.size() > 6)
            drawText(g_ren, "仅显示前 6 个地址", BODY_X, FOUND_ACTIONS_MAX_Y - 22,
                     THEME.textDim, g_fontS);
        if (!g_status_note.empty())
            drawText(g_ren, fitText(g_status_note, g_fontS, BODY_W - 40), cx,
                     foundActionsY((int)devs.size()) - 26, statusToneColor(),
                     g_fontS, true);
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
        drawText(g_ren, "手动输入接收端 IPv4", cx, 230, THEME.text, g_fontL, true);
        drawText(g_ren, "对方开启接收后，在接收详情中查看本机 IPv4", cx, 278,
                 THEME.textDim, g_font, true);
        const SDL_Rect box{cx - 200, 312, 400, 60};
        drawRoundRect(g_ren, box, THEME.radius, THEME.surface,
                      g_manual_field_error ? THEME.danger : THEME.border);
        const bool empty = g_manual_ip.empty();
        drawTextIn(g_ren, empty ? "例如 192.168.1.105" : g_manual_ip, box,
                   empty ? THEME.textDim : THEME.text, g_font, ALIGN_CENTER);
        if (!g_status_note.empty())
            drawText(g_ren, fitText(g_status_note, g_font, BODY_W - 60), cx, 396,
                     statusToneColor(), g_font, true);
        if (!g_manual_field_error && !g_status_note.empty())
            drawText(g_ren, "请确认对方已开启接收、处于同一局域网，并检查防火墙",
                     cx, 420, THEME.textDim, g_fontS, true);
        break;
    }

    case SCR_STREAMING: {
        const std::string target = sendTargetName();
        drawText(g_ren, fitText(target.empty() ? "未选择设备" : target,
                                g_fontL, BODY_W - 40),
                 cx, 142, THEME.text, g_fontL, true);
        const std::string phase = (g_send_phase == SP_LIVE)
                                      ? "已连接" : sendPhaseLabel();
        drawText(g_ren, phase, cx, 190, sendPhaseColor(), g_fontB, true);
        drawText(g_ren, fitText(g_child_label + " · " + sendTargetEndpoint(),
                                g_fontS, BODY_W - 40),
                 cx, 222, THEME.textDim, g_fontS, true);

        if (g_send_phase == SP_FAILED) {
            const std::string why = g_status_note.empty()
                                        ? "连接未建立，请重试或返回"
                                        : g_status_note;
            drawText(g_ren, fitText(why, g_font, BODY_W - 40), cx, 266,
                     THEME.danger, g_font, true);
            drawText(g_ren, "未建立会话 · 可重试，或返回后重新选择目标",
                     cx, 298, THEME.textDim, g_fontS, true);
        } else {
            std::string note = g_auth_wait_note;
            if (note.empty() && g_send_phase == SP_LIVE)
                note = "关闭窗口不会中断 · 停止请使用下方按钮";
            if (note.empty() && g_send_phase == SP_CONNECTING)
                note = "正在建立连接，请稍候…";
            if (!note.empty())
                drawText(g_ren, fitText(note, g_fontS, BODY_W - 40), cx, 266,
                         g_send_phase == SP_LIVE ? THEME.textDim : sendPhaseColor(),
                         g_fontS, true);
            if (!g_status_note.empty() && g_status_note.find("目标: ") != 0)
                drawText(g_ren, fitText(g_status_note, g_fontS, BODY_W - 40),
                         cx, 292, statusToneColor(), g_fontS, true);
        }
        const bool stitch = g_sender.active && g_child_mode == SEND_STITCH;
        const int panelH = (stitch || g_details_open) ? ST_PANEL_H : 68;
        drawStatPanel(g_ren, SDL_Rect{ST_PANEL_X, ST_PANEL_Y, ST_PANEL_W, panelH});
        break;
    }

    case SCR_SETTINGS: {
        drawText(g_ren, "设置", BODY_X, TITLE_Y, THEME.text, g_fontB);
        int noteX = BODY_X + 64;
        if (settingsDirty()) {
            drawText(g_ren, "未保存", noteX, TITLE_Y + 8, THEME.warning, g_fontS);
            noteX += 64;
        }
        if (!g_status_note.empty())
            drawText(g_ren, fitText(g_status_note, g_fontS,
                                    SET_TAB_X(0) - 16 - noteX),
                     noteX, TITLE_Y + 8, statusToneColor(), g_fontS);

        if (g_set_tab == 0) {
            {
                const SDL_Rect card{SET_COL_L, SET_CARD_Y, SET_COL_W, SET_L_H};
                drawCard(g_ren, card, CARD_R);
                drawText(g_ren, "启动", SET_LC_X, card.y + 20,
                         THEME.textDim, g_fontBS);
                static const char *labels[2] = {"启动软件时自动开启接收", "开机自启动"};
                const bool on[2] = {g_cfg_edit.auto_receive != 0,
                                    g_cfg_edit.auto_start != 0};
                const int ids[2] = {BTN_SET_AUTORECV, BTN_SET_AUTOSTART};
                for (int i = 0; i < 2; i++) {
                    const int y = SET_TOGGLE_Y0 + i * 40;
                    const SDL_Rect row{SET_LC_X, y, SET_LC_W, 32};
                    if (g_hot == ids[i] || g_press == ids[i])
                        fillRoundRect(g_ren, row, THEME.radius - 3, THEME.hover);
                    drawTextV(g_ren, labels[i], row.x, y + 16, THEME.text,
                              g_font, ALIGN_LEFT);
                    drawToggle(g_ren, SDL_Rect{row.x + row.w - 44, y + 4, 44, 24}, on[i]);
                }
                drawText(g_ren, "自动接收只影响下次启动。",
                         SET_LC_X, SET_TOGGLE_Y0 + 96, THEME.textDim, g_fontS);
                drawText(g_ren, "开机自启动只负责启动程序。",
                         SET_LC_X, SET_TOGGLE_Y0 + 120, THEME.textDim, g_fontS);
                drawText(g_ren, "本次接收开关位于「接收与安全」。",
                         SET_LC_X, SET_TOGGLE_Y0 + 144, THEME.textDim, g_fontS);
            }
            {
                const SDL_Rect card{SET_COL_R, SET_CARD_Y, SET_COL_W, SET_R_H};
                drawCard(g_ren, card, CARD_R);
                drawText(g_ren, "更新与诊断", SET_RC_X, card.y + 20,
                         THEME.textDim, g_fontBS);
                static const char *labels[3] = {"状态控制台", "日志窗口", "自动检查更新"};
                const bool on[3] = {g_cfg_edit.show_status_console != 0,
                                    g_cfg_edit.show_log_window != 0,
                                    g_cfg_edit.auto_check_update != 0};
                const int ids[3] = {BTN_SET_CONSOLE, BTN_SET_LOGWIN,
                                    BTN_SET_AUTOCHECK};
                for (int i = 0; i < 3; i++) {
                    const int y = SET_TOGGLE_Y0 + i * 40;
                    const SDL_Rect row{SET_RC_X, y, SET_COL_W - SET_PAD * 2, 32};
                    if (g_hot == ids[i] || g_press == ids[i])
                        fillRoundRect(g_ren, row, THEME.radius - 3, THEME.hover);
                    drawTextV(g_ren, labels[i], row.x, y + 16, THEME.text,
                              g_font, ALIGN_LEFT);
                    drawToggle(g_ren, SDL_Rect{row.x + row.w - 44, y + 4, 44, 24}, on[i]);
                }
                drawText(g_ren, fitText("控制台开启后，发送端统计在控制台显示。",
                                        g_fontS, SET_COL_W - SET_PAD * 2),
                         SET_RC_X, SET_TOGGLE_Y0 + 130, THEME.textDim, g_fontS);
            }
            {
                const SDL_Rect card{BODY_X, SET_UPD_Y, BODY_W, SET_UPD_H};
                drawCard(g_ren, card, CARD_R);
                drawText(g_ren, "软件更新", card.x + 20, card.y + 14,
                         THEME.textDim, g_fontBS);
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
        } else if (g_set_tab == 1) {
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

        /* ── right column: 传输 ─────────────────────────────────────────── */
        {
            const SDL_Rect card{SET_COL_R, SET_CARD_Y, SET_COL_W, SET_R_H};
            drawCard(g_ren, card, CARD_R);
            drawText(g_ren, "传输", SET_RC_X, card.y + 20, THEME.textDim, g_fontBS);
            const int y = SET_TOGGLE_Y0;
            const SDL_Rect row{SET_RC_X, y, SET_COL_W - SET_PAD * 2, 32};
            if (g_hot == BTN_SET_HW || g_press == BTN_SET_HW)
                fillRoundRect(g_ren, row, THEME.radius - 3, THEME.hover);
            drawTextV(g_ren, "硬件加速", row.x, y + 16, THEME.text, g_font, ALIGN_LEFT);
            drawToggle(g_ren, SDL_Rect{row.x + row.w - 44, y + 4, 44, 24},
                       g_cfg_edit.hw_accel != 0);
            drawText(g_ren, fitText("优先使用可用的 GPU 编码器。",
                                    g_fontS, SET_COL_W - SET_PAD * 2),
                     SET_RC_X, y + 52, THEME.textDim, g_fontS);
            drawText(g_ren, fitText("不可用时自动回退到软件编码。",
                                    g_fontS, SET_COL_W - SET_PAD * 2),
                     SET_RC_X, y + 76, THEME.textDim, g_fontS);
            drawText(g_ren, "保存后应用；活动会话按现有通道更新。",
                     SET_RC_X, y + 112, THEME.textDim, g_fontS);
        }

        } else {
            /* ── 接收与安全 ─────────────────────────────────────────────── */
            {
                const SDL_Rect card{BODY_X, SEC_SWITCH_Y, BODY_W, SEC_SWITCH_H};
                drawCard(g_ren, card, CARD_R);
                drawText(g_ren, "本次接收", SEC_ROW_X, card.y + 18,
                         THEME.textDim, g_fontBS);
                const int y = SEC_TOGGLE_Y(0);
                if (g_hot == BTN_SET_ALLOW_RECV || g_press == BTN_SET_ALLOW_RECV)
                    fillRoundRect(g_ren, SDL_Rect{SEC_ROW_X, y, SEC_ROW_W, 32},
                                  THEME.radius - 3, THEME.hover);
                drawTextV(g_ren, "允许其他设备连接到此电脑", SEC_ROW_X + 4, y + 16,
                          THEME.text, g_font, ALIGN_LEFT);
                drawToggle(g_ren,
                           SDL_Rect{SEC_ROW_X + SEC_ROW_W - 48, y + 4, 44, 24},
                           receiveRunning());
                drawText(g_ren, std::string("当前状态：") + recvStateLabel(g_recv_state),
                         SEC_ROW_X + 4, SEC_SWITCH_Y + 98,
                         recvStateColor(g_recv_state), g_fontS);
                drawText(g_ren, "此开关立即启停接收，只改变本次运行，不写入配置。",
                         SEC_ROW_X + 4, SEC_SWITCH_Y + 140, THEME.textDim, g_fontS);
                drawText(g_ren, "下次启动是否接收，以及是否随 Windows 登录启动，",
                         SEC_ROW_X + 4, SEC_SWITCH_Y + 164, THEME.textDim, g_fontS);
                drawText(g_ren, "已移至「常规」页分别管理。",
                         SEC_ROW_X + 4, SEC_SWITCH_Y + 188, THEME.textDim, g_fontS);
            }

            {
                const SDL_Rect card{BODY_X, SEC_AUTH_Y, BODY_W, SEC_AUTH_H};
                drawCard(g_ren, card, CARD_R);
                drawText(g_ren, "连接授权方式", SEC_ROW_X, card.y + 18,
                         THEME.textDim, g_fontBS);

                static const char *kPolicy[3] = {
                    "允许任意连接",
                    "仅允许密码连接",
                    "每次手动授权（推荐）"};
                static const char *kPolicySub[3] = {
                    "同一网络中任何人都可以连接，不需要任何确认",
                    "知道连接密码的设备才能连接",
                    "每次连接都需要你在本机弹窗确认"};
                const int cur = (g_cfg_edit.auth_mode == "open")     ? 0
                                : (g_cfg_edit.auth_mode == "password") ? 1
                                                                       : 2;
                for (int i = 0; i < 3; i++) {
                    const int y = SEC_POLICY_Y(i);
                    const SDL_Rect row{SEC_ROW_X, y, SEC_ROW_W, 38};
                    if (cur == i) fillRoundRect(g_ren, row, THEME.radius - 4,
                                                THEME.surfaceAlt);
                    /* radio mark */
                    setColor(g_ren, cur == i ? THEME.primary : THEME.border);
                    fillCircle(g_ren, SEC_POLICY_X, y + 15, 7);
                    if (cur == i) {
                        setColor(g_ren, THEME.bg);
                        fillCircle(g_ren, SEC_POLICY_X, y + 15, 3);
                    }
                    drawText(g_ren, kPolicy[i], SEC_POLICY_X + 18, y + 2,
                             THEME.text, g_fontS);
                    drawText(g_ren, kPolicySub[i], SEC_POLICY_X + 18, y + 20,
                             i == 0 ? THEME.danger : THEME.textDim, g_fontS);
                }

                /* password record row: only meaningful in 密码 mode */
                const bool pwMode = (cur == 1);
                drawText(g_ren, "连接密码", SEC_ROW_X + 4, SEC_PW_ROW_Y,
                         pwMode ? THEME.text : THEME.textDim, g_fontS);
                const bool havePw = !g_cfg_edit.auth_hash.empty();
                drawText(g_ren, havePw ? "已设置（4–8 个字符）" : "尚未设置",
                         SEC_ROW_X + 92, SEC_PW_ROW_Y,
                         havePw ? THEME.textDim
                                : (pwMode ? THEME.warning : THEME.textDim), g_fontS);
                drawText(g_ren, "密码只用于连接准入，画面传输并未加密",
                         SEC_ROW_X + 4, SEC_PW_ROW_Y + 30, THEME.textDim, g_fontS);
                if (cur == 0)
                    drawText(g_ren,
                             "风险：任何能访问本机的设备都会直接接入，请只在完全可信的网络中使用",
                             SEC_ROW_X + 4, SEC_PW_ROW_Y + 54, THEME.danger,
                             g_fontS);
            }
        }

        /* ── 保存设置 / 恢复默认 / 返回会话 ─────────────────────────────── */
        break;
    }
    }

    for (size_t i = 0; i < g_modal_button_begin && i < g_buttons.size(); i++)
        drawButton(g_ren, g_buttons[i]);

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

    switch (topModal()) {
    case MODAL_AUTH:
    case MODAL_PW:
    case MODAL_SETPW:
    case MODAL_DISCARD:
        drawAuthDialogs(g_ren);
        break;
    case MODAL_STITCH:
        drawStitchSideDialog(g_ren);
        break;
    default:
        break;
    }
    for (size_t i = g_modal_button_begin; i < g_buttons.size(); i++)
        drawButton(g_ren, g_buttons[i]);

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
        requestQuit(true);       /* Setup already waited for a free binary */
    } else {
        logPush("[gui] 启动安装程序失败");
        g_upd_installing = false;                /* allow a retry */
    }
}

static void onButton(int id)
{
    /* Exactly one modal owns the input — the same one buildButtons() rendered
     * and renderScreen() painted. A lower dialog stays open but inert. */
    switch (topModal()) {
    case MODAL_AUTH:
        if (id == BTN_AUTH_ALLOW)      answerAuthAsk(true);
        else if (id == BTN_AUTH_DENY)  answerAuthAsk(false);
        return;
    case MODAL_PW:
        if (id == BTN_PW_CONNECT)      submitPassword();
        else if (id == BTN_PW_CANCEL)  closePasswordAsk(true);
        else if (id == BTN_PW_SHOW)    g_pw_ask.show = !g_pw_ask.show;
        return;
    case MODAL_SETPW:
        if (id == BTN_SETPW_OK)            submitSetPassword();
        else if (id == BTN_SETPW_CANCEL)   closeSetPwAsk();
        else if (id == BTN_SETPW_SHOW)     g_set_pw.show = !g_set_pw.show;
        return;
    case MODAL_STITCH:
        if (id >= BTN_STITCH_SIDE_BASE && id < BTN_STITCH_SIDE_BASE + 4)
            chooseStitchSide(id - BTN_STITCH_SIDE_BASE);
        else if (id == BTN_STITCH_SIDE_CANCEL)
            g_stitch_ask.open = false;
        return;
    case MODAL_DISCARD:
        if (id == BTN_DISCARD_KEEP) {
            g_discard_open = false;
        } else if (id == BTN_DISCARD_CONFIRM) {
            g_discard_open = false;
            cancelSettings(g_discard_to_home);
        }
        return;
    default:
        break;
    }
    /* Live side switch on the session screen */
    if (id >= BTN_STITCH_SWITCH_BASE && id < BTN_STITCH_SWITCH_BASE + 4) {
        const int side = id - BTN_STITCH_SWITCH_BASE;
        const std::string key = kStitchSideKey[side];
        /* A 50 ms STAT echo confirms the switch; persist immediately so the
         * next session starts on the same edge even if the echo is missed. */
        g_cfg.stitch_side = key;
        g_cfg_edit.stitch_side = key;
        configSave(g_cfg);
        if (sendToChild(g_sender, std::string("STITCH|side=") + key)) {
            g_status_note = std::string("拼接方向：") + kStitchSideLabel[side];
        } else if (g_child_mode == SEND_STITCH && g_sender.active) {
            /* Classic console mode has no stdin pipe, so the only way to move
             * the edge is to restart the controller — a new TCP connection,
             * so the target shows its consent dialog again. Say so instead of
             * claiming the switch is already live. */
            const std::string ip = g_last_ip, nm = g_last_name;
            const int port = g_last_port;
            stopSender();
            launchStream(ip, port, SEND_STITCH, nm, key.c_str());
            /* launchStream leaves its own failure note (and screen) alone. */
            if (g_sender.active)
                g_status_note = std::string("已按新方向重连：") +
                                kStitchSideLabel[side] + "（对方需再次确认）";
        } else {
            g_status_note = "切换失败：发送进程无响应";
        }
        return;
    }

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
    /* appearance: the header switcher ids land here */
    for (int i = 0; i < kThemeIdCount; i++) {
        if (kThemeIds[i].id != id) continue;
        const ThemeMode picked = kThemeIds[i].mode;
        themeSetMode(picked);
        /* Persist immediately: the header switcher is available on every
         * screen, so requiring 保存设置 would make it look broken elsewhere.
         * 忽略此版本 is also immediate; 日志窗口 is a preview until saved. */
        g_cfg.theme = themeModeKey(picked);
        g_cfg_edit.theme = g_cfg.theme;
        configSave(g_cfg);
        return;
    }

    switch (id) {
    case BTN_NAV_HOME:
        if (g_screen == SCR_SETTINGS) requestLeaveSettings(true);
        else                          setScreen(SCR_HOME);
        break;
    case BTN_RETURN_SESSION:
        if (g_screen == SCR_SETTINGS) requestLeaveSettings(false);
        else                          g_screen = resolveHomeScreen();
        break;
    case BTN_NAV_SETTINGS:
        /* Only re-snapshot when arriving from another screen. openSettings()
         * does g_cfg_edit = g_cfg, so re-clicking the tab that is already open
         * used to throw away every pending edit with no visible cause — the
         * 画质 box just snapped back to its saved value. */
        if (g_screen != SCR_SETTINGS) openSettings();
        break;
    case BTN_HOME_RECEIVE:
    case BTN_HOME_CONNECT:
        g_status_note.clear();
        startSearch();
        break;
    case BTN_HOME_SEND:
        g_status_note.clear();
        startSearch();
        break;
    case BTN_HOME_RECV_START:
        launchReceiver();
        break;
    case BTN_HOME_RECV_DETAIL:
        setScreen(SCR_RECEIVING);
        break;
    case BTN_HOME_RECEIVE_STOP:
        stopReceiver();
        break;
    case BTN_HOME_SENDER_STOP:
        stopSender();
        g_status_note = std::string("已停止") + g_child_label;
        g_status_tone = StatusTone::Info;
        setScreen(SCR_HOME);
        break;

    case BTN_RECV_STOP:
        stopReceiver();
        break;
    case BTN_RECV_LOG:
        toggleLogWindow();
        break;
    case BTN_RECV_DETAILS:
        g_recv_details_open = !g_recv_details_open;
        break;

    case BTN_SEARCH_CANCEL:
        ++g_search_generation;
        g_search_cancel = true;
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
        g_manual_field_error = false;
        g_status_note.clear();
        SDL_StartTextInput();
        setScreen(SCR_MANUAL_IP);
        break;

    case BTN_NF_CONTINUE:
        g_status_note.clear();
        startSearch();
        break;
    case BTN_NF_QUIT:
        requestQuit();
        break;

    case BTN_MANUAL_CONNECT: {
        std::string ip = g_manual_ip;
        if (ip.empty()) {
            g_manual_field_error = true;
            g_status_note = "请输入 IPv4 地址";
            g_status_tone = StatusTone::Error;
            break;
        }
        if (!validIPv4Address(ip)) {
            g_manual_field_error = true;
            g_status_note = "请输入有效的 IPv4 地址，例如 192.168.1.105";
            g_status_tone = StatusTone::Error;
            break;
        }
        g_manual_field_error = false;
        if (!testTcpConnection(ip, 8081, 1500)) {
            g_status_note = "无法连接 " + ip + ":8081";
            g_status_tone = StatusTone::Error;
            break;
        }
        {
            std::lock_guard<std::mutex> lk(g_dev_mtx);
            g_devices.clear();
            g_devices.emplace_back(ip, 8081);
        }
        SDL_StopTextInput();
        g_status_note.clear();
        g_status_tone = StatusTone::Info;
        setScreen(SCR_FOUND);
        break;
    }
    case BTN_MANUAL_BACK:
        SDL_StopTextInput();
        g_manual_field_error = false;
        g_status_note.clear();
        setScreen(SCR_HOME);
        break;

    case BTN_STREAM_STOP:
        stopSender();
        g_status_note = std::string("已停止") + g_child_label;
        g_status_tone = StatusTone::Info;
        setScreen(SCR_HOME);
        break;
    case BTN_STREAM_LOG:
        toggleLogWindow();
        break;
    case BTN_STREAM_DETAILS:
        g_details_open = !g_details_open;
        break;
    case BTN_STREAM_RETRY:
        startSendMode(g_last_ip, g_last_port, g_child_mode, g_last_name);
        break;
    case BTN_STREAM_BACK:
        setScreen(SCR_HOME);
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
        /* The flag is read when a child is spawned; a running receiver keeps
         * the mode it was started with. Say so instead of implying it is live. */
        if (g_receiver.active)
            g_status_note = "状态控制台将在下次启动接收时生效";
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
        /* 自动接收 is a next-launch preference only — never toggle the service
         * from here (PRD: 手动开关只改变当前状态). */
        g_status_note = "「自动接收」只影响下次启动，本次不会开启或关闭接收";
        break;
    case BTN_SET_TAB_BASE:
    case BTN_SET_TAB_BASE + 1:
    case BTN_SET_TAB_BASE + 2:
        g_set_tab = id - BTN_SET_TAB_BASE;
        break;
    case BTN_SET_ALLOW_RECV: {
        /* The switch is the live run state: start/stop now, and stay on this
         * settings page. 保存设置 later only reloads the policy of a running
         * receiver; it never starts or stops one. */
        const Screen keep = g_screen;
        const Screen keep_session = g_session_screen;
        if (receiveRunning()) stopReceiver();
        else                  launchReceiver();
        g_screen = keep;
        g_session_screen = keep_session;
        break;
    }
    case BTN_SET_POLICY_OPEN:
    case BTN_SET_POLICY_PASSWORD:
    case BTN_SET_POLICY_MANUAL: {
        const int idx = id - BTN_SET_POLICY_OPEN;
        g_cfg_edit.auth_mode = kAuthModeKey[idx];
        /* 密码 mode without a record is not a usable policy: open the editor
         * now rather than letting 保存设置 fail later. */
        if (idx == 1 && g_cfg_edit.auth_hash.empty()) openSetPwAsk();
        break;
    }
    case BTN_SET_PW_EDIT:
        openSetPwAsk();
        break;
    case BTN_HOME_RECV_SETTINGS:
        openSettings();
        g_set_tab = 2;
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
        /* 仅允许密码连接 is only a policy when a usable KDF record exists.
         * Refusing here keeps a half-configured receiver from either rejecting
         * every peer or (on an older build) degrading to 任意连接. */
        if (g_cfg_edit.auth_mode == "password") {
            uint8_t salt[16], hash[32];
            std::string why;
            const bool ok =
                mivaraHexDecode(g_cfg_edit.auth_salt, salt, sizeof(salt)) &&
                mivaraHexDecode(g_cfg_edit.auth_hash, hash, sizeof(hash)) &&
                mivaraAuthRecordUsable(hash, salt,
                                       (uint32_t)g_cfg_edit.auth_iter, &why);
            if (!ok) {
                g_set_tab = 2;
                g_status_note = "请先设置连接密码，再启用「仅允许密码连接」";
                break;
            }
        }
        const int want = g_cfg_edit.auto_start ? 1 : 0;
        const int had = g_cfg.auto_start ? 1 : 0;
        /* Update metadata is runtime state, not a user setting: preserve it
         * across the working-copy commit (else the 24h throttle and the
         * ignored-version filter would be silently reset on every save). */
        AppConfig next = g_cfg_edit;
        next.last_update_check = g_cfg.last_update_check;
        next.ignored_version = g_cfg.ignored_version;
        next.tray_hint_shown = g_cfg.tray_hint_shown;
        if (!configSave(next)) {
            g_status_note = "无法写入配置文件，修改尚未保存；请检查目录权限后重试";
            g_status_tone = StatusTone::Error;
            break;
        }
        g_cfg = next;
        g_status_note.clear();
        g_status_tone = StatusTone::Success;
        /* 授权方式：保存设置只做策略热更新。接收的启动/停止由「允许其他设备连接」
         * 开关立即完成，这里绝不隐式启动或停止服务；已建立的会话不受影响。 */
        {
            std::lock_guard<std::mutex> lk(g_recv_mtx);
            g_recv_policy = g_cfg.auth_mode;
        }
        bool reloadFailed = false;
        if (g_receiver.active) {
            if (!sendToChild(g_receiver, "RX|reload")) {
                noteReceiverControlFailure("接收策略热更新");
                reloadFailed = true;
            }
        }
        /* Re-register whenever auto_start is on so a moved exe path stays
         * current; drop the task when it was just switched off. Verify the
         * result against Task Scheduler and, when it differs, write the real
         * state back so the switch never claims logon will do something it
         * will not. */
        if (want != had || want == 1) {
            const bool now = applyAutoStart(want);
            if (now != (want != 0)) {
                g_cfg.auto_start = now ? 1 : 0;
                g_cfg_edit.auto_start = g_cfg.auto_start;
                if (!configSave(g_cfg)) {
                    g_status_note += "；实际状态无法写入配置";
                    reloadFailed = true;
                }
#ifdef _WIN32
                g_status_note = want ? "开机自启动注册失败（计划任务）"
                                     : "开机自启动取消失败（计划任务）";
#else
                g_status_note = "开机自启动目前仅支持 Windows";
#endif
                reloadFailed = true;   /* keep this note below */
            } else if (!reloadFailed) {
                g_status_note.clear();
            }
        }
        /* Live-apply transfer params to a running sender session. A stitch
         * child has no encoder and does not read SETP lines, so applying to
         * it would only report a change that never happens. */
        if (g_sender.active && g_child_mode != SEND_STITCH) {
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
                        reloadFailed = true;
                    }
                } else {
                    /* Console-mode child has no stdin channel – restart the
                     * session with the new params instead. */
                    const int mode = g_child_mode;
                    const std::string ip = g_last_ip, nm = g_last_name;
                    const int port = g_last_port;
                    stopSender();
                    g_status_note = "控制台模式：已按新参数重启会话";
                    launchStream(ip, port, mode, nm);
                    return;
                }
            }
        }
        if (reloadFailed) {
            g_status_tone = StatusTone::Error;
            g_screen = SCR_SETTINGS;
            break;
        }
        g_screen = resolveHomeScreen();
        break;
    }
    case BTN_SET_CANCEL:
        cancelSettings(false);
        break;
    case BTN_SET_RESET:
        g_cfg_edit = AppConfig{};
        themeSetMode(ThemeMode::System);
        if (g_log_open) closeLogWindow();
        g_status_note = "已恢复默认值，保存后生效";
        g_status_tone = StatusTone::Info;
        break;

    case BTN_HIST_CLEAR:
        historyClear();
        g_hist.clear();
        g_hist_scroll = 0;
        break;

    default:
        /* device rows: BTN_DEV_BASE + idx*10 + 1(投屏) / +2(扩展) / +3(拼接) */
        if (id >= BTN_DEV_BASE && id < BTN_HIST_BASE) {
            int rel = id - BTN_DEV_BASE;
            int idx = rel / 10;
            int kind = rel % 10;
            std::vector<DiscoveredDevice> devs;
            {
                std::lock_guard<std::mutex> lk(g_dev_mtx);
                devs = g_devices;
            }
            if (idx >= 0 && idx < (int)devs.size() && kind >= 1 && kind <= 3)
                startSendMode(devs[idx].ip_address, devs[idx].tcp_port,
                              kind - 1, devs[idx].name);
            break;
        }
        /* history rows: BTN_HIST_BASE + idx*10 + 0(row) / 1 / 2 / 3 / 4(删除) */
        if (id >= BTN_HIST_BASE && id < BTN_HIST_CLEAR) {
            int rel = id - BTN_HIST_BASE;
            int idx = rel / 10;
            int kind = rel % 10;
            if (idx >= 0 && idx < (int)g_hist.size()) {
                const HistEntry &e = g_hist[idx];
                if (kind == 4) {
                    historyDelete(e.ip, e.port);
                    g_hist = historyLoad();
                    /* Clamp to the last full page, not to the entry count —
                     * otherwise deleting a middle entry leaves a blank list. */
                    const int maxScroll =
                        std::max(0, (int)g_hist.size() - HIST_VISIBLE);
                    if (g_hist_scroll > maxScroll) g_hist_scroll = maxScroll;
                } else if (kind >= 1 && kind <= 3) {
                    startSendMode(e.ip, e.port, kind - 1, e.name);
                } else {
                    /* row body click: reconnect with the entry's last mode */
                    startSendMode(e.ip, e.port, sendModeFromKey(e.mode), e.name);
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
         * screen: at 125% on a 768-tall panel the bottom row (保存设置 / 取消)
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

    /* Second launch: wake the copy that is already running (and keep exactly
     * one receiver competing for the ports) instead of starting a second tray. */
    if (!traySingleInstance()) {
        std::cout << "Mivara 已在运行，已唤起现有窗口。\n";
        TTF_Quit();
        SDL_Quit();
        return 0;
    }

    /* Only the primary instance touches the logon task. This also heals a
     * task that an earlier failed registration or an uninstall left behind. */
    reconcileAutoStart(g_cfg);

    g_win = SDL_CreateWindow("Mivara v" MIVARA_VERSION, SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, uiPx(WIN_W), uiPx(WIN_H), 0);
    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_win || !g_ren) {
        std::cerr << "SDL_CreateWindow/Renderer: " << SDL_GetError() << "\n";
        /* traySingleInstance() may already own the wake thread; leaving a
         * joinable std::thread behind turns the exit into std::terminate. */
        if (g_ren) SDL_DestroyRenderer(g_ren);
        if (g_win) SDL_DestroyWindow(g_win);
        trayShutdown();
        traySingleInstanceRelease();
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    applyMivaraWindowIcon(g_win);
    /* Required by fillRoundRect: the tint chips, hairlines and the stacked
     * drop shadow all rely on SDL honouring the alpha channel. */
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    /* Every shape primitive below is in logical units; this is what maps them
     * onto the larger back buffer. */
    SDL_RenderSetScale(g_ren, g_uiScale, g_uiScale);

    /* Windows tray: the close button hides the window instead of ending a
     * session. Unsupported platforms keep the normal close-to-exit behaviour. */
    g_tray_ok = trayInit(trayTooltip());
    g_tray_tip_last = trayTooltip();
    if (g_tray_ok) logPush("[gui] 托盘图标已创建");

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

    /* auto_receive is the only startup preference. A manual stop in the
     * previous run is not a reason to stay off this time. */
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
                if (g_exit_no_confirm) {
                    g_exit_no_confirm = false;
                    running = false;              /* installer hand-over */
                } else if (g_exit_requested) {
                    g_exit_requested = false;
                    if (confirmExit()) {
                        stopReceiver();       /* single stop path, even on quit */
                        running = false;
                    }
                } else if (g_tray_ok) {
                    hideToTray();                 /* window X: keep running */
                } else {
                    running = false;
                }
            }
            else if (ev.type == SDL_WINDOWEVENT &&
                     ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                if (g_log_win && ev.window.windowID == SDL_GetWindowID(g_log_win))
                    closeLogWindow();
                else if (g_tray_ok)
                    hideToTray();
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
                    const bool modal = (topModal() != MODAL_NONE);
                    for (size_t i = 0; i < g_buttons.size(); i++) {
                        const Button &b = g_buttons[i];
                        if (!b.enabled || (modal && i < g_modal_button_begin)) continue;
                        if (SDL_PointInRect(&p, &b.rect)) { g_press = b.id; break; }
                    }
                }
            }
            else if (ev.type == SDL_MOUSEBUTTONUP &&
                     ev.button.button == SDL_BUTTON_LEFT) {
                if (ev.button.windowID == SDL_GetWindowID(g_win)) {
                    SDL_Point p{uiCoord(ev.button.x), uiCoord(ev.button.y)};
                    int hit = BTN_NONE;
                    const bool modal = (topModal() != MODAL_NONE);
                    for (size_t i = 0; i < g_buttons.size(); i++) {
                        const Button &b = g_buttons[i];
                        if (!b.enabled || (modal && i < g_modal_button_begin)) continue;
                        if (SDL_PointInRect(&p, &b.rect)) { hit = b.id; break; }
                    }
                    if (g_press != BTN_NONE && hit == g_press) onButton(g_press);
                    g_press = BTN_NONE;
                }
            }
            else if (ev.type == SDL_TEXTINPUT) {
                /* Text goes to the TOP modal only; a lower dialog keeps its
                 * contents but does not receive keystrokes. */
                if (topModal() == MODAL_SETPW) {
                    appendPasswordInput(g_set_pw.a, ev.text.text, g_set_pw.err);
                } else if (topModal() == MODAL_PW) {
                    appendPasswordInput(g_pw_ask.text, ev.text.text,
                                        g_pw_ask.error);
                } else if (topModal() == MODAL_NONE &&
                           g_screen == SCR_MANUAL_IP) {
                    g_manual_field_error = false;
                    g_status_note.clear();
                    for (const char *c = ev.text.text; *c; c++) {
                        if ((isdigit((unsigned char)*c) || *c == '.') &&
                            g_manual_ip.size() < 15)
                            g_manual_ip += *c;
                    }
                }
            }
            else if (ev.type == SDL_KEYDOWN) {
                const Modal modal = topModal();
                if (modal == MODAL_SETPW) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE) {
                        closeSetPwAsk();
                    } else if (ev.key.keysym.sym == SDLK_RETURN ||
                               ev.key.keysym.sym == SDLK_KP_ENTER) {
                        submitSetPassword();
                    } else if (ev.key.keysym.sym == SDLK_BACKSPACE) {
                        if (!g_set_pw.a.empty()) g_set_pw.a.pop_back();
                        g_set_pw.err.clear();
                    } else if ((ev.key.keysym.mod & KMOD_CTRL) &&
                               ev.key.keysym.sym == SDLK_v) {
                        char *clip = SDL_GetClipboardText();
                        if (clip) {
                            appendPasswordInput(g_set_pw.a, clip, g_set_pw.err);
                            SDL_free(clip);
                        }
                    }
                    continue;
                }
                if (modal == MODAL_PW) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE) {
                        closePasswordAsk(true);
                    } else if (ev.key.keysym.sym == SDLK_RETURN ||
                               ev.key.keysym.sym == SDLK_KP_ENTER) {
                        submitPassword();
                    } else if (ev.key.keysym.sym == SDLK_BACKSPACE) {
                        if (!g_pw_ask.text.empty()) g_pw_ask.text.pop_back();
                        g_pw_ask.error.clear();
                    } else if ((ev.key.keysym.mod & KMOD_CTRL) &&
                               ev.key.keysym.sym == SDLK_v) {
                        /* Ctrl+V: SDL only turns a paste into SDL_TEXTINPUT on
                         * some platforms, so read the clipboard directly. */
                        char *clip = SDL_GetClipboardText();
                        if (clip) {
                            appendPasswordInput(g_pw_ask.text, clip,
                                                g_pw_ask.error);
                            SDL_free(clip);
                        }
                    }
                    continue;
                }
                if (modal == MODAL_AUTH) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE)
                        answerAuthAsk(false);   /* default answer is 拒绝 */
                    continue;
                }
                if (modal == MODAL_STITCH) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE)
                        g_stitch_ask.open = false;
                    continue;
                }
                if (modal == MODAL_DISCARD) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE)
                        g_discard_open = false;
                    continue;
                }
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (g_screen == SCR_MANUAL_IP) {
                        SDL_StopTextInput();
                        g_manual_field_error = false;
                        g_status_note.clear();
                        g_status_tone = StatusTone::Info;
                        setScreen(SCR_HOME);
                    } else if (g_screen == SCR_SETTINGS) {
                        requestLeaveSettings(false);
                    } else if (g_screen != SCR_HOME) {
                        /* Session screens return to Home without stopping the
                         * child; stopping stays an explicit button action. */
                        setScreen(SCR_HOME);
                    }
                }
                else if (g_screen == SCR_MANUAL_IP &&
                         ev.key.keysym.sym == SDLK_BACKSPACE) {
                    g_manual_field_error = false;
                    g_status_note.clear();
                    if (!g_manual_ip.empty()) g_manual_ip.pop_back();
                }
                else if (g_screen == SCR_MANUAL_IP &&
                         ev.key.keysym.sym == SDLK_RETURN) {
                    onButton(BTN_MANUAL_CONNECT);
                }
            }
        }

        processSearchUpdates();
        drainChildStats();          /* STAT lines from the reader threads */
        tickAuthDialogs();
        tickRecvState();            /* STARTING must not last forever */

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
            drainChildStats();
            g_child_extend = false;
            std::string err;
            {
                std::lock_guard<std::mutex> lk(g_stats_mtx);
                if (g_stats.valid && g_stats.event == "error" &&
                    !g_stats.msg.empty())
                    err = g_stats.msg;
            }
            if (g_sender_stop_requested) err.clear();
            g_send_phase = err.empty() ? SP_IDLE : SP_FAILED;
            g_auth_wait_note.clear();
            if (!err.empty()) {
                g_status_note = (g_screen == SCR_STREAMING)
                                    ? senderErrorText(err)
                                    : "连接失败：" + senderErrorText(err);
                g_status_tone = StatusTone::Error;
            } else {
                g_status_note = "会话已结束";
                g_status_tone = StatusTone::Info;
                if (g_screen == SCR_STREAMING) setScreen(SCR_HOME);
            }
            if (g_hidden)
                trayNotify("会话已结束", err.empty() ? "会话已结束，单击托盘图标查看"
                                                     : senderErrorText(err));
        }
        if (g_receiver.active && childExited(g_receiver, "receiver")) {
            /* Apply the final lines with the launch generation (so a bind /
             * auth_config token survives), then bump it so nothing this dead
             * process queued can ever write WAITING again. A user stop goes
             * through stopReceiver(), which already made active=false, so
             * reaching this block means an unexpected exit. */
            drainChildStats();
            ++g_recv_epoch;
            closeAuthAsk(-1);
            std::string err;
            {
                std::lock_guard<std::mutex> lk(g_recv_mtx);
                err = g_recv_error;
                g_recv_peer.clear();
                if (err.empty()) g_recv_error = "receiver_exited";
                g_recv_services = RecvServices{};
                g_recv_state = RSV_FAILED;
                err = g_recv_error;
            }
            g_recv_started_at = 0;
            logPush("[gui] 接收服务意外退出：" + receiverErrorText(err));
            if (g_screen == SCR_RECEIVING) {
                g_status_note = "接收服务已退出：" + receiverErrorText(err);
                setScreen(SCR_HOME);
            }
        }

        /* tray requests: 显示主界面 / 退出 (never touches the settings).
         * Drain unconditionally: the single-instance wake channel outlives the
         * icon, so a shell-rejected icon must not strand a second launch's
         * show request (issue 19). Only the tooltip needs a live icon. */
        {
            const TrayRequests tr = trayTakeRequests();
            if (tr.show) showFromTray();
            if (tr.quit && confirmExit()) {
                stopReceiver();           /* single stop path */
                running = false;
            }
            if (g_tray_ok) {
                const std::string tip = trayTooltip();
                if (tip != g_tray_tip_last) {
                    g_tray_tip_last = tip;
                    traySetTooltip(tip);
                }
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
        const bool modal = (topModal() != MODAL_NONE);
        for (size_t i = 0; i < g_buttons.size(); i++) {
            const Button &b = g_buttons[i];
            if (!b.enabled || (modal && i < g_modal_button_begin)) continue;
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
        g_search_cancel = true;
        g_search_thread.join();
    }
    updateJoin();               /* abort + join any in-flight check/download */
    stopChild(g_sender, g_child_extend);
    stopChild(g_receiver);
    trayShutdown();             /* remove the icon before the window goes away */
    traySingleInstanceRelease();
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
