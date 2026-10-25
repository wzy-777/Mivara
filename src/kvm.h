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
 * directions (3 s interval, 9 s receive deadline). Older peers get no PING.
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
    KVM_EV_PONG        = 7    /* reply to PING; never sent to older peers    */
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
 * are released), then the new edge is armed. No-op outside a session.
 */
void kvmSetControllerSide(int side);

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
 *   STAT|event=error|msg=<token>               (on failure)
 *
 * Returns 0 on a clean session, 1 on connection/protocol failure, 2 when the
 * target declined or the authorization failed, and 3 when 拼接 is unsupported
 * on this platform.
 */
int kvmRunController(const char *peer_ip, int peer_port, int side,
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
