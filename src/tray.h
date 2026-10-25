/**
 * TRAY.H - Windows notification-area icon + single instance
 *
 * The GUI's close button must not end a running session: on Windows it hides
 * the window to the notification area and the app keeps serving. Every call
 * here is a no-op on other platforms (`trayInit` returns false), where closing
 * the window keeps its old meaning.
 *
 * The tray icon lives on its own thread with a message-only window; the GUI
 * thread polls `trayTakeRequests()` once per frame, so no tray callback ever
 * touches SDL or the app's state directly.
 *
 * Icon availability and IPC are independent. When the shell rejects the icon
 * (NIM_ADD fails) `trayInit` returns false, but the message-only window and
 * the single-instance wake-up channel stay alive; callers must keep polling
 * `trayTakeRequests()` unconditionally and still call `trayShutdown()` on
 * every exit path.
 */
#ifndef MIVARA_TRAY_H
#define MIVARA_TRAY_H

#include <string>

/** Claim the single-instance mutex.
 *  Returns true when this process is the only instance (it also starts the
 *  wake-up channel). When another instance is already running, that instance
 *  is asked to show its window and false is returned. */
bool traySingleInstance();
/** Release the mutex and stop the wake-up thread. Safe to call when the claim
 *  failed, and safe to call more than once. */
void traySingleInstanceRelease();

/** Create the icon + its message window. `tooltip` is the initial text.
 *  Returns false when the platform has no tray or the shell rejected the icon.
 *  That only means "no icon": the wake-up channel and the request polling may
 *  still be live, so callers must not treat false as "tray dead". */
bool trayInit(const std::string &tooltip);

/** Replace the tooltip. Cheap enough for every frame; the caller is expected
 *  to skip unchanged text. Safe from any thread. */
void traySetTooltip(const std::string &tooltip);

/** Show a balloon notification (first-hide hint, approval request, error).
 *  The newest pending notification wins. Safe from any thread. */
void trayNotify(const std::string &title, const std::string &text);

struct TrayRequests {
    bool show = false;   /* icon click / 显示主界面 */
    bool quit = false;   /* 退出 */
};

/** Take the requests raised since the previous call (and clear them).
 *  Must be polled unconditionally, even when `trayInit` returned false. */
TrayRequests trayTakeRequests();

/** Remove the icon and stop the tray + wake-up threads. Idempotent; call on
 *  every exit path, before `traySingleInstanceRelease()`. */
void trayShutdown();

#endif /* MIVARA_TRAY_H */
