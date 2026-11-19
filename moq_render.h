#ifndef MOQ_RENDER_H
#define MOQ_RENDER_H

/* ============================================================
 * moq_render.h — 播放层（队列 + SDL3 渲染/播放）
 *
 * 线程模型：
 *   后台线程（uv loop）：拉流 + 解码 → PushVideo/PushAudio（线程安全）
 *   主线程：Player::Iterate() 里跑 SDL 事件循环 + 视频渲染 + 音频喂数据
 *
 * 两条队列（video / audio）各自按 pts 有序：
 *   - 正常情况 pts 单调递增，走尾插快路径 O(1)
 *   - 真出现乱序才二分插入
 *   - 队列同时受「时间窗口」和「帧数上限」约束，超了从队头丢最旧
 *
 * av_sync 语义（数值即开关）：
 *   av_sync = 0    直通：解码出来立刻渲染，音频来即喂，延迟最低
 *   av_sync = N>0  精确同步：先攒够 N 毫秒再起播，之后视频按 pts 对齐
 *                  音频时钟（音频连续喂、充当主时钟），落后超阈值丢帧
 * ============================================================ */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_video.h>

extern "C" {
#include <libavutil/frame.h>
}

/* ------------------------- 有序帧队列 ------------------------- */

struct FrameItem {
    AVFrame* frame = nullptr;
    int64_t  pts_ms = 0;
    int64_t  duration_ms = 0;   /* 用来算缓冲时长；视频给 0 即可 */
};

class FrameQueue {
public:
    explicit FrameQueue(const char* name);
    ~FrameQueue();

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    /* 时间窗口（毫秒）与帧数上限，双约束。
     * 超限时丢弃**新到的帧**（队尾），不丢队首 —— 队首是播放位置。 */
    void SetLimits(int window_ms, size_t max_frames);

    /* 入队：内部 av_frame_clone，调用方仍拥有原帧。
     * 返回因超限被丢弃的帧数。 */
    int Push(const AVFrame* frame, int64_t pts_ms, int64_t duration_ms = 0);

    bool    Empty() const;
    size_t  Size() const;
    int64_t BufferedMs() const;   /* 首尾时间跨度 + 末帧时长 */
    int64_t FrontPts() const;
    int64_t BackPts() const;

    /* 取出队头一帧，调用方用 av_frame_free 释放；空则返回 nullptr */
    AVFrame* Pop(int64_t* pts_out = nullptr);

    /* 丢弃 pts 早于阈值的帧（返回丢弃数） */
    int DropBefore(int64_t pts_ms);

    void Clear();

    const char* name() const { return name_; }

private:
    void TrimLocked();

    mutable std::mutex mutex_;
    std::deque<FrameItem> items_;
    const char* name_;
    int    window_ms_  = 0;
    size_t max_frames_ = 0;
};

/* ------------------------- 输出设备 ------------------------- */

class VideoSink {
public:
    VideoSink();
    ~VideoSink();

    VideoSink(const VideoSink&) = delete;
    VideoSink& operator=(const VideoSink&) = delete;

    /* 初始化 SDL 视频子系统（必须在主线程调用） */
    bool Init(std::string* err);

    /* 首帧到达时才知道分辨率，这里懒创建窗口/渲染器/纹理。
     * 分辨率变化会自动重建纹理。 */
    void Configure(int width, int height);

    /* 用 SDL_UpdateYUVTexture 直传 YUV420P，再渲染上屏 */
    void Present(AVFrame* frame);

    void ClearScreen();

    /* 抓取当前渲染结果存 BMP —— 用于验证渲染管线（颜色/平面顺序）是否正确 */
    bool SaveSnapshot(const char* path);

    /* 释放 SDL 资源。必须在 SDL_Quit() 之前调用，
     * 否则 SDL 子系统已销毁，再销毁纹理/窗口会 use-after-free。 */
    void Destroy();

    bool configured() const { return texture_ != nullptr; }

private:
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    int width_  = 0;
    int height_ = 0;
};

class AudioSink {
public:
    AudioSink();
    ~AudioSink();

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    /* 用解码侧 avfilter 已转换好的格式打开设备（无需再做转换） */
    bool Open(int sample_rate, int channels, std::string* err);

    /* 连续喂 PCM，不做 pts 定时弹 */
    bool Feed(const AVFrame* frame);

    /* 仍在设备/缓冲中未播完的字节数 */
    int QueuedBytes() const;

    /* 清空设备里尚未播放的数据（时间轴跳转时用，否则音画会错位） */
    void ClearQueued();

    /* 关闭音频流。同样必须在 SDL_Quit() 之前调用。 */
    void Close();

    bool opened() const { return stream_ != nullptr; }
    int  bytes_per_ms() const;

private:
    SDL_AudioStream* stream_ = nullptr;
    int     sample_rate_ = 0;
    int     channels_    = 0;
    int     bytes_per_sample_frame_ = 0;
};

/* ------------------------- 播放控制器 ------------------------- */

class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    /* 主线程调用。av_sync_ms = 0 表示直通，>0 表示预缓冲该毫秒数 */
    /* video_enabled / audio_enabled 对应 --audio-only / --video-only：
     * 关闭的那一路不建解码器、不建窗口/音频设备，也不进队列。 */
    bool Init(int av_sync_ms, int out_rate, int out_channels,
              bool video_enabled, bool audio_enabled, std::string* err);

    /* 以下两个由后台解码线程调用，内部加锁 */
    void PushVideo(const AVFrame* frame, int64_t pts_ms);
    void PushAudio(const AVFrame* frame, int64_t pts_ms);

    /* 主线程循环调用一次。返回 false 表示需要退出（用户关窗/按 ESC） */
    bool Iterate();

    /* 显式收尾：按正确顺序释放 SDL 资源。可重复调用。 */
    void Shutdown();

    /* 在第 n 个渲染帧后抓一张 BMP 存到 path（验证渲染用，0 表示关闭） */
    void SetSnapshot(const std::string& path, uint64_t at_frame) {
        snapshot_path_ = path;
        snapshot_at_ = at_frame;
    }

    /* 后台线程结束输入后置位，主线程据此在播完后收尾 */
    void SetInputDone() { input_done_ = true; }
    bool input_done() const { return input_done_; }

    /* 队列已空且输入结束 —— 可以退出主循环 */
    bool Finished() const;

    /* 统计 */
    uint64_t rendered_frames() const { return rendered_; }
    uint64_t fed_audio_frames() const { return fed_audio_; }
    uint64_t dropped_frames()  const { return dropped_; }
    uint64_t dropped_late()    const { return dropped_late_; }
    uint64_t dropped_push()    const { return dropped_push_; }
    uint64_t resync_count()    const { return resync_count_; }
    uint64_t rebuffer_count()  const { return rebuffer_count_; }
    size_t   video_queue_size() const { return video_q_.Size(); }
    size_t   audio_queue_size() const { return audio_q_.Size(); }
    int64_t  video_buffered_ms() const { return video_q_.BufferedMs(); }
    int64_t  audio_buffered_ms() const { return audio_q_.BufferedMs(); }
    const char* mode_name() const;

private:
    bool PollEvents();
    void FeedAudio();
    void RenderVideo();
    void MaybeStartPlayback();   /* 预缓冲判定 → 起播（与是否渲染视频无关） */
    void AfterRender();

    /* 当前媒体时钟（毫秒）。有音频时以音频播放位置为准，否则用墙钟 */
    int64_t MediaClockMs();

    /* 时间轴跳转：把播放位置（含音频）跳到指定媒体时间 */
    void ResyncTo(int64_t pts);

    /* 进入「重新缓冲」：暂停播放（同时停掉音频），攒够缓冲后从队首恢复。
     * 稳定优先：宁可短暂停顿，也不带着空缓冲硬播。 */
    void EnterRebuffer();

    FrameQueue video_q_;
    FrameQueue audio_q_;

    VideoSink video_sink_;
    AudioSink audio_sink_;

    int     av_sync_ms_ = 0;
    int     out_rate_   = 48000;
    int     out_channels_ = 2;

    bool    audio_opened_ = false;
    bool    started_      = false;   /* 是否已起播（预缓冲完成后置位） */

    int64_t anchor_pts_  = 0;        /* 起播锚点（媒体时间） */
    int64_t anchor_wall_ = 0;        /* 起播锚点（墙钟，毫秒） */

    /* 音频时钟 */
    int64_t audio_base_pts_   = 0;   /* 送入设备的第一帧的 pts */
    int64_t audio_bytes_sent_ = 0;
    bool    audio_base_set_   = false;
    int64_t last_clk_log_ms_  = 0;   /* 时钟诊断日志节流（调试用） */
    int64_t last_clock_ms_    = 0;   /* 上一次返回的时钟，用于单调保护 */

    bool    video_enabled_ = true;
    bool    audio_enabled_ = true;
    bool    rebuffering_        = false;  /* 是否处于重新缓冲（暂停）状态 */
    int64_t rebuffer_target_ms_ = 0;      /* 恢复播放所需的缓冲深度 */
    int64_t rebuffer_start_ms_  = 0;      /* 进入重新缓冲的时刻（墙钟） */

    std::atomic<bool> input_done_{false};
    std::atomic<bool> quit_{false};
    bool shutdown_ = false;

    /* 渲染结果抓图（验证用） */
    std::string snapshot_path_;
    uint64_t    snapshot_at_ = 0;
    bool        snapshot_done_ = false;

    uint64_t rendered_  = 0;
    uint64_t fed_audio_ = 0;
    uint64_t dropped_   = 0;

    /* ---- 诊断计数（--log-level debug 时每秒打一行 [diag]） ---- */
    void LogDiag();

    uint64_t dropped_late_  = 0;   /* 因「落后时钟」丢弃 */
    uint64_t dropped_push_  = 0;   /* 因「队列超限」丢弃 */
    uint64_t resync_count_  = 0;   /* 播放位置跳转次数 */
    uint64_t rebuffer_count_ = 0;  /* 重新缓冲次数 */
    int64_t  diag_last_ms_  = 0;   /* 上一行 diag 的时间（墙钟） */
    int64_t  last_render_pts_ = -1;/* 最近渲染帧的 pts（算音画偏差） */
    const char* clk_source_ = "wall";  /* 当前主时钟来源 */
    int64_t  clk_skew_      = 0;   /* 音频时钟 - 墙钟（毫秒） */
    int64_t  wait_since_ms_ = 0;   /* 「队首还没到点」的起始时刻（诊断卡顿） */
    bool     wait_logged_   = false;
};

#endif /* MOQ_RENDER_H */
