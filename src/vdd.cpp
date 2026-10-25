/**
 * VDD.CPP - Windows virtual display (MttVDD) lifecycle management
 *
 * Deep-integrated lifecycle for the indirect-display driver
 * (VirtualDrivers/Virtual-Display-Driver, device id "MTT1337"):
 *   - vddStartup()     : enable the PnP device, activate the virtual monitor
 *                        (CCD SetDisplayConfig), place it right of the real
 *                        desktop, aim capture
 *   - vddShutdown()    : detach + disable the device once the session ends
 *   - vddForceDetach() : unconditional detach/disable (GUI safety net)
 *
 * Detaching via CCD alone is NOT enough: the monitor node stays visible in
 * Windows Settings > Display and can still be extended to. The Root\MttVDD
 * PnP device is therefore DISABLED after every session (SetupAPI,
 * DIF_PROPERTYCHANGE/DICS_DISABLE) so the virtual monitor disappears from
 * the system entirely, and re-enabled at the next session start.
 *
 * The virtual display is attached to the desktop only while a Mivara
 * extend-mode session runs. EVERY exit path must reach vddShutdown():
 * normal end, send failure, Ctrl+C, console-window close, GUI stop.
 * The GUI additionally calls vddForceDetach() if the sender had to be
 * hard-killed, so no path can leave the virtual monitor behind.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0601   /* QueryDisplayConfig / SetDisplayConfig */
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "vdd.h"

/* We adopt the virtual display for the duration of the session – whoever
 * happened to activate it (us, or a leftover from a crashed session) – so
 * it is ALWAYS detached again when the session ends. */
static std::atomic<bool> g_vdd_own{false};

/* Enable / disable the MttVDD virtual-display adapter via SetupAPI.
 * The PnP instance is ROOT\DISPLAY\000x (class Display, hardware ID
 * Root\MttVDD) – NOT "ROOT\MTTVDD\*" – so match on hardware IDs, which
 * also avoids touching other vendors' virtual display adapters
 * (Todesk/GameViewer/RayLink/...). Disabling takes the whole IddCx/UMDF
 * stack down, which removes the monitor from display enumeration
 * entirely (Settings > Display no longer lists it). Requires elevation –
 * on failure the caller falls back to CCD-only behaviour.
 * A disabled device has NO CCD path, so vddInstalled() reports false –
 * enable must run before that gate at session start. */
static bool vddHwIdMatch(HDEVINFO hs, SP_DEVINFO_DATA &dd)
{
    DWORD need = 0;
    SetupDiGetDeviceRegistryPropertyA(hs, &dd, SPDRP_HARDWAREID,
                                      nullptr, nullptr, 0, &need);
    if (need == 0) return false;
    std::vector<char> buf(need + 2, 0);
    if (!SetupDiGetDeviceRegistryPropertyA(hs, &dd, SPDRP_HARDWAREID,
                                           nullptr, (PBYTE)buf.data(),
                                           need, nullptr))
        return false;
    for (const char *p = buf.data(); *p; p += strlen(p) + 1) {
        if (_stricmp(p, "Root\\MttVDD") == 0 || _stricmp(p, "MttVDD") == 0)
            return true;
    }
    return false;
}

static bool vddEnableDevice(bool enable)
{
    HDEVINFO hs = SetupDiGetClassDevsA(nullptr, nullptr, nullptr,
                                       DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hs == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    for (DWORD i = 0; ; i++) {
        SP_DEVINFO_DATA dd = {};
        dd.cbSize = sizeof(dd);
        if (!SetupDiEnumDeviceInfo(hs, i, &dd)) break;
        if (!vddHwIdMatch(hs, dd)) continue;

        SP_PROPCHANGE_PARAMS pp = {};
        pp.ClassInstallHeader.cbSize          = sizeof(SP_CLASSINSTALL_HEADER);
        pp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pp.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
        pp.Scope       = DICS_FLAG_GLOBAL;
        ok = SetupDiSetClassInstallParamsA(hs, &dd,
                                           &pp.ClassInstallHeader, sizeof(pp))
          && SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hs, &dd);
        break;
    }
    SetupDiDestroyDeviceInfoList(hs);
    return ok;
}

static bool vddDevicePresent()
{
    HDEVINFO hs = SetupDiGetClassDevsA(nullptr, nullptr, nullptr,
                                       DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hs == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    for (DWORD i = 0; ; i++) {
        SP_DEVINFO_DATA dd = {};
        dd.cbSize = sizeof(dd);
        if (!SetupDiEnumDeviceInfo(hs, i, &dd)) break;
        if (vddHwIdMatch(hs, dd)) { found = true; break; }
    }
    SetupDiDestroyDeviceInfoList(hs);
    return found;
}

/* Poll pred() every 200 ms up to ms. Used to wait for the UMDF driver to
 * come up after enable and for the monitor to appear after CCD attach. */
static bool vddWaitFor(const std::function<bool()> &pred, int ms)
{
    for (int t = 0; t < ms; t += 200) {
        if (pred()) return true;
        Sleep(200);
    }
    return pred();
}

struct VddDisplayState {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

struct VddTarget {
    LUID adapter;
    UINT32 id;
};

static VddDisplayState g_vdd_base;
static bool g_vdd_restore_base = false;
static std::vector<VddTarget> g_vdd_targets;

static bool vddSameLuid(const LUID &a, const LUID &b)
{
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

static bool vddSameId(LUID a, UINT32 ai, LUID b, UINT32 bi)
{
    return vddSameLuid(a, b) && ai == bi;
}

static std::string vddWideLower(const WCHAR *s)
{
    std::string out;
    for (; s && *s; ++s)
        out += (char)tolower((unsigned char)*s);
    return out;
}

static std::string vddLowerText(const char *s)
{
    std::string out;
    for (; s && *s; ++s)
        out += (char)tolower((unsigned char)*s);
    return out;
}

static bool vddMttName(const std::string &name)
{
    return name == "vdd by mtt" || name == "mttvdd" || name == "mtt1337";
}

static bool vddQuery(UINT32 flags, VddDisplayState &state)
{
    flags |= QDC_VIRTUAL_MODE_AWARE;
    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(flags, &np, &nm) != ERROR_SUCCESS)
            return false;
        VddDisplayState next;
        next.paths.assign(np, DISPLAYCONFIG_PATH_INFO{});
        next.modes.assign(nm, DISPLAYCONFIG_MODE_INFO{});
        const LONG r = QueryDisplayConfig(
            flags, &np, next.paths.empty() ? nullptr : next.paths.data(),
            &nm, next.modes.empty() ? nullptr : next.modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;
        if (r != ERROR_SUCCESS) return false;
        next.paths.resize(np);
        next.modes.resize(nm);
        state = next;
        return true;
    }
    return false;
}

static bool vddTargetName(const DISPLAYCONFIG_PATH_INFO &p,
                          DISPLAYCONFIG_TARGET_DEVICE_NAME &tn)
{
    tn = {};
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = p.targetInfo.adapterId;
    tn.header.id = p.targetInfo.id;
    return DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS;
}

static bool vddIsMtt(const DISPLAYCONFIG_PATH_INFO &p)
{
    for (const VddTarget &t : g_vdd_targets) {
        if (vddSameId(t.adapter, t.id, p.targetInfo.adapterId, p.targetInfo.id))
            return true;
    }
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
    if (!vddTargetName(p, tn)) return false;
    const std::string path = vddWideLower(tn.monitorDevicePath);
    const std::string name = vddWideLower(tn.monitorFriendlyDeviceName);
    if (path.find("#mtt1337#") == std::string::npos && !vddMttName(name))
        return false;
    g_vdd_targets.push_back({p.targetInfo.adapterId, p.targetInfo.id});
    return true;
}

static UINT32 vddSourceModeIndex(const DISPLAYCONFIG_PATH_INFO &p)
{
    return (p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)
               ? p.sourceInfo.sourceModeInfoIdx : p.sourceInfo.modeInfoIdx;
}

static UINT32 vddTargetModeIndex(const DISPLAYCONFIG_PATH_INFO &p)
{
    return (p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)
               ? p.targetInfo.targetModeInfoIdx : p.targetInfo.modeInfoIdx;
}

static UINT32 vddDesktopModeIndex(const DISPLAYCONFIG_PATH_INFO &p)
{
    return (p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)
               ? p.targetInfo.desktopModeInfoIdx
               : DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
}

static void vddSetSourceModeIndex(DISPLAYCONFIG_PATH_INFO &p, UINT32 index)
{
    if (p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)
        p.sourceInfo.sourceModeInfoIdx =
            index >= DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID
                ? DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID : index;
    else
        p.sourceInfo.modeInfoIdx = index;
}

static void vddSetTargetModeIndex(DISPLAYCONFIG_PATH_INFO &p, UINT32 index)
{
    if (p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)
        p.targetInfo.targetModeInfoIdx =
            index >= DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID
                ? DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID : index;
    else
        p.targetInfo.modeInfoIdx = index;
}

static UINT32 vddCopyMode(UINT32 index,
                          const std::vector<DISPLAYCONFIG_MODE_INFO> &from,
                          std::vector<DISPLAYCONFIG_MODE_INFO> &to)
{
    if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || index >= from.size())
        return DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    to.push_back(from[index]);
    return (UINT32)to.size() - 1;
}

static void vddCompactModes(VddDisplayState &state,
                            const std::vector<DISPLAYCONFIG_MODE_INFO> &original)
{
    std::vector<DISPLAYCONFIG_MODE_INFO> compact;
    for (DISPLAYCONFIG_PATH_INFO &p : state.paths) {
        if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE)) {
            p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            continue;
        }
        const UINT32 si = vddSourceModeIndex(p);
        const UINT32 ti = vddTargetModeIndex(p);
        vddSetSourceModeIndex(p, vddCopyMode(si, original, compact));
        vddSetTargetModeIndex(p, vddCopyMode(ti, original, compact));
        if (p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
            const UINT32 di = p.targetInfo.desktopModeInfoIdx;
            if (di < original.size()) {
                p.targetInfo.desktopModeInfoIdx = (UINT32)compact.size();
                compact.push_back(original[di]);
            } else {
                p.targetInfo.desktopModeInfoIdx =
                    DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
            }
        }
    }
    state.modes = compact;
}

/* Iterate all CCD paths; run fn(path) for every path whose target monitor
 * is the VDD. fn returns true to keep scanning. Returns true if any VDD
 * path was seen. */
template <typename F>
static bool vddForEachPath(F fn)
{
    VddDisplayState state;
    if (!vddQuery(QDC_ALL_PATHS, state)) return false;
    bool seen = false;
    const UINT32 nPaths = (UINT32)state.paths.size();
    const UINT32 nModes = (UINT32)state.modes.size();
    for (DISPLAYCONFIG_PATH_INFO &p : state.paths) {
        if (!vddIsMtt(p)) continue;
        seen = true;
        if (!fn(p, nPaths, state.paths, nModes, state.modes)) break;
    }
    return seen;
}

bool vddInstalled()
{
    return vddForEachPath([](DISPLAYCONFIG_PATH_INFO &,
                             UINT32, std::vector<DISPLAYCONFIG_PATH_INFO> &,
                             UINT32, std::vector<DISPLAYCONFIG_MODE_INFO> &) {
        return true;
    });
}

static bool vddHasActivePath()
{
    bool active = false;
    vddForEachPath([&](DISPLAYCONFIG_PATH_INFO &p,
                       UINT32, std::vector<DISPLAYCONFIG_PATH_INFO> &,
                       UINT32, std::vector<DISPLAYCONFIG_MODE_INFO> &) {
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) {
            active = true;
            return false;
        }
        return true;
    });
    return active;
}

static bool vddCaptureNonMtt(VddDisplayState &out)
{
    VddDisplayState active;
    if (!vddQuery(QDC_ONLY_ACTIVE_PATHS, active)) return false;
    VddDisplayState kept;
    for (const DISPLAYCONFIG_PATH_INFO &p : active.paths) {
        if (!vddIsMtt(p)) kept.paths.push_back(p);
    }
    vddCompactModes(kept, active.modes);
    out = kept;
    return true;
}

static bool vddApplyState(VddDisplayState state, const char *what)
{
    if (state.paths.empty()) return true;
    const LONG r = SetDisplayConfig(
        (UINT32)state.paths.size(), state.paths.data(),
        (UINT32)state.modes.size(), state.modes.empty() ? nullptr : state.modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
        SDC_VIRTUAL_MODE_AWARE);
    if (r != ERROR_SUCCESS)
        std::cerr << "VDD: " << what << " failed (" << r << ")\n";
    return r == ERROR_SUCCESS;
}

static const DISPLAYCONFIG_MODE_INFO *vddModeAt(
    const VddDisplayState &state, UINT32 index)
{
    if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || index >= state.modes.size())
        return nullptr;
    return &state.modes[index];
}

static bool vddSameMode(const DISPLAYCONFIG_MODE_INFO *a,
                        const DISPLAYCONFIG_MODE_INFO *b)
{
    if (!a || !b) return a == b;
    if (a->infoType != b->infoType || a->id != b->id ||
        !vddSameLuid(a->adapterId, b->adapterId)) return false;
    size_t bytes = 0;
    if (a->infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
        bytes = sizeof(DISPLAYCONFIG_SOURCE_MODE);
    else if (a->infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
        bytes = sizeof(DISPLAYCONFIG_TARGET_MODE);
    else if (a->infoType == DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE)
        bytes = sizeof(DISPLAYCONFIG_DESKTOP_IMAGE_INFO);
    return bytes != 0 && std::memcmp(&a->sourceMode, &b->sourceMode, bytes) == 0;
}

static bool vddSamePath(const DISPLAYCONFIG_PATH_INFO &a,
                        const VddDisplayState &am,
                        const DISPLAYCONFIG_PATH_INFO &b,
                        const VddDisplayState &bm)
{
    if (!vddSameId(a.sourceInfo.adapterId, a.sourceInfo.id,
                   b.sourceInfo.adapterId, b.sourceInfo.id) ||
        !vddSameId(a.targetInfo.adapterId, a.targetInfo.id,
                   b.targetInfo.adapterId, b.targetInfo.id) ||
        a.targetInfo.rotation != b.targetInfo.rotation ||
        a.targetInfo.scaling != b.targetInfo.scaling)
        return false;
    if ((a.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) !=
        (b.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)) return false;
    if ((a.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) &&
        a.sourceInfo.cloneGroupId != b.sourceInfo.cloneGroupId) return false;
    return vddSameMode(vddModeAt(am, vddSourceModeIndex(a)),
                       vddModeAt(bm, vddSourceModeIndex(b))) &&
           vddSameMode(vddModeAt(am, vddTargetModeIndex(a)),
                       vddModeAt(bm, vddTargetModeIndex(b))) &&
           vddSameMode(vddModeAt(am, vddDesktopModeIndex(a)),
                       vddModeAt(bm, vddDesktopModeIndex(b)));
}

static bool vddStateMatches(const VddDisplayState &expected)
{
    VddDisplayState current;
    if (!vddCaptureNonMtt(current) ||
        current.paths.size() != expected.paths.size()) return false;
    for (const DISPLAYCONFIG_PATH_INFO &a : expected.paths) {
        bool found = false;
        for (const DISPLAYCONFIG_PATH_INFO &b : current.paths) {
            if (vddSamePath(a, expected, b, current)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool vddSourceUsed(const VddDisplayState &state,
                          const DISPLAYCONFIG_PATH_INFO &candidate)
{
    for (const DISPLAYCONFIG_PATH_INFO &p : state.paths) {
        if (vddSameId(candidate.sourceInfo.adapterId, candidate.sourceInfo.id,
                      p.sourceInfo.adapterId, p.sourceInfo.id)) return true;
    }
    return false;
}

static UINT32 vddNextCloneGroup(const VddDisplayState &state)
{
    for (UINT32 group = 0; group < DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID; ++group) {
        bool used = false;
        for (const DISPLAYCONFIG_PATH_INFO &p : state.paths) {
            if ((p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) &&
                p.sourceInfo.cloneGroupId == group) {
                used = true;
                break;
            }
        }
        if (!used) return group;
    }
    return DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;
}

static bool vddAttachIndependent()
{
    VddDisplayState keep, all;
    if (!vddCaptureNonMtt(keep) || !vddQuery(QDC_ALL_PATHS, all)) return false;
    DISPLAYCONFIG_PATH_INFO candidate = {};
    int best = -1;
    for (const DISPLAYCONFIG_PATH_INFO &p : all.paths) {
        if (!p.targetInfo.targetAvailable || !vddIsMtt(p) ||
            vddSourceUsed(keep, p)) continue;
        const int score =
            (vddSameLuid(p.sourceInfo.adapterId, p.targetInfo.adapterId) ? 2 : 0) +
            ((p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) ? 1 : 0);
        if (score > best) {
            best = score;
            candidate = p;
        }
    }
    if (best < 0) {
        std::cerr << "VDD: no independent MttVDD CCD source path is available\n";
        return false;
    }
    candidate.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    if (candidate.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
        const UINT32 group = vddNextCloneGroup(keep);
        if (group == DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID) return false;
        candidate.sourceInfo.cloneGroupId = group;
        candidate.sourceInfo.sourceModeInfoIdx =
            DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;
        candidate.targetInfo.targetModeInfoIdx =
            DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
        candidate.targetInfo.desktopModeInfoIdx =
            DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    } else {
        candidate.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        candidate.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }
    keep.paths.push_back(candidate);
    return vddApplyState(keep, "activate independent MttVDD");
}

static bool vddDetachMtt()
{
    if (!vddHasActivePath()) return true;
    VddDisplayState keep;
    if (!vddCaptureNonMtt(keep)) return false;
    return vddApplyState(keep, "detach MttVDD");
}

/* Resolve the actual active CCD source; an MTT monitor under a shared GDI
 * source is a clone, not an independent capture target. */
static bool vddFindActiveDevice(std::string &out)
{
    VddDisplayState state;
    if (!vddQuery(QDC_ONLY_ACTIVE_PATHS, state)) return false;
    for (UINT32 i = 0; i < state.paths.size(); ++i) {
        const DISPLAYCONFIG_PATH_INFO &p = state.paths[i];
        if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE) || !vddIsMtt(p)) continue;
        bool independent = true;
        for (UINT32 j = 0; j < state.paths.size() && independent; ++j) {
            if (i == j || !(state.paths[j].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
            const DISPLAYCONFIG_PATH_INFO &q = state.paths[j];
            if (vddSameId(p.sourceInfo.adapterId, p.sourceInfo.id,
                          q.sourceInfo.adapterId, q.sourceInfo.id)) {
                independent = false;
            } else if ((p.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) &&
                       (q.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) &&
                       p.sourceInfo.cloneGroupId != DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID &&
                       p.sourceInfo.cloneGroupId == q.sourceInfo.cloneGroupId) {
                independent = false;
            }
        }
        if (!independent) continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
        char device[CCHDEVICENAME] = {};
        if (!WideCharToMultiByte(CP_UTF8, 0, sn.viewGdiDeviceName, -1,
                                 device, sizeof(device), nullptr, nullptr)) continue;
        DISPLAY_DEVICEA dd = {};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(device, 0, &dd, 0) ||
            !(dd.StateFlags & DISPLAY_DEVICE_ACTIVE) ||
            !(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        out = device;
        return true;
    }
    return false;
}

/* Set resolution/refresh/position of a display via the classic CDS API.
 * IMPORTANT: use dynamic apply (flags = 0). CDS_UPDATEREGISTRY is rejected
 * by this IddCx virtual display (DISP_CHANGE_FAILED); the dynamic apply
 * works reliably right after attach. The mode is re-applied at every
 * session start, so persistence is not needed. */
static bool vddConfigure(const std::string &device, int x, int y,
                         int w, int h, int hz)
{
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExA(device.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
        return false;
    dm.dmFields           = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT |
                            DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    dm.dmBitsPerPel       = 32;
    dm.dmPosition.x       = x;
    dm.dmPosition.y       = y;
    dm.dmPelsWidth        = w;
    dm.dmPelsHeight       = h;
    dm.dmDisplayFrequency = hz;
    LONG r = ChangeDisplaySettingsExA(device.c_str(), &dm, NULL, 0, NULL);
    return r == DISP_CHANGE_SUCCESSFUL;
}

/* ── Driver mode list (vdd_settings.xml) ────────────────────────────────
 * The UMDF stack reads its mode list once at start-up; a size that is not
 * in the file simply cannot be applied. Extend mode can therefore append
 * the receiver's exact size (backed up first) and restart the device. */

static bool vddReadFile(const std::string &path, std::string &out)
{
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 ||
        sz.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != out.size()) { out.clear(); return false; }
    return true;
}

static bool vddWriteFile(const std::string &path, const std::string &data)
{
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    return ok && written == data.size();
}

static std::string vddLower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

/* Locate the <resolutions> element. Matching only inside it keeps the scan
 * out of comments or unrelated elements that happen to contain <width>. */
static bool vddResolutionsRegion(const std::string &xml, size_t &open,
                                 size_t &close)
{
    const std::string low = vddLower(xml);
    open  = low.find("<resolutions>");
    close = low.rfind("</resolutions>");
    return open != std::string::npos && close != std::string::npos &&
           close > open;
}

/* Does the <resolutions> element contain this exact width/height pair? */
static bool vddXmlHasMode(const std::string &xml, size_t open, size_t close,
                          int w, int h)
{
    const std::string low = vddLower(xml);
    size_t pos = open;
    while ((pos = low.find("<resolution>", pos)) != std::string::npos &&
           pos < close) {
        size_t end = low.find("</resolution>", pos);
        if (end == std::string::npos || end > close) break;
        size_t wp = low.find("<width>", pos);
        size_t hp = low.find("<height>", pos);
        if (wp != std::string::npos && hp != std::string::npos &&
            wp < end && hp < end) {
            long mw = strtol(xml.c_str() + wp + 7, nullptr, 10);
            long mh = strtol(xml.c_str() + hp + 8, nullptr, 10);
            if (mw == w && mh == h) return true;
        }
        pos = end + 13;                 /* strlen("</resolution>") */
    }
    return false;
}

/* Serialise writers of the driver settings file (two Mivara instances may
 * start an extend session at the same time). Session-local name: both
 * processes run in the interactive session. */
struct VddSettingsLock
{
    HANDLE h = nullptr;
    bool held = false;
    VddSettingsLock()
    {
        h = CreateMutexA(nullptr, FALSE, "MivaraVddSettings");
        if (!h) return;
        DWORD r = WaitForSingleObject(h, 10000);
        held = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
    }
    ~VddSettingsLock()
    {
        if (held) ReleaseMutex(h);
        if (h) CloseHandle(h);
    }
};

/* Atomic in-place replace through a same-directory temp file. ReplaceFile
 * keeps the destination's ACL/attributes (important for a System32 file);
 * a unique temp name keeps concurrent writers from clobbering each other. */
static bool vddReplaceWith(const std::string &path, const std::string &data,
                           std::string &err)
{
    const std::string tmp = path + ".mivara." +
                            std::to_string((unsigned long)GetCurrentProcessId()) +
                            ".tmp";
    if (!vddWriteFile(tmp, data)) {
        DeleteFileA(tmp.c_str());
        err = "cannot write " + tmp + " (not elevated?)";
        return false;
    }
    if (!ReplaceFileA(path.c_str(), tmp.c_str(), nullptr,
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        const DWORD e = GetLastError();
        DeleteFileA(tmp.c_str());
        err = "cannot replace " + path + " (error " +
              std::to_string((int)e) + ")";
        return false;
    }
    return true;
}

static bool vddRegString(HKEY root, const char *sub, const char *name,
                         std::string &out)
{
    char buf[MAX_PATH * 2] = {};
    DWORD cb = sizeof(buf), type = 0;
    if (RegGetValueA(root, sub, name, RRF_RT_REG_SZ, &type,
                     buf, &cb) != ERROR_SUCCESS)
        return false;
    if (!buf[0]) return false;
    out = buf;
    return true;
}

/* The vendor driver can relocate its settings file via HKLM. Support both
 * the file and directory forms; fall back to the shipped location. */
static std::string vddSettingsPath()
{
    static const char *kKeys[] = {
        "SOFTWARE\\MikeTheTech\\VirtualDisplayDriver",
        "SOFTWARE\\WOW6432Node\\MikeTheTech\\VirtualDisplayDriver",
    };
    static const char *kValues[] = { "SettingsPath", "VDDPATH", "VddSettingsPath" };
    for (const char *key : kKeys) {
        for (const char *name : kValues) {
            std::string v;
            if (!vddRegString(HKEY_LOCAL_MACHINE, key, name, v)) continue;
            /* Only absolute drive paths: a relative value would resolve
             * against whatever CWD the elevated sender happens to have. */
            const bool absolute = v.size() >= 3 && v[1] == ':' &&
                                  (v[2] == '\\' || v[2] == '/');
            if (!absolute) continue;
            if (v.size() > 4 && vddLower(v.substr(v.size() - 4)) == ".xml")
                return v;
            while (!v.empty() && (v.back() == '\\' || v.back() == '/'))
                v.pop_back();
            return v + "\\vdd_settings.xml";
        }
    }
    char sys[MAX_PATH] = {};
    UINT n = GetSystemDirectoryA(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    return std::string(sys) + "\\drivers\\UMDF\\vdd_settings.xml";
}

VddModeResult vddEnsureMode(int w, int h, bool allowWrite, std::string &err)
{
    err.clear();
    /* Dimensions arrive over the LAN. Require even, sane, in-bounds values:
     * odd sizes break the encoder's even-dimension path, and a bogus peer
     * must not be able to add a 1x1 or 7680x4320 mode to a system file. */
    if (w < 320 || w > 7680 || h < 240 || h > 4320 ||
        (w & 1) || (h & 1)) {
        err = "invalid resolution";
        return VDD_MODE_ABSENT;
    }
    const std::string path = vddSettingsPath();
    if (path.empty()) {
        err = "vdd_settings.xml path not found";
        return VDD_MODE_ABSENT;
    }

    VddSettingsLock lock;
    if (!lock.held) {
        err = "another Mivara instance is updating " + path;
        return VDD_MODE_ABSENT;
    }

    std::string xml;
    if (!vddReadFile(path, xml)) {
        err = "cannot read " + path;
        return VDD_MODE_ABSENT;
    }
    if (xml.size() >= 2 &&
        (((unsigned char)xml[0] == 0xFF && (unsigned char)xml[1] == 0xFE) ||
         ((unsigned char)xml[0] == 0xFE && (unsigned char)xml[1] == 0xFF))) {
        err = "UTF-16 vdd_settings.xml is not supported";
        return VDD_MODE_ABSENT;
    }

    size_t open = 0, close = 0;
    if (!vddResolutionsRegion(xml, open, close)) {
        err = "unexpected vdd_settings.xml layout";
        return VDD_MODE_ABSENT;
    }
    if (vddXmlHasMode(xml, open, close, w, h)) return VDD_MODE_PRESENT;
    if (!allowWrite) {
        err = "mode not listed; adding disabled";
        return VDD_MODE_ABSENT;
    }
    /* Never touch the file (or restart the device) under a live VDD: a
     * leftover session may still be showing it. A mode added now would not
     * be loaded anyway; it becomes available at the next session. */
    if (vddHasActivePath()) {
        err = "virtual display is active; not modifying settings";
        return VDD_MODE_ABSENT;
    }

    const char *nl = xml.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    /* Insert right after the <resolutions> open tag: always inside the
     * element, whatever the driver's formatting (a minified file used to
     * send the block before the XML declaration). Numeric values only –
     * the size comes from the wire and is range-checked above. */
    std::string block;
    block += nl;
    block += "        <resolution>";
    block += nl; block += "            <width>" + std::to_string(w) + "</width>";
    block += nl; block += "            <height>" + std::to_string(h) + "</height>";
    block += nl; block += "            <refresh_rate>60</refresh_rate>";
    block += nl; block += "        </resolution>";
    block += nl;

    const size_t at = open + strlen("<resolutions>");
    const std::string updated =
        xml.substr(0, at) + block + xml.substr(at);

    /* Structural sanity before touching the real file. */
    size_t uo = 0, uc = 0;
    if (updated.find("</vdd_settings>") == std::string::npos ||
        !vddResolutionsRegion(updated, uo, uc) ||
        !vddXmlHasMode(updated, uo, uc, w, h)) {
        err = "insertion would corrupt " + path + "; not written";
        return VDD_MODE_ABSENT;
    }

    /* Keep the pristine original once, for manual recovery. Rollback below
     * uses the in-memory pre-write copy, so a backup left over from an older
     * driver install can never clobber a newer file. */
    const std::string bak = path + ".mivara.bak";
    if (GetFileAttributesA(bak.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !CopyFileA(path.c_str(), bak.c_str(), TRUE)) {
        err = "cannot back up " + path;
        return VDD_MODE_ABSENT;
    }

    if (!vddReplaceWith(path, updated, err))
        return VDD_MODE_ABSENT;

    std::string verify;
    size_t vo = 0, vc = 0;
    if (!vddReadFile(path, verify) ||
        !vddResolutionsRegion(verify, vo, vc) ||
        !vddXmlHasMode(verify, vo, vc, w, h)) {
        std::string rerr;
        if (!vddReplaceWith(path, xml, rerr))
            std::cerr << "VDD: rollback failed: " << rerr << "\n";
        err = "verification failed; original restored";
        return VDD_MODE_ABSENT;
    }

    std::cout << "Added " << w << "x" << h << " to " << path
              << " (backup: " << bak << ")\n";
    return VDD_MODE_ADDED;
}

/* Enumerate the modes the driver currently exposes for this device and pick
 * the closest aspect ratio (area proximity breaks ties). Used when the
 * requested size could not be added to the settings file. */
static bool vddPickClosestMode(const std::string &dev, int wantW, int wantH,
                               int &outW, int &outH)
{
    int bestW = 0, bestH = 0;
    double best = 1e300;
    const double wantAR   = (double)wantW / (double)wantH;
    const double wantArea = (double)wantW * (double)wantH;

    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; i < 4096; i++) {
        if (!EnumDisplaySettingsA(dev.c_str(), i, &dm)) break;
        const int mw = (int)dm.dmPelsWidth, mh = (int)dm.dmPelsHeight;
        if (mw < 1 || mh < 1) continue;
        if (mw == wantW && mh == wantH) { outW = mw; outH = mh; return true; }
        const double ar = (double)mw / (double)mh;
        const double score =
            std::fabs(std::log(ar / wantAR)) * 10.0 +
            std::fabs(std::log(((double)mw * mh) / wantArea));
        if (score < best) { best = score; bestW = mw; bestH = mh; }
    }
    if (!bestW) return false;
    outW = bestW;
    outH = bestH;
    return true;
}

static bool vddModeAvailable(const std::string &dev, int w, int h)
{
    int mw = 0, mh = 0;
    return vddPickClosestMode(dev, w, h, mw, mh) && mw == w && mh == h;
}

/* Is this GDI display device backed by the virtual display driver? */
static bool isVddMonitorDevice(const char *gdiDevice)
{
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEA mon = {};
        mon.cb = sizeof(mon);
        if (!EnumDisplayDevicesA(gdiDevice, i, &mon, EDD_GET_DEVICE_INTERFACE_NAME))
            break;
        if (vddLowerText(mon.DeviceID).find("mtt1337") != std::string::npos ||
            vddMttName(vddLowerText(mon.DeviceString))) return true;
    }
    return false;
}

/* Rightmost desktop edge EXCLUDING virtual-display monitors, so the VDD
 * can be placed right after the real desktop (not after itself). */
struct EdgeScanCtx { int right; };
static BOOL CALLBACK edgeScanProc(HMONITOR hmon, HDC, LPRECT rc, LPARAM lp)
{
    auto *ctx = (EdgeScanCtx *)lp;
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(hmon, (MONITORINFO *)&mi) &&
        !isVddMonitorDevice(mi.szDevice)) {
        if (rc->right > ctx->right) ctx->right = rc->right;
    }
    return TRUE;
}

static int desktopRightEdge()
{
    EdgeScanCtx ctx = {0};
    EnumDisplayMonitors(NULL, NULL, edgeScanProc, (LPARAM)&ctx);
    return ctx.right;
}

/* Locate a monitor rect in the virtual desktop by GDI device name. */
struct MonRectSearch { const char *dev; RECT rc; bool found; };
static BOOL CALLBACK findMonRectProc(HMONITOR hmon, HDC, LPRECT, LPARAM lp)
{
    auto *s = (MonRectSearch *)lp;
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(hmon, (MONITORINFO *)&mi) &&
        _stricmp(mi.szDevice, s->dev) == 0) {
        s->rc    = mi.rcMonitor;
        s->found = true;
        return FALSE;
    }
    return TRUE;
}

bool vddFindMonitorRect(const std::string &dev, int &x, int &y, int &w, int &h)
{
    MonRectSearch s = {};
    s.dev = dev.c_str();
    EnumDisplayMonitors(NULL, NULL, findMonRectProc, (LPARAM)&s);
    if (!s.found) return false;
    x = s.rc.left;
    y = s.rc.top;
    w = s.rc.right  - s.rc.left;
    h = s.rc.bottom - s.rc.top;
    return true;
}

/* Full startup: enable device, attach VDD, place it right of the desktop,
 * report the rect. w/h = requested virtual display resolution. */
bool vddStartup(int w, int h, VddInfo &out, bool reloadFirst)
{
    if (w < 1 || w > 7680) w = 1920;
    if (h < 1 || h > 4320) h = 1080;
    g_vdd_base = VddDisplayState{};
    g_vdd_restore_base = false;
    if (!vddCaptureNonMtt(g_vdd_base)) {
        std::cerr << "Cannot save the current display topology; MttVDD was not enabled.\n";
        return false;
    }
    g_vdd_restore_base = true;

    /* The driver reads its mode list once at start-up. Never restart it while
     * a VDD path is active. */
    if (reloadFirst && !vddHasActivePath()) {
        std::cout << "Restarting virtual display driver to load the new mode...\n";
        vddEnableDevice(false);
        vddWaitFor([] { return !vddInstalled(); }, 4000);
    } else if (reloadFirst) {
        std::cout << "Virtual display is in use; the new mode will load at "
                     "the next session.\n";
    }

    const bool enabled = vddEnableDevice(true);
    const bool present = vddDevicePresent();
    if (!enabled && !present) {
        g_vdd_base = VddDisplayState{};
        g_vdd_restore_base = false;
        std::cerr << "Virtual display driver is not installed on the sender.\n"
                     "  Reinstall Mivara with the virtual display driver task enabled,\n"
                     "  or run vdd\\install_vdd.ps1 from an administrator PowerShell.\n";
        return false;
    }
    g_vdd_own = true;
    if (!enabled)
        std::cerr << "MttVDD is installed, but Windows could not enable it.\n"
                     "  Check the Root\\MttVDD device status in Device Manager.\n";
    if (!vddWaitFor(vddInstalled, 8000)) {
        std::cerr << "MttVDD is installed, but no virtual monitor was exposed.\n"
                     "  Check the device for a driver error, review vdd\\install_log.txt,\n"
                     "  or reinstall the virtual display driver.\n";
        vddShutdown();
        return false;
    }

    /* Enabling an IddCx device can automatically clone it with physical
     * targets. Remove that path, restore the saved non-MTT layout, then add
     * MttVDD through an independently sourced CCD path. */
    if (vddHasActivePath() && !vddDetachMtt()) {
        std::cerr << "Failed to remove the automatic MttVDD clone\n";
        vddShutdown();
        return false;
    }
    if (!vddApplyState(g_vdd_base, "restore the pre-enable display layout")) {
        vddShutdown();
        return false;
    }

    std::cout << "Activating independent virtual display...\n";
    if (!vddAttachIndependent()) {
        std::cerr << "Failed to activate an independent virtual display\n";
        vddShutdown();
        return false;
    }

    std::string dev;
    if (!vddWaitFor([&] { return vddFindActiveDevice(dev); }, 5000)) {
        std::cerr << "Virtual display did not come up as an independent MTT source\n";
        vddShutdown();
        return false;
    }
    if (!vddStateMatches(g_vdd_base)) {
        std::cerr << "Activating MttVDD changed the existing display layout\n";
        vddShutdown();
        return false;
    }
    const int x = desktopRightEdge();

    /* A freshly (re)loaded driver may take a moment to refresh its modes. */
    if (!vddModeAvailable(dev, w, h)) {
        for (int t = 0; t < 3 && !vddModeAvailable(dev, w, h); t++)
            Sleep(300);
    }
    if (!vddModeAvailable(dev, w, h)) {
        int aw = 0, ah = 0;
        if (vddPickClosestMode(dev, w, h, aw, ah)) {
            std::cerr << "Mode " << w << "x" << h
                      << " is not exposed by the driver - using closest "
                      << aw << "x" << ah << "\n";
            w = aw;
            h = ah;
        }
    }

    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        ok = vddConfigure(dev, x, 0, w, h, 60);
        if (!ok) Sleep(600);
    }
    if (!ok) {
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            ok = vddConfigure(dev, x, 0, w, h, 30);
            if (!ok) Sleep(600);
        }
    }
    if (!ok && (w != 1920 || h != 1080)) {
        std::cerr << "Requested mode " << w << "x" << h
                  << " rejected – falling back to 1920x1080\n";
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            ok = vddConfigure(dev, x, 0, 1920, 1080, 60);
            if (!ok) Sleep(600);
        }
    }
    if (!ok)
        std::cerr << "Mode change not applied – checking the current mode and position\n";
    Sleep(800);

    VddInfo info;
    info.device = dev;
    if (!vddFindMonitorRect(dev, info.x, info.y, info.w, info.h)) {
        std::cerr << "Virtual display rect not found\n";
        vddShutdown();
        return false;
    }
    bool positioned = info.x == x && info.y == 0;
    for (int attempt = 0; attempt < 4 && !positioned; ++attempt) {
        vddConfigure(dev, x, 0, info.w, info.h, attempt == 3 ? 30 : 60);
        Sleep(300);
        if (vddFindMonitorRect(dev, info.x, info.y, info.w, info.h))
            positioned = info.x == x && info.y == 0;
    }
    if (!positioned) {
        std::cerr << "Virtual display position could not be applied\n";
        vddShutdown();
        return false;
    }
    std::string active;
    if (!vddFindActiveDevice(active) || active != dev) {
        std::cerr << "Virtual display became unavailable or shared during mode setup\n";
        vddShutdown();
        return false;
    }
    if (!vddStateMatches(g_vdd_base)) {
        std::cerr << "Mode setup changed the existing display layout\n";
        vddShutdown();
        return false;
    }

    out = info;
    g_vdd_base = VddDisplayState{};
    g_vdd_restore_base = false;
    std::cout << "Virtual display ready: " << dev << "  "
              << info.w << "x" << info.h
              << " @(" << info.x << "," << info.y << ")\n";
    return true;
}

/* Tear down: detach the virtual display AND disable the Root\MttVDD device
 * (so it disappears from Settings > Display) if this session adopted it.
 * Idempotent; safe from any exit path (incl. the console-close handler). */
void vddShutdown()
{
    if (!g_vdd_own.exchange(false)) return;
    VddDisplayState restore;
    bool haveRestore = false;
    if (g_vdd_restore_base) {
        restore = g_vdd_base;
        haveRestore = true;
    } else {
        haveRestore = vddCaptureNonMtt(restore);
    }
    std::cout << "Deactivating virtual display...\n";
    if (!vddDetachMtt())
        std::cerr << "Warning: failed to detach virtual display\n";
    Sleep(300);
    if (!vddEnableDevice(false))
        std::cerr << "Warning: failed to disable MttVDD device "
                     "(not elevated?) – it may still appear in Settings\n";
    if (haveRestore && !vddStateMatches(restore) &&
        !vddApplyState(restore, "restore the non-MttVDD display layout")) {
        std::cerr << "Warning: failed to restore the non-MttVDD display layout\n";
    }
    g_vdd_base = VddDisplayState{};
    g_vdd_restore_base = false;
}

/* Unconditional detach + device disable – GUI safety net after a hard-killed
 * sender, and for cleaning up leftovers. Clears ownership so vddShutdown()
 * stays a no-op afterwards. Return value = CCD detach succeeded. */
bool vddForceDetach()
{
    g_vdd_own = false;
    g_vdd_base = VddDisplayState{};
    g_vdd_restore_base = false;
    VddDisplayState restore;
    const bool haveRestore = vddCaptureNonMtt(restore);
    const bool ok = vddDetachMtt();
    Sleep(300);
    if (!vddEnableDevice(false))
        std::cerr << "Warning: failed to disable MttVDD device "
                     "(not elevated?) – it may still appear in Settings\n";
    if (haveRestore && !vddStateMatches(restore) &&
        !vddApplyState(restore, "restore the non-MttVDD display layout")) {
        std::cerr << "Warning: failed to restore the non-MttVDD display layout\n";
    }
    return ok;
}

#endif /* _WIN32 */
