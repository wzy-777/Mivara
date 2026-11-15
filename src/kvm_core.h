/**
 * KVM_CORE.H – platform-neutral plumbing for the stitch (拼接) channel.
 *
 * The 8084 wire protocol, the v2 authorization exchange and the socket/time
 * helpers are identical on every platform; only the local input backend
 * differs (Windows hooks/Raw Input/SendInput vs X11 XInput2/XTest). This
 * header is the seam between those backends and the shared code:
 *
 *   - kvm_sock_t / time / error helpers
 *   - the controller half of the v2 authorization exchange
 *   - a heartbeat-aware fixed-size receive loop
 *
 * Backends (kvm.cpp on Windows, kvm_x11.cpp on Linux, kvm_stub.cpp elsewhere)
 * provide kvmRunController() and the target-side KvmTargetOps; kvm_target.cpp
 * owns the 8084 listener and the shared session skeleton.
 */
#ifndef MIVARA_KVM_CORE_H
#define MIVARA_KVM_CORE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "kvm.h"
#include "proto.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
using kvm_sock_t = SOCKET;
#  define KVM_INVALID_SOCK INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
using kvm_sock_t = int;
#  define KVM_INVALID_SOCK (-1)
#endif

/* Monotonic milliseconds. A receiver can run for weeks, so this is never a
 * wrapping tick count. */
uint64_t kvmNowMs();
void kvmSleepMs(unsigned ms);

/* Last socket error, and the platform-independent classifications the shared
 * loops need (a recv timeout is EAGAIN/EWOULDBLOCK on POSIX, WSAETIMEDOUT on
 * Windows). */
int  kvmLastSockError();
bool kvmSockWouldBlock(int err);
bool kvmSockTimedOut(int err);
bool kvmSockInterrupted(int err);

void kvmClose(kvm_sock_t s);
void kvmShutdownSend(kvm_sock_t s);
/* Never retries a timed-out blocking send: Winsock can have copied an
 * unreported prefix first, and the fixed 16-byte event stream must not get a
 * duplicated frame. The send timeout is the stall tolerance instead. */
bool kvmSendAll(kvm_sock_t s, const void *buf, size_t len);
void kvmSetTimeouts(kvm_sock_t s, int recv_ms, int send_ms);
void kvmEnableKeepAlive(kvm_sock_t s);

/* recv exactly len bytes, checking `running` between 200 ms waits.
 * 1 = ok, 0 = deadline, -1 = closed/error/caller stopped. */
int kvmRecvAllPoll(kvm_sock_t s, void *buf, size_t len,
                   const std::atomic<bool> &running, int timeout_ms);

kvm_sock_t kvmConnectTimeout(const char *ip, int port,
                             const std::atomic<bool> &running, int ms);

/* This machine's name, capped the same way the 8081 identity frame is. */
std::string kvmLocalDeviceName();
/* Sanitize a device name arriving from the wire before echoing it into a
 * STAT line or an approval dialog ('|' is the field separator). */
std::string kvmDeviceNameFromWire(const HsRequestExtV3 &ext);
bool kvmSendHsResponse(kvm_sock_t s, uint32_t status, uint32_t w, uint32_t h);
void kvmAuthErrorText(uint32_t status, std::string &out);

enum class AuthOutcome { Ok, Status, Cancelled, Transport, Protocol };

/* Controller half of the v2 authorization exchange: identity frame, optional
 * password proof, then the receiver's verdict. `status` is the receiver's
 * HS_* code for AuthOutcome::Status; `why` is a user-facing reason. */
AuthOutcome kvmClientAuthorize(kvm_sock_t s, const KvmHello &hello,
                               const HsAuthStart &as,
                               const std::atomic<bool> &running,
                               const KvmClientAuth *auth,
                               uint32_t &status, std::string &why);

/* Heartbeat state for a KVM event stream. The link is silent whenever the
 * user is idle, so liveness has to be probed. */
struct KvmHeartbeatState {
    bool enabled = false;
    uint64_t last_rx = 0;
    uint64_t next_tx = 0;
    /* Stable end reason tokens for the session-end log: local_stop /
     * deadline / heartbeat_timeout / ping_send_error / peer_closed /
     * recv_error. */
    const char *reason = nullptr;
    int err = 0;
};

/* Receive exactly len bytes of the fixed 16-byte event stream.
 * deadline_ms == 0 means "until shutdown/EOF".
 * `tx` serialises a heartbeat PING with any other writer on the connection
 * (the Windows reverse-drop watcher). */
bool kvmRecvAll(kvm_sock_t s, void *buf, size_t len,
                const std::atomic<bool> &running, uint64_t deadline_ms,
                KvmHeartbeatState *hb, size_t *got, bool *timed_out,
                std::mutex *tx);

/* Non-blocking check for a peer that has already closed (or reset) the
 * connection. Used before opening a modal approval prompt. */
bool kvmPeerClosed(kvm_sock_t s);

#endif /* MIVARA_KVM_CORE_H */
