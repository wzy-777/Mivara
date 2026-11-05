/**
 * FILEXFER.H - independent, authorized two-way file transfer on TCP 8086.
 *
 * Unlike the 8085 stitch drop companion, this channel does not need a video or
 * stitch session. The app owns a background client; receiver owns the listener.
 * One authenticated connection supports directory listing plus single-file
 * upload/download. Paths on the wire are length-prefixed UTF-8, never STAT or
 * command-line tokens. File data is plaintext; authorization is admission only.
 *
 * The receive-side file_transfer_accept switch is a separate opt-in. The normal
 * open/password/manual policy is still enforced with a new auth purpose (3).
 * The service has its own single-client slot and does not claim the 8081/8084
 * cross-mode lease, so file transfer can coexist with a display session.
 */
#ifndef MIVARA_FILEXFER_H
#define MIVARA_FILEXFER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "auth.h"

/* Overridable only for local loopback diagnostics; production builds use
 * 8086. The service and the default client port both follow this macro. */
#ifndef FILEXFER_PORT
#define FILEXFER_PORT              8086u
#endif
#define FILEXFER_MAGIC             0x4D564631u  /* 'MVF1' */
/* v2: LIST entries carry flags (directory + hidden) and a modification time;
 * the service sends one FTF_INFO frame with its device name after admission;
 * FTF_MKDIR creates one directory in a pinned parent. Both ends must be the
 * same build. */
#define FILEXFER_VERSION           2u
#define FILEXFER_CHUNK_BYTES       (64u * 1024u)
#define FILEXFER_MAX_PATH_BYTES    4096u
/* A Windows component limit is 255 UTF-16 units, which is at most 765 UTF-8
 * bytes for BMP characters; 1024 keeps every locally valid name representable
 * on the wire while still bounding a frame entry. */
#define FILEXFER_MAX_NAME_BYTES    1024u
#define FILEXFER_MAX_ENTRIES       1000u
#define FILEXFER_MAX_FRAME_BYTES   (4u * 1024u * 1024u)
#define FILEXFER_MAX_FILE_BYTES    (64ull * 1024ull * 1024ull * 1024ull)
/* Entry timestamps are unix seconds; 9999-12-31T23:59:59Z is the largest
 * value the UI can format, and a bound keeps sign/overflow bugs out. */
#define FILEXFER_MAX_MTIME         253402300799ull

enum FtStatus : uint32_t {
    FT_OK = 0,
    FT_ERR_VERSION = 1,
    FT_ERR_AUTH = 2,
    FT_ERR_BUSY = 3,
    FT_ERR_DISABLED = 4,
    FT_ERR_LIMIT = 5,
    FT_ERR_PATH = 6,
    FT_ERR_IO = 7,
    FT_ERR_CANCELLED = 8,
    FT_ERR_UNSUPPORTED = 9,
    FT_ERR_NOT_FOUND = 10,
    FT_ERR_DISK = 11,
    FT_ERR_EXISTS = 12,
    FT_ERR_PROTOCOL = 13
};

struct FtEntry {
    std::string name;             /* one UTF-8 path component, never a path */
    uint64_t size = 0;            /* regular files; zero for directories */
    bool directory = false;
    bool hidden = false;          /* display hint only; never an ACL */
    uint64_t mtime = 0;           /* unix seconds; 0 = unknown */
};

struct FtListing {
    std::string path;             /* canonical/display absolute UTF-8 path;
                                   * empty only for the roots pseudo-listing */
    std::vector<FtEntry> entries; /* directories first, then files */
    bool truncated = false;       /* FILEXFER_MAX_ENTRIES was reached */
};

/* Local browsing helpers used by the SDL page. All paths are UTF-8. Empty path
 * means the roots pseudo-listing (drive letters on Windows, / on POSIX).
 * Directory enumeration skips symlinks/reparse points and non-file/non-dir
 * entries. These functions perform filesystem I/O and must not be called from
 * code that cannot tolerate a short local-disk stall. */
bool ftLocalHome(std::string &path, std::string &error);
bool ftLocalList(const std::string &path, FtListing &out, std::string &error);
/* Create exactly one directory component under dir. Fails rather than
 * overwriting; the created absolute path is returned in path. */
bool ftLocalMkdir(const std::string &dir, const std::string &name,
                  std::string &path, std::string &error);
/* This machine's display name (hostname), sanitised for the wire. */
std::string ftLocalDeviceName();
std::string ftPathParent(const std::string &path);
std::string ftPathJoin(const std::string &dir, const std::string &name);
std::string ftPathName(const std::string &path);

/* ── receiver service ────────────────────────────────────────────────────── */
enum FtApprovalResult : int {
    FT_APPROVAL_ALLOW = 1,
    FT_APPROVAL_DENY = 0,
    FT_APPROVAL_TIMEOUT = -1,
    FT_APPROVAL_CANCELLED = -2,
    FT_APPROVAL_FAILED = -3,
    FT_APPROVAL_BUSY = -4
};

struct FtTargetAuth {
    /* Snapshot the live receive policy. False fails closed as manual. */
    bool (*policy)(MivaraAuthMode *mode, uint8_t salt[16], uint8_t hash[32],
                   uint32_t *iterations, bool *record_ok, void *ctx) = nullptr;
    /* Uses the receiver's normal manual approval path with req=files. */
    FtApprovalResult (*approve)(const char *ip, const char *device,
                                intptr_t client, void *ctx) = nullptr;
    /* Separate permission for remote browse/read/write. Checked before every
     * request as well as during admission, so RX|reload takes effect live. */
    std::atomic<int> enabled{0};
    void *ctx = nullptr;
};

struct FtStartupStatus {
    int state = 0;                /* 0 starting, 1 listening, -1 failed */
    int stage = 0;                /* 0 none, 1 socket, 3 bind, 4 listen */
    int error_code = 0;
    uint16_t port = FILEXFER_PORT;
};

FtStartupStatus ftServiceStatus();
void ftServiceRun(std::atomic<bool> &running, const FtTargetAuth *auth);
void ftSetStatSink(void (*sink)(const char *line));

/* ── app client ──────────────────────────────────────────────────────────── */
enum FtClientState : int {
    FT_CLIENT_DISCONNECTED = 0,
    FT_CLIENT_CONNECTING,
    FT_CLIENT_NEED_PASSWORD,
    FT_CLIENT_WAIT_AUTH,
    FT_CLIENT_VERIFYING,
    FT_CLIENT_READY,
    FT_CLIENT_ERROR
};

enum FtEventType : int {
    FT_EV_STATE = 0,              /* state + optional message */
    FT_EV_LIST,                   /* status + listing or message */
    FT_EV_PROGRESS,               /* upload + name + bytes/total */
    FT_EV_COMPLETE,               /* upload + status + bytes + message/path */
    FT_EV_NOTICE,                 /* non-fatal status + message */
    FT_EV_INFO                    /* device name announced after admission */
};

struct FtEvent {
    FtEventType type = FT_EV_STATE;
    FtClientState state = FT_CLIENT_DISCONNECTED;
    FtStatus status = FT_OK;
    FtListing listing;
    bool upload = false;
    bool roots = false;           /* FT_EV_LIST: roots pseudo-listing */
    bool mkdir = false;           /* FT_EV_COMPLETE came from ftClientMkdir */
    std::string name;
    std::string device;           /* FT_EV_INFO: remote display name */
    uint64_t bytes = 0;
    uint64_t total = 0;
    std::string message;
};

/* Start/stop the single process-wide app client. Start is asynchronous.
 * ip must be a numeric IPv4/IPv6 literal (hostnames are rejected before any
 * worker or DNS lookup). The service itself listens on IPv4 only. Stop closes
 * the socket, releases a pending password wait and joins the worker. It is
 * safe to call Stop when no client exists. */
bool ftClientStart(const std::string &ip, uint16_t port = FILEXFER_PORT);
void ftClientStop();
bool ftClientActive();
bool ftClientBusy();

/* Queue one operation. Empty list path means roots; ListHome selects the
 * remote user's home. Upload/Download handle one regular file, never a dir.
 * They return false instead of queueing when no client is running or another
 * operation is active. */
bool ftClientListHome();
bool ftClientList(const std::string &path);
/* List drive letters / filesystem roots. The reply carries roots=true. */
bool ftClientRoots();
bool ftClientUpload(const std::string &local_file,
                    const std::string &remote_dir);
bool ftClientDownload(const std::string &remote_file,
                      const std::string &local_dir);
/* Create one directory under remote_dir. Completion arrives as FT_EV_COMPLETE
 * with mkdir=true; the created path is in message on success. */
bool ftClientMkdir(const std::string &remote_dir, const std::string &name);

/* Cancel closes the current connection (including a blocked transfer) and
 * leaves the page disconnected so it can explicitly reconnect. */
void ftClientCancel();

/* Abort only the operation currently running. An upload is cancelled with an
 * ABORT frame and the connection stays usable; a download cannot be stopped
 * mid-stream safely, so it falls back to closing the connection. No effect
 * when the client is idle. */
void ftClientCancelOperation();

/* GUI thread drains events. Poll never blocks. Progress events are coalesced
 * and the queue is bounded. */
bool ftClientPoll(FtEvent &out);

/* Answer a pending FT_CLIENT_NEED_PASSWORD event. Empty password cancels.
 * The implementation wipes its temporary copy after deriving the proof. */
void ftClientPassword(const std::string &password);

/* Stable, short UI text for a protocol status. Never contains secrets. */
const char *ftStatusText(FtStatus status);

#endif /* MIVARA_FILEXFER_H */
