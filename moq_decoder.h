#ifndef MOQ_DECODER_H
#define MOQ_DECODER_H

/* ============================================================
 * moq_decoder.h — H.264 / AAC 解码层
 *
 * 输入（来自 moq_transport_client 的 on_data）：
 *   视频：AVCC 格式（4 字节长度前缀 + NAL），配 avcC extradata
 *   音频：裸 AAC 帧（无 ADTS 头），配 AudioSpecificConfig extradata
 *
 * 输出：AVFrame*
 *   视频帧 = YUV420P（Y/U/V 三个平面），可直接上传 SDL IYUV 纹理
 *   音频帧 = F32 interleaved PCM，采样率/声道数已由 avfilter 转成
 *            设备友好值（默认 48000 Hz / 立体声），SDL 可直接播
 *
 * 音频链路的滤镜图：
 *   abuffer → aresample(<= 目标采样率) → aformat(F32, stereo) → abuffersink
 *
 * 注意：回调里的 AVFrame* 归解码器所有，回调返回后即失效。
 * 需要留存请自行 av_frame_clone / av_frame_ref。
 * ============================================================ */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

/* ------------------------- 视频解码 ------------------------- */

/* 支持的视频编码。两种都用「长度前缀 + 配置记录」的形式喂进来：
 *   H.264 → avcC  (AVCDecoderConfigurationRecord)
 *   H.265 → hvcC  (HEVCDecoderConfigurationRecord)
 * FFmpeg 靠 extradata 自己识别封装方式，调用方只需原样传 config 和码流。 */
enum class VideoCodec { kH264 = 0, kH265 = 1 };

const char* VideoCodecName(VideoCodec c);   /* "H.264" / "H.265" */

class VideoDecoder {
public:
    using FrameCb = std::function<void(AVFrame* frame)>;

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /* config 来自 on_stream_ready 的 info->config：
     * H.264 轨是 avcC，H.265 轨是 hvcC。codec 须与 config 一致。 */
    bool Init(VideoCodec codec, const uint8_t* config, size_t len, std::string* err);

    void SetFrameCallback(FrameCb cb) { cb_ = std::move(cb); }

    /* 喂一个访问单元（4 字节长度前缀的 NAL 序列）；解码出的帧通过回调送出 */
    void Decode(const uint8_t* data, size_t len,
                int64_t dts_ms, int64_t pts_ms, bool key);

    /* 冲刷解码器内部缓存的帧（流结束时调用） */
    void Flush();

    void Close();

    bool opened() const { return opened_; }
    VideoCodec codec() const { return codec_; }
    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t frames() const { return frames_; }

private:
    void DrainFrames();
    /* 把非 YUV420P（如 10bit 的 yuv420p10le）转成 YUV420P 再交给渲染层。
     * 渲染用的是 SDL_UpdateYUVTexture，只接受 8bit 三平面。 */
    AVFrame* ToYuv420p(AVFrame* in);

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    FrameCb cb_;
    bool opened_ = false;
    VideoCodec codec_ = VideoCodec::kH264;
    int width_ = 0;
    int height_ = 0;
    uint64_t frames_ = 0;

    /* 像素格式转换（仅在需要时创建） */
    SwsContext* sws_ = nullptr;
    AVFrame*    sws_frame_ = nullptr;
    int         sws_w_ = 0;
    int         sws_h_ = 0;
    int         sws_fmt_ = -1;
    uint64_t    sws_converted_ = 0;
};

/* ------------------------- 音频解码 ------------------------- */

class AudioDecoder {
public:
    using FrameCb = std::function<void(AVFrame* frame)>;

    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /* asc 来自 on_stream_ready 的 info->config（AAC 轨）
     * out_rate / out_channels 是 avfilter 转换的目标格式，
     * 默认 48000 Hz 立体声 —— SDL3 用 SDL_AUDIO_F32 直接播。 */
    bool Init(const uint8_t* asc, size_t len,
              int out_rate, int out_channels, std::string* err);

    void SetFrameCallback(FrameCb cb) { cb_ = std::move(cb); }

    /* 喂一个裸 AAC 帧；输出的 AVFrame 已是目标采样率/声道的 F32 PCM */
    void Decode(const uint8_t* data, size_t len,
                int64_t dts_ms, int64_t pts_ms);

    void Flush();

    void Close();

    bool opened() const { return opened_; }
    int out_sample_rate() const { return out_rate_; }
    int out_channels() const { return out_channels_; }
    int in_sample_rate() const { return in_rate_; }
    int in_channels() const { return in_channels_; }
    uint64_t frames() const { return frames_; }

private:
    /* 用解码器实际输出的格式来建图（首次解码后才知道） */
    bool BuildFilterGraph(const AVFrame* in, std::string* err);
    void PushToFilter(AVFrame* frame);
    void DrainFilter();

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    FrameCb cb_;

    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* src_ctx_ = nullptr;
    AVFilterContext* sink_ctx_ = nullptr;

    bool opened_ = false;
    int out_rate_ = 48000;
    int out_channels_ = 2;
    int in_rate_ = 0;
    int in_channels_ = 0;
    uint64_t frames_ = 0;
};

#endif /* MOQ_DECODER_H */
