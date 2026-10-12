/**
 * CODEC.H – FFmpeg-based video encoder/decoder (H.264/H.265/AV1)
 *
 * Linked into sender + receiver only (NOT app). All FFmpeg types are hidden
 * behind pimpl – this header only needs proto.h.
 *
 * Raw/RLE streams do NOT go through Encoder/Decoder (passthrough).
 */
#ifndef TWINVIEW_CODEC_H
#define TWINVIEW_CODEC_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "proto.h"

/* Encoder configuration (see plan §2.1/§2.2).
 * width/height  – coded (stream) size, must be even.
 * src_width/height – RGB24 input size; 0 = same as width/height. When
 *                    different the encoder downscales while converting.
 * bitrate_kbps  – 0 = quality/CRF mode, >0 = bitrate mode.
 * quality       – 0..51 (CRF); 23 = h264/h265 default, 35 = av1 default.
 * want_hw       – prefer hardware encoders (nvenc/qsv/amf/vaapi) first.
 * gop_frames    – IDR interval; 0 = default 2*fps.
 * chroma_444    – software 4:4:4 (YUV444P) for sharper text; hw stays 4:2:0. */
struct EncoderConfig {
    CodecId codec = CODEC_H264;
    int width = 0;
    int height = 0;
    int fps = 60;
    int src_width = 0;
    int src_height = 0;
    int bitrate_kbps = 0;
    int quality = 23;
    bool want_hw = true;
    int gop_frames = 0;
    int chroma_444 = 0;
};

/* Access unit / raw encoded bitstream chunk (one AU per frame). */
class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;

    /* Try the hardware-then-software candidate list until one opens.
     * Returns "" on success, otherwise a human-readable error. */
    std::string open(const EncoderConfig &cfg);

    /* Encode one RGB24 frame (src_width*src_height*3 bytes).
     * `out` receives one access unit; an EMPTY `out` means "no packet for
     * this frame – skip it" (not an error). Returns "" on success. */
    std::string encode(const uint8_t *rgb24, std::vector<uint8_t> &out);

    /* Request a keyframe for the next encode (best-effort IDR/I-frame). */
    void forceKeyframe();

    void close();
    const char *implName() const;   /* e.g. "h264_nvenc", "libx264" */
    bool isHw() const;
    int width() const;
    int height() const;
    int gopFrames() const;

private:
    struct Priv;
    std::unique_ptr<Priv> p_;
};

/* Decoded frame planes (pointers owned by the Decoder, valid until the next
 * decode()/flush()/close()). */
struct DecodedFrame {
    enum Fmt {
        FMT_NONE = 0,
        FMT_I420,    /* planar Y, U, V – SDL_UpdateYUVTexture            */
        FMT_NV12,    /* Y + interleaved UV                               */
        FMT_OTHER    /* rare (e.g. 4:4:4) – convert via frameToRGB24()   */
    };
    Fmt fmt = FMT_NONE;
    int width = 0;
    int height = 0;
    bool keyframe = false;
    const uint8_t *plane[3] = {nullptr, nullptr, nullptr};
    int stride[3] = {0, 0, 0};
    int avpix = -1;   /* internal: AVPixelFormat, needed by frameToRGB24() */

    bool valid() const { return fmt != FMT_NONE; }
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    /* Returns "" on success, error string when no decoder is available. */
    std::string open(CodecId codec, int width, int height);

    /* Feed one access unit. `out.valid()` becomes true when a frame is
     * ready (parser buffering may yield no frame for some AUs). */
    std::string decode(const uint8_t *data, size_t size, DecodedFrame &out);

    /* Drain delayed frames (call with empty input at end of stream). */
    std::string flush(DecodedFrame &out);

    void close();

private:
    struct Priv;
    std::unique_ptr<Priv> p_;
};

/* Rare-format helper: convert any decoded frame to packed RGB24.
 * dst must hold width*height*3 bytes; dst_stride = width*3 normally. */
std::string frameToRGB24(const DecodedFrame &f, uint8_t *dst, int dst_stride);

/* CLI / display helpers (one source of truth with proto.h CodecId). */
const char *codecToString(CodecId c);              /* "h264", "raw", ... */
CodecId codecFromString(const std::string &s);     /* "auto" → CODEC_H264 */

#endif /* TWINVIEW_CODEC_H */
