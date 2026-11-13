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
}

/* ------------------------- 视频解码 ------------------------- */

class VideoDecoder {
public:
    using FrameCb = std::function<void(AVFrame* frame)>;

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /* avcc 来自 on_stream_ready 的 info->config（H264 轨） */
    bool Init(const uint8_t* avcc, size_t len, std::string* err);

    void SetFrameCallback(FrameCb cb) { cb_ = std::move(cb); }

    /* 喂一个 AVCC 访问单元；解码出的帧通过回调送出 */
    void Decode(const uint8_t* data, size_t len,
                int64_t dts_ms, int64_t pts_ms, bool key);

    /* 冲刷解码器内部缓存的帧（流结束时调用） */
    void Flush();

    void Close();

    bool opened() const { return opened_; }
    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t frames() const { return frames_; }

private:
    void DrainFrames();

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    FrameCb cb_;
    bool opened_ = false;
    int width_ = 0;
    int height_ = 0;
    uint64_t frames_ = 0;
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
