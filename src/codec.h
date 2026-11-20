/**
 * CODEC.H – FFmpeg-based video encoder/decoder (H.264/H.265/AV1)
 *
 * Linked into sender + receiver only (NOT app). All FFmpeg types are hidden
 * behind pimpl – this header only needs proto.h.
 *
 * Raw/RLE streams do NOT go through Encoder/Decoder (passthrough).
 */
#ifndef MIVARA_CODEC_H
#define MIVARA_CODEC_H

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

    /* BGR24 variant for Windows capture. src_stride includes any row padding.
     * The swscale pass converts directly to the encoder's YUV format, avoiding
     * a separate full-frame BGR->RGB copy. */
    std::string encodeBGR24(const uint8_t *bgr24, int src_stride,
                            std::vector<uint8_t> &out);

    /* BGRA variant for DXGI Desktop Duplication. Feeding the duplicated
     * surface straight to swscale skips the per-frame BGRA->BGR24 pack.
     * Only safe when outputsNV12() is true: swscale's BGRA->YUV420P kernel
     * differs from its BGR24 one (measured max 29, mean 0.57 levels on a
     * full-channel gradient), while the BGRA->NV12 kernel is byte-identical
     * (max 0). Callers must pack BGR24 for a YUV420P encoder. */
    std::string encodeBGRA(const uint8_t *bgra, int src_stride,
                           std::vector<uint8_t> &out);

    /* True when the encoder consumes NV12 (every hardware encoder), i.e. the
     * BGRA fast path is colour-exact for this configuration. */
    bool outputsNV12() const;

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

    /* Returns "" on success, error string when no decoder is available.
     * The default is the multi-threaded software decoder (predictable across
     * drivers). want_hw asks for a platform hardware decoder first (Windows
     * D3D11VA); any failure silently falls back to software, so a caller
     * never has to care why the fast path was unavailable. */
    std::string open(CodecId codec, int width, int height, bool want_hw = false);

    /* Feed one access unit. `out.valid()` becomes true when a frame is
     * ready (parser buffering may yield no frame for some AUs). */
    std::string decode(const uint8_t *data, size_t size, DecodedFrame &out);

    /* Drain delayed frames (call with empty input at end of stream). */
    std::string flush(DecodedFrame &out);

    void close();

    /* e.g. "hevc_d3d11va" or "hevc (sw, 4 threads)" – for logs only. */
    const char *implName() const;
    bool isHw() const;
    int threadCount() const;

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

#endif /* MIVARA_CODEC_H */
