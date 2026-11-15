/**
 * KVM_TARGET.H – shared target (被控端) side of the stitch channel.
 *
 * kvm_target.cpp owns the 8084 listener, the v2 authorization handshake, the
 * session grant and the heartbeat/event loop. A platform backend supplies only
 * the local input primitives through KvmTargetOps:
 *
 *   Windows (kvm.cpp)  SendInput + GDI layered drag ghost + 8085 reverse drop
 *   Linux   (kvm_x11.cpp) XTest + Xlib desktop metrics
 *   other   (kvm_stub.cpp) unavailable
 *
 * The shared code never touches a platform input API. Documented Windows
 * invariants (hook vs. injection separation, release-all, drop token, feature
 * negotiation) remain in the backend callbacks, not here.
 */
#ifndef MIVARA_KVM_TARGET_H
#define MIVARA_KVM_TARGET_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "drop.h"
#include "kvm.h"
#include "kvm_core.h"

/* Everything the listener negotiated for one session. The lease and the 8085
 * drop gate are owned here and released by kvmTargetReleaseGrant() on every
 * exit path. */
struct KvmTargetGrant {
    KvmHello hello = {};
    int      side = KVM_SIDE_RIGHT;
    int      sx = 0, sy = 0;          /* target virtual-desktop origin */
    uint32_t sw = 1, sh = 1;          /* target virtual-desktop size   */
    uint32_t features = 0;            /* echoed KVM_FEATURE_* bits     */
    uint8_t  drop_token[DROP_TOKEN_BYTES] = {};
    bool     reverse_ok = false;
    uint32_t rev_port = (uint32_t)DROP_PORT;
    bool     drag_icon_ok = false;
    bool     drag_image_ok = false;
    MivaraSessionLease lease = {};
    bool     lease_held = false;
    bool     drop_installed = false;
};

/* Target-side platform primitives. All callbacks run on the single session
 * thread. `init` runs once on the listener thread before bind; false means
 * this platform cannot serve 拼接 (reported as KvmStartupStatus.state = 2). */
struct KvmTargetOps {
    bool (*init)();
    void (*metrics)(int &x, int &y, uint32_t &w, uint32_t &h);
    uint32_t platform_features;       /* drag bits this backend understands */
    void (*injectMove)(int x, int y, uint32_t w, uint32_t h,
                       int sx, int sy);
    void (*injectButton)(uint32_t btn, bool down);
    void (*injectWheel)(int delta, bool horizontal);
    void (*injectKey)(bool down, uint32_t vk, uint32_t scan, bool ext);
    void (*releaseAll)();
    /* Optional visual/drag frames; null when the backend ignores them. */
    void (*dragEvent)(const KvmEvent &e);
    /* Fallback consent prompt when the caller supplied no approve hook.
     * Null means "cannot ask a human" and resolves to KVM_APPROVAL_FAILED. */
    KvmApprovalResult (*askConsent)(const std::string &ip, int side,
                                    std::atomic<bool> &running,
                                    kvm_sock_t client);
    /* Optional per-session backend (Windows starts its 8085 reverse-drop
     * watcher here). Returned pointer is passed unchanged to sessionEnd. */
    void *(*sessionBegin)(const KvmTargetGrant &grant,
                          const std::string &peer_ip, kvm_sock_t client,
                          const KvmTargetAuth *auth,
                          std::atomic<bool> &running, std::mutex &tx);
    void (*sessionEnd)(void *state);
};

const KvmTargetOps &kvmTargetOps();

#endif /* MIVARA_KVM_TARGET_H */
