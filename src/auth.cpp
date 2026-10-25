/**
 * AUTH.CPP – see auth.h for the security model.
 *
 * Contains a from-spec implementation of SHA-256 (FIPS 180-4), HMAC
 * (RFC 2104) and PBKDF2 (RFC 8018), plus the config/policy helpers shared by
 * the GUI, the sender and the receiver. Self-test vectors live behind
 * -DAUTH_TEST_MAIN (the makefile never sets it):
 *
 *   g++ -std=c++17 -DAUTH_TEST_MAIN src/auth.cpp -o build/auth_test
 *   ./build/auth_test
 */
#include "auth.h"

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <mutex>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * SHA-256 (FIPS 180-4)
 * ══════════════════════════════════════════════════════════════════════════ */
namespace {

const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t total;          /* bytes hashed so far */
    uint8_t  buf[64];
    size_t   buflen;
};

void sha256Init(Sha256Ctx &c)
{
    c.h[0] = 0x6a09e667u; c.h[1] = 0xbb67ae85u;
    c.h[2] = 0x3c6ef372u; c.h[3] = 0xa54ff53au;
    c.h[4] = 0x510e527fu; c.h[5] = 0x9b05688cu;
    c.h[6] = 0x1f83d9abu; c.h[7] = 0x5be0cd19u;
    c.total = 0;
    c.buflen = 0;
}

void sha256Block(Sha256Ctx &c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                            (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                            (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

void sha256Update(Sha256Ctx &c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c.total += len;
    if (c.buflen) {
        const size_t take = std::min(len, sizeof(c.buf) - c.buflen);
        std::memcpy(c.buf + c.buflen, p, take);
        c.buflen += take;
        p += take;
        len -= take;
        if (c.buflen == sizeof(c.buf)) {
            sha256Block(c, c.buf);
            c.buflen = 0;
        }
    }
    while (len >= 64) {
        sha256Block(c, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        std::memcpy(c.buf, p, len);
        c.buflen = len;
    }
}

void sha256Final(Sha256Ctx &c, uint8_t out[32])
{
    const uint64_t bits = c.total * 8u;
    const uint8_t pad = 0x80;
    sha256Update(c, &pad, 1);
    static const uint8_t zero = 0;
    while (c.buflen != 56)
        sha256Update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
    /* Feed the length without counting it (sha256Update would add to total,
     * which no longer matters — the digest is already determined). */
    std::memcpy(c.buf + 56, lenbuf, 8);
    sha256Block(c, c.buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c.h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c.h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c.h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c.h[i]);
    }
}

void hmacSha256Raw(const uint8_t *key, size_t key_len,
                   const void *data, size_t len, uint8_t out[32])
{
    uint8_t k[64];
    std::memset(k, 0, sizeof(k));
    if (key_len > 64) {
        uint8_t kh[32];
        mivaraSha256(key, key_len, kh);
        std::memcpy(k, kh, 32);
    } else if (key_len) {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    uint8_t inner[32];
    {
        Sha256Ctx c;
        sha256Init(c);
        sha256Update(c, ipad, 64);
        sha256Update(c, data, len);
        sha256Final(c, inner);
    }
    Sha256Ctx c;
    sha256Init(c);
    sha256Update(c, opad, 64);
    sha256Update(c, inner, 32);
    sha256Final(c, out);
}

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

} /* namespace */

void mivaraSha256(const void *data, size_t len, uint8_t out[32])
{
    Sha256Ctx c;
    sha256Init(c);
    sha256Update(c, data, len);
    sha256Final(c, out);
}

void mivaraHmacSha256(const uint8_t *key, size_t key_len,
                      const void *data, size_t len, uint8_t out[32])
{
    hmacSha256Raw(key, key_len, data, len, out);
}

void mivaraPbkdf2Sha256(const void *password, size_t pw_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations, uint8_t out[32])
{
    /* dkLen == hLen == 32, so exactly one PBKDF2 block is produced.
     * U1 = PRF(P, S || INT(1)); the hash of that salt block happens inside the
     * HMAC, not before it (RFC 8018 §5.2). */
    uint8_t saltblock[60 + 4];
    const uint8_t *sb = salt;
    size_t sb_len = salt_len;
    if (salt_len <= sizeof(saltblock) - 4) {
        std::memcpy(saltblock, salt, salt_len);
        saltblock[salt_len + 0] = 0;
        saltblock[salt_len + 1] = 0;
        saltblock[salt_len + 2] = 0;
        saltblock[salt_len + 3] = 1;
        sb = saltblock;
        sb_len = salt_len + 4;
    }
    uint8_t u[32], acc[32];
    hmacSha256Raw((const uint8_t *)password, pw_len, sb, sb_len, u);
    std::memcpy(acc, u, 32);
    for (uint32_t i = 1; i < iterations; i++) {
        hmacSha256Raw((const uint8_t *)password, pw_len, u, 32, u);
        for (int j = 0; j < 32; j++) acc[j] ^= u[j];
    }
    std::memcpy(out, acc, 32);
}

bool mivaraConstEq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

bool mivaraRandomBytes(uint8_t *out, size_t n)
{
#ifdef _WIN32
    /* RtlGenRandom (SystemFunction036) from advapi32 — already linked by all
     * three binaries; no extra import library or DLL is needed. */
    typedef BOOLEAN (WINAPI *RtlGenRandomFn)(PVOID, ULONG);
    /* Function-local static: C++11 guarantees exactly-once, thread-safe
     * initialisation, so concurrent handshake workers cannot observe a
     * half-initialised pointer. */
    static RtlGenRandomFn fn = []() -> RtlGenRandomFn {
        HMODULE adv = LoadLibraryA("advapi32.dll");
        return adv ? (RtlGenRandomFn)GetProcAddress(adv, "SystemFunction036")
                   : nullptr;
    }();
    if (!fn) return false;
    while (n) {
        const ULONG chunk = (ULONG)(n > 0x10000000u ? 0x10000000u : n);
        if (!fn(out, chunk)) return false;
        out += chunk;
        n -= chunk;
    }
    return true;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < n) {
        const ssize_t r = read(fd, out + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n;
#endif
}

/* ══════════════════════════════════════════════════════════════════════════
 * policy
 * ══════════════════════════════════════════════════════════════════════════ */

const char *mivaraAuthModeKey(MivaraAuthMode mode)
{
    switch (mode) {
    case MIVARA_AUTH_OPEN:     return "open";
    case MIVARA_AUTH_PASSWORD: return "password";
    default:                   return "manual";
    }
}

bool mivaraAuthModeFromKey(const std::string &key, MivaraAuthMode &out)
{
    if (key == "open")     { out = MIVARA_AUTH_OPEN;     return true; }
    if (key == "password") { out = MIVARA_AUTH_PASSWORD; return true; }
    if (key == "manual")   { out = MIVARA_AUTH_MANUAL;   return true; }
    return false;           /* caller must fall back to manual, never open */
}

bool mivaraPasswordValid(const std::string &pw)
{
    if (pw.size() < 4 || pw.size() > 8) return false;
    bool nonSpace = false;
    for (unsigned char c : pw) {
        if (c < 0x20 || c > 0x7e) return false;
        if (c != ' ' && c != '\t') nonSpace = true;
    }
    return nonSpace;
}

const char *mivaraPasswordRule(void)
{
    return "密码需为 4～8 个字符，不能全为空白（字母不分大小写）";
}

void mivaraPasswordFoldAscii(std::string &pw)
{
    /* Plain ASCII only: std::tolower would follow the process locale and can
     * map non-ASCII bytes (e.g. the Turkish dotless i), while the wire rule
     * and PBKDF2 input must be identical on every platform. */
    for (char &ch : pw) {
        const unsigned char c = (unsigned char)ch;
        if (c >= 'A' && c <= 'Z') ch = (char)(c - 'A' + 'a');
    }
}

void mivaraPasswordKey(const std::string &pw, const uint8_t *salt,
                       size_t salt_len, uint32_t iterations, uint8_t out[32])
{
    /* The folded copy is a second plaintext buffer; wipe it before returning.
     * The caller's string is deliberately left untouched so its own wipe (and
     * any UI echo) still sees exactly what the user typed. */
    std::string folded = pw;
    mivaraPasswordFoldAscii(folded);
    mivaraPbkdf2Sha256(folded.data(), folded.size(), salt, salt_len,
                       iterations, out);
    if (!folded.empty()) {
        mivaraSecureZero(&folded[0], folded.size());
        folded.clear();
    }
}

bool mivaraAuthRecordUsable(uint32_t iterations, std::string *why)
{
    if (iterations < 10000 || iterations > 1000000) {
        if (why) *why = "KDF 参数损坏";
        return false;
    }
    return true;
}

bool mivaraAuthRecordUsable(const uint8_t hash[32], const uint8_t salt[16],
                            uint32_t iterations, std::string *why)
{
    if (!mivaraAuthRecordUsable(iterations, why)) return false;
    bool nz = false;
    for (int i = 0; i < 32; i++) if (hash[i]) { nz = true; break; }
    if (!nz) {
        if (why) *why = "密码记录损坏";
        return false;
    }
    (void)salt;
    return true;
}

bool mivaraAuthMakeRecord(const std::string &pw, uint8_t salt[16],
                          uint8_t hash[32], uint32_t &iterations)
{
    if (!mivaraPasswordValid(pw)) return false;
    if (!mivaraRandomBytes(salt, 16)) return false;
    iterations = MIVARA_AUTH_DEFAULT_ITER;
    mivaraPasswordKey(pw, salt, 16, iterations, hash);
    return true;
}

std::string mivaraHexEncode(const uint8_t *p, size_t n)
{
    static const char *hex = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s.push_back(hex[p[i] >> 4]);
        s.push_back(hex[p[i] & 0xf]);
    }
    return s;
}

bool mivaraHexDecode(const std::string &s, uint8_t *out, size_t n)
{
    if (s.size() != n * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; i++) {
        const int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void mivaraSecureZero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

void mivaraAuthPurposeKey(const uint8_t record[32], MivaraAuthPurpose purpose,
                          uint8_t out[32])
{
    /* Purpose-separate the MAC key from the stored verifier: the config value
     * is never used directly as a key, and a 8081 key can never verify a 8084
     * proof (or vice versa). */
    static const char kLabel[] = "Mivara/v1/auth-key";
    uint8_t msg[sizeof(kLabel) - 1 + 1];
    std::memcpy(msg, kLabel, sizeof(kLabel) - 1);
    msg[sizeof(kLabel) - 1] = (uint8_t)purpose;
    mivaraHmacSha256(record, 32, msg, sizeof(msg), out);
    mivaraSecureZero(msg, sizeof(msg));
}

void mivaraAuthProof(const uint8_t record[32], MivaraAuthPurpose purpose,
                     const uint8_t nonce[32], const void *bound,
                     size_t bound_len, uint8_t out[32])
{
    static const char kLabel[] = "Mivara/v1/auth-proof";
    uint8_t key[32];
    mivaraAuthPurposeKey(record, purpose, key);

    uint8_t msg[sizeof(kLabel) - 1 + 1 + 32 + 256];
    size_t n = 0;
    std::memcpy(msg, kLabel, sizeof(kLabel) - 1);
    n += sizeof(kLabel) - 1;
    msg[n++] = (uint8_t)purpose;
    std::memcpy(msg + n, nonce, 32);
    n += 32;
    if (bound_len > sizeof(msg) - n) bound_len = sizeof(msg) - n;
    if (bound_len) std::memcpy(msg + n, bound, bound_len);
    n += bound_len;
    mivaraHmacSha256(key, 32, msg, n, out);

    mivaraSecureZero(key, sizeof(key));
    mivaraSecureZero(msg, sizeof(msg));
}

/* ══════════════════════════════════════════════════════════════════════════
 * session token (receiver side)
 * ══════════════════════════════════════════════════════════════════════════ */
namespace {
std::mutex &tokenMutex()
{
    static std::mutex m;
    return m;
}
uint8_t g_session_token[32] = {};
} /* namespace */

void mivaraSessionTokenSet(const uint8_t token[32])
{
    std::lock_guard<std::mutex> lk(tokenMutex());
    std::memcpy(g_session_token, token, 32);
}

bool mivaraSessionTokenCheck(const uint8_t token[32])
{
    std::lock_guard<std::mutex> lk(tokenMutex());
    bool any = false;
    for (int i = 0; i < 32; i++)
        if (g_session_token[i]) { any = true; break; }
    if (!any) return false;               /* no active session */
    return mivaraConstEq(g_session_token, token, 32);
}

uint64_t mivaraSessionTokenInstall(const uint8_t token[32])
{
    std::lock_guard<std::mutex> lk(tokenMutex());
    std::memcpy(g_session_token, token, 32);
    static uint64_t gen = 0;
    return ++gen;
}

bool mivaraSessionTokenClearIf(const uint8_t token[32])
{
    std::lock_guard<std::mutex> lk(tokenMutex());
    if (!mivaraConstEq(g_session_token, token, 32)) return false;
    std::memset(g_session_token, 0, sizeof(g_session_token));
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * cross-service session gate (8081 video + 8084 stitch)
 * ══════════════════════════════════════════════════════════════════════════ */
namespace {

struct SessionSlot {
    uint64_t          epoch = 0;                  /* 0 = free */
    MivaraSessionKind kind = MIVARA_SESSION_NONE;
    bool              active = false;
    int64_t           expires_ms = 0;             /* 0 = never (active) */
};

std::mutex &sessionMutex()
{
    static std::mutex m;
    return m;
}

SessionSlot &sessionSlot()
{
    static SessionSlot s;
    return s;
}

uint64_t sessionEpochCounter = 0;

int64_t monoMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* Caller holds sessionMutex(). An unpromoted claim that outlived its budget
 * stops blocking the port; the caller that owns it will fail to promote. */
void reapExpiredClaim(SessionSlot &s)
{
    if (s.epoch && !s.active && s.expires_ms && monoMs() >= s.expires_ms) {
        s = SessionSlot{};
    }
}

} /* namespace */

bool mivaraSessionClaim(MivaraSessionKind kind, int ttl_ms,
                        MivaraSessionLease *out_lease)
{
    if (!out_lease || kind == MIVARA_SESSION_NONE) return false;
    std::lock_guard<std::mutex> lk(sessionMutex());
    SessionSlot &s = sessionSlot();
    reapExpiredClaim(s);
    if (s.epoch) return false;

    if (++sessionEpochCounter == 0) ++sessionEpochCounter;
    s.epoch = sessionEpochCounter;
    s.kind = kind;
    s.active = false;
    s.expires_ms = monoMs() + (ttl_ms > 0 ? ttl_ms : 120000);
    out_lease->epoch = s.epoch;
    out_lease->kind = kind;
    return true;
}

bool mivaraSessionPromote(const MivaraSessionLease &lease)
{
    std::lock_guard<std::mutex> lk(sessionMutex());
    SessionSlot &s = sessionSlot();
    reapExpiredClaim(s);
    if (!s.epoch || s.epoch != lease.epoch) return false;
    s.active = true;
    s.expires_ms = 0;
    return true;
}

void mivaraSessionRelease(const MivaraSessionLease &lease)
{
    std::lock_guard<std::mutex> lk(sessionMutex());
    SessionSlot &s = sessionSlot();
    if (s.epoch && s.epoch == lease.epoch) s = SessionSlot{};
}

MivaraSessionKind mivaraSessionOwner()
{
    std::lock_guard<std::mutex> lk(sessionMutex());
    SessionSlot &s = sessionSlot();
    reapExpiredClaim(s);
    return s.epoch ? s.kind : MIVARA_SESSION_NONE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * per-source failure throttle
 * ══════════════════════════════════════════════════════════════════════════ */

MivaraAuthThrottle::Entry &MivaraAuthThrottle::slot(const std::string &ip)
{
    for (int i = 0; i < kCap; i++)
        if (_e[i].ip == ip) return _e[i];
    /* Empty slot first, else the least recently touched one. Capacity is a
     * hard bound: an attacker rotating addresses cannot exhaust memory. */
    Entry *victim = nullptr;
    for (int i = 0; i < kCap; i++) {
        if (_e[i].ip.empty()) { victim = &_e[i]; break; }
        if (!victim || _e[i].last_ms < victim->last_ms) victim = &_e[i];
    }
    victim->ip = ip;
    victim->fails = 0;
    victim->until_ms = 0;
    victim->last_ms = 0;
    return *victim;
}

const MivaraAuthThrottle::Entry *MivaraAuthThrottle::find(const std::string &ip) const
{
    for (int i = 0; i < kCap; i++)
        if (_e[i].ip == ip) return &_e[i];
    return nullptr;
}

int MivaraAuthThrottle::retryAfter(const std::string &ip) const
{
    const Entry *e = find(ip);
    if (!e) return 0;
    const int64_t left = e->until_ms - nowMs();
    return left > 0 ? (int)((left + 999) / 1000) : 0;
}

int MivaraAuthThrottle::applyCooldown(Entry &e) const
{
    /* First few mistakes are free (typos happen). From the 4th failure the
     * cooldown doubles: 2 s, 4 s, 8 s … capped at 60 s. */
    if (e.fails >= 4) {
        int shift = e.fails - 4;
        if (shift > 5) shift = 5;
        int cd = 2 << shift;
        if (cd > 60) cd = 60;
        e.until_ms = nowMs() + (int64_t)cd * 1000;
        return cd;
    }
    return 0;
}

int MivaraAuthThrottle::noteFailure(const std::string &ip)
{
    Entry &e = slot(ip);
    e.last_ms = nowMs();
    if (e.fails < 1000) e.fails++;
    return applyCooldown(e);
}

int MivaraAuthThrottle::noteAbuse(const std::string &ip)
{
    Entry &e = slot(ip);
    e.last_ms = nowMs();
    if (e.fails < 1000) e.fails += 2;      /* malformed proof counts double */
    return applyCooldown(e);
}

void MivaraAuthThrottle::noteSuccess(const std::string &ip)
{
    for (int i = 0; i < kCap; i++) {
        if (_e[i].ip == ip) {
            _e[i].fails = 0;
            _e[i].until_ms = 0;
            _e[i].last_ms = nowMs();
            return;
        }
    }
}

/* One process-wide bucket: 8081 and 8084 share the same password, so a source
 * must not get two budgets by alternating ports. */
namespace {
MivaraAuthThrottle &sharedThrottle()
{
    static MivaraAuthThrottle t;
    return t;
}
std::mutex &sharedThrottleMutex()
{
    static std::mutex m;
    return m;
}
} /* namespace */

int mivaraAuthThrottleRetryAfter(const std::string &ip)
{
    std::lock_guard<std::mutex> lk(sharedThrottleMutex());
    return sharedThrottle().retryAfter(ip);
}

int mivaraAuthThrottleNoteFailure(const std::string &ip)
{
    std::lock_guard<std::mutex> lk(sharedThrottleMutex());
    return sharedThrottle().noteFailure(ip);
}

int mivaraAuthThrottleNoteAbuse(const std::string &ip)
{
    std::lock_guard<std::mutex> lk(sharedThrottleMutex());
    return sharedThrottle().noteAbuse(ip);
}

void mivaraAuthThrottleNoteSuccess(const std::string &ip)
{
    std::lock_guard<std::mutex> lk(sharedThrottleMutex());
    sharedThrottle().noteSuccess(ip);
}

/* ══════════════════════════════════════════════════════════════════════════
 * self-test (compile with -DAUTH_TEST_MAIN; never part of the normal build)
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef AUTH_TEST_MAIN
static int fails = 0;

static void expectHex(const char *what, const uint8_t *got, const char *want)
{
    const std::string g = mivaraHexEncode(got, 32);
    if (g != want) {
        std::printf("FAIL %s\n  got  %s\n  want %s\n", what, g.c_str(), want);
        fails++;
    } else {
        std::printf("ok   %s\n", what);
    }
}

int main()
{
    /* FIPS 180-4 examples */
    uint8_t d[32];
    mivaraSha256("abc", 3, d);
    expectHex("sha256('abc')", d,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    mivaraSha256("", 0, d);
    expectHex("sha256('')", d,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char *longmsg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    mivaraSha256(longmsg, std::strlen(longmsg), d);
    expectHex("sha256(2-block)", d,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* RFC 4231 test case 2 */
    {
        const uint8_t key[4] = {'J', 'e', 'f', 'e'};
        mivaraHmacSha256(key, 4, "what do ya want for nothing?", 28, d);
        expectHex("hmac-sha256 rfc4231#2", d,
                  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    /* RFC 4231 test case 6: key longer than the block size */
    {
        uint8_t key[131];
        std::memset(key, 0xaa, sizeof(key));
        mivaraHmacSha256(key, sizeof(key),
                         "Test Using Larger Than Block-Size Key - Hash Key First",
                         54, d);
        expectHex("hmac-sha256 rfc4231#6", d,
                  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }
    /* PBKDF2-HMAC-SHA256 test vectors (RFC 7914 era, widely published) */
    {
        const uint8_t salt[4] = {'s', 'a', 'l', 't'};
        mivaraPbkdf2Sha256("password", 8, salt, 4, 1, d);
        expectHex("pbkdf2-sha256 iter=1", d,
                  "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
        mivaraPbkdf2Sha256("password", 8, salt, 4, 4096, d);
        expectHex("pbkdf2-sha256 iter=4096", d,
                  "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    }
    /* Purpose-separated proof: fixed vector + cross-purpose separation. This
     * pins the wire formula shared by sender, receiver and kvm. */
    {
        uint8_t rec[32], nonce[32], pv[32], ps[32];
        std::memset(rec, 0x11, sizeof(rec));
        std::memset(nonce, 0x22, sizeof(nonce));
        mivaraAuthProof(rec, MIVARA_AUTH_PURPOSE_VIDEO, nonce, "abc", 3, pv);
        mivaraAuthProof(rec, MIVARA_AUTH_PURPOSE_STITCH, nonce, "abc", 3, ps);
        expectHex("auth proof (video)", pv,
                  "00f1095a1d7f9aad6c81f554d9a3bb5b952cd54f1195e743b193354314ba9011");
        expectHex("auth proof (stitch)", ps,
                  "b936f3c3a1ead20cc6793c3d299fb531faae9517a1dc3a68c8adbbca28c7cd96");
        std::printf("proof purpose separation: %s\n",
                    !mivaraConstEq(pv, ps, sizeof(pv)) ? "ok" : "FAIL");
    }
    {
        std::printf("rule check: %s\n",
                    mivaraPasswordValid("abcd") && mivaraPasswordValid("AbCd") &&
                    mivaraPasswordValid("!@#$") && mivaraPasswordValid("a b c d") &&
                    !mivaraPasswordValid("abc") && !mivaraPasswordValid("        ") &&
                    !mivaraPasswordValid("abcdefghi") ? "ok" : "FAIL");
        uint8_t salt[16], hash[32];
        uint32_t it = 0;
        if (!mivaraAuthMakeRecord("test1234", salt, hash, it)) {
            std::printf("FAIL record\n");
            fails++;
        }
    }
    /* Case-insensitive passwords: the fold is ASCII-only and idempotent, the
     * derived key ignores case, and the raw PBKDF2 primitive stays RFC-exact
     * (a regression that folds inside PBKDF2 itself would pass every vector
     * above because they are all lower-case). */
    {
        auto check = [](const char *what, bool ok) {
            if (ok) {
                std::printf("ok   %s\n", what);
            } else {
                std::printf("FAIL %s\n", what);
                fails++;
            }
        };

        std::string f = " AbC1! ";
        mivaraPasswordFoldAscii(f);
        const std::string once = f;
        mivaraPasswordFoldAscii(f);          /* second pass must be a no-op */
        std::string hi = "\xC3\x84";
        mivaraPasswordFoldAscii(hi);
        check("password fold is ASCII-only, idempotent and does not trim",
              once == " abc1! " && f == once && hi == "\xC3\x84");

        const uint8_t fsalt[4] = {'s', 'a', 'l', 't'};
        uint8_t k1[32], k2[32], k3[32];
        mivaraPasswordKey("PassWord", fsalt, sizeof(fsalt), 1, k1);
        mivaraPasswordKey("password", fsalt, sizeof(fsalt), 1, k2);
        mivaraPasswordKey("passwore", fsalt, sizeof(fsalt), 1, k3);
        check("password key ignores ASCII case",
              mivaraConstEq(k1, k2, sizeof(k1)) &&
              !mivaraConstEq(k1, k3, sizeof(k1)));

        uint8_t raw1[32], raw2[32];
        mivaraPbkdf2Sha256("Password", 8, fsalt, sizeof(fsalt), 1, raw1);
        mivaraPbkdf2Sha256("password", 8, fsalt, sizeof(fsalt), 1, raw2);
        check("pbkdf2 primitive stays case-sensitive",
              !mivaraConstEq(raw1, raw2, sizeof(raw1)));

        uint8_t rsalt[16], rhash[32], rcheck[32];
        uint32_t rit = 0;
        if (mivaraAuthMakeRecord("AbCd", rsalt, rhash, rit)) {
            mivaraPasswordKey("abcd", rsalt, sizeof(rsalt), rit, rcheck);
            const bool lower_ok = mivaraConstEq(rhash, rcheck, sizeof(rhash));
            mivaraPasswordKey("ABCD", rsalt, sizeof(rsalt), rit, rcheck);
            check("record matches any case",
                  lower_ok && mivaraConstEq(rhash, rcheck, sizeof(rhash)));
        } else {
            std::printf("FAIL record case test\n");
            fails++;
        }
    }
    std::printf(fails ? "AUTH SELF-TEST FAILED (%d)\n" : "AUTH SELF-TEST OK\n", fails);
    return fails ? 1 : 0;
}
#endif
