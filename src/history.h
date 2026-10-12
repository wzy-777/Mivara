/**
 * HISTORY.H - TwinView connection history (history.ini next to config.ini)
 *
 * Lines: ip|port|name|mode|last_used_unix|use_count   (newest first, cap 20)
 */
#ifndef TWINVIEW_HISTORY_H
#define TWINVIEW_HISTORY_H

#include <string>
#include <vector>

struct HistEntry
{
    std::string ip;
    int port = 8081;
    std::string name;              /* friendly name; empty → display falls back to ip */
    std::string mode;              /* "mirror" | "extend" */
    long long last_used_unix = 0;
    int use_count = 1;
};

/** Load entries (empty vector when the file is missing). */
std::vector<HistEntry> historyLoad();

/** Insert or refresh entry keyed by ip:port (bump count, refresh time/mode/name,
 *  move to front, cap 20). name is kept unless the new one is a real name or
 *  the stored one was an IP fallback. */
void historyUpsert(const std::string &ip, int port, const std::string &name,
                   const std::string &mode);

/** Remove the ip:port entry. Returns true if something was removed. */
bool historyDelete(const std::string &ip, int port);

/** Drop all entries. */
void historyClear();

#endif
