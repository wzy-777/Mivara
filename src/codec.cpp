/**
 * CODEC.CPP – FFmpeg encoder/decoder implementation (see codec.h)
 *
 * Low-latency policy (mandatory, plan §2.2):
 *   max_b_frames=0 · AV_CODEC_FLAG_LOW_DELAY · no GLOBAL_HEADER (annex-B)
 *   x264/x265  preset=veryfast + tune=zerolatency
 *   SVT-AV1    preset=8 + fast-decode=1
 *   NVENC      tune=ull / delay=0 ·  QSV async_depth=1 ·  AMF ultralowlatency
 *
 * Hardware encoders are fed system-memory NV12 (swscale RGB24→NV12) –
 * no hw_frames_ctx. If a hardware encoder fails to open, the next
 * candidate is tried (software last).
 */
#include "codec.h"

#include <cstring>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

/* ── small helpers ──────────────────────────────────────────────────────── */

static std::string avErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

static bool nameIsHw(const char *name)
{
    return std::strstr(name, "nvenc") || std::strstr(name, "_qsv") ||
           std::strstr(name, "_amf") || std::strstr(name, "vaapi") ||
           std::strstr(name, "_mf") || std::strstr(name, "d3d12va");
}

const char *codecToString(CodecId c)
{
    switch (c) {
    case CODEC_RAW:  return "raw";
    case CODEC_RLE:  return "rle";
    case CODEC_H264: return "h264";
    case CODEC_H265: return "h265";
    case CODEC_AV1:  return "av1";
    }
    return "raw";
}

CodecId codecFromString(const std::string &s)
{
    if (s == "h264" || s == "auto") return CODEC_H264;
    if (s == "h265" || s == "hevc") return CODEC_H265;
    if (s == "av1")  return CODEC_AV1;
    if (s == "rle")  return CODEC_RLE;
    return CODEC_RAW;
}

static int defaultQuality(CodecId c)
{
    return c == CODEC_AV1 ? 35 : 23;
}

static enum AVCodecID codecToAv(CodecId c)
{
    switch (c) {
    case CODEC_H264: return AV_CODEC_ID_H264;
    case CODEC_H265: return AV_CODEC_ID_HEVC;
    case CODEC_AV1:  return AV_CODEC_ID_AV1;
    default:         return AV_CODEC_ID_NONE;
    }
}

/* Encoder candidate list per codec (plan §2.2). Hardware first when
 * want_hw, then the software reference encoders. */
static void encoderCandidates(CodecId codec, bool want_hw,
                              std::vector<std::string> &out)
{
    out.clear();
#ifdef _WIN32
    const bool win = true;
#else
    const bool win = false;
#endif
#ifndef __linux__
    const bool linux_os = false;
#else
    const bool linux_os = true;
#endif
    (void)win; (void)linux_os;

    if (want_hw) {
        switch (codec) {
        case CODEC_H264:
            out.push_back("h264_nvenc");
            out.push_back("h264_qsv");
            if (win) out.push_back("h264_amf");
            if (linux_os) out.push_back("h264_vaapi");
            break;
        case CODEC_H265:
            out.push_back("hevc_nvenc");
            out.push_back("hevc_qsv");
            if (win) out.push_back("hevc_amf");
            if (linux_os) out.push_back("hevc_vaapi");
            break;
        case CODEC_AV1:
            out.push_back("av1_nvenc");
            out.push_back("av1_qsv");
            if (win) out.push_back("av1_amf");
            if (linux_os) out.push_back("av1_vaapi");
            break;
        default:
            break;
        }
    }
    switch (codec) {
    case CODEC_H264: out.push_back("libx264");            break;
    case CODEC_H265: out.push_back("libx265");            break;
    case CODEC_AV1:
        out.push_back("libsvtav1");
        out.push_back("libaom-av1");
        break;
    default:
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * ENCODER
 * ══════════════════════════════════════════════════════════════════════════ */

struct Encoder::Priv {
    AVCodecContext *ctx  = nullptr;
    AVFrame        *frame = nullptr;
    AVPacket       *pkt   = nullptr;
    SwsContext     *sws   = nullptr;
    int src_w = 0, src_h = 0;
    int dst_w = 0, dst_h = 0;
    int64_t pts = 0;
    bool force_idr = false;
    bool hw = false;
    std::string name;
    std::string last_err;
    CodecId codec = CODEC_H264;
};

Encoder::Encoder() : p_(new Priv) {}
Encoder::~Encoder() { close(); }

const char *Encoder::implName() const
{
    return p_->name.empty() ? "none" : p_->name.c_str();
}
bool Encoder::isHw() const { return p_->hw; }
int Encoder::width() const { return p_->dst_w; }
int Encoder::height() const { return p_->dst_h; }
int Encoder::gopFrames() const { return p_->ctx ? p_->ctx->gop_size : 0; }

void Encoder::forceKeyframe() { p_->force_idr = true; }

void Encoder::close()
{
    if (p_->sws)   { sws_freeContext(p_->sws); p_->sws = nullptr; }
    if (p_->pkt)   { av_packet_free(&p_->pkt); }
    if (p_->frame) { av_frame_free(&p_->frame); }
    if (p_->ctx)   { avcodec_free_context(&p_->ctx); }
    p_->pts = 0;
}

/* Apply codec-specific low-latency + rate-control options.
 * av_opt_set failures on unknown options are ignored (harmless). */
static void applyEncoderOptions(const AVCodec *codec, AVCodecContext *ctx,
                                const EncoderConfig &cfg, bool hw)
{
    void *priv = ctx->priv_data;
    char num[32];
    const int quality = (cfg.quality < 0 || cfg.quality > 51)
                            ? defaultQuality(cfg.codec) : cfg.quality;

    /* Rate control */
    if (cfg.bitrate_kbps > 0) {
        ctx->bit_rate   = (int64_t)cfg.bitrate_kbps * 1000;
        ctx->rc_max_rate  = ctx->bit_rate;
        ctx->rc_buffer_size = (int)(ctx->bit_rate / 2); /* small VBV */
    }

    const char *n = codec->name;
    const bool is_x264 = std::strcmp(n, "libx264") == 0;
    const bool is_x265 = std::strcmp(n, "libx265") == 0;
    const bool is_svt  = std::strcmp(n, "libsvtav1") == 0;
    const bool is_aom  = std::strcmp(n, "libaom-av1") == 0;
    const bool is_nvenc = std::strstr(n, "nvenc") != nullptr;
    const bool is_qsv   = std::strstr(n, "_qsv") != nullptr;
    const bool is_amf   = std::strstr(n, "_amf") != nullptr;

    if (is_x264) {
        av_opt_set(priv, "preset", "veryfast", 0);
        av_opt_set(priv, "tune", "zerolatency", 0);
        if (cfg.chroma_444) av_opt_set(priv, "profile", "high444", 0);
        if (cfg.bitrate_kbps <= 0) {
            std::snprintf(num, sizeof(num), "%d", quality);
            av_opt_set(priv, "crf", num, 0);
        }
    } else if (is_x265) {
        av_opt_set(priv, "preset", "veryfast", 0);
        av_opt_set(priv, "tune", "zerolatency", 0);
        if (cfg.chroma_444) av_opt_set(priv, "x265-params", "profile=main444", 0);
        if (cfg.bitrate_kbps <= 0) {
            std::snprintf(num, sizeof(num), "%d", quality);
            av_opt_set(priv, "crf", num, 0);
        }
    } else if (is_svt) {
        av_opt_set(priv, "preset", "8", 0);
        av_opt_set(priv, "fast-decode", "1", 0);
        /* Random-access mini-GOP buffers ~1s of frames; force low-delay.
         * SVT-AV1 option names: pred-struct=low-delay (not "pred-structure"),
         * lookahead=0 (not "lag-in-frames"). Values are strings. */
        av_opt_set(priv, "svtav1-params",
                   "pred-struct=low-delay:lookahead=0", 0);
        if (cfg.bitrate_kbps <= 0) {
            std::snprintf(num, sizeof(num), "%d", quality);
            av_opt_set(priv, "crf", num, 0);
        }
    } else if (is_aom) {
        av_opt_set(priv, "usage", "realtime", 0);
        av_opt_set(priv, "cpu-used", "8", 0);
        av_opt_set(priv, "lag-in-frames", "0", 0);
        if (cfg.bitrate_kbps <= 0) {
            std::snprintf(num, sizeof(num), "%d", quality);
            av_opt_set(priv, "crf", num, 0);
        }
    } else if (is_nvenc) {
        av_opt_set(priv, "tune", "ull", 0);
        av_opt_set(priv, "delay", "0", 0);
        av_opt_set(priv, "zerolatency", "1", 0);   /* legacy option name */
        if (cfg.bitrate_kbps <= 0) {
            av_opt_set(priv, "rc", "vbr", 0);
            std::snprintf(num, sizeof(num), "%d", quality);
            av_opt_set(priv, "cq", num, 0);
        } else {
            av_opt_set(priv, "rc", "cbr", 0);
        }
    } else if (is_qsv) {
        av_opt_set(priv, "async_depth", "1", 0);
        if (cfg.bitrate_kbps <= 0) {
            /* ICQ-ish quality mode */
            ctx->global_quality = quality;
            std::snprintf(num, sizeof(num), "%d", quality);
            av_opt_set(priv, "global_quality", num, 0);
        }
    } else if (is_amf) {
        av_opt_set(priv, "usage", "ultralowlatency", 0);
        if (cfg.bitrate_kbps <= 0) {
            std::snprintf(num, sizeof(num), "%d", quality);
            av_opt_set(priv, "rc", "cqp", 0);
            av_opt_set(priv, "qp_i", num, 0);
            av_opt_set(priv, "qp_p", num, 0);
        }
    }
    if (hw) {
        /* nothing extra – system-memory NV12 frames */
    }
}

std::string Encoder::open(const EncoderConfig &cfg)
{
    close();

    if (cfg.width <= 0 || cfg.height <= 0 || cfg.fps <= 0)
        return "encoder: invalid dimensions/fps";
    if (cfg.width & 1 || cfg.height & 1)
        return "encoder: dimensions must be even";

    p_->codec = cfg.codec;
    p_->src_w = cfg.src_width > 0 ? cfg.src_width : cfg.width;
    p_->src_h = cfg.src_height > 0 ? cfg.src_height : cfg.height;
    p_->dst_w = cfg.width;
    p_->dst_h = cfg.height;

    std::vector<std::string> cands;
    encoderCandidates(cfg.codec, cfg.want_hw, cands);
    if (cands.empty())
        return "encoder: codec not encodable (use raw/rle)";

    std::string errs;
    for (const auto &cname : cands) {
        const AVCodec *codec = avcodec_find_encoder_by_name(cname.c_str());
        if (!codec) {
            errs += "[" + cname + ": not built in] ";
            continue;
        }

        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            errs += "[" + cname + ": alloc failed] ";
            continue;
        }

        ctx->width  = cfg.width;
        ctx->height = cfg.height;
        ctx->time_base  = AVRational{1, cfg.fps};
        ctx->framerate  = AVRational{cfg.fps, 1};
        ctx->pix_fmt    = nameIsHw(cname.c_str())
                              ? AV_PIX_FMT_NV12
                              : (cfg.chroma_444 ? AV_PIX_FMT_YUV444P
                                                : AV_PIX_FMT_YUV420P);
        /* Low latency */
        ctx->max_b_frames = 0;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;
        ctx->gop_size = cfg.gop_frames > 0 ? cfg.gop_frames : 2 * cfg.fps;
        ctx->keyint_min = ctx->gop_size;
        /* frame threading adds latency; slice threading does not */
        if (std::strcmp(codec->name, "libx264") == 0 ||
            std::strcmp(codec->name, "libx265") == 0) {
            ctx->thread_count = 2;
            ctx->thread_type  = FF_THREAD_SLICE;
        } else {
            ctx->thread_count = 1;
        }

        applyEncoderOptions(codec, ctx, cfg, nameIsHw(cname.c_str()));

        int ret = avcodec_open2(ctx, codec, nullptr);
        if (ret < 0) {
            errs += "[" + cname + ": " + avErr(ret) + "] ";
            avcodec_free_context(&ctx);
            continue;
        }

        /* swscale RGB24 → encoder pixel format (also handles --scale) */
        SwsContext *sws = sws_getContext(
            p_->src_w, p_->src_h, AV_PIX_FMT_RGB24,
            cfg.width, cfg.height, ctx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            errs += "[" + cname + ": swscale failed] ";
            avcodec_free_context(&ctx);
            continue;
        }

        AVFrame *frame = av_frame_alloc();
        AVPacket *pkt  = av_packet_alloc();
        if (!frame || !pkt) {
            errs += "[" + cname + ": frame/packet alloc failed] ";
            if (frame) av_frame_free(&frame);
            if (pkt) av_packet_free(&pkt);
            sws_freeContext(sws);
            avcodec_free_context(&ctx);
            continue;
        }
        frame->format = ctx->pix_fmt;
        frame->width  = cfg.width;
        frame->height = cfg.height;
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            errs += "[" + cname + ": frame buffer: " + avErr(ret) + "] ";
            av_frame_free(&frame);
            av_packet_free(&pkt);
            sws_freeContext(sws);
            avcodec_free_context(&ctx);
            continue;
        }

        p_->ctx = ctx;
        p_->frame = frame;
        p_->pkt = pkt;
        p_->sws = sws;
        p_->name = codec->name;
        p_->hw = nameIsHw(codec->name);
        p_->force_idr = true;   /* first frame is always a keyframe */
        return "";
    }

    return "encoder: all candidates failed – " + errs;
}

std::string Encoder::encode(const uint8_t *rgb24, std::vector<uint8_t> &out)
{
    out.clear();
    if (!p_->ctx || !rgb24) return "encoder: not open";

    /* One sws pass: RGB24 (src size) → encoder pix_fmt (coded size). */
    const uint8_t *slices[1] = {rgb24};
    int strides[1] = {p_->src_w * 3};
    if (av_frame_make_writable(p_->frame) < 0)
        return "encoder: frame not writable";

    int ret = sws_scale(p_->sws, slices, strides, 0, p_->src_h,
                        p_->frame->data, p_->frame->linesize);
    if (ret <= 0) return "encoder: swscale failed";

    p_->frame->pts = p_->pts++;
    if (p_->force_idr) {
        p_->frame->pict_type = AV_PICTURE_TYPE_I;
        p_->force_idr = false;
    } else {
        p_->frame->pict_type = AV_PICTURE_TYPE_NONE;
    }

    ret = avcodec_send_frame(p_->ctx, p_->frame);
    while (ret == AVERROR(EAGAIN)) {
        /* Output not drained – flush packets first, then retry the frame
         * (send_frame does NOT consume the frame on EAGAIN). */
        while ((ret = avcodec_receive_packet(p_->ctx, p_->pkt)) == 0) {
            out.insert(out.end(), p_->pkt->data, p_->pkt->data + p_->pkt->size);
            av_packet_unref(p_->pkt);
        }
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return "encoder: receive_packet: " + avErr(ret);
        ret = avcodec_send_frame(p_->ctx, p_->frame);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return "encoder: send_frame: " + avErr(ret);
    }
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return "encoder: send_frame: " + avErr(ret);

    while ((ret = avcodec_receive_packet(p_->ctx, p_->pkt)) == 0) {
        out.insert(out.end(), p_->pkt->data, p_->pkt->data + p_->pkt->size);
        av_packet_unref(p_->pkt);
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return "";              /* no AU this frame – caller skips */
    return "encoder: receive_packet: " + avErr(ret);
}

/* ══════════════════════════════════════════════════════════════════════════
 * DECODER
 * ══════════════════════════════════════════════════════════════════════════ */

struct Decoder::Priv {
    AVCodecContext *ctx = nullptr;
    AVCodecParserContext *parser = nullptr;
    AVFrame *frame = nullptr;
    std::vector<uint8_t> in_scratch;   /* padded parser input  */
    std::vector<uint8_t> pkt_scratch;  /* padded packet buffer */
    CodecId codec = CODEC_RAW;
};

Decoder::Decoder() : p_(new Priv) {}
Decoder::~Decoder() { close(); }

void Decoder::close()
{
    if (p_->parser) { av_parser_close(p_->parser); p_->parser = nullptr; }
    if (p_->frame)  { av_frame_free(&p_->frame); }
    if (p_->ctx)    { avcodec_free_context(&p_->ctx); }
}

std::string Decoder::open(CodecId codec, int width, int height)
{
    close();
    if (width <= 0 || height <= 0) return "decoder: invalid dimensions";

    enum AVCodecID av_id = codecToAv(codec);
    if (av_id == AV_CODEC_ID_NONE) return "decoder: not a coded codec";

    const AVCodec *c = avcodec_find_decoder(av_id);
    if (!c) return "decoder: no decoder for codec";

    p_->ctx = avcodec_alloc_context3(c);
    if (!p_->ctx) return "decoder: alloc failed";
    p_->ctx->thread_count = 1;   /* frame threading adds latency */
    p_->ctx->thread_type  = 0;
    p_->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    int ret = avcodec_open2(p_->ctx, c, nullptr);
    if (ret < 0) {
        std::string e = "decoder: open: " + avErr(ret);
        close();
        return e;
    }

    p_->parser = av_parser_init(av_id);
    if (!p_->parser) {
        close();
        return "decoder: parser init failed";
    }

    p_->frame = av_frame_alloc();
    if (!p_->frame) {
        close();
        return "decoder: frame alloc failed";
    }
    p_->codec = codec;
    return "";
}

static DecodedFrame mapFrame(const AVFrame *f)
{
    DecodedFrame d;
    d.width  = f->width;
    d.height = f->height;
    d.keyframe = (f->pict_type == AV_PICTURE_TYPE_I ||
                  f->pict_type == AV_PICTURE_TYPE_SI);
    d.avpix = f->format;
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        d.fmt = DecodedFrame::FMT_I420;
        for (int i = 0; i < 3; i++) {
            d.plane[i] = f->data[i];
            d.stride[i] = f->linesize[i];
        }
    } else if (f->format == AV_PIX_FMT_NV12) {
        d.fmt = DecodedFrame::FMT_NV12;
        d.plane[0] = f->data[0]; d.stride[0] = f->linesize[0];
        d.plane[1] = f->data[1]; d.stride[1] = f->linesize[1];
    } else {
        d.fmt = DecodedFrame::FMT_OTHER;
        for (int i = 0; i < 3; i++) {
            d.plane[i] = f->data[i];
            d.stride[i] = f->linesize[i];
        }
    }
    return d;
}

/* Feed one access-unit to the decoder. Never drops a packet on EAGAIN: drains
 * ready frames first, then retries the send. The latest decoded frame is
 * stored in `out` (got=true). */
static std::string feedPacket(AVCodecContext *ctx, AVFrame *frame,
                              uint8_t *data, int size,
                              DecodedFrame &out, bool &got)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return "decoder: packet alloc failed";
    pkt->data = data;
    pkt->size = size;

    /* Receive one frame. Returns: 0=got frame, 1=EAGAIN/EOF, <0 error. */
    auto recv_one = [&](int &status) -> std::string {
        status = avcodec_receive_frame(ctx, frame);
        if (status == 0) { out = mapFrame(frame); got = true; return ""; }
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return "";
        return "decoder: receive_frame: " + avErr(status);
    };

    int ret = avcodec_send_packet(ctx, pkt);
    for (int guard = 0; ret == AVERROR(EAGAIN) && guard < 16; ++guard) {
        int st = 0;
        std::string err = recv_one(st);
        if (!err.empty()) {
            pkt->data = nullptr;
            av_packet_free(&pkt);
            return err;
        }
        if (st != 0) break;   /* nothing to drain – cannot accept yet */
        ret = avcodec_send_packet(ctx, pkt);   /* retry after drain */
    }
    pkt->data = nullptr;
    av_packet_free(&pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return "decoder: send_packet: " + avErr(ret);
    if (ret == AVERROR(EAGAIN))
        return "decoder: stalled (send EAGAIN after drain)";

    for (;;) {
        int st = 0;
        std::string err = recv_one(st);
        if (!err.empty()) return err;
        if (st != 0) break;
    }
    return "";
}

std::string Decoder::decode(const uint8_t *data, size_t size, DecodedFrame &out)
{
    out = DecodedFrame();
    if (!p_->ctx) return "decoder: not open";
    if (!data || size == 0) return "decoder: empty input";

    /* The wire protocol already frames one complete access unit per packet
     * (annex-B / OBU), so feed it to the decoder directly. Running it through
     * av_parser_parse2 first caused whole AUs to be held back (parser waits
     * for the next start code) and every other frame to be lost. */
    p_->in_scratch.assign(data, data + size);
    p_->in_scratch.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    bool got = false;
    return feedPacket(p_->ctx, p_->frame, p_->in_scratch.data(),
                      (int)size, out, got);
}

std::string Decoder::flush(DecodedFrame &out)
{
    out = DecodedFrame();
    if (!p_->ctx) return "";
    avcodec_send_packet(p_->ctx, nullptr);
    int ret = avcodec_receive_frame(p_->ctx, p_->frame);
    if (ret == 0) out = mapFrame(p_->frame);
    return "";
}

std::string frameToRGB24(const DecodedFrame &f, uint8_t *dst, int dst_stride)
{
    if (!f.valid() || !dst) return "convert: invalid frame";
    if (f.avpix < 0) return "convert: unknown pixel format";

    SwsContext *sws = sws_getContext(f.width, f.height, (AVPixelFormat)f.avpix,
                                     f.width, f.height, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return "convert: swscale init failed";

    uint8_t *dp[1] = {dst};
    int ds[1] = {dst_stride};
    sws_scale(sws, f.plane, (const int *)f.stride, 0, f.height, dp, ds);
    sws_freeContext(sws);
    return "";
}
