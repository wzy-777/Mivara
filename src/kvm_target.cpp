/**
 * KVM_TARGET.CPP – shared 8084 listener, authorization and session loop.
 *
 * The v2 handshake, admission/cooldown rules, heartbeat deadlines and
 * release-all semantics are identical to the original Windows implementation;
 * only `KvmTargetOps` differs per platform. See kvm_target.h.
 */
#include "kvm_target.h"

#include "auth.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

/* ── per-source consent throttles ─────────────────────────────────────────
 * Keyed on the source address so one hostile host cannot auto-deny (or
 * prompt-flood) every other controller on the LAN. A fixed table (no
 * allocation, bounded memory) is enough: the listener serves one connection
 * at a time, so only the session thread ever touches it.
 *
 *   deny_tick   — an explicit "No": silently refuse this source for 60 s.
 *   cancel_tick — the peer hung up before answering: delay the next prompt
 *                 for this source by a few seconds (not a refusal). A global
 *                 tick (g_kvm_last_cancel) applies the same delay to every
 *                 source, so rotating addresses cannot bypass it. */
struct PeerThrottle {
    std::string ip;
    uint64_t deny_tick = 0;
    uint64_t cancel_tick = 0;
};
PeerThrottle g_kvm_peers[8];
unsigned g_kvm_peer_next = 0;

static const uint64_t KVM_DENY_COOLDOWN_MS = 60000;
static const uint64_t KVM_CANCEL_BACKOFF_MS = 3000;
static uint64_t g_kvm_last_cancel = 0;

void noteCancel(uint64_t now)
{
    g_kvm_last_cancel = now;
}

PeerThrottle *peerThrottle(const std::string &ip, bool create)
{
    for (auto &p : g_kvm_peers)
        if (!p.ip.empty() && p.ip == ip) return &p;
    if (!create) return nullptr;
    for (auto &p : g_kvm_peers) {
        if (p.ip.empty()) {
            p.ip = ip;
            return &p;
        }
    }
    /* Full: evict round-robin. Only a hostile host that keeps changing
     * addresses can reach this. */
    PeerThrottle &p = g_kvm_peers[g_kvm_peer_next++ % 8];
    p = PeerThrottle{};
    p.ip = ip;
    return &p;
}

/* ── handshake budget and per-source admission ─────────────────────────────
 * kvmServiceRun() serves one connection at a time (the session thread stays
 * responsive to everyone else with a busy reply), so an unauthenticated peer
 * that stalls would otherwise hold the only slot for the whole identity or
 * proof deadline. Every stage is bounded, the whole exchange has one budget,
 * and a source that keeps abandoning a handshake is refused early. */
static const uint64_t KVM_HELLO_TIMEOUT_MS = 3000;
static const uint64_t KVM_IDENTITY_TIMEOUT_MS = 10000;
/* The controller's password prompt allows 120 s, so the proof window must not
 * close earlier than that (the per-source admission and abandon cooldown, not
 * a short timeout, are what bound a hostile peer). */
static const uint64_t KVM_PROOF_TIMEOUT_MS = 125000;
static const uint64_t KVM_HANDSHAKE_BUDGET_MS = 140000;

static const int KVM_HS_SLOTS = 8;
static const int KVM_HS_MAX_PENDING_PER_IP = 1;   /* same source: one at a time */
static const int KVM_HS_FREE_ABANDONS = 2;        /* tolerate transients first */
static const uint64_t KVM_HS_COOLDOWN_BASE_MS = 2000;
static const uint64_t KVM_HS_COOLDOWN_MAX_MS = 60000;

struct HsEntry {
    std::string ip;
    int         pending = 0;
    int         strikes = 0;
    uint64_t    until = 0;       /* abuse-cooldown end (0 = none) */
    uint64_t    last = 0;        /* last admission, for eviction */
    uint64_t    last_success = 0; /* completed session; see hsRelease */
};
HsEntry g_kvm_hs[KVM_HS_SLOTS];
unsigned g_kvm_hs_next = 0;
std::mutex g_kvm_hs_mtx;

HsEntry *hsFind(const std::string &ip)
{
    for (auto &e : g_kvm_hs)
        if (!e.ip.empty() && e.ip == ip) return &e;
    return nullptr;
}

/* Caller holds g_kvm_hs_mtx. Evicts the least recently used idle entry; an
 * entry with an unfinished handshake is never evicted. */
HsEntry *hsSlot(const std::string &ip)
{
    if (HsEntry *e = hsFind(ip)) return e;
    for (auto &e : g_kvm_hs) {
        if (e.ip.empty()) {
            e.ip = ip;
            return &e;
        }
    }
    HsEntry *victim = nullptr;
    for (auto &e : g_kvm_hs) {
        if (e.pending > 0) continue;
        if (!victim || e.last < victim->last) victim = &e;
    }
    if (!victim) victim = &g_kvm_hs[g_kvm_hs_next++ % KVM_HS_SLOTS];
    *victim = HsEntry{};
    victim->ip = ip;
    return victim;
}

/* Admit one handshake from `ip`. False when the source is in its local abuse
 * cooldown or already has an unfinished handshake. */
bool hsAdmit(const std::string &ip)
{
    std::lock_guard<std::mutex> lk(g_kvm_hs_mtx);
    HsEntry *e = hsSlot(ip);
    const uint64_t now = kvmNowMs();
    if (e->until) {
        if (now < e->until) return false;
        e->until = 0;
        e->strikes = 0;
    }
    if (e->pending >= KVM_HS_MAX_PENDING_PER_IP) return false;
    e->pending++;
    e->last = now;
    return true;
}

/* Release an admission. `abandoned` is only for a protocol-stage timeout/EOF
 * before authorization; a refusal, cancellation or claim failure must not
 * punish the source, and a completed authorization clears its strikes. */
void hsRelease(const std::string &ip, bool admitted, bool abandoned,
               bool success)
{
    /* A source that never reached admission and did not abandon anything
     * (version mismatch, cross-mode busy) leaves no trace. */
    if (!admitted && !abandoned && !success) return;
    std::lock_guard<std::mutex> lk(g_kvm_hs_mtx);
    HsEntry *e = hsSlot(ip);
    if (admitted && e->pending > 0) e->pending--;
    if (success) {
        e->strikes = 0;
        e->until = 0;
        e->last_success = kvmNowMs();
        return;
    }
    if (!abandoned) return;
    /* Abandons right after a completed session are a reconnect over a flaky
     * link, not handshake abuse; punishing them turns a 20 s outage into a
     * minute-long cooldown. */
    if (e->last_success && kvmNowMs() - e->last_success < 60000) return;
    e->strikes++;
    if (e->strikes > KVM_HS_FREE_ABANDONS) {
        const int step = e->strikes - KVM_HS_FREE_ABANDONS - 1; /* 0,1,2... */
        const int shift = step < 5 ? step : 5;
        uint64_t cd = KVM_HS_COOLDOWN_BASE_MS << shift;
        if (cd > KVM_HS_COOLDOWN_MAX_MS) cd = KVM_HS_COOLDOWN_MAX_MS;
        e->until = kvmNowMs() + cd;
    }
}

/* Milliseconds left in the whole-handshake budget; 0 = already spent. */
uint64_t hsRemaining(uint64_t start)
{
    const uint64_t now = kvmNowMs();
    const uint64_t elapsed = now >= start ? now - start : 0;
    return elapsed >= KVM_HANDSHAKE_BUDGET_MS
               ? 0 : KVM_HANDSHAKE_BUDGET_MS - elapsed;
}

/* recv bounded by both the stage limit and the remaining total budget. */
bool recvHandshake(kvm_sock_t s, void *buf, size_t len,
                   std::atomic<bool> &running, uint64_t stage_ms,
                   uint64_t start, size_t *got = nullptr,
                   bool *timed_out = nullptr)
{
    const uint64_t left = hsRemaining(start);
    if (left == 0) {
        if (got) *got = 0;
        if (timed_out) *timed_out = true;
        return false;
    }
    const uint64_t wait = stage_ms < left ? stage_ms : left;
    return kvmRecvAll(s, buf, len, running, wait, nullptr, got, timed_out,
                      nullptr);
}

/* Releases the source's pending slot on every serveClient exit path. */
struct HsGuard {
    explicit HsGuard(const std::string &peer) : ip(peer) {}
    ~HsGuard() { hsRelease(ip, admitted, abandoned, success); }

    bool admit() { admitted = hsAdmit(ip); return admitted; }
    void markAbandoned() { abandoned = true; }
    void markSuccess() { success = true; }

    std::string ip;
    bool admitted = false;
    bool abandoned = false;
    bool success = false;
};

/* Releases the process-wide session lease on every exit path after the claim
 * (HS_OK/metrics send failure, protocol error, session end). */
struct SessionLeaseGuard {
    MivaraSessionLease lease{};
    bool held = false;
    bool drop = false;
    ~SessionLeaseGuard()
    {
        if (drop) dropGateClear(lease.epoch);
        if (held) mivaraSessionRelease(lease);
    }
};

/* ── grant lifetime ──────────────────────────────────────────────────────── */

void releaseGrant(KvmTargetGrant &grant)
{
    if (grant.drop_installed) {
        dropGateClear(grant.lease.epoch);
        grant.drop_installed = false;
    }
    if (grant.lease_held) {
        mivaraSessionRelease(grant.lease);
        grant.lease_held = false;
    }
}

/* ── handshake ───────────────────────────────────────────────────────────── */

/* Returns false after already sending the refusal reply (or on a transport
 * error before any reply). On success the grant owns the session lease and
 * the optional 8085 token. */
bool targetBegin(kvm_sock_t client, const std::string &peer_ip,
                 std::atomic<bool> &running, const KvmTargetAuth *auth,
                 KvmTargetGrant &grant)
{
    const KvmTargetOps &ops = kvmTargetOps();
    const uint64_t hs_start = kvmNowMs();
    HsGuard hs(peer_ip);

    kvmSetTimeouts(client, 1000, 2000);
    kvmEnableKeepAlive(client);

    KvmHello hello = {};
    size_t got = 0;
    bool timed_out = false;
    if (!recvHandshake(client, &hello, sizeof(hello), running,
                       KVM_HELLO_TIMEOUT_MS, hs_start, &got, &timed_out)) {
        /* A bare TCP probe (the GUI tests the port that way) connects and
         * closes without sending a byte; that is not an abandoned handshake
         * and must not arm the cooldown, otherwise normal discovery would
         * lock out the real controller. A partial hello or a source that
         * stays silent until the deadline is a real abandonment. A local
         * shutdown is not the peer's fault either. */
        if (running.load() && (got > 0 || timed_out)) hs.markAbandoned();
        return false;
    }

    KvmReply reply = {};
    reply.magic = htonl(KVM_MAGIC);
    reply.version = htonl(KVM_VERSION);
    reply.reserved = htonl((ntohl(hello.reserved[1]) & KVM_FEATURE_HEARTBEAT)
                               ? KVM_FEATURE_HEARTBEAT : 0);

    /* A v1 peer (or a foreign service) is refused before authorization, so an
     * old client can never reach the event stream around the policy. */
    if (ntohl(hello.magic) != KVM_MAGIC ||
        ntohl(hello.version) != KVM_VERSION) {
        reply.status = htonl(KVM_ERR_VERSION);
        kvmSendAll(client, &reply, sizeof(reply));
        return false;
    }

    /* 8081 and 8084 share one process-wide session slot (auth.h). Refuse
     * before asking for a password or showing a prompt: neither a video
     * session nor a video handshake that already claimed the slot may be
     * joined. HsResponseV2(HS_ERR_BUSY) is the cross-mode "receiving another
     * connection" answer the controller understands. */
    if (mivaraSessionOwner() != MIVARA_SESSION_NONE) {
        kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
        std::cout << "[kvm] request from " << peer_ip
                  << " refused: another session is active\n" << std::flush;
        return false;
    }

    /* Same source: at most one unfinished handshake plus a local cooldown
     * after repeated protocol-stage abandons. Keep the bare KvmReply so the
     * controller retains its single 300 ms retry (a TVHS refusal ends it). */
    if (!hs.admit()) {
        reply.status = htonl(KVM_ERR_BUSY);
        kvmSendAll(client, &reply, sizeof(reply));
        std::cout << "[kvm] request from " << peer_ip
                  << " refused: source busy or in handshake cooldown\n"
                  << std::flush;
        return false;
    }

    PeerThrottle *throttle = peerThrottle(peer_ip, false);
    if (throttle && throttle->deny_tick &&
        kvmNowMs() - throttle->deny_tick < KVM_DENY_COOLDOWN_MS) {
        reply.status = htonl(KVM_ERR_DENIED);
        kvmSendAll(client, &reply, sizeof(reply));
        std::cout << "[kvm] request from " << peer_ip
                  << " auto-declined (recent refusal)\n" << std::flush;
        return false;
    }

    /* Where the peer sits relative to the controller (informational: it only
     * tailors the approval text). The controller encodes side + 1 so a zeroed
     * field (older build, or a raw KvmHello = {}) means "unspecified" rather
     * than KVM_SIDE_LEFT; anything out of range falls back to the default. */
    const uint32_t raw_side = ntohl(hello.reserved[0]);
    const int side = (raw_side >= 1 && raw_side <= (uint32_t)KVM_SIDE_BOTTOM + 1)
                         ? (int)raw_side - 1
                         : (int)KVM_SIDE_RIGHT;

    /* ══ 接收策略 (same one 8081 uses) ═══════════════════════════════════
     * No hook means manual: the native prompt is the fallback, but a caller
     * that forgets the hooks must not expose an open desktop. */
    MivaraAuthMode mode = MIVARA_AUTH_MANUAL;
    uint8_t salt[16] = {}, hash[32] = {};
    uint32_t iter = 0;
    bool record_ok = false;
    if (auth && auth->policy &&
        !auth->policy(&mode, salt, hash, &iter, &record_ok, auth->ctx))
        mode = MIVARA_AUTH_MANUAL;

    if (mode == MIVARA_AUTH_PASSWORD && !record_ok) {
        /* Fail closed: a missing/corrupt record is never a downgrade. */
        kvmSendHsResponse(client, HS_ERR_AUTH_CONFIG, 0, 0);
        std::cout << "[kvm] request from " << peer_ip
                  << " refused: password record unusable\n" << std::flush;
        return false;
    }

    int sx, sy;
    uint32_t sw, sh;
    ops.metrics(sx, sy, sw, sh);

    /* The process-wide slot is acquired before a manual prompt (so the human
     * decision cannot be stolen by a video handshake that finishes first) or,
     * for password/open, right before the grant. It is released on every exit
     * path by the guard. */
    SessionLeaseGuard lease;

    if (mode != MIVARA_AUTH_OPEN) {
        HsAuthStart as = {};
        as.magic = htonl(TVHA_MAGIC);
        as.version = htonl(TVHA_VERSION);
        as.mode = htonl(mode == MIVARA_AUTH_PASSWORD ? AUTH_MODE_PASSWORD
                                                     : AUTH_MODE_MANUAL);
        as.kdf_iter = htonl(iter);
        if (mode == MIVARA_AUTH_PASSWORD)
            std::memcpy(as.salt, salt, sizeof(as.salt));
        if (!mivaraRandomBytes(as.nonce, sizeof(as.nonce))) {
            std::cout << "[kvm] request from " << peer_ip
                      << " refused: no CSPRNG\n" << std::flush;
            return false;
        }
        if (!kvmSendAll(client, &as, sizeof(as))) return false;

        /* The identity frame arrives before any verdict so a manual prompt
         * can name the device and a password proof can bind it. The sender
         * sends it immediately, so a short stage limit is enough; the proof
         * gets the long one because it waits for a typed password. Both are
         * additionally bounded by what is left of the total budget. */
        HsRequestExtV3 ext = {};
        if (!recvHandshake(client, &ext, sizeof(ext), running,
                           KVM_IDENTITY_TIMEOUT_MS, hs_start)) {
            if (running.load()) hs.markAbandoned();
            return false;
        }
        if (ntohl(ext.magic) != TVHX_MAGIC ||
            ntohl(ext.version) != TVHA_VERSION) {
            kvmSendHsResponse(client, HS_ERR_VERSION, 0, 0);
            return false;
        }
        std::string device = kvmDeviceNameFromWire(ext);
        if (device.empty()) device = peer_ip;
        std::cout << "[kvm] stitch request from " << peer_ip << " (" << device
                  << ", peer on the " << kvmSideKey(side) << ")\n" << std::flush;

        if (mode == MIVARA_AUTH_PASSWORD) {
            /* One process-wide budget shared with the 8081 handshake: a source
             * cannot alternate ports for extra free guesses or a shorter
             * cooldown. The helpers lock internally. */
            const int wait_s = mivaraAuthThrottleRetryAfter(peer_ip);
            if (wait_s > 0) {
                kvmSendHsResponse(client, HS_ERR_AUTH, 0, 0);
                std::cout << "[kvm] request from " << peer_ip
                          << " throttled after failed passwords\n" << std::flush;
                return false;
            }
            HsAuthProof proof = {};
            if (!recvHandshake(client, &proof, sizeof(proof), running,
                               KVM_PROOF_TIMEOUT_MS, hs_start)) {
                if (running.load()) hs.markAbandoned();
                return false;
            }
            if (ntohl(proof.magic) != TVHA_MAGIC ||
                ntohl(proof.version) != TVHA_VERSION) {
                mivaraAuthThrottleNoteAbuse(peer_ip);
                kvmSendHsResponse(client, HS_ERR_AUTH, 0, 0);
                return false;
            }
            uint8_t bound[sizeof(KvmHello) + sizeof(HsRequestExtV3)];
            std::memcpy(bound, &hello, sizeof(hello));
            std::memcpy(bound + sizeof(hello), &ext, sizeof(ext));
            uint8_t expect[32] = {};
            /* The password itself never reaches this machine; the proof is
             * domain-separated for 8084 (see auth.h). */
            mivaraAuthProof(hash, MIVARA_AUTH_PURPOSE_STITCH, as.nonce, bound,
                            sizeof(bound), expect);
            if (!mivaraConstEq(expect, proof.proof, sizeof(expect))) {
                mivaraAuthThrottleNoteFailure(peer_ip);
                kvmSendHsResponse(client, HS_ERR_AUTH, 0, 0);
                std::cout << "[kvm] request from " << peer_ip
                          << " refused: bad password\n" << std::flush;
                return false;
            }
            mivaraAuthThrottleNoteSuccess(peer_ip);
        } else {
            /* Manual policy. Before opening a modal prompt, make sure the peer
             * is still there: a controller that sent the identity frame and
             * hung up must not hold the slot for the whole 30 s deadline. */
            if (kvmPeerClosed(client)) {
                std::cout << "[kvm] request from " << peer_ip
                          << " cancelled before approval\n" << std::flush;
                return false;
            }
            /* Reserve the slot across the prompt: a video handshake that
             * grants while the user is deciding must not swallow the result. */
            if (!lease.held) {
                if (!mivaraSessionClaim(MIVARA_SESSION_STITCH, 60000,
                                        &lease.lease)) {
                    kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
                    std::cout << "[kvm] request from " << peer_ip
                              << " refused: another session claimed the slot\n"
                              << std::flush;
                    return false;
                }
                lease.held = true;
            }
            KvmApprovalResult verdict = KVM_APPROVAL_DENY;
            if (auth && auth->approve) {
                verdict = auth->approve(peer_ip.c_str(), device.c_str(),
                                        (intptr_t)client, auth->ctx);
            } else {
                /* Native-prompt fallback (no GUI hooks): keep the cancel
                 * backoff so a peer that connects + closes cannot flash a
                 * dialog in a loop. */
                uint64_t backoff = 0;
                if (throttle && throttle->cancel_tick) {
                    const uint64_t age = kvmNowMs() - throttle->cancel_tick;
                    if (age < KVM_CANCEL_BACKOFF_MS)
                        backoff = KVM_CANCEL_BACKOFF_MS - age;
                }
                if (g_kvm_last_cancel) {
                    const uint64_t age = kvmNowMs() - g_kvm_last_cancel;
                    if (age < KVM_CANCEL_BACKOFF_MS &&
                        (KVM_CANCEL_BACKOFF_MS - age) > backoff)
                        backoff = KVM_CANCEL_BACKOFF_MS - age;
                }
                while (backoff > 0) {
                    if (!running.load() || kvmPeerClosed(client)) {
                        noteCancel(kvmNowMs());
                        if (PeerThrottle *p = peerThrottle(peer_ip, true))
                            p->cancel_tick = kvmNowMs();
                        return false;
                    }
                    const uint64_t slice = backoff < 100 ? backoff : 100;
                    kvmSleepMs((unsigned)slice);
                    backoff -= slice;
                }
                verdict = ops.askConsent
                              ? ops.askConsent(peer_ip, side, running, client)
                              : KVM_APPROVAL_FAILED;
                if (verdict == KVM_APPROVAL_CANCELLED) {
                    noteCancel(kvmNowMs());
                    if (PeerThrottle *p = peerThrottle(peer_ip, true))
                        p->cancel_tick = kvmNowMs();
                    std::cout << "[kvm] request from " << peer_ip
                              << " cancelled before consent\n" << std::flush;
                    return false;
                }
            }

            /* Handle the verdict value by value. Folding timeout, cancel and
             * prompt failure into "deny" would arm the 60 s cooldown for a
             * request that nobody actually refused. */
            switch (verdict) {
            case KVM_APPROVAL_ALLOW:
                break;
            case KVM_APPROVAL_DENY:
                /* Only an explicit human "No" starts the per-peer cooldown. */
                if (PeerThrottle *p = peerThrottle(peer_ip, true))
                    p->deny_tick = kvmNowMs();
                kvmSendHsResponse(client, HS_ERR_DENIED, 0, 0);
                std::cout << "[kvm] request " << peer_ip << " declined\n"
                          << std::flush;
                return false;
            case KVM_APPROVAL_TIMEOUT:
                /* An unanswered prompt is not a refusal: no cooldown. */
                kvmSendHsResponse(client, HS_ERR_AUTH_TIMEOUT, 0, 0);
                std::cout << "[kvm] request " << peer_ip << " timed out\n"
                          << std::flush;
                return false;
            case KVM_APPROVAL_CANCELLED:
                /* Peer hung up or the receiver is stopping. Not a refusal and
                 * not the peer's fault; the socket is usually already gone, so
                 * skip the reply and free the slot now. */
                std::cout << "[kvm] request " << peer_ip
                          << " cancelled during approval\n" << std::flush;
                return false;
            case KVM_APPROVAL_FAILED:
                /* No human was asked because the prompt could not be created.
                 * Log the real cause but answer like a refusal; a local
                 * failure must not arm the peer cooldown. */
                kvmSendHsResponse(client, HS_ERR_DENIED, 0, 0);
                std::cout << "[kvm] request " << peer_ip
                          << " denied: approval prompt failed\n" << std::flush;
                return false;
            case KVM_APPROVAL_BUSY:
                /* Another device is already asking the local user. Answer
                 * "busy" instead of dropping the request silently; not a
                 * refusal, so no cooldown. */
                kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
                std::cout << "[kvm] request " << peer_ip
                          << " refused: another approval is pending\n"
                          << std::flush;
                return false;
            default:
                kvmSendHsResponse(client, HS_ERR_DENIED, 0, 0);
                std::cout << "[kvm] request " << peer_ip
                          << " denied: unknown approval verdict\n" << std::flush;
                return false;
            }
        }
    }

    /* Authorization succeeded (or the policy is open). Password/open claim
     * here; manual already holds the slot from before its prompt. Promote
     * before telling the controller it may start. */
    if (!lease.held) {
        if (!mivaraSessionClaim(MIVARA_SESSION_STITCH, 30000, &lease.lease)) {
            kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
            std::cout << "[kvm] request from " << peer_ip
                      << " refused: another session claimed the slot\n"
                      << std::flush;
            return false;
        }
        lease.held = true;
    }
    if (!mivaraSessionPromote(lease.lease)) {
        /* The claim expired while we were authorizing; the destructor releases
         * whatever is left, and the caller is told the slot is busy. */
        kvmSendHsResponse(client, HS_ERR_BUSY, 0, 0);
        std::cout << "[kvm] request from " << peer_ip
                  << " refused: session claim expired\n" << std::flush;
        return false;
    }
    hs.markSuccess();

    const uint32_t hello_feat = ntohl(hello.reserved[1]);

    /* File drop is a separate 8085 service and a separate opt-in. Install its
     * token before the KVM reply goes out, so a controller that dials 8085
     * immediately cannot race the gate. The guard clears it on every exit. */
    bool drop_ok = (auth &&
                    auth->accept_files.load(std::memory_order_relaxed) != 0) &&
                   dropServiceStatus().state == 1 &&
                   (hello_feat & KVM_FEATURE_FILE_DROP) != 0;
    /* Reverse (this machine → controller): independent of the forward gate —
     * it needs OUR file_drop_send, the 8085 listener and the controller's
     * advertised REVERSE bit, not file_drop_accept. */
    bool reverse_ok =
        (auth && auth->file_send.load(std::memory_order_relaxed) != 0) &&
        dropServiceStatus().state == 1 &&
        (hello_feat & KVM_FEATURE_REVERSE_DROP) != 0;
    /* The session token is needed for EITHER direction: a reverse-only session
     * (drop_ok = false) must still get a real random token, otherwise both
     * sides would end up with an all-zero token and the 8085 gate would be no
     * protection at all. */
    uint8_t drop_token[DROP_TOKEN_BYTES] = {};
    const bool need_token = drop_ok || reverse_ok;
    if (need_token && !mivaraRandomBytes(drop_token, sizeof(drop_token))) {
        drop_ok = false;
        reverse_ok = false;
    }
    if (drop_ok) {
        if (dropGateInstall(drop_token, lease.lease.epoch)) lease.drop = true;
        else drop_ok = false;
    }
    uint32_t feat = (hello_feat & KVM_FEATURE_HEARTBEAT)
                        ? KVM_FEATURE_HEARTBEAT : 0u;
    if (drop_ok) feat |= KVM_FEATURE_FILE_DROP;
    if (reverse_ok) feat |= KVM_FEATURE_REVERSE_DROP;
    /* Echo a visual capability only to a peer that advertised it AND to a
     * backend that can actually draw it. An older controller deletes the
     * session on an unknown event type. */
    const uint32_t drag_bits = ops.platform_features &
                               (KVM_FEATURE_DRAG_ICON | KVM_FEATURE_DRAG_IMAGE);
    if ((hello_feat & drag_bits & KVM_FEATURE_DRAG_ICON) != 0)
        feat |= KVM_FEATURE_DRAG_ICON;
    if ((hello_feat & drag_bits & KVM_FEATURE_DRAG_IMAGE) != 0)
        feat |= KVM_FEATURE_DRAG_IMAGE;
    reply.reserved = htonl(feat);

    if (mode != MIVARA_AUTH_OPEN) {
        if (!kvmSendHsResponse(client, HS_OK, sw, sh)) return false;
    }

    reply.status = htonl(KVM_OK);
    reply.screen_x = (int32_t)htonl((uint32_t)sx);
    reply.screen_y = (int32_t)htonl((uint32_t)sy);
    reply.screen_w = htonl(sw);
    reply.screen_h = htonl(sh);
    if (!kvmSendAll(client, &reply, sizeof(reply))) {
        mivaraSecureZero(drop_token, sizeof(drop_token));
        return false;
    }

    /* Fixed 1 + 8 event sequence: the controller reads it before it starts
     * drainLoop(), so the 16-byte event parser never sees a variable frame.
     * Sent for EITHER direction: a reverse-only session needs the token too. */
    if (drop_ok || reverse_ok) {
        KvmEvent info = {};
        info.type = htonl(KVM_EV_DROP_INFO);
        info.a = htonl(0x4D564B58u);   /* 'MVKX' marker */
        info.b = htonl(1u);
        info.c = htonl(8u);
        if (!kvmSendAll(client, &info, sizeof(info))) {
            mivaraSecureZero(drop_token, sizeof(drop_token));
            return false;
        }
        for (int i = 0; i < 8; i++) {
            uint32_t word = 0;
            std::memcpy(&word, drop_token + i * 4, 4);
            KvmEvent ev = {};
            ev.type = htonl(KVM_EV_DROP_INFO);
            ev.a = htonl((uint32_t)i);
            ev.b = htonl(word);
            if (!kvmSendAll(client, &ev, sizeof(ev))) {
                mivaraSecureZero(drop_token, sizeof(drop_token));
                return false;
            }
        }
    }

    /* Everything negotiated now belongs to the session. Disarm the local
     * guard; kvmTargetReleaseGrant() takes over after the event loop. */
    grant.hello = hello;
    grant.side = side;
    grant.sx = sx;
    grant.sy = sy;
    grant.sw = sw;
    grant.sh = sh;
    grant.features = feat;
    grant.reverse_ok = reverse_ok;
    const uint32_t rev_port = hello_feat >> 16;
    grant.rev_port = (rev_port >= 1 && rev_port <= 65535u)
                         ? rev_port : (uint32_t)DROP_PORT;
    grant.drag_icon_ok = (feat & KVM_FEATURE_DRAG_ICON) != 0;
    grant.drag_image_ok = (feat & KVM_FEATURE_DRAG_IMAGE) != 0;
    if (drop_ok || reverse_ok)
        std::memcpy(grant.drop_token, drop_token, sizeof(grant.drop_token));
    mivaraSecureZero(drop_token, sizeof(drop_token));

    grant.lease = lease.lease;
    grant.lease_held = true;
    grant.drop_installed = lease.drop;
    lease.held = false;
    lease.drop = false;
    return true;
}

/* ── event loop ──────────────────────────────────────────────────────────── */

void targetLoop(kvm_sock_t client, const std::string &peer_ip,
                std::atomic<bool> &running, const KvmTargetAuth *auth,
                KvmTargetGrant &grant)
{
    const KvmTargetOps &ops = kvmTargetOps();

    std::mutex tx;
    void *sess = nullptr;
    if (ops.sessionBegin)
        sess = ops.sessionBegin(grant, peer_ip, client, auth, running, tx);
    mivaraSecureZero(grant.drop_token, sizeof(grant.drop_token));

    std::cout << "[kvm] stitch session started (" << grant.sw << "x"
              << grant.sh << "); the peer now controls this desktop\n"
              << std::flush;

    KvmHeartbeatState hb;
    hb.enabled = (grant.features & KVM_FEATURE_HEARTBEAT) != 0;
    hb.last_rx = kvmNowMs();
    hb.next_tx = hb.last_rx + KVM_HEARTBEAT_INTERVAL_MS;

    KvmEvent wire;
    bool io_ok = true;
    while (io_ok) {
        if (!running.load()) break;
        if (!kvmRecvAll(client, &wire, sizeof(wire), running, 0, &hb,
                        nullptr, nullptr, &tx))
            break;
        KvmEvent e;
        e.type = ntohl(wire.type);
        e.a = ntohl(wire.a);
        e.b = ntohl(wire.b);
        e.c = ntohl(wire.c);
        switch (e.type) {
        case KVM_EV_MOVE:
            ops.injectMove((int)e.a, (int)e.b, grant.sw, grant.sh,
                           grant.sx, grant.sy);
            break;
        case KVM_EV_BUTTON:
            ops.injectButton(e.a, e.b != 0);
            break;
        case KVM_EV_WHEEL:
            ops.injectWheel((int)(int32_t)e.a, e.b != 0);
            break;
        case KVM_EV_KEY:
            ops.injectKey(e.a != 0, e.b, e.c & 0xFFFFu,
                          (e.c & 0x10000u) != 0);
            break;
        case KVM_EV_RELEASE_ALL:
            ops.releaseAll();
            break;
        case KVM_EV_PING:
            if (hb.enabled) {
                KvmEvent pong = {};
                pong.type = htonl(KVM_EV_PONG);
                std::lock_guard<std::mutex> lk(tx);
                io_ok = kvmSendAll(client, &pong, sizeof(pong));
                if (!io_ok) {
                    hb.reason = "pong_send_error";
                    hb.err = kvmLastSockError();
                }
            }
            break;
        case KVM_EV_DRAG_INFO:
        case KVM_EV_DRAG_TEXT:
        case KVM_EV_DRAG_ICON_INFO:
        case KVM_EV_DRAG_ICON_DATA:
        case KVM_EV_DRAG_IMAGE_INFO:
        case KVM_EV_DRAG_IMAGE_DATA:
            if (ops.dragEvent) ops.dragEvent(e);
            break;
        case KVM_EV_PONG:
            break;
        default:
            break;
        }
    }
    /* Release injected input before any helper/thread teardown: the session is
     * already over, and a reverse transfer can take seconds to wind down. The
     * backend's sessionEnd() clears its liveness flag and joins its watcher. */
    ops.releaseAll();
    if (ops.sessionEnd) ops.sessionEnd(sess);
    releaseGrant(grant);

    const char *end_reason =
        hb.reason ? hb.reason : (io_ok ? "local_stop" : "pong_send_error");
    std::cout << "[kvm] stitch session from " << peer_ip << " ended (reason="
              << end_reason
              << (hb.err ? ", err=" + std::to_string(hb.err) : "") << ")\n"
              << std::flush;
}

void serveClient(kvm_sock_t client, const std::string &peer_ip,
                 std::atomic<bool> &running, const KvmTargetAuth *auth)
{
    KvmTargetGrant grant;
    if (!targetBegin(client, peer_ip, running, auth, grant)) return;
    targetLoop(client, peer_ip, running, auth, grant);
}

} /* namespace */

/* ── listener ────────────────────────────────────────────────────────────── */

static std::atomic<int> g_kvm_start_state{0};
static std::atomic<int> g_kvm_start_stage{0};
static std::atomic<int> g_kvm_start_error{0};

static void kvmReportStartup(int state, int stage, int error)
{
    g_kvm_start_error.store(error);
    g_kvm_start_stage.store(stage);
    g_kvm_start_state.store(state);   /* publish the state last */
}

KvmStartupStatus kvmServiceStatus()
{
    KvmStartupStatus st = {};
    st.state = g_kvm_start_state.load();
    st.stage = g_kvm_start_stage.load();
    st.error_code = g_kvm_start_error.load();
    st.port = (uint16_t)KVM_PORT;
    return st;
}

void kvmServiceRun(std::atomic<bool> &running, const KvmTargetAuth *auth)
{
    const KvmTargetOps &ops = kvmTargetOps();
    kvmReportStartup(0, 0, 0);
    if (!ops.init || !ops.init()) {
        /* Platform has no usable input backend (non-Windows non-X11 build, or
         * no X display). Not a bind failure; report "not supported" so the GUI
         * does not show a port conflict. */
        kvmReportStartup(2, 0, 0);
        std::cout << "[kvm] stitch target unavailable on this platform\n"
                  << std::flush;
        return;
    }

    kvm_sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == KVM_INVALID_SOCK) {
        const int err = kvmLastSockError();
        kvmReportStartup(-1, 1, err);
        std::cout << "[kvm] socket init failed (" << err
                  << "); stitch disabled\n" << std::flush;
        return;
    }
#ifdef _WIN32
    /* SO_EXCLUSIVEADDRUSE, not SO_REUSEADDR: on Windows the latter allows a
     * second bind on the same port (and would let this process hide an
     * existing listener behind it), so a real port conflict would look like
     * success. Exclusive use makes bind() fail and the GUI sees the truth. */
    BOOL exclusive = TRUE;
    if (setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive, sizeof(exclusive)) != 0) {
        const int err = kvmLastSockError();
        kvmReportStartup(-1, 1, err);
        std::cout << "[kvm] SO_EXCLUSIVEADDRUSE failed (" << err
                  << "); stitch disabled\n" << std::flush;
        kvmClose(srv);
        return;
    }
#else
    /* POSIX SO_REUSEADDR only skips TIME_WAIT; it does not allow two live
     * listeners, so a real conflict still fails bind(). */
    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(KVM_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0) {
        const int err = kvmLastSockError();
        kvmReportStartup(-1, 3, err);
        std::cout << "[kvm] bind failed on " << KVM_PORT << " (error " << err
                  << "); stitch disabled\n" << std::flush;
        kvmClose(srv);
        return;
    }
    if (listen(srv, 4) < 0) {
        const int err = kvmLastSockError();
        kvmReportStartup(-1, 4, err);
        std::cout << "[kvm] listen failed on " << KVM_PORT << " (error " << err
                  << "); stitch disabled\n" << std::flush;
        kvmClose(srv);
        return;
    }
    kvmReportStartup(1, 4, 0);
    std::cout << "[kvm] stitch listener on TCP " << KVM_PORT << "\n"
              << std::flush;

    /* The session runs on its own thread so the listener stays responsive:
     * a second controller gets an immediate KVM_ERR_BUSY instead of sitting
     * in the accept backlog for the whole (possibly hours-long) session, and
     * shutdown never has to wait for the modal consent box to be answered.
     * Only one serveClient runs at a time. `session_active` is the same-port
     * admission only; the cross-mode (8081 vs 8084) slot is the shared gate
     * in auth.h, checked and claimed inside serveClient. */
    std::atomic<bool> session_active{false};
    std::thread session;

    while (running.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        struct timeval tv = {1, 0};
#ifdef _WIN32
        const int sel = select(0, &rd, nullptr, nullptr, &tv);
#else
        const int sel = select((int)srv + 1, &rd, nullptr, nullptr, &tv);
#endif
        if (sel <= 0) continue;

        sockaddr_in cli = {};
#ifdef _WIN32
        int cli_len = sizeof(cli);
#else
        socklen_t cli_len = sizeof(cli);
#endif
        kvm_sock_t client = accept(srv, (sockaddr *)&cli, &cli_len);
        if (client == KVM_INVALID_SOCK) continue;

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));

        if (session_active.load()) {
            /* Read the peer's hello first. Closing with unread data makes
             * Windows send an RST, which discards the BUSY reply in flight
             * and surfaces as WSAECONNRESET on the controller. */
            kvmSetTimeouts(client, 500, 500);
            KvmHello hello = {};
            kvmRecvAll(client, &hello, sizeof(hello), running, 1500, nullptr,
                       nullptr, nullptr, nullptr);

            KvmReply busy = {};
            busy.magic = htonl(KVM_MAGIC);
            busy.version = htonl(KVM_VERSION);
            busy.status = htonl(KVM_ERR_BUSY);
            kvmSendAll(client, &busy, sizeof(busy));

            /* Drain anything still in flight before the graceful close. The
             * socket carries a 500 ms receive timeout, but a peer that keeps
             * sending would spin this loop forever — bound it by time and by
             * the service's running flag so shutdown is never pinned. */
            kvmShutdownSend(client);
            char sink[64];
            const uint64_t drain_deadline = kvmNowMs() + 1000;
            while (running.load() && kvmNowMs() < drain_deadline &&
                   recv(client, sink, sizeof(sink), 0) > 0) {}
            kvmClose(client);
            std::cout << "[kvm] busy, refused extra controller " << ip << "\n"
                      << std::flush;
            continue;
        }

        if (session.joinable()) session.join();
        session_active = true;
        std::string peer(ip);
        session = std::thread([client, peer, &running, &session_active, auth] {
            serveClient(client, peer, running, auth);
            kvmClose(client);
            session_active = false;
        });
    }

    if (session.joinable()) session.join();
    kvmClose(srv);
    std::cout << "[kvm] stitch listener stopped\n" << std::flush;
}
