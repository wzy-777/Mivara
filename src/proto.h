/**
 * PROTO.H – Mivara handshake & control protocol v3 (shared sender/receiver)
 *
 * Handshake over TCP 8081, ALL uint32 fields in network byte order.
 * v3 keeps the v2 48-byte request and 36-byte response on the wire and adds a
 * connection-authorization exchange between them:
 *
 *   sender   → receiver : HsRequestV2   (48 B, version = 3)
 *   receiver → sender   : HsAuthStart   (64 B; mode, KDF salt, 32-byte nonce)
 *   sender   → receiver : HsRequestExtV3 (48 B; sender device name)
 *                         HsAuthProof   (40 B; password mode only)
 *   receiver → sender   : HsResponseV2  (36 B)
 *                         HsResponseExtV3 (44 B; session token, OK only)
 *
 * An old (v2) peer is rejected with HS_ERR_VERSION and never reaches the
 * authorization exchange, so it cannot bypass the policy. A v2-shaped
 * display-size probe (width=height=0) is still answered without auth: it only
 * reveals the display size and starts no session.
 *
 * Stream packets after the handshake:
 *   DATA:    u32 pkt_size (network order) + payload
 *   CONTROL: u32 marker 0xFFFFFFFF + u32 ctrl_type + body
 *     CTRL_RESIZE=1    body = HsRequestV2 (stream geometry changed)
 *     CTRL_FORCE_IDR=2 body empty
 *     CTRL_EOS=3       body empty
 *     CTRL_RECONFIG=4  body = HsRequestV2 (runtime param change: codec/
 *                      quality/bitrate/fps/scale/chroma; receiver re-inits
 *                      the decoder before the next AU arrives)
 *
 * Both ends must run the same protocol version – a version mismatch is
 * reported explicitly on both sides (no black screen).
 */
#ifndef MIVARA_PROTO_H
#define MIVARA_PROTO_H

#include <cstdint>

#define TVHS_MAGIC   0x54564853u   /* 'TVHS' big-endian on the wire */
#define TVHS_VERSION 3u

/* v3 authorization frames. TVHA is the challenge (receiver → sender) and the
 * proof (sender → receiver); TVHX / TVHR are the request / response
 * extensions that follow them. */
#define TVHA_MAGIC   0x54564841u   /* 'TVHA' */
#define TVHA_VERSION 2u            /* v2 = purpose-separated proof (auth.h) */
#define TVHX_MAGIC   0x54564858u   /* 'TVHX' request extension  */
#define TVHR_MAGIC   0x54564852u   /* 'TVHR' response extension */

/* HsAuthStart.mode */
enum AuthMode : uint32_t {
    AUTH_MODE_OPEN     = 0,
    AUTH_MODE_PASSWORD = 1,
    AUTH_MODE_MANUAL   = 2
};

/* Bounds for the KDF parameters accepted off the wire. The GUI stores
 * 120000; anything outside this range is a corrupt record, not a negotiation. */
#define HS_KDF_ITER_MIN 10000u
#define HS_KDF_ITER_MAX 1000000u

/* Session token width — the 8082/8083 sidecar services require it. */
#define HS_TOKEN_BYTES 32

/* Control stream marker: a DATA pkt_size of 0xFFFFFFFF means control. */
#define CTRL_MARKER  0xFFFFFFFFu

/* HsRequestV2.quality: "encoder default" (23 h264/h265, 35 av1). */
#define HS_QUALITY_DEFAULT 0xFFFFFFFFu

/* CodecId values – also used as HsRequestV2.codec / HsResponseV2.codec. */
enum CodecId : uint32_t {
    CODEC_RAW  = 0,
    CODEC_RLE  = 1,
    CODEC_H264 = 2,
    CODEC_H265 = 3,
    CODEC_AV1  = 4
};

/* HsResponseV2.status */
enum HsStatus : uint32_t {
    HS_OK          = 0,
    HS_ERR_VERSION = 1,   /* protocol version mismatch            */
    HS_ERR_CODEC   = 2,   /* codec rejected (no silent downgrade) */
    HS_ERR_DIMS    = 3,   /* invalid stream dimensions            */
    HS_ERR_DECODER = 4,   /* receiver decoder unavailable         */
    HS_ERR_LEGACY  = 5,   /* legacy (pre-v2) peer                 */
    HS_ERR_BUSY    = 6,   /* another session or handshake is active */
    HS_ERR_AUTH    = 7,   /* password proof rejected / throttled  */
    HS_ERR_AUTH_TIMEOUT = 8, /* manual approval was not answered in time */
    HS_ERR_DENIED  = 9,   /* the receiver user refused            */
    HS_ERR_AUTH_CONFIG = 10 /* receiver auth policy is corrupt — fail closed */
};

/* Control packet types (follow CTRL_MARKER) */
enum CtrlType : uint32_t {
    CTRL_RESIZE    = 1,
    CTRL_FORCE_IDR = 2,
    CTRL_EOS       = 3,
    CTRL_RECONFIG  = 4
};

/* HsRequestV2.codec_flags */
#define CODEC_FLAG_WANT_HW 0x1u   /* bit0: sender would like hardware encode */

/* Display-size probe convention (no extra packet type, no version bump):
 * a sender may send a normal HsRequestV2 with width=0 AND height=0 before
 * starting a session. The receiver answers HsResponseV2 with its current
 * display size in recv_width/recv_height and status=HS_ERR_DIMS, then
 * closes without opening a window or a decoder. Extend mode uses this to
 * size the virtual display before the real handshake. A receiver treats
 * width=0 as a probe exactly when both dimensions are 0. */

#pragma pack(push, 1)
/* sender → receiver, 48 bytes */
struct HsRequestV2 {
    uint32_t magic;         /* TVHS_MAGIC                                  */
    uint32_t version;       /* TVHS_VERSION                                */
    uint32_t width;         /* stream width  (coded size)                  */
    uint32_t height;        /* stream height (coded size)                  */
    uint32_t fps;           /* target frame rate                           */
    uint32_t mode;          /* 0=mirror  1=extend                          */
    uint32_t codec;         /* CodecId                                     */
    uint32_t codec_flags;   /* CODEC_FLAG_*                                */
    uint32_t bitrate_kbps;  /* 0 = quality/CRF mode                        */
    uint32_t quality;       /* 0..51; HS_QUALITY_DEFAULT = encoder default */
    uint32_t gop_frames;    /* IDR interval; 0 = default 2*fps             */
    uint32_t reserved;      /* 0                                           */
};

/* receiver → sender, 36 bytes */
struct HsResponseV2 {
    uint32_t magic;
    uint32_t version;
    uint32_t recv_width;    /* receiver display width  */
    uint32_t recv_height;   /* receiver display height */
    uint32_t status;        /* HsStatus                */
    uint32_t codec;         /* accepted CodecId        */
    uint32_t codec_flags;
    uint32_t reserved[2];
};

/* sender → receiver, 48 bytes, immediately after HsAuthStart. Carries the
 * sender's display name for the manual-approval dialog and the receiver's
 * status line. `flags` is reserved (0). */
struct HsRequestExtV3 {
    uint32_t magic;         /* TVHX_MAGIC                              */
    uint32_t version;       /* TVHA_VERSION                            */
    char     device[32];    /* UTF-8 hostname, NUL padded              */
    uint32_t flags;         /* 0                                       */
    uint32_t reserved;
};

/* receiver → sender, 64 bytes. Sent for every policy so the sender learns the
 * policy and gets a fresh nonce; `salt`/`kdf_iter` are meaningful only for
 * AUTH_MODE_PASSWORD. */
struct HsAuthStart {
    uint32_t magic;         /* TVHA_MAGIC                              */
    uint32_t version;       /* TVHA_VERSION                            */
    uint32_t mode;          /* AuthMode                                */
    uint32_t kdf_iter;      /* PBKDF2 iterations (password mode)       */
    uint8_t  salt[16];      /* password KDF salt                       */
    uint8_t  nonce[32];     /* fresh per connection                    */
};

/* sender → receiver, 40 bytes, password mode only.
 * proof = HMAC-SHA256(stored_key, nonce || HsRequestV2 || HsRequestExtV3) */
struct HsAuthProof {
    uint32_t magic;         /* TVHA_MAGIC                              */
    uint32_t version;       /* TVHA_VERSION                            */
    uint8_t  proof[32];
};

/* receiver → sender, 44 bytes, status = HS_OK only. The token authorizes the
 * 8082/8083 sidecar services for exactly this session. */
struct HsResponseExtV3 {
    uint32_t magic;         /* TVHR_MAGIC                              */
    uint32_t version;       /* TVHA_VERSION                            */
    uint8_t  token[HS_TOKEN_BYTES];
    uint32_t flags;         /* 0                                       */
};
#pragma pack(pop)

static_assert(sizeof(HsRequestV2) == 48, "HsRequestV2 must be 48 bytes");
static_assert(sizeof(HsResponseV2) == 36, "HsResponseV2 must be 36 bytes");
static_assert(sizeof(HsRequestExtV3) == 48, "HsRequestExtV3 must be 48 bytes");
static_assert(sizeof(HsAuthStart) == 64, "HsAuthStart must be 64 bytes");
static_assert(sizeof(HsAuthProof) == 40, "HsAuthProof must be 40 bytes");
static_assert(sizeof(HsResponseExtV3) == 44, "HsResponseExtV3 must be 44 bytes");

#endif /* MIVARA_PROTO_H */
