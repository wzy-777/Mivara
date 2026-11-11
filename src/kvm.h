/**
 * KVM.H – 拼接 (stitch) input channel: wire protocol and service entry points.
 *
 * 拼接 is NOT a display mode. Both machines keep their own physical monitor
 * and desktop; only the keyboard and mouse travel. The controller side
 * (bin/sender --mode stitch) watches its local input with low-level hooks +
 * Raw Input and forwards events over TCP 8084. The target side (bin/receiver)
 * injects them with SendInput. No capture, no encoder, no virtual display.
 *
 * Wire format (ALL uint32 in network byte order, little structs packed):
 *
 *   controller → target : KvmHello  (16 bytes)
 *   target → controller : KvmReply  (32 bytes, includes the target's
 *                         virtual-desktop metrics and the verdict)
 *   controller → target : KvmEvent  (16 bytes, repeated)
 *
 * After a KVM_FEATURE_HEARTBEAT negotiation, PING/PONG events travel in both
 * directions (KVM_HEARTBEAT_INTERVAL_MS, with a KVM_HEARTBEAT_TIMEOUT_MS
 * receive deadline). Older peers get no PING. The controller automatically
 * reconnects after an established session loses its socket.
 *
 * Authorization (protocol v2, PRD: 接收与授权)
 * -------------------------------------------
 * 8084 is an externally reachable service, so it obeys the SAME receive
 * policy as 8081 — there is no separate consent dialog that could be friendlier
 * than the configured policy:
 *
 *   open      : KvmReply(KVM_OK + metrics) immediately, as in v1.
 *   password  : HsAuthStart → HsRequestExtV3 → HsAuthProof → HsResponseV2
 *   manual    : HsAuthStart → HsRequestExtV3 → (local approval) → HsResponseV2
 *
 * On HS_OK both modes are followed by the usual KvmReply with the metrics. A
 * password policy whose stored record is missing/corrupt is refused before the
 * challenge with HsResponseV2(HS_ERR_AUTH_CONFIG) — it never downgrades to
 * open or to a prompt. The same hold applies to the session slot: 8084 shares
 * the process-wide single-session slot with the 8081 video handshake (see
 * auth.h), so while either mode holds a session or a handshake claim the other
 * answers HsResponseV2(HS_ERR_BUSY) instead of asking for a password or
 * opening a prompt. The proof binds this connection's hello and identity:
 *
 *   key   = PBKDF2(fold(pw), salt, iter)   // fold: ASCII A–Z -> a–z only
 *   pkey  = HMAC-SHA256(key, "Mivara/v1/auth-key" || 2)
 *   proof = HMAC-SHA256(pkey, "Mivara/v1/auth-proof" || 2 || nonce ||
 *                       KvmHello || HsRequestExtV3)
 *
 * The controller reads the first four bytes of the target's answer to tell a
 * bare KvmReply (KVM_MAGIC) from an authorization challenge (TVHA_MAGIC) or an
 * immediate HsResponseV2 refusal (TVHS_MAGIC). v1 peers fail the version check
 * with KVM_ERR_VERSION, so an old client cannot reach the event stream without
 * satisfying the current policy.
 *
 * Which edge hands control over is a per-session choice: the peer can sit to
 * the left, right, above or below this machine. The side is decided by the
 * controller (the GUI asks before connecting) and is carried in the hello's
 * first reserved word, ENCODED AS (KvmSide + 1), only so the target's approval
 * dialog can name the right edge. 0 therefore means "not specified" — a
 * zero-filled hello from an older build is not mistaken for KVM_SIDE_LEFT and
 * falls back to the default right side. Mid-session switches are purely
 * controller-side state: the target just injects whatever absolute
 * coordinates it is sent.
 *
 * Under the manual policy the target asks its user before answering; the
 * challenge is still sent first so the controller can present its device
 * name. A denied request gets HsResponseV2(HS_ERR_DENIED), an unanswered one
 * HS_ERR_AUTH_TIMEOUT, and the connection is closed.
 *
 * Windows only. On other platforms kvmRunController() reports an error and
 * kvmServiceRun() returns immediately, so the rest of the program is
 * unaffected.
 */
#ifndef MIVARA_KVM_H
#define MIVARA_KVM_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "auth.h"

/* Dedicated TCP port for the stitch input channel. It is deliberately not
 * the video port (a stitch session never performs the video handshake), but
 * the split is wire-level only: 8081 and 8084 share one process-wide session
 * slot, so a stitch session and a video session are mutually exclusive and
 * the second connection is refused as busy. */
#define KVM_PORT    8084
#define KVM_MAGIC   0x4D564B31u   /* 'MVK1' on the wire */
#define KVM_VERSION 2u
#define KVM_FEATURE_HEARTBEAT 1u  /* KvmHello.reserved[1] / KvmReply.reserved */
/* Heartbeat cadence. A stitch link is silent whenever the user is idle, so
 * liveness has to be probed. Keep the deadline at three missed beats: a link
 * change reports a socket error immediately, so a longer deadline would only
 * make the target hold injected keys and its single-session slot longer. */
#define KVM_HEARTBEAT_INTERVAL_MS 3000u
#define KVM_HEARTBEAT_TIMEOUT_MS  9000u
/* Reverse (target -> controller) drag copy. The controller advertises it when
 * its own file_drop_accept=1 (it can RECEIVE); the target echoes it when its
 * own file_drop_send=1 (it can SEND) and the 8085 listener is up. */
#define KVM_FEATURE_REVERSE_DROP 4u
/* Drag-ghost icon payload, visual only. Both peers understand the icon frames
 * below; the bit is echoed in KvmReply so neither side sends them to a build
 * that would disconnect on an unknown event type. Never security-relevant. */
#define KVM_FEATURE_DRAG_ICON    8u
/* Source Explorer's real drag image ("DragImageBits"), visual only. A peer
 * that understands KVM_EV_DRAG_IMAGE_* echoes this bit; it supersedes the
 * reconstructed icon card on the receiving screen. */
#define KVM_FEATURE_DRAG_IMAGE   16u

/* KvmReply.status */
enum KvmStatus : uint32_t {
    KVM_OK          = 0,
    KVM_ERR_VERSION = 1,      /* protocol version mismatch            */
    KVM_ERR_DENIED  = 2,      /* the target's user refused the request */
    KVM_ERR_BUSY    = 3       /* another controller is already active  */
};

/* KvmEvent.type */
enum KvmEventType : uint32_t {
    KVM_EV_MOVE        = 1,   /* a = x, b = y (target virtual-desktop px)  */
    KVM_EV_BUTTON      = 2,   /* a = 1 left 2 right 3 middle 4/5 X, b = down */
    KVM_EV_WHEEL       = 3,   /* a = signed delta, b = 1 horizontal        */
    KVM_EV_KEY         = 4,   /* a = down, b = vk, c = scan | ext << 16    */
    KVM_EV_RELEASE_ALL = 5,   /* release every key/button held on the target */
    KVM_EV_PING        = 6,   /* negotiated heartbeat; empty payload         */
    KVM_EV_PONG        = 7,   /* reply to PING; never sent to older peers    */
    /* Target → controller, only when the controller set
     * KVM_FEATURE_FILE_DROP in KvmHello.reserved[1] and the target accepted:
     * one marker (a='MVKX', b=version, c=token word count = 8) followed by
     * eight frames whose b holds one 32-bit token word. The controller reads
     * this fixed sequence before it starts drainLoop(), so no variable-length
     * frame ever enters the 16-byte event stream. */
    KVM_EV_DROP_INFO   = 8,
    /* Target → controller, only with KVM_FEATURE_REVERSE_DROP: the target's
     * edge helper announced (a=1) or cancelled (a=0) a reverse file drag.
     * The controller suppresses back-crossing while a=1 so the target's OLE
     * drag can finish on release, then hands control back. */
    KVM_EV_RDRAG       = 9,
    /* Controller → target, visual only. The name frames need no negotiation
     * (an older target ignores unknown types); the optional icon frames below
     * are gated by KVM_FEATURE_DRAG_ICON. Shows a small drag ghost on the
     * target screen while a file drag is being carried over:
     *   DRAG_INFO a=1 (begin, b=item count, c=DRAG_TEXT frames that follow)
     *   DRAG_INFO a=0 (end)
     *   DRAG_TEXT carries 12 raw UTF-8 bytes per frame in a/b/c (byte order
     *   round-trips untouched through htonl/ntohl). */
    KVM_EV_DRAG_INFO   = 10,
    KVM_EV_DRAG_TEXT   = 11,
    /* Drag-ghost image, visual only and only when KVM_FEATURE_DRAG_ICON was
     * negotiated. Sent in the same direction as DRAG_INFO/TEXT (controller →
     * target for a forward drag, target → controller while carrying a reverse
     * drag back):
     *   DRAG_ICON_INFO a = width, b = height, c = number of DATA frames
     *   DRAG_ICON_DATA 12 raw bytes per frame in a/b/c, exactly like DRAG_TEXT
     * Pixels are tightly packed, top-down, non-premultiplied BGRA (the same
     * order as a Windows 32bpp DIB). The receiver accepts at most 2,048 per
     * dimension and 36,864 pixels (a cap shared with the real drag image) and
     * ignores an incomplete sequence; the ghost then keeps the page glyph.
     * Older peers never set the feature bit, so they never see these frames. */
    KVM_EV_DRAG_ICON_INFO = 12,
    KVM_EV_DRAG_ICON_DATA = 13,
    /* The SOURCE's real shell drag image, only when KVM_FEATURE_DRAG_IMAGE is
     * negotiated. Same direction and frame convention as the icon frames:
     *   DRAG_IMAGE_INFO a = width | (offset_x << 16),
     *                   b = height | (offset_y << 16), c = DATA frames
     *   DRAG_IMAGE_DATA 12 raw premultiplied BGRA bytes per frame
     * The image is top-down; the receiver draws its top-left at
     * (cursor - offset), exactly like IDropTargetHelper. The source caps it at
     * DROP_DRAG_IMAGE_MAX_PIXELS and downsizes larger images first. A complete
     * image replaces the reconstructed icon card; an incomplete one is
     * ignored (the card stays). */
    KVM_EV_DRAG_IMAGE_INFO = 14,
    KVM_EV_DRAG_IMAGE_DATA = 15
};

/* Where the peer sits relative to the controller. KVM_SIDE_RIGHT is the
 * original (and default) layout: push past the local right edge, the remote
 * cursor enters at the target's left edge. The values are stable — an older
 * target that ignores KvmHello.reserved[0] behaves exactly like the default. */
enum KvmSide : uint32_t {
    KVM_SIDE_LEFT   = 0,
    KVM_SIDE_RIGHT  = 1,
    KVM_SIDE_TOP    = 2,
    KVM_SIDE_BOTTOM = 3
};

#pragma pack(push, 1)
struct KvmHello {
    uint32_t magic;           /* KVM_MAGIC                                  */
    uint32_t version;         /* KVM_VERSION                                */
    uint32_t reserved[2];     /* [0] = peer side + 1 (0 = unspecified);
                               * [1] = KVM_FEATURE_* capability bits         */
};

struct KvmReply {
    uint32_t magic;           /* KVM_MAGIC                                  */
    uint32_t version;         /* KVM_VERSION                                */
    uint32_t status;          /* KvmStatus                                  */
    int32_t  screen_x;        /* virtual-desktop origin (physical pixels)   */
    int32_t  screen_y;
    uint32_t screen_w;        /* virtual-desktop size (physical pixels)     */
    uint32_t screen_h;
    uint32_t reserved;
};

struct KvmEvent {
    uint32_t type;            /* KvmEventType                               */
    uint32_t a;
    uint32_t b;
    uint32_t c;
};
#pragma pack(pop)

static_assert(sizeof(KvmHello) == 16, "KvmHello must be 16 bytes");
static_assert(sizeof(KvmReply) == 32, "KvmReply must be 32 bytes");
static_assert(sizeof(KvmEvent) == 16, "KvmEvent must be 16 bytes");

/* ── authorization hooks ───────────────────────────────────────────────────
 * kvm.cpp stays free of file/GUI policy: the receiver hands its live 接收策略
 * and approval machinery in, the sender hands its password prompt in. */

/** Target-side hooks, supplied by bin/receiver. */
/** Manual-approval verdict. Only KVM_APPROVAL_DENY is a human refusal and may
 *  arm the 60 s per-source cooldown; CANCELLED/FAILED must release the slot
 *  immediately and must never be counted as a refusal. */
enum KvmApprovalResult : int {
    KVM_APPROVAL_ALLOW     =  1,
    KVM_APPROVAL_DENY      =  0,
    KVM_APPROVAL_TIMEOUT   = -1,
    KVM_APPROVAL_CANCELLED = -2,   /* peer hung up / receiver is stopping */
    KVM_APPROVAL_FAILED    = -3,   /* no human was asked (prompt failed)   */
    KVM_APPROVAL_BUSY      = -4    /* another device's prompt is open      */
};

struct KvmTargetAuth {
    /** Snapshot the current policy. Return false to fail closed (the request
     *  is then handled as manual, never as open). */
    bool (*policy)(MivaraAuthMode *mode, uint8_t salt[16], uint8_t hash[32],
                   uint32_t *iterations, bool *record_ok, void *ctx);
    /** Manual approval, blocking for at most 30 s.
     *  `client` is the platform socket, so the waiter can notice a peer that
     *  hung up (present it as intptr_t; it is a SOCKET on Windows). */
    KvmApprovalResult (*approve)(const char *ip, const char *device,
                                 intptr_t client, void *ctx);
    /** 1 = offer the 8085 file-drop capability to an authorized controller.
     *  The token is still generated per session and the 8085 listener must be
     *  up; this only says the user allowed receiving files. */
    /* Written by the receiver's stdin/policy thread (RX|reload) and read by
     * the 8084 session thread. */
    std::atomic<int> accept_files{0};
    /** 1 = this machine may SEND files to the controller (reverse drag copy,
     *  from --file-drop / file_drop_send). Independent of accept_files. */
    std::atomic<int> file_send{0};
    void *ctx = nullptr;
};

/** Real startup result of the 8084 listener: state 0 = starting, 1 =
 *  listening, -1 = failed, 2 = not supported on this platform. */
struct KvmStartupStatus {
    int      state;
    int      stage;        /* 0 none, 1 socket, 3 bind, 4 listen */
    int      error_code;   /* native errno / WSA error */
    uint16_t port;
};
KvmStartupStatus kvmServiceStatus();

/** Controller-side hooks, supplied by bin/sender. Both are optional: with a
 *  password policy and no hook the request fails closed. */
struct KvmClientAuth {
    /** Ask the local user for the connection password. Returns false when the
     *  prompt was cancelled or timed out. `*out` is wiped by the caller. */
    bool (*password)(std::string *out, void *ctx);
    /** Progress line for the GUI. States match the 8081 exchange:
     *  "waiting" | "need_password" | "authenticating" | "granted" |
     *  "cancelled". Must not include the password. */
    void (*report)(const char *state, const char *extra, void *ctx);
    /** 1 = capture dragged files at the shared edge and offer the 8085 file
     *  drop to the target. The sender sets this from --file-drop. */
    int file_drop = 0;
    /** 1 = accept REVERSE drag copies (target -> this machine): advertise
     *  KVM_FEATURE_REVERSE_DROP and run the 8085 listener locally.
     *  The sender sets this from --accept-files / file_drop_accept. */
    int accept_files = 0;
    void *ctx = nullptr;
};

/** True on platforms where 拼接 is implemented (currently Windows only). */
inline bool kvmSupported()
{
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

/** Wire/CLI key for a side: "left" | "right" | "top" | "bottom".
 *  Out-of-range values read as "right" so a corrupt config never breaks the
 *  session — the caller that parses user input must use kvmSideFromKey(). */
inline const char *kvmSideKey(int side)
{
    switch (side) {
    case KVM_SIDE_LEFT:   return "left";
    case KVM_SIDE_TOP:    return "top";
    case KVM_SIDE_BOTTOM: return "bottom";
    default:              return "right";
    }
}

/** Parse "left" | "right" | "top" | "bottom" (exact, case-sensitive).
 *  Returns -1 for anything else. Inline because `app` includes this header
 *  but never links kvm.cpp. */
inline int kvmSideFromKey(const char *key)
{
    if (!key) return -1;
    if (std::strcmp(key, "left") == 0)   return KVM_SIDE_LEFT;
    if (std::strcmp(key, "right") == 0)  return KVM_SIDE_RIGHT;
    if (std::strcmp(key, "top") == 0)    return KVM_SIDE_TOP;
    if (std::strcmp(key, "bottom") == 0) return KVM_SIDE_BOTTOM;
    return -1;
}

/**
 * Controller only: change the active side while a session is running. Safe to
 * call from any thread; the message loop applies it within one 50 ms tick.
 * If control is currently on the target it is handed back first (held keys
 * are released), then the new edge is armed. Outside a session the request is
 * queued and used by the next session (including an automatic reconnect).
 */
void kvmSetControllerSide(int side);

/** Result codes for kvmRunController(). Only OK means "the caller stopped
 *  it"; every other value carries a machine-readable failure class so the
 *  resilient wrapper knows what may be retried. */
enum KvmRunResult : int {
    KVM_RUN_OK          = 0,   /* session ended by *running clearing        */
    KVM_RUN_TRANSPORT   = 1,   /* connect/handshake/session I/O failure     */
    KVM_RUN_DENIED      = 2,   /* target declined / local password cancelled */
    KVM_RUN_UNSUPPORTED = 3,   /* stitch is not available on this platform  */
    KVM_RUN_LOST        = 4,   /* an established session lost its socket    */
    KVM_RUN_BUSY        = 5,   /* target's single session slot still held   */
    KVM_RUN_PROTOCOL    = 6    /* version/protocol/local init; retry won't help */
};

/**
 * Controller side (sender): connect to peer_ip:peer_port, wait for the
 * target's consent, then run the local input hooks until *running clears or
 * the connection dies. Blocks until the session ends.
 *
 * `side` says where the peer sits relative to this machine (KvmSide) and so
 * which local edge hands control over. kvmSetControllerSide() may change it
 * mid-session.
 *
 * "running" is the caller's liveness flag (true = keep going) – the sender
 * passes its g_running atomic, which its Ctrl+C handler clears.
 *
 * Emits machine-readable status lines on stdout for the GUI:
 *   STAT|event=start|mode=stitch|dst=<ip>      (consent granted)
 *   STAT|event=stitch|state=waiting|…|side=<side>
 *   STAT|event=stitch|state=remote|mode=stitch|side=<side> (control on the target)
 *   STAT|event=stitch|state=local|mode=stitch|side=<side>  (control came back)
 *   STAT|event=stop|mode=stitch
 *   STAT|event=error|msg=<token>               (on a non-retryable failure)
 *
 * Returns a KvmRunResult. KVM_RUN_LOST means an established session's socket
 * died (link change, peer restart, Wi-Fi roam); it deliberately emits no
 * event=error because the resilient wrapper will retry.
 */
int kvmRunController(const char *peer_ip, int peer_port, int side,
                     const std::atomic<bool> &running,
                     const KvmClientAuth *auth = nullptr);

/**
 * Controller side with automatic reconnect: runs kvmRunController and, after
 * a session was established and then lost, reconnects with 1/2/4/8/16/30 s
 * backoff until *running clears. Each wait is interruptible within 200 ms and
 * emits
 *   STAT|event=stitch|state=reconnecting|mode=stitch|side=<side>
 *       |reason=<token>|err=<wsa>|retry=<n>|delay_ms=<ms>
 * so the GUI keeps the session page and its stop button. A `STITCH|side=…`
 * line received during the outage is applied to the next attempt. Failures
 * before the first session (connect refused, busy, denied) return immediately
 * as before; only a genuinely established-then-lost session is retried.
 */
int kvmRunControllerResilient(const char *peer_ip, int peer_port, int side,
                              const std::atomic<bool> &running,
                              const KvmClientAuth *auth = nullptr);

/**
 * Target side (receiver): listen on KVM_PORT and serve one controller at a
 * time, injecting input. *running (true = keep serving) is polled while
 * idling and during a session; the function returns once it clears. No-op on
 * non-Windows platforms. The serving slot is the process-wide one shared with
 * 8081 (auth.h): a video session or handshake makes this listener answer
 * busy, and vice versa.
 *
 * `auth` supplies the receive policy and the approval prompt. Passing nullptr
 * keeps the v1-style native consent dialog, but the request is treated as
 * manual (never open), so a caller that forgets the hooks cannot expose the
 * desktop without a human answer.
 */
void kvmServiceRun(std::atomic<bool> &running,
                   const KvmTargetAuth *auth = nullptr);

#endif /* MIVARA_KVM_H */
