/* ============================================================
 * moq_render.cpp — 队列 + SDL3 渲染/播放实现
 * ============================================================ */

#include "moq_render.h"

#include "logger.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

extern "C" {
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace {

/* 落后主时钟超过该值就丢弃该视频帧（毫秒） */
const int64_t kLateDropMs = 300;

/* 判定「这帧到点了」的容差（毫秒） */
const int64_t kDueToleranceMs = 8;

/* 音频时钟相对墙钟的允许偏差（毫秒）。
 * 超出即视为「设备积压读数异常」，本帧改用墙钟，防止时钟瞬跳导致误丢帧。 */
const int64_t kAudioClkToleranceMs = 250;

/* 队首领先时钟超过该值，判定为时间轴断层，需要跳过去而不是干等（毫秒）。
 * 正常情况下队首与时钟基本重合（误差只有 kDueToleranceMs 量级）。 */
const int64_t kResyncGapMs = 500;

/* 单帧字节数（音频 F32 interleaved） */
int AudioFrameBytes(const AVFrame* f) {
    const int bps = av_get_bytes_per_sample(static_cast<AVSampleFormat>(f->format));
    if (bps <= 0) return 0;
    return f->nb_samples * f->ch_layout.nb_channels * bps;
}

} /* namespace */

/* ============================================================
 * FrameQueue
 * ============================================================ */

FrameQueue::FrameQueue(const char* name) : name_(name) {}

FrameQueue::~FrameQueue() {
    Clear();
}

void FrameQueue::SetLimits(int window_ms, size_t max_frames) {
    std::lock_guard<std::mutex> lk(mutex_);
    window_ms_ = window_ms;
    max_frames_ = max_frames;
    TrimLocked();
}

int FrameQueue::Push(const AVFrame* frame, int64_t pts_ms, int64_t duration_ms) {
    if (!frame) return 0;

    AVFrame* clone = av_frame_clone(frame);
    if (!clone) return 0;

    {
        std::lock_guard<std::mutex> lk(mutex_);

        /* ---- 溢出策略：丢「新到的这帧」，而不是丢队首 ----
         *
         * 队首就是播放位置（下一次要取的那帧）。如果溢出时把它丢掉，
         * 等于在没有通知主时钟的情况下来了一次 seek：队首被推到更晚的
         * pts，而时钟还按原速走，于是队首永远「还没到点」，画面定格。
         * （实测：服务端订阅瞬间补发 5.5s 缓存 GOP，把队列顶到窗口上限，
         *   连丢几十帧后队首领先时钟约 900ms 且不再收敛 → 卡在第一帧。）
         *
         * 播放位置应该只由「消费」推进，所以这里丢弃新到的帧，让缓冲保持
         * 在窗口深度；代价是突发期间的额外延迟，而不是画面停住。
         * 真正「已经晚了」的帧由 RenderVideo 里的 DropBefore(clock - 300)
         * 显式处理。 */
        if (max_frames_ > 0 && items_.size() >= max_frames_) {
            av_frame_free(&clone);
            return 1;
        }
        if (window_ms_ > 0 && !items_.empty()) {
            const int64_t front   = items_.front().pts_ms;
            const int64_t cur_end = items_.back().pts_ms + items_.back().duration_ms;
            const int64_t new_end = pts_ms + duration_ms;
            const int64_t end     = new_end > cur_end ? new_end : cur_end;
            if (end - front > window_ms_ && pts_ms >= front) {
                av_frame_free(&clone);
                return 1;
            }
        }

        FrameItem item;
        item.frame = clone;
        item.pts_ms = pts_ms;
        item.duration_ms = duration_ms;

        /* 快路径：pts 单调递增（正常情况），直接尾插 O(1) */
        if (items_.empty() || pts_ms >= items_.back().pts_ms) {
            items_.push_back(item);
        } else {
            /* 真出现乱序才做有序插入 */
            auto it = std::upper_bound(
                items_.begin(), items_.end(), pts_ms,
                [](int64_t v, const FrameItem& fi) { return v < fi.pts_ms; });
            items_.insert(it, item);
        }
    }
    return 0;
}

void FrameQueue::TrimLocked() {
    while (!items_.empty()) {
        const bool too_many = max_frames_ > 0 && items_.size() > max_frames_;
        const bool too_long = window_ms_ > 0 && items_.size() > 1 &&
                              ((items_.back().pts_ms + items_.back().duration_ms) -
                               items_.front().pts_ms) > window_ms_;
        if (!too_many && !too_long) break;
        av_frame_free(&items_.front().frame);
        items_.pop_front();
    }
}

bool FrameQueue::Empty() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return items_.empty();
}

size_t FrameQueue::Size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return items_.size();
}

int64_t FrameQueue::BufferedMs() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (items_.empty()) return 0;
    return (items_.back().pts_ms + items_.back().duration_ms) - items_.front().pts_ms;
}

int64_t FrameQueue::FrontPts() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return items_.empty() ? 0 : items_.front().pts_ms;
}

int64_t FrameQueue::BackPts() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return items_.empty() ? 0 : items_.back().pts_ms;
}

AVFrame* FrameQueue::Pop(int64_t* pts_out) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (items_.empty()) return nullptr;
    FrameItem item = items_.front();
    items_.pop_front();
    if (pts_out) *pts_out = item.pts_ms;
    return item.frame;   /* 调用方负责 av_frame_free */
}

int FrameQueue::DropBefore(int64_t pts_ms) {
    int dropped = 0;
    std::lock_guard<std::mutex> lk(mutex_);
    while (!items_.empty() && items_.front().pts_ms < pts_ms) {
        av_frame_free(&items_.front().frame);
        items_.pop_front();
        dropped++;
    }
    return dropped;
}

void FrameQueue::Clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& it : items_) av_frame_free(&it.frame);
    items_.clear();
}

/* ============================================================
 * VideoSink
 * ============================================================ */

VideoSink::VideoSink() = default;

VideoSink::~VideoSink() {
    Destroy();
}

void VideoSink::Destroy() {
    if (texture_)  { SDL_DestroyTexture(texture_);   texture_  = nullptr; }
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_   = nullptr; }
    width_ = height_ = 0;
}

bool VideoSink::Init(std::string* err) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        if (err) *err = std::string("SDL_InitSubSystem(VIDEO) 失败: ") + SDL_GetError();
        return false;
    }
    return true;
}

void VideoSink::Configure(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (texture_ && width == width_ && height == height_) return;

    if (!window_) {
        window_ = SDL_CreateWindow("moq_player", width, height, SDL_WINDOW_RESIZABLE);
        if (!window_) {
            LOG_ERROR("SDL_CreateWindow 失败: %s", SDL_GetError());
            return;
        }
    }

    if (!renderer_) {
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            LOG_ERROR("SDL_CreateRenderer 失败: %s", SDL_GetError());
            return;
        }
        /* vsync 由渲染器自己节流，避免主循环忙等 */
        SDL_SetRenderVSync(renderer_, 1);
    }

    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }

    /* YUV420P 直接映射 IYUV，省掉 swscale 转 RGB 的开销 */
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture_) {
        LOG_ERROR("SDL_CreateTexture(IYUV %dx%d) 失败: %s",
                  width, height, SDL_GetError());
        return;
    }

    /* 分辨率变了就同步调整窗口尺寸 */
    if (width != width_ || height != height_) {
        SDL_SetWindowSize(window_, width, height);
    }

    width_ = width;
    height_ = height;
    LOG_INFO("video sink 已就绪: %dx%d  IYUV 直传", width, height);
}

void VideoSink::Present(AVFrame* frame) {
    if (!frame || !texture_ || !renderer_) return;

    if (SDL_UpdateYUVTexture(texture_, nullptr,
                             frame->data[0], frame->linesize[0],
                             frame->data[1], frame->linesize[1],
                             frame->data[2], frame->linesize[2])) {
        SDL_RenderClear(renderer_);
        SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }
}

void VideoSink::ClearScreen() {
    if (!renderer_) return;
    SDL_RenderClear(renderer_);
    SDL_RenderPresent(renderer_);
}

bool VideoSink::SaveSnapshot(const char* path) {
    if (!renderer_ || !path) return false;
    SDL_Surface* surf = SDL_RenderReadPixels(renderer_, nullptr);
    if (!surf) {
        LOG_ERROR("SDL_RenderReadPixels 失败: %s", SDL_GetError());
        return false;
    }
    const bool ok = SDL_SaveBMP(surf, path);
    if (!ok) {
        LOG_ERROR("SDL_SaveBMP(%s) 失败: %s", path, SDL_GetError());
    }
    SDL_DestroySurface(surf);
    return ok;
}

/* ============================================================
 * AudioSink
 * ============================================================ */

AudioSink::AudioSink() = default;

AudioSink::~AudioSink() {
    Close();
}

void AudioSink::Close() {
    if (stream_) {
        /* SDL_DestroyAudioStream 会连带关闭绑定到它上面的设备 */
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

bool AudioSink::Open(int sample_rate, int channels, std::string* err) {
    if (stream_) return true;

    sample_rate_ = sample_rate;
    channels_ = channels;
    bytes_per_sample_frame_ = 4 * channels;   /* F32 */

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = channels;
    spec.freq = sample_rate;

    /* NULL 回调 = 纯推模式：我们自己往流里塞数据，设备自己抽干 */
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                        &spec, nullptr, nullptr);
    if (!stream_) {
        if (err) *err = std::string("SDL_OpenAudioDeviceStream 失败: ") + SDL_GetError();
        return false;
    }

    SDL_ResumeAudioStreamDevice(stream_);
    LOG_INFO("audio sink 已就绪: %dHz %dch F32", sample_rate, channels);
    return true;
}

bool AudioSink::Feed(const AVFrame* frame) {
    if (!stream_ || !frame || !frame->data[0]) return false;
    const int bytes = AudioFrameBytes(frame);
    if (bytes <= 0) return false;
    return SDL_PutAudioStreamData(stream_, frame->data[0], bytes);
}

int AudioSink::QueuedBytes() const {
    if (!stream_) return 0;
    return SDL_GetAudioStreamQueued(stream_);
}

void AudioSink::ClearQueued() {
    if (stream_) SDL_ClearAudioStream(stream_);
}

int AudioSink::bytes_per_ms() const {
    return bytes_per_sample_frame_ * sample_rate_ / 1000;
}

/* ============================================================
 * Player
 * ============================================================ */

Player::Player()
    : video_q_("video"), audio_q_("audio") {}

Player::~Player() {
    Shutdown();
}

void Player::Shutdown() {
    if (shutdown_) return;
    shutdown_ = true;

    video_q_.Clear();
    audio_q_.Clear();

    /* 顺序很重要：先释放依赖 SDL 子系统的对象，最后才 SDL_Quit。
     * 反过来（先 SDL_Quit 再 SDL_Destroy*）会 use-after-free，退出时 SIGSEGV。 */
    audio_sink_.Close();
    video_sink_.Destroy();
    SDL_Quit();
}

bool Player::Init(int av_sync_ms, int out_rate, int out_channels, std::string* err) {
    av_sync_ms_ = av_sync_ms > 0 ? av_sync_ms : 0;
    out_rate_ = out_rate;
    out_channels_ = out_channels;

    /* 队列约束：时间窗口 + 帧数上限双保险。
     * 直通模式下窗口给小，保证低延迟；同步模式窗口随 av_sync 放大。 */
    int vwin = av_sync_ms_ > 0 ? av_sync_ms_ * 2 : 500;
    size_t vmax = av_sync_ms_ > 0 ? 120 : 30;
    video_q_.SetLimits(vwin, vmax);

    int awin = av_sync_ms_ > 0 ? av_sync_ms_ * 2 : 800;
    size_t amax = av_sync_ms_ > 0 ? 300 : 120;
    audio_q_.SetLimits(awin, amax);

    /* 重新缓冲的目标深度：就是用户指定的预缓冲量（至少 300ms）。
     * 出问题时宁可停一下攒回这么多，也不带着空缓冲硬播。 */
    rebuffer_target_ms_ = av_sync_ms_ > 0 ? av_sync_ms_ : 0;
    if (rebuffer_target_ms_ > 0 && rebuffer_target_ms_ < 300) rebuffer_target_ms_ = 300;

    if (!video_sink_.Init(err)) return false;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_WARN("SDL 音频子系统初始化失败: %s（将无声播放）", SDL_GetError());
    } else {
        std::string aerr;
        audio_opened_ = audio_sink_.Open(out_rate_, out_channels_, &aerr);
        if (!audio_opened_) {
            LOG_WARN("音频设备打开失败: %s（将无声播放）", aerr.c_str());
        }
    }

    LOG_INFO("播放模式: %s", mode_name());
    return true;
}

const char* Player::mode_name() const {
    return av_sync_ms_ > 0 ? "av_sync 精确同步" : "直通（最低延迟）";
}

void Player::PushVideo(const AVFrame* frame, int64_t pts_ms) {
    const int d = video_q_.Push(frame, pts_ms, 0);
    if (d > 0) {
        dropped_      += static_cast<uint64_t>(d);
        dropped_push_ += static_cast<uint64_t>(d);
    }
}

void Player::PushAudio(const AVFrame* frame, int64_t pts_ms) {
    const int dur = (out_rate_ > 0 && frame)
                        ? static_cast<int>(frame->nb_samples * 1000LL / out_rate_)
                        : 0;
    const int d = audio_q_.Push(frame, pts_ms, dur);
    if (d > 0) {
        dropped_      += static_cast<uint64_t>(d);
        dropped_push_ += static_cast<uint64_t>(d);
    }
}

bool Player::Finished() const {
    return input_done_ && video_q_.Empty() && audio_q_.Empty();
}

bool Player::PollEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) {
            quit_ = true;
            return false;
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) {
            LOG_INFO("收到 ESC，准备退出");
            quit_ = true;
            return false;
        }
    }
    return true;
}

int64_t Player::MediaClockMs() {
    if (!started_) return anchor_pts_;

    /* 墙钟基准：起播锚点 + 实际流逝时间。它是兜底，也是上限/下限的参照。 */
    const int64_t wall =
        anchor_pts_ + static_cast<int64_t>(SDL_GetTicks() - anchor_wall_);
    int64_t clk = wall;

    /* 有音频时以音频播放位置为主时钟：设备匀速抽干，最稳。
     *
     * 但 audio_clock 是由「已发送字节 − 设备积压字节」推算的，SDLK 的
     * QueuedBytes() 在刚开流/刚灌数据的瞬间可能短期报不准（典型是报 0），
     * 这会让 played 被高估，音频时钟瞬时钟前跳（实测跳 ~2s）。一旦前跳，
     * 下面 DropBefore(clock - 300) 会把队首一大片帧当“迟到”全丢掉，
     * 而队首 pts 从此永远领先真实时钟 → 画面卡在第一帧不动。
     *
     * 所以对音频时钟做双向夹紧：只接受落在墙钟 ±250ms 内的值，
     * 超出就判定为读数异常，退回墙钟。正常播放时音频与墙钟偏差远小于此，
     * 夹紧不会影响音画同步精度。 */
    if (audio_opened_ && audio_base_set_) {
        const int64_t queued = audio_sink_.QueuedBytes();
        const int64_t played = audio_bytes_sent_ - queued;
        const int bpm = audio_sink_.bytes_per_ms();
        if (played >= 0 && bpm > 0) {
            const int64_t audio_clock = audio_base_pts_ + played / bpm;

            /* 诊断：每秒打印一次时钟内部量（--log-level debug 才输出） */
            const int64_t now_ticks = static_cast<int64_t>(SDL_GetTicks());
            if (now_ticks - last_clk_log_ms_ >= 1000) {
                last_clk_log_ms_ = now_ticks;
                LOG_DEBUG("[clk] wall=%lld audio=%lld base_pts=%lld "
                          "played=%lldms queued=%lldms sent=%lldB bpm=%d "
                          "vfront=%lld",
                          (long long)(wall - anchor_pts_),
                          (long long)(audio_clock - anchor_pts_),
                          (long long)audio_base_pts_,
                          (long long)(played / bpm), (long long)(queued / bpm),
                          (long long)audio_bytes_sent_, bpm,
                          (long long)video_q_.FrontPts());
            }

            const int64_t diff = audio_clock - wall;
            const bool ok = (diff >= -kAudioClkToleranceMs &&
                             diff <= kAudioClkToleranceMs);
            const char* src = ok ? "audio" : "wall";
            if (std::strcmp(src, clk_source_) != 0) {
                LOG_DEBUG("[evt] 主时钟切换 %s -> %s（音频-墙钟偏差 %+" PRId64
                          "ms，阈值 ±%d）",
                          clk_source_, src, diff, kAudioClkToleranceMs);
                clk_source_ = src;
            }
            clk_skew_ = diff;
            if (ok) clk = audio_clock;
        }
    }

    /* 单调保护：时钟绝不回退。否则前后两次判定用的基准不一致，
     * 会出现「先判迟到丢一批、再判未到点等着」的抖动。 */
    if (clk < last_clock_ms_) clk = last_clock_ms_;
    last_clock_ms_ = clk;
    return clk;
}

void Player::ResyncTo(int64_t pts) {
    const int64_t old_anchor = anchor_pts_;
    resync_count_++;
    anchor_pts_    = pts;
    anchor_wall_   = static_cast<int64_t>(SDL_GetTicks());
    last_clock_ms_ = pts;

    /* 音频必须跟着跳，否则视频跳了、音频还在原处，音画差好几秒 */
    const int da = audio_q_.DropBefore(pts);
    if (audio_opened_) {
        audio_sink_.ClearQueued();     /* 设备里还没播的也丢掉 */
        audio_bytes_sent_ = 0;         /* 重置 played 计算基准 */
        audio_base_set_   = false;     /* 下一帧音频重新确立基准 pts */
    }
    dropped_ += static_cast<uint64_t>(da);

    LOG_DEBUG("播放位置跳转：%" PRId64 "ms（原 %" PRId64 "ms，差 %" PRId64
              "ms；同步丢弃音频 %d 帧）[此前 front=%" PRId64 " back=%" PRId64
              " 缓冲=%" PRId64 "ms]",
              pts, old_anchor, pts - old_anchor, da,
              video_q_.FrontPts(), video_q_.BackPts(), video_q_.BufferedMs());
}

void Player::EnterRebuffer() {
    if (rebuffering_) return;
    rebuffering_       = true;
    rebuffer_count_++;
    rebuffer_start_ms_ = static_cast<int64_t>(SDL_GetTicks());

    /* 音频也要停：清掉设备里没播完的，暂停投喂，否则画面停着声音还在走 */
    if (audio_opened_) {
        audio_sink_.ClearQueued();
        audio_bytes_sent_ = 0;
        audio_base_set_   = false;
    }
    LOG_INFO("缓冲中……（目标 %" PRId64 "ms，当前 %" PRId64 "ms）",
             rebuffer_target_ms_, video_q_.BufferedMs());
}

void Player::FeedAudio() {
    if (!audio_opened_) {
        /* 没有音频设备就把音频帧丢掉，避免队列无限涨 */
        for (;;) {
            AVFrame* f = audio_q_.Pop();
            if (!f) break;
            av_frame_free(&f);
        }
        return;
    }

    /* 同步模式下，未起播前先攒着，保证音画从同一时刻开始；
     * 重新缓冲期间同样暂停投喂（画面都停了，声音不能继续跑） */
    if (av_sync_ms_ > 0 && (!started_ || rebuffering_)) return;

    /* 喂水线：设备里已积压够多就先不喂，否则延迟会一路涨上去。
     * 直通模式控制在 200ms，同步模式控制在 av_sync 深度。 */
    const int bpm = audio_sink_.bytes_per_ms();
    const int limit_ms = av_sync_ms_ > 0 ? av_sync_ms_ : 200;
    const int64_t max_queued = static_cast<int64_t>(bpm) * limit_ms;

    for (;;) {
        if (bpm > 0 && audio_sink_.QueuedBytes() >= max_queued) break;

        int64_t pts = 0;
        AVFrame* f = audio_q_.Pop(&pts);
        if (!f) break;

        if (!audio_base_set_) {
            audio_base_pts_ = pts;
            audio_base_set_ = true;
        }
        if (audio_sink_.Feed(f)) {
            audio_bytes_sent_ += AudioFrameBytes(f);
            fed_audio_++;
        }
        av_frame_free(&f);
    }
}

void Player::RenderVideo() {
    /* ---- 同步模式：先攒够缓冲再起播 ---- */
    if (av_sync_ms_ > 0 && !started_) {
        const bool v_ready = video_q_.BufferedMs() >= av_sync_ms_;
        /* 纯音频流（没有视频轨）也要能起播 */
        const bool a_only_ready = video_q_.Empty() &&
                                  audio_q_.BufferedMs() >= av_sync_ms_;

        /* 关键：输入已结束时（Ctrl-C / 流断 / 时长到）不再死等预缓冲。
         * 否则缓冲差一点没攒满就永远起不了播，队列也不消费，
         * Finished() 永远不为真 —— 表现为退出时卡死。 */
        if (!v_ready && !a_only_ready && !input_done_) return;

        if (video_q_.Empty() && audio_q_.Empty()) {
            started_ = true;   /* 没数据，直接放行，避免卡住退出流程 */
            return;
        }

        if (video_q_.Empty()) {
            anchor_pts_ = audio_q_.FrontPts();
        } else {
            anchor_pts_ = video_q_.FrontPts();
            /* 音频从同一媒体时间起播，避免音画错位 */
            if (audio_opened_ && !audio_q_.Empty()) {
                anchor_pts_ = std::min(anchor_pts_, audio_q_.FrontPts());
            }
        }
        anchor_wall_ = static_cast<int64_t>(SDL_GetTicks());
        started_ = true;
        last_clock_ms_ = anchor_pts_;   /* 单调保护从锚点起步 */

        const int dv = video_q_.DropBefore(anchor_pts_);
        const int da = audio_q_.DropBefore(anchor_pts_);
        LOG_INFO("预缓冲完成，开始播放：锚点=%" PRId64 "ms  video缓冲=%" PRId64
                 "ms  丢弃 v=%d a=%d%s",
                 anchor_pts_, video_q_.BufferedMs(), dv, da,
                 input_done_ ? "（输入已结束，按现有缓冲起播）" : "");
        dropped_ += static_cast<uint64_t>(dv + da);
        return;
    }

    if (video_q_.Empty()) return;

    /* ---- 直通模式：来即渲染 ---- */
    if (av_sync_ms_ == 0) {
        if (!started_) {
            anchor_pts_ = video_q_.FrontPts();
            anchor_wall_ = static_cast<int64_t>(SDL_GetTicks());
            started_ = true;
        }
        int64_t pts = 0;
        AVFrame* f = video_q_.Pop(&pts);
        if (!f) return;
        video_sink_.Configure(f->width, f->height);
        video_sink_.Present(f);
        av_frame_free(&f);
        last_render_pts_ = pts;
        AfterRender();
        return;
    }

    /* ---- 同步模式：按 pts 对齐主时钟 ---- */

    /* 重新缓冲中：攒够目标深度再恢复，恢复时把播放位置对齐到队首 */
    if (rebuffering_) {
        const bool enough = video_q_.BufferedMs() >= rebuffer_target_ms_;
        const bool stalled = static_cast<int64_t>(SDL_GetTicks()) - rebuffer_start_ms_ > 5000;
        if (!enough && !input_done_ && !(stalled && !video_q_.Empty())) return;

        const int64_t pts = video_q_.Empty() ? last_clock_ms_ : video_q_.FrontPts();
        ResyncTo(pts);
        rebuffering_ = false;
        LOG_INFO("缓冲完成，恢复播放（可用 %" PRId64 "ms）", video_q_.BufferedMs());
        return;
    }

    /* 欠载：没有可播的帧。稳定优先 —— 不硬撑，重新攒缓冲。
     * 收尾阶段（输入已结束）不重缓冲，直接返回让主循环判 Finished 退出。 */
    if (video_q_.Empty()) {
        if (input_done_) return;
        EnterRebuffer();
        return;
    }

    int64_t clock = MediaClockMs();

    /* 时间轴断层（队首远超前于时钟）：典型是订阅时补发的缓存 GOP 被丢弃、
     * 缓存放完后直接接到直播最新位置，中间有几秒空档。
     * 稳定优先：不硬接（硬接会冻结整个空档），而是重新缓冲后从队首恢复。
     * 收尾阶段没有后续数据可等，直接跳过去把剩下的播完。 */
    if (video_q_.FrontPts() - clock > kResyncGapMs) {
        if (input_done_) {
            ResyncTo(video_q_.FrontPts());
            clock = MediaClockMs();
        } else {
            EnterRebuffer();
            return;
        }
    }

    /* 落后的帧直接丢，避免越积越多 */
    const int late = video_q_.DropBefore(clock - kLateDropMs);
    if (late > 0) {
        dropped_      += static_cast<uint64_t>(late);
        dropped_late_ += static_cast<uint64_t>(late);
    }
    if (video_q_.Empty()) return;

    if (video_q_.FrontPts() > clock + kDueToleranceMs) {
        /* 诊断：队首长期「还没到点」正是画面卡住的典型特征，持续 1 秒报一次警 */
        const int64_t now_ms = static_cast<int64_t>(SDL_GetTicks());
        if (wait_since_ms_ == 0) {
            wait_since_ms_ = now_ms;
        } else if (!wait_logged_ && now_ms - wait_since_ms_ > 1000) {
            wait_logged_ = true;
            LOG_INFO("[evt] 等待队首到点: 领先主时钟 %" PRId64 "ms，已持续 %" PRId64
                     "ms（队首=%" PRId64 " 时钟=%" PRId64 " 时钟源=%s）",
                     video_q_.FrontPts() - clock, now_ms - wait_since_ms_,
                     video_q_.FrontPts(), clock, clk_source_);
        }
        return;   /* 还没到点 */
    }
    wait_since_ms_ = 0;
    wait_logged_   = false;

    int64_t pts = 0;
    AVFrame* f = video_q_.Pop(&pts);
    if (!f) return;
    video_sink_.Configure(f->width, f->height);
    video_sink_.Present(f);
    av_frame_free(&f);
    last_render_pts_ = pts;
    AfterRender();
}

void Player::AfterRender() {
    rendered_++;
    if (!snapshot_done_ && snapshot_at_ > 0 && rendered_ >= snapshot_at_) {
        snapshot_done_ = true;
        if (video_sink_.SaveSnapshot(snapshot_path_.c_str())) {
            LOG_INFO("已抓取渲染快照: %s（第 %" PRIu64 " 帧）",
                     snapshot_path_.c_str(), rendered_);
        }
    }
}

bool Player::Iterate() {
    if (!PollEvents()) return false;

    FeedAudio();
    RenderVideo();
    LogDiag();

    /* 没到点的帧可渲染时，vsync 会自己节流；无帧可渲时歇 1ms 避免空转 */
    if (video_q_.Empty()) {
        SDL_Delay(1);
    }
    return true;
}

/* 每秒一行诊断（仅 --log-level debug 输出）。
 * 设计目标：一行内给足「时钟 / 队列 / 音画偏差 / 丢弃原因 / 状态」，
 * 复现卡顿后只看这些行就能判断是哪种失效。 */
void Player::LogDiag() {
    const int64_t now = static_cast<int64_t>(SDL_GetTicks());
    if (now - diag_last_ms_ < 1000) return;
    diag_last_ms_ = now;

    const int64_t wall = started_ ? anchor_pts_ + (now - anchor_wall_) : 0;

    const int     bpm    = audio_opened_ ? audio_sink_.bytes_per_ms() : 0;
    const int64_t queued = audio_opened_ ? audio_sink_.QueuedBytes() : 0;
    const int64_t played_ms = (bpm > 0) ? (audio_bytes_sent_ - queued) / bpm : -1;
    const int64_t apos = (audio_base_set_ && played_ms >= 0)
                             ? audio_base_pts_ + played_ms : -1;
    const int64_t avskew = (last_render_pts_ >= 0 && apos >= 0)
                               ? last_render_pts_ - apos : 0;
    const int64_t aq_ms = (bpm > 0) ? queued / bpm : 0;

    LOG_DEBUG("[diag] t=%" PRId64 "s 时钟=%" PRId64 "(%s 偏差%+" PRId64 ")"
              " 墙钟=%" PRId64 " | V 首=%" PRId64 " 尾=%" PRId64 " 缓冲=%" PRId64
              "ms n=%zu | A 位置=%" PRId64 " 缓冲=%" PRId64 "ms n=%zu"
              " 设备积压=%" PRId64 "ms | 音画差=%+" PRId64 "ms"
              " | 渲染=%" PRIu64 " 喂音=%" PRIu64 " 丢晚=%" PRIu64
              " 丢满=%" PRIu64 " 跳转=%" PRIu64 " 重缓冲=%" PRIu64 " %s",
              now / 1000, last_clock_ms_, clk_source_, clk_skew_, wall,
              video_q_.FrontPts(), video_q_.BackPts(), video_q_.BufferedMs(),
              video_q_.Size(),
              apos, audio_q_.BufferedMs(), audio_q_.Size(), aq_ms,
              avskew, rendered_, fed_audio_, dropped_late_, dropped_push_,
              resync_count_, rebuffer_count_,
              rebuffering_ ? "[缓冲中]" : (started_ ? "" : "[未起播]"));
}
