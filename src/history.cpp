/**
 * HISTORY.CPP - Mivara connection history persistence
 *
 * history.ini sits next to config.ini (configDir()). Line format:
 *   ip|port|name|mode|last_used_unix|use_count
 * Newest first, capped at 20 entries. '#' comments allowed.
 */
#include "history.h"
#include "config.h"

#include <algorithm>
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
#  include <direct.h>
#endif

namespace {

const size_t HIST_CAP = 20;

std::string historyPath()
{
#ifdef _WIN32
    return configDir() + "\\history.ini";
#else
    return configDir() + "/history.ini";
#endif
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitPipe(const std::string &s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find('|', start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

bool isRealName(const std::string &name, const std::string &ip)
{
    return !name.empty() && name != ip;
}

void writeAll(const std::vector<HistEntry> &entries)
{
    std::string dir = configDir();
#ifdef _WIN32
    _mkdir(dir.c_str()); /* best effort; configSave creates the full tree */
#else
    mkdir(dir.c_str(), 0755);
#endif
    (void)dir;

    std::string path = historyPath();
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return;
    fputs("# ip|port|name|mode|last_used_unix|use_count\n", f);
    for (const auto &e : entries) {
        fprintf(f, "%s|%d|%s|%s|%lld|%d\n", e.ip.c_str(), e.port, e.name.c_str(),
                e.mode.c_str(), (long long)e.last_used_unix, e.use_count);
    }
    fclose(f);
#ifdef _WIN32
    /* MoveFileEx replaces in one step; remove()+rename() would leave the old
     * history.ini deleted when the rename fails. */
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        remove(tmp.c_str());
#else
    if (rename(tmp.c_str(), path.c_str()) != 0)
        remove(tmp.c_str());
#endif
}

} // namespace

std::vector<HistEntry> historyLoad()
{
    std::vector<HistEntry> out;
    FILE *f = fopen(historyPath().c_str(), "rb");
    if (!f)
        return out;
    char line[512];
    while (fgets(line, sizeof(line), f) && out.size() < HIST_CAP) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#')
            continue;
        std::vector<std::string> parts = splitPipe(s);
        if (parts.size() < 6)
            continue;
        HistEntry e;
        e.ip = trim(parts[0]);
        if (e.ip.empty())
            continue;
        e.port = atoi(parts[1].c_str());
        if (e.port <= 0 || e.port > 65535)
            continue;
        e.name = trim(parts[2]);
        e.mode = trim(parts[3]);
        if (e.mode != "mirror" && e.mode != "extend")
            e.mode = "mirror";
        e.last_used_unix = atoll(parts[4].c_str());
        e.use_count = atoi(parts[5].c_str());
        if (e.use_count < 1)
            e.use_count = 1;
        out.push_back(e);
    }
    fclose(f);
    return out;
}

void historyUpsert(const std::string &ip, int port, const std::string &name,
                   const std::string &mode)
{
    if (ip.empty() || port <= 0 || port > 65535)
        return;
    std::vector<HistEntry> entries = historyLoad();

    std::vector<HistEntry> next;
    next.reserve(entries.size() + 1);
    HistEntry fresh;
    fresh.ip = ip;
    fresh.port = port;
    fresh.name = name;
    fresh.mode = (mode == "extend") ? "extend" : "mirror";
    fresh.last_used_unix = (long long)time(nullptr);
    fresh.use_count = 1;

    bool merged = false;
    for (auto &e : entries) {
        if (!merged && e.ip == ip && e.port == port) {
            fresh.use_count = e.use_count + 1;
            /* overwrite name only if the new one is real or the old was an IP fallback */
            if (!isRealName(fresh.name, ip) && isRealName(e.name, ip))
                fresh.name = e.name;
            merged = true;
            continue; /* skip the old copy; fresh goes to the front */
        }
        next.push_back(e);
    }
    next.insert(next.begin(), fresh);
    if (next.size() > HIST_CAP)
        next.resize(HIST_CAP);
    writeAll(next);
}

bool historyDelete(const std::string &ip, int port)
{
    std::vector<HistEntry> entries = historyLoad();
    std::vector<HistEntry> next;
    bool removed = false;
    for (auto &e : entries) {
        if (e.ip == ip && e.port == port) {
            removed = true;
            continue;
        }
        next.push_back(e);
    }
    if (removed)
        writeAll(next);
    return removed;
}

void historyClear()
{
    writeAll(std::vector<HistEntry>());
}
