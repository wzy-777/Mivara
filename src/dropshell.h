/**
 * DROPSHELL.H – Windows Shell/OLE half of the 8085 drop channel.
 *
 * Kept separate from drop.cpp so the wire protocol/staging code stays free of
 * Shell COM details. Windows only; callers must run the resolver on an STA
 * thread (drop.cpp's worker calls CoInitializeEx before using these).
 */
#ifndef MIVARA_DROPSHELL_H
#define MIVARA_DROPSHELL_H

#include <cstdint>
#include <string>
#include <vector>

/** Resolve the folder under a release point (target screen pixels).
 *  Handles Explorer windows and the desktop; falls back to false for virtual
 *  folders, non-filesystem views and rejected locations. */
bool dropResolveFolder(int screen_x, int screen_y, std::wstring &folder,
                       std::string &why);

/** %USERPROFILE%\\Downloads\\Mivara, created on demand. */
bool dropDefaultRecvDir(std::wstring &folder);

/** Copy each top-level item from `staging_root` into `dest_folder` using the
 *  shell's copy engine. Never overwrites: a name collision gets a unique
 *  "name (n).ext". Returns per-item counts; a partial result is reported. */
bool dropCommitItems(const std::wstring &staging_root,
                     const std::wstring &dest_folder,
                     const std::vector<std::wstring> &top_level_names,
                     uint32_t &committed, uint32_t &failed,
                     std::string &message);

/** %LOCALAPPDATA%\\Mivara\\Drop (created on demand). */
std::wstring dropStagingBase();
/** %LOCALAPPDATA%\\Mivara\\Drop\\<transfer>-<tick>-<seq> (created). */
std::wstring dropStagingRoot(uint32_t transfer_id);

#endif /* MIVARA_DROPSHELL_H */
