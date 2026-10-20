/**
 * UPDATE.H – Mivara in-app update check + one-click updater (GUI app only)
 *
 * Check: one HTTPS GET to the fixed path releases/latest; the new version is
 * read from the 302 Location header. api.github.com is NOT used on purpose –
 * it is rate limited (60/h per IP unauthenticated) and would fail behind
 * shared/CGNAT addresses; the web redirect has no such limit.
 *
 * Windows update flow (POSIX offers check + browser handoff only):
 *   Available -> download Mivara-Setup.exe over HTTPS into a locked-down
 *   %ProgramData%\Mivara\Updates directory, verify size + SHA-256 against the
 *   release's SHA256SUMS.txt (fail closed: no sums file => no in-app install),
 *   re-verify the hash on disk right before launching the Inno Setup installer.
 *
 * Threading: one worker thread owned by this module. The worker never touches
 * SDL, the log ring or the config; the UI polls updateTake() once per frame.
 */
#ifndef MIVARA_UPDATE_H
#define MIVARA_UPDATE_H

#include <string>

enum class UpdateState {
    Idle,         /* nothing checked yet */
    Checking,     /* worker is running the check */
    UpToDate,     /* remote <= local */
    Available,    /* a newer release exists */
    Downloading,  /* worker is downloading the installer */
    Downloaded,   /* installer on disk + verified; ready to launch */
    Failed,       /* check/download/launch failed; 'detail' holds a token */
};

struct UpdateInfo {
    UpdateState state = UpdateState::Idle;
    bool        is_download = false;  /* completion came from the download stage */
    std::string remote_tag;           /* e.g. "v0.9.2" (Available and later) */
    std::string version;              /* e.g. "0.9.2" (Available and later) */
    std::string install_path;         /* absolute installer path (Downloaded) */
    std::string sha256;               /* expected installer hash (Downloaded) */
    std::string detail;               /* failure token (Failed / cancel) */
};

/* Chinese text for a failure token ("" -> generic). */
std::string updateErrorText(const std::string &detail);

/* True when this build can download+install in-app (Windows only). */
bool updateDownloadSupported();

/* ── main thread API ─────────────────────────────────────────────────────── */
bool updateCheckStart();      /* false if a check/download is already running */
bool updateDownloadStart();   /* false if not Available or already running   */
void updateDownloadCancel();  /* abort an in-flight download (worker cleans up) */
bool updateTake(UpdateInfo &out);  /* true exactly once per finished operation */
UpdateInfo updateGetInfo();        /* cheap snapshot, safe from any thread */
int  updateProgress();             /* 0..100 while Downloading, else -1 */
void updateJoin();                 /* abort + join the worker; idempotent */
void updateStartupSweep();         /* delete stale installers from earlier runs */

/* ── actions ─────────────────────────────────────────────────────────────── */
bool updateOpenReleasePage();   /* open the constant release URL in the browser */
bool updateLaunchInstaller();   /* launch the verified installer (Windows) */

/* ── pure helpers (also exercised by the -DUPDATE_TEST_MAIN CLI) ─────────── */
bool updateParseReleaseLocation(const std::string &location, std::string &tag_out);
bool updateVersionGreater(const std::string &remote_tag, const std::string &current);
bool updateParseSha256Sums(const std::string &text, const std::string &filename,
                           std::string &hex_out);
bool updateFileSha256(const std::string &path, std::string &hex_out);

#endif
