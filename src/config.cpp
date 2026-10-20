/**
 * CONFIG.CPP - Mivara app settings persistence
 *
 * key=value ini with '#' comments. Written atomically (tmp + rename).
 * Location: %LOCALAPPDATA%\Mivara\config.ini (Windows)
 *           $XDG_CONFIG_HOME/mivara/ or ~/.config/mivara/ (POSIX)
 */
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>
#  include <direct.h>
#else
#  include <unistd.h>
#endif

namespace {

#ifdef _WIN32
const char *DIR_SEP = "\\";
#else
const char *DIR_SEP = "/";
#endif

bool dirExists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

bool makeDirs(const std::string &p)
{
    if (p.empty() || dirExists(p))
        return true;
    /* create parents first (LOCALAPPDATA\Mivara is one level, but be safe) */
    size_t start = 0;
#ifdef _WIN32
    if (p.size() >= 2 && p[1] == ':')
        start = 2;
#endif
    for (size_t i = start; i < p.size(); i++) {
        if (p[i] == '\\' || p[i] == '/') {
            if (i == 0 || (i == start && p[i] == p[start]))
                continue;
            std::string sub = p.substr(0, i);
            if (!dirExists(sub)) {
#ifdef _WIN32
                _mkdir(sub.c_str());
#else
                mkdir(sub.c_str(), 0755);
#endif
            }
        }
    }
#ifdef _WIN32
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
    return dirExists(p);
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

int clampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void applyKey(AppConfig &cfg, const std::string &key, const std::string &val)
{
    if (key == "codec") {
        if (val == "auto" || val == "h264" || val == "h265" || val == "av1" ||
            val == "raw" || val == "rle")
            cfg.codec = val;
    } else if (key == "hw_accel") {
        cfg.hw_accel = (val == "1") ? 1 : 0;
    } else if (key == "bitrate_kbps") {
        int v = atoi(val.c_str());
        if (v < 0) v = 0;
        cfg.bitrate_kbps = v;
    } else if (key == "quality") {
        cfg.quality = clampInt(atoi(val.c_str()), 0, 51);
    } else if (key == "fps_limit") {
        cfg.fps_limit = clampInt(atoi(val.c_str()), 1, 240);
    } else if (key == "scale_percent") {
        int v = atoi(val.c_str());
        /* accept only the offered steps, otherwise clamp into range */
        if (v != 50 && v != 75 && v != 100)
            v = clampInt(v, 25, 100);
        cfg.scale_percent = v;
    } else if (key == "chroma") {
        if (val == "420" || val == "444")
            cfg.chroma = val;
    } else if (key == "show_status_console") {
        cfg.show_status_console = (val == "1") ? 1 : 0;
    } else if (key == "show_log_window") {
        cfg.show_log_window = (val == "1") ? 1 : 0;
    } else if (key == "auto_start") {
        cfg.auto_start = (val == "1") ? 1 : 0;
    } else if (key == "auto_receive") {
        cfg.auto_receive = (val == "1") ? 1 : 0;
    } else if (key == "auto_check_update") {
        cfg.auto_check_update = (val == "1") ? 1 : 0;
    } else if (key == "theme") {
        /* unknown value → keep the default (system) rather than a broken mode */
        if (val == "light" || val == "dark" || val == "system")
            cfg.theme = val;
    } else if (key == "last_update_check") {
        /* runtime metadata: reject garbage and future clock skew */
        long long v = strtoll(val.c_str(), nullptr, 10);
        long long now = (long long)time(nullptr);
        if (v < 0 || v > now) v = 0;
        cfg.last_update_check = v;
    } else if (key == "ignored_version") {
        if (!val.empty() && val.size() <= 32 &&
            val.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                  "abcdefghijklmnopqrstuvwxyz0123456789._-") ==
                std::string::npos)
            cfg.ignored_version = val;
    }
}

} // namespace

std::string configDir()
{
#ifdef _WIN32
    std::string base;
    const char *la = getenv("LOCALAPPDATA");
    if (la && *la)
        base = la;
    else {
        char buf[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)) && buf[0])
            base = buf;
        else
            base = ".";
    }
    return base + "\\Mivara";
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return std::string(xdg) + "/mivara";
    const char *home = getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.config/mivara";
    return "./mivara";
#endif
}

std::string configPath()
{
    return configDir() + DIR_SEP + "config.ini";
}

bool configLoad(AppConfig &out)
{
    out = AppConfig(); /* defaults first */
    FILE *f = fopen(configPath().c_str(), "rb");
    if (!f)
        return true; /* missing file = defaults, success */

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';')
            continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));
        applyKey(out, key, val);
    }
    fclose(f);
    return true;
}

bool configSave(const AppConfig &cfg)
{
    std::string dir = configDir();
    if (!makeDirs(dir))
        return false;

    std::string path = configPath();
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;

    fputs("# Mivara settings\n", f);
    fprintf(f, "codec=%s\n", cfg.codec.c_str());
    fprintf(f, "hw_accel=%d\n", cfg.hw_accel ? 1 : 0);
    fprintf(f, "bitrate_kbps=%d\n", cfg.bitrate_kbps);
    fprintf(f, "quality=%d\n", clampInt(cfg.quality, 0, 51));
    fprintf(f, "fps_limit=%d\n", clampInt(cfg.fps_limit, 1, 240));
    fprintf(f, "scale_percent=%d\n", clampInt(cfg.scale_percent, 25, 100));
    fprintf(f, "chroma=%s\n", cfg.chroma.c_str());
    fprintf(f, "show_status_console=%d\n", cfg.show_status_console ? 1 : 0);
    fprintf(f, "show_log_window=%d\n", cfg.show_log_window ? 1 : 0);
    fprintf(f, "auto_start=%d\n", cfg.auto_start ? 1 : 0);
    fprintf(f, "auto_receive=%d\n", cfg.auto_receive ? 1 : 0);
    fprintf(f, "auto_check_update=%d\n", cfg.auto_check_update ? 1 : 0);
    fprintf(f, "theme=%s\n", cfg.theme.c_str());
    fprintf(f, "last_update_check=%lld\n", cfg.last_update_check);
    fprintf(f, "ignored_version=%s\n", cfg.ignored_version.c_str());
    fclose(f);

#ifdef _WIN32
    /* MoveFileEx replaces in one step; remove()+rename() would leave the old
     * config.ini deleted when the rename fails. */
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp.c_str());
        return false;
    }
    return true;
#else
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
#endif
}
