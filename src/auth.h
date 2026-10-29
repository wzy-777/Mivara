/**
 * AUTH.H – shared connection-authorization primitives (sender / receiver / app)
 *
 * Security model
 * --------------
 * The receive side offers exactly one of three policies (see the PRD):
 *   open      – no password, no prompt (explicitly risky, never the default)
 *   password  – challenge/response with a 4–8 character password (ASCII
 *               letters are case-insensitive)
 *   manual    – a human on the receiver confirms every new connection
 *
 * Passwords are never sent over the wire and never stored in clear. The app
 * stores only PBKDF2-HMAC-SHA256(password, salt, iterations) in config.ini;
 * the receiver reads that record and verifies a per-connection HMAC proof
 * (see `mivaraAuthProof` below for the exact, purpose-separated formula).
 *
 * The KDF key and the salt are public inputs to the exchange, the nonce is
 * fresh 32-byte randomness for every connection, and the bound bytes are the
 * handshake request itself. A recorded proof cannot be replayed (new nonce),
 * cannot be reused with different stream parameters (bound bytes), and carries
 * a purpose tag so 8081 and 8084 transcripts are not interchangeable.
 *
 * These are standard primitives (FIPS 180-4 SHA-256, RFC 2104 HMAC,
 * RFC 8018 PBKDF2). Nothing here invents a cipher or a custom construction.
 * This is connection admission, NOT stream encryption: the video payload is
 * not encrypted by Mivara.
 */
#ifndef MIVARA_AUTH_H
#define MIVARA_AUTH_H

#include <cstddef>
#include <cstdint>
#include <string>

/* ── standard primitives ─────────────────────────────────────────────────── */

/** SHA-256 of one buffer. */
void mivaraSha256(const void *data, size_t len, uint8_t out[32]);

/** Incremental SHA-256 for streaming a file through the drop channel.
 *  The context is plain data; the caller owns it. Do not copy it mid-stream. */
struct MivaraSha256Ctx {
    uint32_t h[8];
    uint64_t total;          /* bytes hashed so far */
    uint8_t  buf[64];
    size_t   buflen;
};
void mivaraSha256Init(MivaraSha256Ctx *c);
void mivaraSha256Update(MivaraSha256Ctx *c, const void *data, size_t len);
void mivaraSha256Final(MivaraSha256Ctx *c, uint8_t out[32]);

/** HMAC-SHA256 (RFC 2104). */
void mivaraHmacSha256(const uint8_t *key, size_t key_len,
                      const void *data, size_t len, uint8_t out[32]);

/** PBKDF2-HMAC-SHA256 with a 32-byte derived key (RFC 8018).
 *  Raw primitive: it does NOT normalise the password. Password-derived keys
 *  must go through `mivaraPasswordKey` so the case-folding rule is shared. */
void mivaraPbkdf2Sha256(const void *password, size_t pw_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations, uint8_t out[32]);

/** Constant-time comparison (length is public). */
bool mivaraConstEq(const uint8_t *a, const uint8_t *b, size_t n);

/** Cryptographically secure random bytes. Returns false if the platform
 *  source is unavailable — callers must fail closed. */
bool mivaraRandomBytes(uint8_t *out, size_t n);

/* ── policy (config.ini spelling) ────────────────────────────────────────── */

enum MivaraAuthMode : uint32_t {
    MIVARA_AUTH_OPEN     = 0,
    MIVARA_AUTH_PASSWORD = 1,
    MIVARA_AUTH_MANUAL   = 2
};

/** "open" | "password" | "manual". Unknown input falls back to manual. */
const char *mivaraAuthModeKey(MivaraAuthMode mode);
bool mivaraAuthModeFromKey(const std::string &key, MivaraAuthMode &out);

/** 4–8 printable ASCII characters, at least one non-space. Matching is
 *  case-insensitive: derive keys with `mivaraPasswordKey`, never the raw
 *  PBKDF2 primitive. */
bool mivaraPasswordValid(const std::string &pw);
/** Human-readable reason a password was rejected (never echoes the password). */
const char *mivaraPasswordRule(void);

/** Fold ASCII 'A'–'Z' to 'a'–'z' in place. Locale-independent, no trimming:
 *  everything else (spaces, punctuation, non-ASCII bytes) is left untouched.
 *  Connection passwords are case-insensitive, so this is applied to the
 *  plaintext before every password PBKDF2 derivation. */
void mivaraPasswordFoldAscii(std::string &pw);

/** Derive the PBKDF2 record key for a typed password. Copies the plaintext,
 *  folds ASCII case, runs PBKDF2 and wipes the copy. Record creation, the
 *  8081 sender and the 8084 stitch controller must all use this function so
 *  they agree byte-for-byte; the receiver only ever holds the record. */
void mivaraPasswordKey(const std::string &pw, const uint8_t *salt,
                       size_t salt_len, uint32_t iterations, uint8_t out[32]);

/** True when a stored record is present and internally consistent.
 *  `why` receives a short reason when the record is unusable. */
bool mivaraAuthRecordUsable(const uint8_t hash[32], const uint8_t salt[16],
                            uint32_t iterations, std::string *why);
bool mivaraAuthRecordUsable(uint32_t iterations, std::string *why);

/** Build a fresh salt + PBKDF2 record for `pw`. */
bool mivaraAuthMakeRecord(const std::string &pw, uint8_t salt[16],
                          uint8_t hash[32], uint32_t &iterations);

/** Lowercase hex helpers (32/16-byte records in config.ini). */
std::string mivaraHexEncode(const uint8_t *p, size_t n);
bool mivaraHexDecode(const std::string &s, uint8_t *out, size_t n);

/** Wipe a secret buffer in a way the optimiser may not elide. */
void mivaraSecureZero(void *p, size_t n);

/* ── password proof ────────────────────────────────────────────────────────
 * `record` is the stored PBKDF2 output (or the value a sender derived from the
 * typed password — they are equal on success). The proof is domain-separated
 * per purpose so a 8081 transcript can never be replayed against 8084 and the
 * stored record is never used directly as a MAC key:
 *
 *   purpose_key = HMAC-SHA256(record, "Mivara/v1/auth-key"    || purpose)
 *   proof       = HMAC-SHA256(purpose_key,
 *                             "Mivara/v1/auth-proof" || purpose || nonce ||
 *                             bound)
 *
 * Residual risk (documented, not removed): an observer who captures a
 * challenge and proof can still test candidate passwords offline, and a
 * config.ini leak is credential-equivalent — a symmetric verifier cannot
 * prevent either. A future protocol should move to an audited aPAKE. */
enum MivaraAuthPurpose : uint8_t {
    MIVARA_AUTH_PURPOSE_VIDEO  = 1,   /* 8081 video / extend */
    MIVARA_AUTH_PURPOSE_STITCH = 2    /* 8084 stitch input  */
};

/** Iteration target for new records. Existing records keep the count they were
 *  created with (the handshake carries it), so raising this only strengthens
 *  passwords set from now on. */
#define MIVARA_AUTH_DEFAULT_ITER 300000u

void mivaraAuthPurposeKey(const uint8_t record[32], MivaraAuthPurpose purpose,
                          uint8_t out[32]);
void mivaraAuthProof(const uint8_t record[32], MivaraAuthPurpose purpose,
                     const uint8_t nonce[32], const void *bound,
                     size_t bound_len, uint8_t out[32]);

/* ── session token (receiver side) ─────────────────────────────────────────
 * An authorized 8081 session receives a random token; the 8082/8083 services
 * only serve connections that present it. `token` is always 32 bytes.
 * Setting the all-zero token clears the gate (no session -> no access). */

void mivaraSessionTokenSet(const uint8_t token[32]);
bool mivaraSessionTokenCheck(const uint8_t token[32]);
/** Install `token` and return its generation. Call before the grant is put on
 *  the wire so a sender that dials immediately cannot race the gate. */
uint64_t mivaraSessionTokenInstall(const uint8_t token[32]);
/** Clear the token only when it is still `token` — an ending session must not
 *  wipe a newer session's token. Returns true when it was cleared. */
bool mivaraSessionTokenClearIf(const uint8_t token[32]);

/* ── cross-service session gate (receiver side) ────────────────────────────
 * 8081 (video) and 8084 (stitch) must admit exactly one session per process.
 * A handshake claims the slot before authorization; promotion turns the claim
 * into the active session. An unpromoted claim expires on its own, so a peer
 * that stalls before proving a password cannot pin both ports indefinitely.
 *
 * Lives in auth.cpp (not receiver.cpp) because kvm.o is linked into both
 * sender and receiver and must not reference receiver symbols. */

enum MivaraSessionKind : uint32_t {
    MIVARA_SESSION_NONE   = 0,
    MIVARA_SESSION_VIDEO  = 1,
    MIVARA_SESSION_STITCH = 2
};

struct MivaraSessionLease {
    uint64_t         epoch = 0;                   /* 0 = empty */
    MivaraSessionKind kind = MIVARA_SESSION_NONE;
};

/** Claim the single slot. Returns false when another handshake or session owns
 *  it. `ttl_ms` bounds an unpromoted claim (<= 0 selects the default). */
bool mivaraSessionClaim(MivaraSessionKind kind, int ttl_ms,
                        MivaraSessionLease *out_lease);
/** Turn a claim into the active session (no longer expires). False when the
 *  claim expired or is not the current owner. */
bool mivaraSessionPromote(const MivaraSessionLease &lease);
/** Release exactly this claim; stale or foreign leases are ignored. */
void mivaraSessionRelease(const MivaraSessionLease &lease);
/** Kind currently holding the slot (pending or active), NONE when free. */
MivaraSessionKind mivaraSessionOwner();

/* ── failure throttling (receiver side, per source address) ──────────────── */

/** Sliding per-source failure state. Fixed capacity, no allocation storm:
 *  a hostile host cannot grow it without bound. */
class MivaraAuthThrottle
{
public:
    /** Seconds this source must still wait before another attempt, 0 = none. */
    int retryAfter(const std::string &ip) const;
    /** Count a rejected attempt; returns the cooldown now applied. */
    int noteFailure(const std::string &ip);
    void noteSuccess(const std::string &ip);
    /** A failed attempt that never reached verification (e.g. bad packet). */
    int noteAbuse(const std::string &ip);

private:
    struct Entry {
        std::string ip;
        int fails = 0;
        int64_t until_ms = 0;
        int64_t last_ms = 0;
    };
    static const int kCap = 32;
    mutable Entry _e[kCap];
    Entry &slot(const std::string &ip);
    const Entry *find(const std::string &ip) const;
    int applyCooldown(Entry &e) const;
};

/* Process-wide throttle shared by 8081 and 8084: the two paths use the same
 * connection password, so alternating ports must not grant two free budgets.
 * The implementation serialises access internally; callers must not hold
 * another lock while calling these. */
int  mivaraAuthThrottleRetryAfter(const std::string &ip);
int  mivaraAuthThrottleNoteFailure(const std::string &ip);
int  mivaraAuthThrottleNoteAbuse(const std::string &ip);
void mivaraAuthThrottleNoteSuccess(const std::string &ip);

#endif /* MIVARA_AUTH_H */
