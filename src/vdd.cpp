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
 * The virtual display is attached to the desktop only while a TwinView
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
#include <cstdint>
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
    if (!touched) return false;

    LONG st = SetDisplayConfig(nPaths, paths.data(), nModes, modes.data(),
                               SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY);
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
bool vddStartup(int w, int h, VddInfo &out)
{
    /* Re-enable the Root\MttVDD device first: a disabled device exposes no
     * CCD path, so vddInstalled() would wrongly report "not installed".
     * No-op when the device is already enabled (leftover / first run). */
    if (!vddEnableDevice(true))
        std::cerr << "Warning: could not enable MttVDD device (not elevated?)\n";
    if (!vddWaitFor(vddInstalled, 8000)) {
        std::cerr << "Virtual display driver not found.\n"
                     "  Install VirtualDrivers/Virtual-Display-Driver (MttVDD)\n"
                     "  first – see README.md.\n";
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

    /* Deterministic placement: requested mode @60 right of the real desktop.
     * A freshly attached display may reject mode changes for a moment,
     * so retry a few times before falling back. */
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        ok = vddConfigure(dev, x, 0, w, h, 60);
        if (!ok) Sleep(600);
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
