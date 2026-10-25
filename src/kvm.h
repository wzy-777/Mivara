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
 *                         virtual-desktop metrics and the consent verdict)
 *   controller → target : KvmEvent  (16 bytes, repeated)
 *
 * After a KVM_FEATURE_HEARTBEAT negotiation, PING/PONG events travel in both
 * directions (3 s interval, 9 s receive deadline). Older peers get no PING.
 *
 * Which edge hands control over is a per-session choice: the peer can sit to
 * the left, right, above or below this machine. The side is decided by the
 * controller (the GUI asks before connecting) and is carried in the hello's
 * first reserved word, ENCODED AS (KvmSide + 1), only so the target's consent
 * dialog can name the right edge. 0 therefore means "not specified" — a
 * zero-filled hello from an older build is not mistaken for KVM_SIDE_LEFT and
 * falls back to the default right side. Mid-session switches are purely
 * controller-side state: the target just injects whatever absolute
 * coordinates it is sent.
 *
 * The target asks its user for consent before answering KVM_OK; the reply is
 * delayed until the user answers (or the request is cancelled). A denied
 * request gets KVM_ERR_DENIED and the connection is closed.
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

/* Dedicated TCP port for the stitch input channel. It is deliberately not
 * the video port: a stitch session never performs the video handshake and
 * the two can run side by side. */
#define KVM_PORT    8084
#define KVM_MAGIC   0x4D564B31u   /* 'MVK1' on the wire */
#define KVM_VERSION 1u
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
 * target declined and 3 when 拼接 is unsupported on this platform.
 */
int kvmRunController(const char *peer_ip, int peer_port, int side,
                     const std::atomic<bool> &running);

/**
 * Target side (receiver): listen on KVM_PORT and serve one controller at a
 * time, injecting input. *running (true = keep serving) is polled while
 * idling and during a session; the function returns once it clears. No-op on
 * non-Windows platforms.
 */
void kvmServiceRun(std::atomic<bool> &running);

#endif /* MIVARA_KVM_H */
