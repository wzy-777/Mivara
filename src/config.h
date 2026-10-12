/**
 * CONFIG.H - TwinView app settings (key=value ini, held by the GUI app)
 *
 * The GUI (app) owns the config file and passes values to sender/receiver
 * via command line; children are stateless.
 */
#ifndef TWINVIEW_CONFIG_H
#define TWINVIEW_CONFIG_H

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
    int auto_start = 0;              /* 1 = launch at logon (Task Scheduler) */
    int auto_receive = 0;            /* 1 = enter receive mode on app open */
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
