/**
 * KVM_CORE.CPP – portable socket/time/auth pieces of the stitch channel.
 * See kvm_core.h; the platform backends live in kvm.cpp / kvm_x11.cpp.
 */
#include "kvm_core.h"
#include "auth.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#  include <mstcpip.h>
#else
#  include <fcntl.h>
#  include <netinet/tcp.h>
#  include <sys/time.h>
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#endif

uint64_t kvmNowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

void kvmSleepMs(unsigned ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int kvmLastSockError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool kvmSockWouldBlock(int err)
{
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

bool kvmSockTimedOut(int err)
{
#ifdef _WIN32
    return err == WSAETIMEDOUT;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT;
#endif
}

bool kvmSockInterrupted(int err)
{
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

void kvmClose(kvm_sock_t s)
{
    if (s == KVM_INVALID_SOCK) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void kvmShutdownSend(kvm_sock_t s)
{
    if (s == KVM_INVALID_SOCK) return;
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
}

bool kvmSendAll(kvm_sock_t s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        const int n = send(s, p + off, (int)(len - off), 0);
#else
        const ssize_t n = send(s, p + off, len - off, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

#ifndef _WIN32
static int kvmSocketFlags(kvm_sock_t s)
{
    return fcntl(s, F_GETFL, 0);
}

static void kvmSetNonBlocking(kvm_sock_t s, bool on)
{
    int flags = kvmSocketFlags(s);
    if (flags < 0) return;
    if (on) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    fcntl(s, F_SETFL, flags);
}
#endif

void kvmSetTimeouts(kvm_sock_t s, int recv_ms, int send_ms)
{
    if (!recv_ms && !send_ms) return;
#ifdef _WIN32
    if (recv_ms) {
        const DWORD v = (DWORD)recv_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&v, sizeof(v));
    }
    if (send_ms) {
        const DWORD v = (DWORD)send_ms;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&v, sizeof(v));
    }
#else
    if (recv_ms) {
        struct timeval tv = {recv_ms / 1000, (recv_ms % 1000) * 1000};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (send_ms) {
        struct timeval tv = {send_ms / 1000, (send_ms % 1000) * 1000};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#endif
}

void kvmEnableKeepAlive(kvm_sock_t s)
{
#ifdef _WIN32
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof(on));
    tcp_keepalive ka = {};
    ka.onoff = 1;
    ka.keepalivetime = 15000;    /* idle before the first probe */
    ka.keepaliveinterval = 3000; /* between probes             */
    DWORD ret = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &ret,
             nullptr, nullptr);
#else
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#  ifdef TCP_KEEPIDLE
    int idle = 15;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#  endif
#  ifdef TCP_KEEPINTVL
    int intvl = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#  endif
#endif
}

int kvmRecvAllPoll(kvm_sock_t s, void *buf, size_t len,
                   const std::atomic<bool> &running, int timeout_ms)
{
    char *p = (char *)buf;
    size_t off = 0;
    const uint64_t start = kvmNowMs();
    while (off < len) {
        if (!running.load()) return -1;
        if (timeout_ms > 0 && (int)(kvmNowMs() - start) > timeout_ms) return 0;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {0, 200000};
#ifdef _WIN32
        const int sel = select(0, &rd, nullptr, nullptr, &tv);
#else
        const int sel = select((int)s + 1, &rd, nullptr, nullptr, &tv);
#endif
        if (sel < 0) {
            const int err = kvmLastSockError();
            if (kvmSockInterrupted(err)) continue;
            return -1;
        }
        if (sel == 0) continue;
        const int n = (int)recv(s, p + off, (int)(len - off), 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 1;
}

kvm_sock_t kvmConnectTimeout(const char *ip, int port,
                             const std::atomic<bool> &running, int ms)
{
    kvm_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == KVM_INVALID_SOCK) return KVM_INVALID_SOCK;

#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    kvmSetNonBlocking(s, true);
#endif

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        kvmClose(s);
        return KVM_INVALID_SOCK;
    }

    const int rc = connect(s, (sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
#ifdef _WIN32
        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
#else
        kvmSetNonBlocking(s, false);
#endif
        return s;
    }
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        kvmClose(s);
        return KVM_INVALID_SOCK;
    }
#else
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        kvmClose(s);
        return KVM_INVALID_SOCK;
    }
#endif

    const uint64_t start = kvmNowMs();
    while ((int)(kvmNowMs() - start) < ms) {
        if (!running.load()) {
            kvmClose(s);
            return KVM_INVALID_SOCK;
        }
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        struct timeval tv = {0, 100000};
#ifdef _WIN32
        const int sel = select(0, nullptr, &wr, nullptr, &tv);
#else
        const int sel = select((int)s + 1, nullptr, &wr, nullptr, &tv);
#endif
        if (sel > 0) {
            int err = 0;
#ifdef _WIN32
            int len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
#else
            socklen_t len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
            if (err != 0) {
                kvmClose(s);
                return KVM_INVALID_SOCK;
            }
#ifdef _WIN32
            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking);
#else
            kvmSetNonBlocking(s, false);
#endif
            return s;
        }
        if (sel < 0) break;
    }
    kvmClose(s);
    return KVM_INVALID_SOCK;
}

std::string kvmLocalDeviceName()
{
    char name[256] = {0};
#ifdef _WIN32
    DWORD n = (DWORD)sizeof(name);
    if (!GetComputerNameA(name, &n) || name[0] == '\0') return "Mivara";
    name[n < sizeof(name) ? n : sizeof(name) - 1] = '\0';
#else
    if (gethostname(name, sizeof(name) - 1) != 0 || name[0] == '\0')
        return "Mivara";
    name[sizeof(name) - 1] = '\0';
#endif
    std::string s(name);
    if (s.size() > 31) s.resize(31);
    return s;
}

/* The device name comes straight off the wire and is echoed into STAT lines
 * (and the app's approval dialog), where '|' separates fields. Replace control
 * characters and '|' the same way the 8081 receiver does, so a hostile client
 * cannot smuggle extra keys into `auth|...|dev=` and spoof the address text. */
std::string kvmDeviceNameFromWire(const HsRequestExtV3 &ext)
{
    std::string d;
    for (size_t i = 0; i < sizeof(ext.device); i++) {
        const unsigned char c = (unsigned char)ext.device[i];
        if (!c) break;
        d.push_back((c < 0x20 || c == 0x7f || c == '|') ? '?' : (char)c);
    }
    if (d.size() > 31) d.resize(31);
    return d;
}

bool kvmSendHsResponse(kvm_sock_t s, uint32_t status, uint32_t w, uint32_t h)
{
    HsResponseV2 res = {};
    res.magic = htonl(TVHS_MAGIC);
    res.version = htonl(TVHS_VERSION);
    res.recv_width = htonl(w);
    res.recv_height = htonl(h);
    res.status = htonl(status);
    return kvmSendAll(s, &res, sizeof(res));
}

void kvmAuthErrorText(uint32_t status, std::string &out)
{
    switch (status) {
    case HS_ERR_AUTH:
        out = "连接密码错误，或尝试次数过多";
        break;
    case HS_ERR_AUTH_TIMEOUT:
        out = "对方未在 30 秒内确认拼接请求";
        break;
    case HS_ERR_DENIED:
        out = "对方拒绝了拼接请求";
        break;
    case HS_ERR_BUSY:
        out = "对方正在接收其他连接";
        break;
    case HS_ERR_AUTH_CONFIG:
        out = "对方接收授权配置异常，请在设置中重新设置连接密码";
        break;
    case HS_ERR_VERSION:
        out = "拼接协议版本不一致，请将两端更新到同一版本";
        break;
    default:
        out = "拼接请求被拒绝（错误码 " + std::to_string(status) + "）";
        break;
    }
}

AuthOutcome kvmClientAuthorize(kvm_sock_t s, const KvmHello &hello,
                               const HsAuthStart &as,
                               const std::atomic<bool> &running,
                               const KvmClientAuth *auth,
                               uint32_t &status, std::string &why)
{
    const auto report = [auth](const char *state) {
        if (auth && auth->report) auth->report(state, nullptr, auth->ctx);
    };

    HsRequestExtV3 ext = {};
    ext.magic = htonl(TVHX_MAGIC);
    ext.version = htonl(TVHA_VERSION);
    const std::string device = kvmLocalDeviceName();
    std::memcpy(ext.device, device.data(),
                device.size() < sizeof(ext.device) ? device.size()
                                                   : sizeof(ext.device));
    if (!kvmSendAll(s, &ext, sizeof(ext))) {
        why = "拼接授权请求发送失败";
        return AuthOutcome::Transport;
    }

    const uint32_t mode = ntohl(as.mode);
    if (mode == AUTH_MODE_PASSWORD) {
        const uint32_t iter = ntohl(as.kdf_iter);
        if (iter < HS_KDF_ITER_MIN || iter > HS_KDF_ITER_MAX) {
            why = "对方下发的密码参数无效";
            return AuthOutcome::Protocol;
        }
        std::string pw;
        if (!auth || !auth->password) {
            report("cancelled");
            why = "对方要求连接密码，但当前客户端无法输入";
            return AuthOutcome::Cancelled;
        }
        report("need_password");
        if (!auth->password(&pw, auth->ctx) || pw.empty()) {
            if (!pw.empty()) {
                volatile char *v = &pw[0];
                for (size_t i = 0; i < pw.size(); i++) v[i] = 0;
            }
            report("cancelled");
            why = "已取消输入连接密码";
            return AuthOutcome::Cancelled;
        }
        report("authenticating");

        uint8_t key[32] = {}, proof[32] = {};
        mivaraPasswordKey(pw, as.salt, sizeof(as.salt), iter, key);
        {
            volatile char *v = &pw[0];
            for (size_t i = 0; i < pw.size(); i++) v[i] = 0;
        }
        uint8_t bound[sizeof(KvmHello) + sizeof(HsRequestExtV3)];
        std::memcpy(bound, &hello, sizeof(hello));
        std::memcpy(bound + sizeof(hello), &ext, sizeof(ext));
        mivaraAuthProof(key, MIVARA_AUTH_PURPOSE_STITCH, as.nonce, bound,
                        sizeof(bound), proof);
        std::memset(key, 0, sizeof(key));

        HsAuthProof hp = {};
        hp.magic = htonl(TVHA_MAGIC);
        hp.version = htonl(TVHA_VERSION);
        std::memcpy(hp.proof, proof, sizeof(proof));
        std::memset(proof, 0, sizeof(proof));
        if (!kvmSendAll(s, &hp, sizeof(hp))) {
            why = "拼接授权应答发送失败";
            return AuthOutcome::Transport;
        }
    } else {
        report("waiting");
    }

    HsResponseV2 res = {};
    const int rv = kvmRecvAllPoll(s, &res, sizeof(res), running, 35000);
    if (rv == 0) {
        why = (mode == AUTH_MODE_MANUAL)
                  ? "对方未在 30 秒内确认拼接请求"
                  : "对方未在 35 秒内回应拼接授权";
        return AuthOutcome::Transport;
    }
    if (rv < 0) {
        why = "拼接授权过程中连接已断开";
        return AuthOutcome::Transport;
    }
    if (ntohl(res.magic) != TVHS_MAGIC || ntohl(res.version) != TVHS_VERSION) {
        why = "拼接授权应答无效";
        return AuthOutcome::Protocol;
    }
    status = ntohl(res.status);
    return status == HS_OK ? AuthOutcome::Ok : AuthOutcome::Status;
}

bool kvmRecvAll(kvm_sock_t s, void *buf, size_t len,
                const std::atomic<bool> &running, uint64_t deadline_ms,
                KvmHeartbeatState *hb, size_t *got, bool *timed_out,
                std::mutex *tx)
{
    char *p = (char *)buf;
    size_t off = 0;
    const uint64_t start = kvmNowMs();
    uint64_t last_iter = start;
    const auto finish = [&](bool ok, bool deadline) {
        if (got) *got = off;
        if (timed_out) *timed_out = deadline;
        return ok;
    };
    const auto fail = [&](const char *why, int err) {
        if (hb) {
            hb->reason = why;
            hb->err = err;
        }
        return finish(false, false);
    };
    while (off < len) {
        const uint64_t now = kvmNowMs();
        /* Suspend/starve: the process, not the peer, was frozen. Restart the
         * heartbeat clock; a socket that really died still errors below. */
        if (hb && hb->enabled && now - last_iter > 3000) {
            hb->last_rx = now;
            hb->next_tx = now + KVM_HEARTBEAT_INTERVAL_MS;
        }
        last_iter = now;
        if (!running.load()) return fail("local_stop", 0);
        if (deadline_ms && now - start > deadline_ms) {
            if (hb) hb->reason = "deadline";
            return finish(false, true);
        }
        if (hb && hb->enabled) {
            if (now - hb->last_rx >= KVM_HEARTBEAT_TIMEOUT_MS)
                return fail("heartbeat_timeout", 0);
            if (now >= hb->next_tx) {
                KvmEvent ping = {};
                ping.type = htonl(KVM_EV_PING);
                bool sent;
                if (tx) {
                    std::lock_guard<std::mutex> lk(*tx);
                    sent = kvmSendAll(s, &ping, sizeof(ping));
                } else {
                    sent = kvmSendAll(s, &ping, sizeof(ping));
                }
                if (!sent) return fail("ping_send_error", kvmLastSockError());
                hb->next_tx = now + KVM_HEARTBEAT_INTERVAL_MS;
            }
        }
        const int n = (int)recv(s, p + off, (int)(len - off), 0);
        if (n == 0) return fail("peer_closed", 0);
        if (n < 0) {
            const int err = kvmLastSockError();
            if (kvmSockTimedOut(err) || kvmSockWouldBlock(err) ||
                kvmSockInterrupted(err))
                continue;
            return fail("recv_error", err);
        }
        if (hb) hb->last_rx = kvmNowMs();
        off += (size_t)n;
    }
    return finish(true, false);
}

bool kvmPeerClosed(kvm_sock_t s)
{
    /* Drain (rather than MSG_PEEK) so a FIN queued behind protocol-violating
     * bytes is still seen; nothing legitimate arrives before the reply. The
     * loop is bounded so a peer that keeps sending cannot stall this check. */
    char sink[256];
    for (int i = 0; i < 8; i++) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval zero = {0, 0};
#ifdef _WIN32
        const int sel = select(0, &rd, nullptr, nullptr, &zero);
#else
        const int sel = select((int)s + 1, &rd, nullptr, nullptr, &zero);
#endif
        if (sel <= 0) return false;
        if (!FD_ISSET(s, &rd)) return false;
        const int n = (int)recv(s, sink, sizeof(sink), 0);
        if (n == 0) return true; /* orderly close / FIN */
        if (n < 0) {
            const int err = kvmLastSockError();
            return !(kvmSockWouldBlock(err) || kvmSockTimedOut(err) ||
                     kvmSockInterrupted(err));
        }
    }
    return false;
}
