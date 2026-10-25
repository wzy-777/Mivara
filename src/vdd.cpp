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

/* Match a CCD target whose friendly monitor name identifies the VDD
 * ("VDD by MTT" / "MTT1337" family). Real monitors never match. */
static bool vddNameMatch(const WCHAR *wname)
{
    char buf[128];
    int  n = 0;
    for (; wname && wname[n] && n < 127; n++)
        buf[n] = (char)wname[n];
    buf[n] = '\0';
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(buf, "vdd") || strstr(buf, "mtt");
}

/* Iterate all CCD paths; run fn(path) for every path whose target monitor
 * is the VDD. fn returns true to keep scanning. Returns true if any VDD
 * path was seen. */
template <typename F>
static bool vddForEachPath(F fn)
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &nPaths, &nModes) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &nPaths, paths.data(),
                           &nModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    bool seen = false;
    for (UINT32 i = 0; i < nPaths; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
        tn.header.type       = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size       = sizeof(tn);
        tn.header.adapterId  = paths[i].targetInfo.adapterId;
        tn.header.id         = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS)
            continue;
        if (!vddNameMatch(tn.monitorFriendlyDeviceName))
            continue;
        seen = true;
        if (!fn(paths[i], nPaths, paths, nModes, modes))
            break;
    }
    return seen;
}

/* Does the VDD driver expose a monitor at all (active or not)? */
bool vddInstalled()
{
    return vddForEachPath([](DISPLAYCONFIG_PATH_INFO &,
                             UINT32, std::vector<DISPLAYCONFIG_PATH_INFO> &,
                             UINT32, std::vector<DISPLAYCONFIG_MODE_INFO> &) {
        return true;
    });
}

/* Is a VDD path ACTIVE, i.e. attached to the desktop right now? Writing the
 * settings file or restarting the device in that state would yank a display
 * a live session may be showing, so both are deferred until no path is
 * active (the next session after a clean shutdown). */
static bool vddHasActivePath()
{
    bool active = false;
    vddForEachPath([&](DISPLAYCONFIG_PATH_INFO &p,
                       UINT32, std::vector<DISPLAYCONFIG_PATH_INFO> &,
                       UINT32, std::vector<DISPLAYCONFIG_MODE_INFO> &) {
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) {
            active = true;
            return false;             /* stop scanning */
        }
        return true;
    });
    return active;
}

/* Activate / deactivate every VDD path via the CCD topology. */
static bool vddApplyTopology(bool active)
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &nPaths, &nModes) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &nPaths, paths.data(),
                           &nModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    bool touched = false;
    for (UINT32 i = 0; i < nPaths; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
        tn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size      = sizeof(tn);
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
        if (!vddNameMatch(tn.monitorFriendlyDeviceName))            continue;

        if (active) paths[i].flags |=  DISPLAYCONFIG_PATH_ACTIVE;
        else        paths[i].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
        touched = true;
    }
    if (!touched) {
        std::cerr << "VDD: no CCD paths matched the MTT/VDD monitor name\n";
        return false;
    }

    LONG st = SetDisplayConfig(nPaths, paths.data(), nModes, modes.data(),
                               SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY);
    if (st != ERROR_SUCCESS)
        std::cerr << "VDD: SetDisplayConfig failed (" << st << ")\n";
    return st == ERROR_SUCCESS;
}

/* Find the GDI device name (\\.\DISPLAYn) of the *active* VDD monitor. */
static bool vddFindActiveDevice(std::string &out)
{
    for (DWORD i = 0; ; i++) {
        DISPLAY_DEVICEA dd = {};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE) ||
            !(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        DISPLAY_DEVICEA mon = {};
        mon.cb = sizeof(mon);
        if (EnumDisplayDevicesA(dd.DeviceName, 0, &mon, 0) &&
            (strstr(mon.DeviceID, "MTT") || strstr(mon.DeviceString, "VDD"))) {
            out = dd.DeviceName;
            return true;
        }
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
    DISPLAY_DEVICEA mon = {};
    mon.cb = sizeof(mon);
    if (EnumDisplayDevicesA(gdiDevice, 0, &mon, 0))
        return strstr(mon.DeviceID, "MTT")   ||
               strstr(mon.DeviceID, "VDD")   ||
               strstr(mon.DeviceString, "VDD");
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

    /* vddEnsureMode() appended a mode to vdd_settings.xml while the driver
     * may still be loaded with the old list (leftover from a crashed
     * session). One bounded disable/enable cycle makes the UMDF stack
     * re-read the file; disabling an already-disabled device is a no-op and
     * the re-enable below is what performs the reload. Never restart while
     * a VDD path is active – that would yank a display a live session owns. */
    if (reloadFirst && !vddHasActivePath()) {
        std::cout << "Restarting virtual display driver to load the new mode...\n";
        vddEnableDevice(false);
        vddWaitFor([] { return !vddInstalled(); }, 4000);
    } else if (reloadFirst) {
        std::cout << "Virtual display is in use; the new mode will load at "
                     "the next session.\n";
    }

    /* Re-enable the Root\MttVDD device first: a disabled device exposes no
     * CCD path, so vddInstalled() would wrongly report "not installed".
     * No-op when the device is already enabled (leftover / first run). */
    const bool enabled = vddEnableDevice(true);
    if (!enabled && !vddDevicePresent()) {
        std::cerr << "Virtual display driver is not installed on the sender.\n"
                     "  Reinstall Mivara with the virtual display driver task enabled,\n"
                     "  or run vdd\\install_vdd.ps1 from an administrator PowerShell.\n";
        return false;
    }
    if (!enabled)
        std::cerr << "MttVDD is installed, but Windows could not enable it.\n"
                     "  Check the Root\\MttVDD device status in Device Manager.\n";
    if (!vddWaitFor(vddInstalled, 8000)) {
        std::cerr << "MttVDD is installed, but no virtual monitor was exposed.\n"
                     "  Check the device for a driver error, review vdd\\install_log.txt,\n"
                     "  or reinstall the virtual display driver.\n";
        return false;
    }

    /* Adopt the VDD for this session even if it is already active (e.g. a
     * leftover from a crashed/killed session): the session end must ALWAYS
     * detach it again, otherwise the virtual monitor sticks around forever. */
    g_vdd_own = true;

    bool alreadyActive = false;
    vddForEachPath([&](DISPLAYCONFIG_PATH_INFO &p, UINT32,
                       std::vector<DISPLAYCONFIG_PATH_INFO> &,
                       UINT32, std::vector<DISPLAYCONFIG_MODE_INFO> &) {
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) alreadyActive = true;
        return !alreadyActive;   /* stop early once found */
    });

    /* Compute the anchor BEFORE attaching: right edge of the real desktop
     * (VDD excluded), so the virtual display lands right after it. */
    int x = desktopRightEdge();

    if (!alreadyActive) {
        std::cout << "Activating virtual display...\n";
        if (!vddApplyTopology(true)) {
            std::cerr << "Failed to activate virtual display\n";
            vddShutdown();
            return false;
        }
        /* Let the topology settle: poll until the GDI device shows up. */
        vddWaitFor([] {
            std::string d;
            return vddFindActiveDevice(d);
        }, 5000);
        Sleep(300);
    }

    std::string dev;
    if (!vddFindActiveDevice(dev)) {
        std::cerr << "Virtual display did not come up (no active MTT device)\n";
        vddShutdown();
        return false;
    }

    /* If the requested size is not in the driver's mode list (the XML could
     * not be extended), switch to the closest advertised mode. Scaling a
     * slightly different size uniformly looks far better than a rejected
     * mode, and the receiver aspect-fits whatever we send. */
    /* A freshly (re)loaded driver may take a moment to refresh the mode list
     * exposed through EnumDisplaySettings; polling briefly avoids a needless
     * downgrade to a different size. */
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

    /* Deterministic placement: requested mode @60 right of the real desktop.
     * A freshly attached display may reject mode changes for a moment,
     * so retry a few times before falling back. */
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        ok = vddConfigure(dev, x, 0, w, h, 60);
        if (!ok) Sleep(600);
    }
    /* Some driver lists use 30 Hz for a mode while the global list adds 60;
     * a 60 Hz rejection is not necessarily a rejected size. */
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
        std::cerr << "Mode change not applied – streaming current mode\n";
    Sleep(800);

    VddInfo info;
    info.device = dev;
    if (!vddFindMonitorRect(dev, info.x, info.y, info.w, info.h)) {
        std::cerr << "Virtual display rect not found\n";
        vddShutdown();
        return false;
    }

    out = info;
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
    std::cout << "Deactivating virtual display...\n";
    if (!vddApplyTopology(false))
        std::cerr << "Warning: failed to detach virtual display\n";
    Sleep(300);                   /* let the topology settle */
    if (!vddEnableDevice(false))
        std::cerr << "Warning: failed to disable MttVDD device "
                     "(not elevated?) – it may still appear in Settings\n";
}

/* Unconditional detach + device disable – GUI safety net after a hard-killed
 * sender, and for cleaning up leftovers. Clears ownership so vddShutdown()
 * stays a no-op afterwards. Return value = CCD detach succeeded. */
bool vddForceDetach()
{
    g_vdd_own = false;
    bool ok = vddApplyTopology(false);
    Sleep(300);
    if (!vddEnableDevice(false))
        std::cerr << "Warning: failed to disable MttVDD device "
                     "(not elevated?) – it may still appear in Settings\n";
    return ok;
}

#endif /* _WIN32 */
