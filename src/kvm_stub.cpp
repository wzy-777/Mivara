/**
 * KVM_STUB.CPP – stitch backend for platforms without an input backend
 * (currently macOS). Keeps the public kvm.h symbols linkable; the 8084
 * listener reports "not supported" and the controller returns
 * KVM_RUN_UNSUPPORTED, exactly like the previous non-Windows stubs.
 */
#include "kvm_core.h"
#include "kvm_target.h"

#if !defined(_WIN32) && !defined(__linux__)

namespace {

bool stubInit() { return false; }
void stubMetrics(int &x, int &y, uint32_t &w, uint32_t &h)
{
    x = y = 0;
    w = h = 1;
}
void stubMove(int, int, uint32_t, uint32_t, int, int) {}
void stubButton(uint32_t, bool) {}
void stubWheel(int, bool) {}
void stubKey(bool, uint32_t, uint32_t, bool) {}
void stubReleaseAll() {}

} /* namespace */

const KvmTargetOps &kvmTargetOps()
{
    static const KvmTargetOps ops = {
        stubInit, stubMetrics, 0, stubMove, stubButton, stubWheel, stubKey,
        stubReleaseAll, nullptr, nullptr, nullptr, nullptr
    };
    return ops;
}

int kvmRunController(const char *peer_ip, int peer_port, int side,
                     const std::atomic<bool> &running,
                     const KvmClientAuth *auth)
{
    (void)peer_ip;
    (void)peer_port;
    (void)side;
    (void)running;
    (void)auth;
    return KVM_RUN_UNSUPPORTED;
}

int kvmRunControllerResilient(const char *peer_ip, int peer_port, int side,
                              const std::atomic<bool> &running,
                              const KvmClientAuth *auth)
{
    return kvmRunController(peer_ip, peer_port, side, running, auth);
}

void kvmSetControllerSide(int side)
{
    (void)side;
}

#endif /* !_WIN32 && !__linux__ */
