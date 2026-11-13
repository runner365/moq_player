/* ============================================================
 * moq_decoder.cpp — H.264 / AAC 解码实现
 *
 * 视频：avcC 作为 extradata，喂 AVCC 数据即可直接解（FFmpeg 的 h264
 *       解码器内置 NALFF 长度前缀拆包，见 h264dec.c / h2645_parse.c）。
 * 音频：ASC 作为 extradata，喂裸 AAC 帧；输出 FLTP，再经 avfilter
 *       转成目标采样率/声道的 F32 interleaved。
 * ============================================================ */

#include "moq_decoder.h"

extern "C" {
#include <libavutil/opt.h>
}

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

/* 取 FFmpeg 错误码的可读描述 */
std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf) + " (" + std::to_string(err) + ")";
}

/* 声道布局 → 字符串（"stereo" / "mono" / ...），失败回退到 "stereo" */
std::string LayoutName(const AVChannelLayout& cl) {
    char buf[128] = {0};
    if (av_channel_layout_describe(&cl, buf, sizeof(buf)) > 0) {
        return buf;
    }
    return "stereo";
}

/* 最多打印 n 字节的十六进制预览 */
std::string HexPreview(const uint8_t* p, size_t len, size_t n = 16) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    size_t m = len < n ? len : n;
    for (size_t i = 0; i < m; ++i) {
        if (i) out.push_back(' ');
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 0x0F]);
    }
    if (len > m) out += " ...";
    return out;
}

} /* namespace */

/* ============================================================
 * VideoDecoder
 * ============================================================ */

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() {
    Close();
}

bool VideoDecoder::Init(const uint8_t* avcc, size_t len, std::string* err) {
    if (opened_) return true;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        if (err) *err = "找不到 h264 解码器（FFmpeg 是否启用了 --enable-decoder=h264？）";
        return false;
    }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) {
        if (err) *err = "avcodec_alloc_context3 失败";
        return false;
    }

    /* avcC 作为 extradata：FFmpeg 据此识别出 AVCC 模式
     * （is_avc=1 + nal_length_size），后续喂长度前缀的包即可 */
    if (avcc && len > 0) {
        ctx_->extradata = static_cast<uint8_t*>(av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx_->extradata) {
            if (err) *err = "av_mallocz(extradata) 失败";
            return false;
        }
        memcpy(ctx_->extradata, avcc, len);
        ctx_->extradata_size = static_cast<int>(len);
    } else {
        if (err) *err = "缺少 avcC extradata，无法初始化解码器";
        return false;
    }

    /* 单线程解码：拉流回调本身在 uv 线程，避免额外线程带来的同步开销 */
    ctx_->thread_count = 1;

    int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0) {
        if (err) *err = "avcodec_open2(h264) 失败: " + AvErr(ret);
        return false;
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        if (err) *err = "av_frame_alloc 失败";
        return false;
    }

    opened_ = true;
    return true;
}

void VideoDecoder::Decode(const uint8_t* data, size_t len,
                          int64_t dts_ms, int64_t pts_ms, bool key) {
    if (!opened_ || !data || len == 0) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    /* 用 av_new_packet 拷贝：SDK 的缓冲区在回调返回后即失效，
     * 而 send_packet 可能保留引用（尤其有 B 帧重排序时） */
    if (av_new_packet(pkt, static_cast<int>(len)) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, data, len);
    pkt->dts = dts_ms;
    pkt->pts = pts_ms;
    if (key) pkt->flags |= AV_PKT_FLAG_KEY;

    int ret = avcodec_send_packet(ctx_, pkt);
    av_packet_free(&pkt);

    if (ret < 0) {
        /* EAGAIN 表示解码器要先把已有帧吐出来，属正常流程 */
        if (ret != AVERROR(EAGAIN)) return;
    }

    DrainFrames();
}

void VideoDecoder::DrainFrames() {
    for (;;) {
        int ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        width_ = frame_->width;
        height_ = frame_->height;
        frames_++;
        if (cb_) cb_(frame_);
        av_frame_unref(frame_);
    }
}

void VideoDecoder::Flush() {
    if (!opened_) return;
    avcodec_send_packet(ctx_, nullptr);
    DrainFrames();
}

void VideoDecoder::Close() {
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (ctx_) {
        avcodec_free_context(&ctx_);
        ctx_ = nullptr;
    }
    opened_ = false;
    cb_ = nullptr;
}

/* ============================================================
 * AudioDecoder
 * ============================================================ */

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder() {
    Close();
}

bool AudioDecoder::Init(const uint8_t* asc, size_t len,
                        int out_rate, int out_channels, std::string* err) {
    if (opened_) return true;

    out_rate_ = out_rate > 0 ? out_rate : 48000;
    out_channels_ = out_channels > 0 ? out_channels : 2;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) {
        if (err) *err = "找不到 aac 解码器（FFmpeg 是否启用了 --enable-decoder=aac？）";
        return false;
    }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) {
        if (err) *err = "avcodec_alloc_context3 失败";
        return false;
    }

    /* AudioSpecificConfig 作为 extradata：解码器据此得知采样率/声道/object type */
    if (asc && len > 0) {
        ctx_->extradata = static_cast<uint8_t*>(av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx_->extradata) {
            if (err) *err = "av_mallocz(extradata) 失败";
            return false;
        }
        memcpy(ctx_->extradata, asc, len);
        ctx_->extradata_size = static_cast<int>(len);
    } else {
        if (err) *err = "缺少 AudioSpecificConfig，无法初始化解码器";
        return false;
    }

    ctx_->thread_count = 1;

    int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0) {
        if (err) *err = "avcodec_open2(aac) 失败: " + AvErr(ret);
        return false;
    }

    /* 打开后解码器多数情况已根据 ASC 解析出原始采样率/声道。
     * AAC 解码器偶尔会延迟到首帧才确定采样率，此时直接从 ASC 补解析。 */
    in_rate_ = ctx_->sample_rate;
    in_channels_ = ctx_->ch_layout.nb_channels;
    if ((in_rate_ == 0 || in_channels_ == 0) && asc && len >= 2) {
        static const int kRates[13] = {
            96000, 88200, 64000, 48000, 44100, 32000,
            24000, 22050, 16000, 12000, 11025, 8000, 7350
        };
        const unsigned aot      = (asc[0] >> 3) & 0x1F;
        const unsigned rate_idx = ((asc[0] & 0x07) << 1) | ((asc[1] >> 7) & 0x01);
        const unsigned chans    = (asc[1] >> 3) & 0x0F;
        /* AOT 31 是扩展转义，头部布局不同；AAC-LC(2) 等走常规布局 */
        if (aot != 31) {
            if (in_rate_ == 0 && rate_idx < 13) in_rate_ = kRates[rate_idx];
            if (in_channels_ == 0) in_channels_ = static_cast<int>(chans);
        }
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        if (err) *err = "av_frame_alloc 失败";
        return false;
    }

    opened_ = true;
    return true;
}

bool AudioDecoder::BuildFilterGraph(const AVFrame* in, std::string* err) {
    graph_ = avfilter_graph_alloc();
    if (!graph_) {
        if (err) *err = "avfilter_graph_alloc 失败";
        return false;
    }

    const char* fmt_name = av_get_sample_fmt_name(static_cast<AVSampleFormat>(in->format));
    if (!fmt_name) fmt_name = "fltp";
    const std::string layout = LayoutName(in->ch_layout);

    in_rate_ = in->sample_rate;
    in_channels_ = in->ch_layout.nb_channels;

    /* abuffer：描述解码器输出的音频格式 */
    char src_args[512];
    snprintf(src_args, sizeof(src_args),
             "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             in->sample_rate, in->sample_rate, fmt_name, layout.c_str());

    int ret = avfilter_graph_create_filter(&src_ctx_, avfilter_get_by_name("abuffer"),
                                           "in", src_args, nullptr, graph_);
    if (ret < 0) {
        if (err) *err = "创建 abuffer 失败: " + AvErr(ret);
        return false;
    }

    ret = avfilter_graph_create_filter(&sink_ctx_, avfilter_get_by_name("abuffersink"),
                                       "out", nullptr, nullptr, graph_);
    if (ret < 0) {
        if (err) *err = "创建 abuffersink 失败: " + AvErr(ret);
        return false;
    }

    /* aresample：重采样到目标采样率 */
    char rs_args[64];
    snprintf(rs_args, sizeof(rs_args), "%d", out_rate_);
    AVFilterContext* rs_ctx = nullptr;
    ret = avfilter_graph_create_filter(&rs_ctx, avfilter_get_by_name("aresample"),
                                       "rs", rs_args, nullptr, graph_);
    if (ret < 0) {
        if (err) *err = "创建 aresample 失败: " + AvErr(ret);
        return false;
    }

    /* aformat：统一成 F32 interleaved + 目标声道布局（SDL 直接可播） */
    char af_args[256];
    snprintf(af_args, sizeof(af_args),
             "sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
             av_get_sample_fmt_name(AV_SAMPLE_FMT_FLT),
             out_rate_,
             out_channels_ == 1 ? "mono" : (out_channels_ == 2 ? "stereo" : "5.1"));

    AVFilterContext* af_ctx = nullptr;
    ret = avfilter_graph_create_filter(&af_ctx, avfilter_get_by_name("aformat"),
                                       "af", af_args, nullptr, graph_);
    if (ret < 0) {
        if (err) *err = "创建 aformat 失败: " + AvErr(ret);
        return false;
    }

    if ((ret = avfilter_link(src_ctx_, 0, rs_ctx, 0)) < 0 ||
        (ret = avfilter_link(rs_ctx, 0, af_ctx, 0)) < 0 ||
        (ret = avfilter_link(af_ctx, 0, sink_ctx_, 0)) < 0) {
        if (err) *err = "连接滤镜失败: " + AvErr(ret);
        return false;
    }

    ret = avfilter_graph_config(graph_, nullptr);
    if (ret < 0) {
        if (err) *err = "avfilter_graph_config 失败: " + AvErr(ret);
        return false;
    }

    return true;
}

void AudioDecoder::Decode(const uint8_t* data, size_t len,
                          int64_t dts_ms, int64_t pts_ms) {
    if (!opened_ || !data || len == 0) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    if (av_new_packet(pkt, static_cast<int>(len)) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, data, len);
    pkt->dts = dts_ms;
    pkt->pts = pts_ms;

    int ret = avcodec_send_packet(ctx_, pkt);
    av_packet_free(&pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) return;

    for (;;) {
        ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        /* 首次拿到帧才知道解码器的真实输出格式，此时建图 */
        if (!graph_) {
            std::string err;
            if (!BuildFilterGraph(frame_, &err)) {
                av_frame_unref(frame_);
                return;
            }
        }

        PushToFilter(frame_);
        av_frame_unref(frame_);
    }
}

void AudioDecoder::PushToFilter(AVFrame* frame) {
    if (!graph_) return;

    /* 保留引用，滤镜消费后我们自己还要 unref */
    int ret = av_buffersrc_add_frame_flags(src_ctx_, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) return;

    DrainFilter();
}

void AudioDecoder::DrainFilter() {
    if (!sink_ctx_) return;

    AVFrame* out = av_frame_alloc();
    if (!out) return;

    for (;;) {
        int ret = av_buffersink_get_frame(sink_ctx_, out);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        frames_++;
        if (cb_) cb_(out);
        av_frame_unref(out);
    }
    av_frame_free(&out);
}

void AudioDecoder::Flush() {
    if (!opened_) return;

    /* 冲刷解码器 */
    avcodec_send_packet(ctx_, nullptr);
    for (;;) {
        int ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        if (!graph_) {
            std::string err;
            if (!BuildFilterGraph(frame_, &err)) {
                av_frame_unref(frame_);
                break;
            }
        }
        PushToFilter(frame_);
        av_frame_unref(frame_);
    }

    /* 冲刷滤镜（把残留样本吐出来） */
    if (src_ctx_) {
        int r = av_buffersrc_add_frame_flags(src_ctx_, nullptr, 0);
        (void)r;
        DrainFilter();
    }
}

void AudioDecoder::Close() {
    if (graph_) {
        avfilter_graph_free(&graph_);
        graph_ = nullptr;
        src_ctx_ = nullptr;
        sink_ctx_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (ctx_) {
        avcodec_free_context(&ctx_);
        ctx_ = nullptr;
    }
    opened_ = false;
    cb_ = nullptr;
}
