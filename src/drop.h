/**
 * DROP.H – 拼接 (stitch) file-drop companion channel on TCP 8085.
 *
 * This is the v1 "copy into the folder under the release point" feature. It
 * is deliberately separate from the 16-byte KVM event stream on 8084:
 *
 *   8084  keyboard/mouse only, plus a one-shot capability bit and a 32-byte
 *         per-session token sent as fixed KvmEvent frames after KvmReply.
 *   8085  file transfer only. The controller connects with the token after
 *         the user releases a dragged selection; the target validates the
 *         token against the active stitch session, receives into a private
 *         staging directory, verifies SHA-256, and copies the top-level
 *         items into the Explorer/Desktop folder under the release point.
 *
 * Copy-only by design: no move, no delete, no overwrite. The source is opened
 * read-only and is never modified. A failed transfer never deletes or changes
 * the source. Per-top-level-item commit is not atomic: a partial commit
 * reports the per-item counts and may leave a failed item's partial data in
 * the destination (the status is then an error, not "done").
 *
 * Windows only (like stitch itself). Non-Windows builds get no-op stubs.
 */
#ifndef MIVARA_DROP_H
#define MIVARA_DROP_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#define DROP_PORT              8085u
#define DROP_MAGIC             0x4D564431u   /* 'MVD1' */
#define DROP_VERSION           1u
#define DROP_TOKEN_BYTES       32
#define KVM_FEATURE_FILE_DROP  2u            /* KvmHello.reserved[1] bit */

/* v1 limits. The receiver enforces the same numbers; the sender checks them
 * before opening the socket so the user gets an immediate reason. */
#define DROP_MAX_ITEMS         1000u
#define DROP_MAX_TOTAL_BYTES   (2ull * 1024ull * 1024ull * 1024ull)
#define DROP_MAX_FILE_BYTES    (2ull * 1024ull * 1024ull * 1024ull)
#define DROP_MAX_PATH_BYTES    1024u
#define DROP_MAX_DEPTH         64u

enum DropStatus : uint32_t {
    DROP_OK             = 0,
    DROP_ERR_VERSION    = 1,
    DROP_ERR_AUTH       = 2,
    DROP_ERR_BUSY       = 3,
    DROP_ERR_DISABLED   = 4,
    DROP_ERR_LIMIT      = 5,
    DROP_ERR_PATH       = 6,
    DROP_ERR_IO         = 7,
    DROP_ERR_CANCELLED  = 8,
    DROP_ERR_UNSUPPORTED = 9,
    DROP_ERR_NO_TARGET  = 10,
    DROP_ERR_DISK       = 11
};

/** Real 8085 listener state, same shape as KvmStartupStatus. state: 0 =
 *  starting, 1 = listening, -1 = failed, 2 = not supported. */
struct DropStartupStatus {
    int      state;
    int      stage;        /* 0 none, 1 socket, 3 bind, 4 listen */
    int      error_code;
    uint16_t port;
};
DropStartupStatus dropServiceStatus();

/** Target (receiver): listen on DROP_PORT until *running clears. The token
 *  gate is installed by the 8084 session thread; without a gate every
 *  connection is refused (DROP_ERR_AUTH). */
void dropServiceRun(std::atomic<bool> &running);

/** Target: install/clear the per-session token. `epoch` is the 8084 session
 *  lease epoch so an ending session cannot clear a newer one. */
bool dropGateInstall(const uint8_t token[DROP_TOKEN_BYTES], uint64_t epoch);
void dropGateClear(uint64_t epoch);
bool dropGateCheck(const uint8_t token[DROP_TOKEN_BYTES]);

/** Install the STAT sink (receiver/sender stdout). The sink is called from
 *  worker threads; implementations must serialise their own writes. */
void dropSetStatSink(void (*sink)(const char *line));

struct DropOptions {
    const char *peer_ip = nullptr;
    uint16_t    port = (uint16_t)DROP_PORT;
    int         x = 0;             /* release point, target screen pixels */
    int         y = 0;
    uint32_t    transfer_id = 0;
    uint64_t    max_bytes = DROP_MAX_TOTAL_BYTES;
    uint32_t    max_items = DROP_MAX_ITEMS;
};

struct DropResult {
    uint32_t    status = DROP_ERR_IO;
    uint32_t    committed = 0;
    uint32_t    failed = 0;
    bool        fallback = false;  /* destination was the default recv dir */
    std::string message;
};

/** Controller: copy `paths` (absolute local files/directories, UTF-8) to the
 *  target at opt.x/opt.y. Blocks until the transfer and commit finish. */
DropResult dropSendFiles(const DropOptions &opt,
                         const std::vector<std::string> &paths,
                         const uint8_t token[DROP_TOKEN_BYTES],
                         const std::atomic<bool> &running,
                         void (*progress)(uint64_t sent, uint64_t total,
                                          void *ctx) = nullptr,
                         void *progress_ctx = nullptr);

/* ── source-side drag capture helper (controller only) ─────────────────────
 * A tiny asInvoker process hosts a 1-pixel OLE drop target on the shared
 * edge. It must run at Medium integrity, otherwise UIPI blocks Explorer's
 * (Medium) drop. The controller launches it with the shell token. It reports
 * the dragged CF_HDROP paths over a named pipe; it never copies anything
 * locally. */

/** Start the helper and wait (up to 3 s) for its READY + integrity level.
 *  Returns false when helper.exe is missing, cannot be launched at Medium, or
 *  does not answer; drag-to-send is then unavailable but the session works. */
bool dropHelperStart(int side, std::string &err);
void dropHelperStop();
/** True when the helper is connected and running at Medium or lower. */
bool dropHelperReady();
/** Take the most recently captured drag selection (UTF-8 paths). Clears it. */
bool dropHelperTakeFiles(std::vector<std::string> &out);
/** Drop a stale captured selection (called on a new local button-down). */
void dropHelperDiscard();
/** Move the helper's 1-pixel strip to another edge. */
void dropHelperSetSide(int side);
/** Target-side: let the Shell resolver/committer notice receiver shutdown
 *  between items (the shell copy call itself is not interruptible). */
void dropSetRunFlag(const std::atomic<bool> *running);

#endif /* MIVARA_DROP_H */
