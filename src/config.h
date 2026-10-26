/**
 * CONFIG.H - Mivara app settings (key=value ini, held by the GUI app)
 *
 * The GUI (app) owns the config file and passes values to sender/receiver
 * via command line; children are stateless.
 */
#ifndef MIVARA_CONFIG_H
#define MIVARA_CONFIG_H

#include <string>

struct AppConfig
{
    std::string codec = "h264";      /* auto|h264|h265|av1|raw|rle (auto=>h264) */
    int hw_accel = 1;                /* 1 = prefer hardware encoding */
    int bitrate_kbps = 0;            /* 0 = quality/CRF mode */
    int quality = 23;                /* 0..51 */
    int fps_limit = 60;              /* 1..240 */
    int scale_percent = 100;         /* 25..100 */
    std::string chroma = "420";      /* 420|444 */
    int show_status_console = 0;     /* 1 = classic cmd console for children */
    int show_log_window = 0;         /* 1 = auto-open the log window */
    int auto_start = 0;              /* 1 = launch at logon (Task Scheduler;
                                        verified by read-back after apply) */
    int auto_receive = 0;            /* the ONLY persisted startup preference:
                                        1 = enter receive mode on app open */
    int allow_receive = 1;           /* runtime mirror only — NOT persisted and
                                        NOT read from config.ini. Whether the
                                        receiver is running lives in the app's
                                        recv state; at startup only
                                        auto_receive decides whether to start
                                        receiving. */
    int auto_check_update = 1;       /* 1 = check for updates on app open */
    std::string theme = "system";    /* light|dark|system (GUI appearance) */
    std::string extend_res = "auto"; /* extend mode VDD size: auto|WxH
                                        (auto = the receiver's resolution) */
    std::string vdd_mode_write = "always"; /* extend mode: always|never (legacy ask→always) –
                                           may Mivara add the receiver's
                                           resolution to vdd_settings.xml? */
    std::string stitch_side = "right";  /* 拼接: left|right|top|bottom –
                                           which edge the peer sits on (the
                                           GUI preselects it in the dialog) */
    /* ── 接收与安全 ────────────────────────────────────────────────────────
     * Connection policy. The default for a new install (and for an upgraded
     * config with no policy key) is manual approval — never open. */
    std::string auth_mode = "manual";   /* open|password|manual */
    std::string auth_salt;              /* 32 hex chars, 16 bytes */
    std::string auth_hash;              /* 64 hex chars — PBKDF2 output, NOT
                                           the password (see auth.h) */
    int auth_iter = 120000;             /* PBKDF2-HMAC-SHA256 iterations */
    int tray_hint_shown = 0;            /* one-time "still running in tray" hint */
    long long last_update_check = 0; /* unix seconds of the last attempt (runtime metadata) */
    std::string ignored_version;     /* e.g. "v0.9.2"; runtime metadata */
};

/** Directory holding config.ini / history.ini (created on demand). */
std::string configDir();
/** Full path of config.ini. */
std::string configPath();

/** Fill out with defaults, then override from disk if present.
 *  Missing file is success (defaults stand). */
bool configLoad(AppConfig &out);
/** Atomic write: tmp file + rename. */
bool configSave(const AppConfig &cfg);

#endif
