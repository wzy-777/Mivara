/**
 * PROTO.H – Mivara handshake & control protocol v2 (shared sender/receiver)
 *
 * Handshake over TCP 8081, ALL uint32 fields in network byte order.
 *
 *   sender → receiver : HsRequestV2  (48 bytes)
 *   receiver → sender : HsResponseV2 (36 bytes)
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
#define TVHS_VERSION 2u

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
    HS_ERR_LEGACY  = 5    /* legacy (pre-v2) peer                 */
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
#pragma pack(pop)

static_assert(sizeof(HsRequestV2) == 48, "HsRequestV2 must be 48 bytes");
static_assert(sizeof(HsResponseV2) == 36, "HsResponseV2 must be 36 bytes");

#endif /* MIVARA_PROTO_H */
