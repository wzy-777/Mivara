/**
 * VDD.H - Windows virtual display (MttVDD) lifecycle management
 *
 * Owns the full attach/detach story for the indirect-display driver used
 * by extend mode:
 *   vddStartup()     attach + place + size the virtual monitor, adopt it
 *   vddShutdown()    detach it again (idempotent, any process exit path)
 *   vddForceDetach() unconditional detach – safety net for the GUI after
 *                    a hard-killed sender, and for leftover cleanup
 *
 * Windows only; every symbol is a no-op / absent on other platforms.
 */
#pragma once

#ifdef _WIN32

#include <string>

struct VddInfo {
    std::string device;   /* GDI device name (\\.\DISPLAYn) of the virtual monitor */
    int x = 0, y = 0;     /* position on the virtual desktop */
    int w = 0, h = 0;     /* its current resolution */
};

/* Is the virtual display driver present (monitor exposed at all)? */
bool vddInstalled();

/* Attach the virtual monitor through an independent source without changing
 * the existing display layout, place it right of the real desktop at
 * reqW x reqH @60Hz, and adopt ownership so vddShutdown() will detach it when
 * the session ends. If the driver does not expose the
 * requested size, the closest advertised mode is used instead.
 * reloadFirst restarts an already-loaded driver once (needed after
 * vddEnsureMode() appended a mode to vdd_settings.xml).
 * Returns false on failure (and detaches anything we attached). */
bool vddStartup(int reqW, int reqH, VddInfo &out, bool reloadFirst = false);

/* Result of making sure the driver's settings file lists a mode. */
enum VddModeResult {
    VDD_MODE_PRESENT,   /* exact w x h already listed */
    VDD_MODE_ADDED,     /* appended now; the driver must be restarted */
    VDD_MODE_ABSENT,    /* not listed (writing disabled or not possible) */
};

/* Ensure w x h is in the driver's vdd_settings.xml. With allowWrite=false
 * the file is never modified. The original file is backed up once as
 * vdd_settings.xml.mivara.bak before the first write. err is set when the
 * mode cannot be made available. Windows only. */
VddModeResult vddEnsureMode(int w, int h, bool allowWrite, std::string &err);

/* Detach the virtual monitor if this process adopted it. Idempotent and
 * safe to call from any exit path (normal return, Ctrl+C cleanup,
 * console-close handler, atexit). */
void vddShutdown();

/* Detach every virtual-monitor path regardless of ownership. Used by the
 * GUI as a last-resort clean-up when the sender had to be hard-killed. */
bool vddForceDetach();

/* Refresh the virtual-desktop rect of an active monitor by GDI device name.
 * Used to follow live resolution/position changes mid-stream. */
bool vddFindMonitorRect(const std::string &dev, int &x, int &y, int &w, int &h);

#endif /* _WIN32 */
