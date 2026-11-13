/* ============================================================
 * moq_player — Media over QUIC 播放器
 *
 * 第 1 步：拉流层
 *   通过 upstream/moq_transport_client（moq-transport draft-22 C SDK）
 *   以 SUBSCRIBE 方式向 moq 服务器拉流。
 *
 * 第 2 步：解码层（moq_decoder.h/.cpp）
 *   H.264（AVCC + avcC）→ AVFrame YUV420P
 *   AAC（裸帧 + ASC）→ AVFrame PCM，经 avfilter(aresample+aformat)
 *   转成 48000Hz / 立体声 / F32 interleaved。
 *
 * 第 3 步：播放层（moq_render.h/.cpp）
 *   SDL3 渲染视频（IYUV 直传）+ 音频设备播放。
 *
 * 线程模型：
 *   后台线程 = uv loop 拉流 + FFmpeg 解码 → 把 AVFrame 推进两个队列
 *   主线程   = SDL 事件循环 + 视频渲染 + 音频连续喂
 *   （SDL3 在 macOS 上要求窗口/渲染在主线程）
 *
 * 日志统一走 moqlivecast 的 logger（upstream/logger/logger.c）：
 *   LOG_ERROR / LOG_WARN / LOG_INFO / LOG_DEBUG
 * 该 logger 是异步的（独立日志线程 + 队列），退出时必须 log_shutdown()
 * 才会把队列里剩余的行刷完。
 *
 * 用法：
 *   moq_player <host> <port> <app> <stream> [选项]
 *
 *   host       IPv4 地址或域名（QUIC 栈仅支持 IPv4）
 *   port       服务器端口，通常 4433
 *   app        命名空间
 *   stream     流名
 *
 * 选项：
 *   --av-sync <ms>      音视频同步开关：0=直通（默认，最低延迟），
 *                       >0=预缓冲该毫秒数后按 pts 精确同步（如 2000）
 *   --path <p>          WebTransport 路径，默认 /moq
 *   --duration <sec>    运行 N 秒后自动退出（默认一直跑到 Ctrl-C/关窗）
 *   --out-rate <hz>     avfilter 音频输出采样率，默认 48000
 *   --out-channels <n>  avfilter 音频输出声道数，默认 2
 *   --log-packets <n>   每条轨前 n 个报文逐条打印，0 表示全部，默认 0（关）
 *   --log-frames <n>    前 n 个解码帧逐帧打印，0 表示全部，默认 5
 *   --dump-yuv <file>   把解码后的 YUV420P 落盘（裸流，可用 ffprobe 校验）
 *   --dump-pcm <file>   把 avfilter 转换后的 PCM 落盘（F32 裸流）
 *   --log-level <l>     error|warn|info|debug，默认 info
 *   --log-file <f>      额外把日志追加写入文件
 *
 * 示例：
 *   ./build/bin/moq_player 127.0.0.1 4433 live demo
 *   ./build/bin/moq_player 127.0.0.1 4433 live demo --av-sync 2000
 * ============================================================ */

#include "moq_transport_client.h"

#include "logger.h"
#include "moq_decoder.h"
#include "moq_render.h"

#include <uv.h>
#include <netdb.h>
#include <sys/socket.h>

#include <SDL3/SDL_timer.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <string>
#include <strings.h>   /* strcasecmp */
#include <thread>

namespace {

/* ------------------------- 工具 ------------------------- */

const char* ResultName(moqt_result_t r) {
    switch (r) {
        case MOQT_OK:          return "OK";
        case MOQT_ERR_INVALID: return "INVALID";
        case MOQT_ERR_STATE:   return "STATE";
        case MOQT_ERR_CONNECT: return "CONNECT";
        case MOQT_ERR_TIMEOUT: return "TIMEOUT";
        case MOQT_ERR_CLOSED:  return "CLOSED";
        case MOQT_ERR_AGAIN:   return "AGAIN";
        case MOQT_ERR_WRITE:   return "WRITE";
        case MOQT_ERR_NOMEM:   return "NOMEM";
        default:               return "UNKNOWN";
    }
}

const char* CodecName(moqt_codec_t c) {
    return c == MOQT_CODEC_H264 ? "H264" : "AAC";
}

/* 最多打印 n 字节的十六进制预览 */
std::string HexPreview(const uint8_t* p, size_t len, size_t n = 16) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(n * 3 + 4);
    const size_t m = len < n ? len : n;
    for (size_t i = 0; i < m; ++i) {
        if (i) out.push_back(' ');
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 0x0F]);
    }
    if (len > m) out += " ...";
    return out;
}

/* 解析 avcC，返回 profile/level 与 SPS/PPS 概况 */
std::string DescribeAvcC(const uint8_t* c, size_t n) {
    char buf[512];
    if (n < 7) {
        snprintf(buf, sizeof(buf), "avcC 过短（%zu 字节），无法解析", n);
        return buf;
    }
    const unsigned profile  = c[1];
    const unsigned level    = c[3];
    const unsigned nalu_len = (c[4] & 0x03) + 1;
    const unsigned n_sps    = c[5] & 0x1F;

    std::string out;
    snprintf(buf, sizeof(buf),
             "avcC: profile=%u level=%u nal_length=%u sps_count=%u",
             profile, level, nalu_len, n_sps);
    out += buf;

    size_t off = 6;
    if (n_sps > 0 && off + 2 <= n) {
        const unsigned sps_len = (c[off] << 8) | c[off + 1];
        snprintf(buf, sizeof(buf), " sps_len=%u", sps_len);
        out += buf;
        off += 2 + sps_len;
    }
    if (off + 1 <= n) {
        const unsigned n_pps = c[off];
        snprintf(buf, sizeof(buf), " pps_count=%u", n_pps);
        out += buf;
    }
    out += " | hex=";
    out += HexPreview(c, n, 24);
    return out;
}

/* 解析 AudioSpecificConfig */
std::string DescribeAsc(const uint8_t* c, size_t n) {
    char buf[512];
    if (n < 2) {
        snprintf(buf, sizeof(buf), "ASC 过短（%zu 字节），无法解析", n);
        return buf;
    }
    static const int kRates[13] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350
    };
    const unsigned aot      = (c[0] >> 3) & 0x1F;
    const unsigned rate_idx = ((c[0] & 0x07) << 1) | ((c[1] >> 7) & 0x01);
    const unsigned chans    = (c[1] >> 3) & 0x0F;

    char rate_buf[32];
    const char* rate_str = "custom";
    if (rate_idx < 13) {
        snprintf(rate_buf, sizeof(rate_buf), "%dHz", kRates[rate_idx]);
        rate_str = rate_buf;
    }

    std::string out;
    snprintf(buf, sizeof(buf), "ASC: object_type=%u rate=%s channels=%u",
             aot, rate_str, chans);
    out += buf;
    out += " | hex=";
    out += HexPreview(c, n, 24);
    return out;
}

/* 解析 AVCC 包内第一个 NAL 的类型 */
std::string DescribeFirstNal(const uint8_t* d, size_t len) {
    char buf[96];
    if (len < 5) return std::string();

    const uint32_t nal_len = (static_cast<uint32_t>(d[0]) << 24) |
                             (static_cast<uint32_t>(d[1]) << 16) |
                             (static_cast<uint32_t>(d[2]) << 8) |
                              static_cast<uint32_t>(d[3]);
    if (nal_len == 0 || nal_len > len - 4) {
        snprintf(buf, sizeof(buf), "nal_len=%u 非AVCC?", nal_len);
        return buf;
    }

    const unsigned type = d[4] & 0x1F;
    const char* name = "other";
    switch (type) {
        case 1:  name = "non-IDR"; break;
        case 5:  name = "IDR";     break;
        case 6:  name = "SEI";     break;
        case 7:  name = "SPS";     break;
        case 8:  name = "PPS";     break;
        case 9:  name = "AUD";     break;
        default: break;
    }
    snprintf(buf, sizeof(buf), "nal_len=%u type=%u %s", nal_len, type, name);
    return buf;
}

/* ------------------------- 运行状态 ------------------------- */

struct TrackStat {
    uint64_t objects   = 0;
    uint64_t bytes     = 0;
    uint64_t keyframes = 0;
    int64_t  first_dts = -1;
    int64_t  last_dts  = -1;
    int64_t  prev_dts  = -1;
    bool     config_logged = false;
};

struct App {
    /* --- uv loop（后台线程） --- */
    uv_loop_t        loop;
    uv_async_t       stop_async;
    uv_timer_t       duration_timer;
    uv_timer_t       finish_timer;
    uv_timer_t       stats_timer;
    uv_signal_t      sigint;
    uv_getaddrinfo_t resolve;

    moqt_client_t* client = nullptr;
    Player*        player = nullptr;

    std::string path = "/moq";
    std::string host;
    std::string connect_ip;
    std::string appname;
    std::string stream;
    int    port         = 4433;
    int    log_packets  = 0;
    int    log_frames   = 5;
    double duration_sec = 0;

    int  out_rate     = 48000;
    int  out_channels = 2;
    int  av_sync_ms   = 0;
    std::string snapshot_path;   /* 渲染快照（验证用） */

    std::atomic<bool> failed{false};
    std::atomic<bool> done{false};
    std::atomic<bool> got_media{false};

    /* --- 解码器 --- */
    std::unique_ptr<VideoDecoder> vdec;
    std::unique_ptr<AudioDecoder> adec;

    /* 音频 pts 自己按样本数推进：avfilter 重采样后 pts 不可靠 */
    bool    audio_pts_seeded = false;
    int64_t audio_first_pts  = 0;
    int64_t audio_pts_next   = 0;

    /* --- 到达侧诊断：时间戳跳变/乱序检测（-1 表示还没收到过） --- */
    int64_t  in_last_v_pts = -1;
    int64_t  in_last_a_pts = -1;
    uint64_t in_v_jumps    = 0;   /* 视频 pts 跳变次数 */
    uint64_t in_a_jumps    = 0;   /* 音频 pts 跳变次数 */
    uint64_t in_v_backwards = 0;  /* 视频 pts 倒退次数 */

    /* --- 落盘 --- */
    FILE*    dump_yuv = nullptr;
    FILE*    dump_pcm = nullptr;
    uint64_t dump_yuv_frames = 0;
    uint64_t dump_pcm_frames = 0;

    /* --- 解码统计 --- */
    uint64_t decoded_video = 0;
    uint64_t decoded_audio = 0;
    uint64_t decoded_audio_samples = 0;

    TrackStat video;
    TrackStat audio;

    uint64_t snap_video_objs = 0, snap_audio_objs = 0;
    uint64_t snap_dec_video  = 0, snap_dec_audio  = 0;
    int      elapsed_sec = 0;
};

/* ------------------------- 落盘 ------------------------- */

void DumpYuv(App* a, AVFrame* f) {
    if (!a->dump_yuv || !f) return;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) return;

    const int w = f->width;
    const int h = f->height;
    for (int plane = 0; plane < 3; ++plane) {
        const int pw = (plane == 0) ? w : (w + 1) / 2;
        const int ph = (plane == 0) ? h : (h + 1) / 2;
        for (int y = 0; y < ph; ++y) {
            fwrite(f->data[plane] + static_cast<ptrdiff_t>(y) * f->linesize[plane],
                   1, static_cast<size_t>(pw), a->dump_yuv);
        }
    }
    a->dump_yuv_frames++;
}

void DumpPcm(App* a, AVFrame* f) {
    if (!a->dump_pcm || !f) return;
    if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(f->format))) return;
    const int bps = av_get_bytes_per_sample(static_cast<AVSampleFormat>(f->format));
    if (bps <= 0) return;
    const size_t n = static_cast<size_t>(f->nb_samples) *
                     static_cast<size_t>(f->ch_layout.nb_channels) *
                     static_cast<size_t>(bps);
    fwrite(f->data[0], 1, n, a->dump_pcm);
    a->dump_pcm_frames++;
}

/* ------------------------- 解码帧回调（后台线程） ------------------------- */

void OnVideoFrame(App* a, AVFrame* f) {
    a->decoded_video++;

    const int64_t idx = static_cast<int64_t>(a->decoded_video);
    if (a->log_frames == 0 || idx <= a->log_frames) {
        const char* pix = av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format));
        LOG_INFO("dec  video #%-5" PRId64 " %dx%d %s", idx, f->width, f->height,
                 pix ? pix : "?");
    }

    DumpYuv(a, f);

    if (a->player) {
        int64_t pts = f->pts;
        if (pts == AV_NOPTS_VALUE) pts = 0;
        a->player->PushVideo(f, pts);
    }
}

void OnAudioFrame(App* a, AVFrame* f) {
    a->decoded_audio++;
    a->decoded_audio_samples += static_cast<uint64_t>(f->nb_samples);

    const int64_t idx = static_cast<int64_t>(a->decoded_audio);
    if (a->log_frames == 0 || idx <= a->log_frames) {
        const char* fmt = av_get_sample_fmt_name(static_cast<AVSampleFormat>(f->format));
        char layout[64] = {0};
        av_channel_layout_describe(&f->ch_layout, layout, sizeof(layout));
        LOG_INFO("dec  audio #%-5" PRId64 " %d samples rate=%d %s %s",
                 idx, f->nb_samples, f->sample_rate, fmt ? fmt : "?", layout);
    }

    DumpPcm(a, f);

    if (!a->player) return;

    /* avfilter 重采样后帧的 pts 语义变了，这里按样本数自己推进，
     * 保证音频队列的 pts 是一条连续、单调的毫秒时间线 */
    if (!a->audio_pts_seeded) {
        a->audio_pts_seeded = true;
        a->audio_pts_next = a->audio_first_pts;
    }
    const int64_t pts = a->audio_pts_next;
    const int rate = a->out_rate > 0 ? a->out_rate : 48000;
    a->audio_pts_next += static_cast<int64_t>(f->nb_samples) * 1000 / rate;

    a->player->PushAudio(f, pts);
}

/* ------------------------- 报文日志 ------------------------- */

void LogObject(App* a, moqt_stream_t* s, const uint8_t* data, size_t len,
               int64_t dts_ms, int64_t pts_ms, int keyframe) {
    const moqt_codec_t codec = moqt_stream_codec(s);
    TrackStat& st = (codec == MOQT_CODEC_H264) ? a->video : a->audio;

    st.objects++;
    st.bytes += len;
    if (keyframe) st.keyframes++;
    if (st.first_dts < 0) st.first_dts = dts_ms;
    st.prev_dts = st.last_dts;
    st.last_dts = dts_ms;

    const bool verbose = (a->log_packets == 0) ||
                         (static_cast<int64_t>(st.objects) <= a->log_packets);
    if (!verbose) return;

    /* log_packets 默认 0 会打印全部，这里改语义：<=0 且未显式开启就不打 */
    if (a->log_packets <= 0) return;

    char line[512];
    int n = snprintf(line, sizeof(line),
                     "recv %-4s #%-5" PRIu64 " %6zu B  dts=%6" PRId64
                     "ms  pts=%6" PRId64 "ms  %s",
                     CodecName(codec), st.objects, len, dts_ms, pts_ms,
                     keyframe ? "KEY" : "   ");
    if (n < 0) return;

    if (codec == MOQT_CODEC_H264 && n < (int)sizeof(line)) {
        const std::string nal = DescribeFirstNal(data, len);
        if (!nal.empty()) {
            snprintf(line + n, sizeof(line) - n, "  [%s]", nal.c_str());
        }
    }
    LOG_INFO("%s", line);
}

/* ------------------------- SDK 回调（后台线程） ------------------------- */

void OnConnected(moqt_client_t*, moqt_result_t status,
                 const char* reason, void* user) {
    App* a = static_cast<App*>(user);
    if (status != MOQT_OK) {
        LOG_ERROR("连接失败: %s (%s)",
                  reason && reason[0] ? reason : "unknown", ResultName(status));
        a->failed = true;
        uv_stop(&a->loop);
        return;
    }
    LOG_INFO("已连接 %s:%d%s  命名空间=%s/%s",
             a->host.c_str(), a->port, a->path.c_str(),
             a->appname.c_str(), a->stream.c_str());
    LOG_INFO("已发送 SUBSCRIBE，等待轨道就绪...");
}

void OnStreamReady(moqt_client_t*, moqt_stream_t* s,
                   const moqt_stream_info_t* info, void* user) {
    App* a = static_cast<App*>(user);
    TrackStat& st = (info->codec == MOQT_CODEC_H264) ? a->video : a->audio;

    LOG_INFO("轨道就绪: %-4s track=%s config=%zu 字节",
             CodecName(info->codec),
             info->track_name ? info->track_name : "?",
             info->config_len);

    if (info->config && info->config_len > 0 && !st.config_logged) {
        const std::string desc = (info->codec == MOQT_CODEC_H264)
                                     ? DescribeAvcC(info->config, info->config_len)
                                     : DescribeAsc(info->config, info->config_len);
        LOG_INFO("%s", desc.c_str());
        st.config_logged = true;
    }

    if (!info->config || info->config_len == 0) return;

    std::string err;
    if (info->codec == MOQT_CODEC_H264) {
        if (!a->vdec) {
            a->vdec.reset(new VideoDecoder());
            a->vdec->SetFrameCallback([a](AVFrame* f) { OnVideoFrame(a, f); });
            if (!a->vdec->Init(info->config, info->config_len, &err)) {
                LOG_ERROR("视频解码器初始化失败: %s", err.c_str());
                a->vdec.reset();
            } else {
                LOG_INFO("视频解码器已就绪（H.264 / avcC）");
            }
        }
    } else {
        if (!a->adec) {
            a->adec.reset(new AudioDecoder());
            a->adec->SetFrameCallback([a](AVFrame* f) { OnAudioFrame(a, f); });
            if (!a->adec->Init(info->config, info->config_len,
                               a->out_rate, a->out_channels, &err)) {
                LOG_ERROR("音频解码器初始化失败: %s", err.c_str());
                a->adec.reset();
            } else {
                LOG_INFO("音频解码器已就绪（AAC %dHz %dch → %dHz %dch F32）",
                         a->adec->in_sample_rate(), a->adec->in_channels(),
                         a->out_rate, a->out_channels);
            }
        }
    }
    (void)s;
}

void OnData(moqt_client_t*, moqt_stream_t* s,
            const uint8_t* data, size_t len,
            int64_t dts_ms, int64_t pts_ms, int keyframe, void* user) {
    App* a = static_cast<App*>(user);
    if (!data || len == 0) return;

    size_t cfg_len = 0;
    const uint8_t* cfg = moqt_stream_config(s, &cfg_len);
    if (cfg && cfg_len == len && memcmp(cfg, data, len) == 0) {
        /* 纯 config 对象，不是媒体数据 */
        const moqt_codec_t codec = moqt_stream_codec(s);
        TrackStat& st = (codec == MOQT_CODEC_H264) ? a->video : a->audio;
        if (!st.config_logged) {
            const std::string desc = (codec == MOQT_CODEC_H264)
                                         ? DescribeAvcC(cfg, cfg_len)
                                         : DescribeAsc(cfg, cfg_len);
            LOG_INFO("%s", desc.c_str());
            st.config_logged = true;
        }
        return;
    }

    LogObject(a, s, data, len, dts_ms, pts_ms, keyframe);
    a->got_media = true;

    const moqt_codec_t codec = moqt_stream_codec(s);

    /* 到达侧跳变检测：直播流里 pts 突然前跳或倒退，是卡顿、音画错位的先兆。
     * 视频帧间隔 ~40ms、音频 ~21ms，超过 200ms 的空档或任何倒退都记一笔。 */
    {
        int64_t* last = (codec == MOQT_CODEC_H264) ? &a->in_last_v_pts
                                                   : &a->in_last_a_pts;
        const char* nm = (codec == MOQT_CODEC_H264) ? "video" : "audio";
        if (*last >= 0) {
            const int64_t d = pts_ms - *last;
            if (d < 0) {
                if (codec == MOQT_CODEC_H264) a->in_v_backwards++;
                LOG_DEBUG("[evt] %s pts 倒退: %" PRId64 " -> %" PRId64
                          " (Δ=%+" PRId64 "ms)", nm, *last, pts_ms, d);
            } else if (d > 200) {
                if (codec == MOQT_CODEC_H264) a->in_v_jumps++; else a->in_a_jumps++;
                LOG_INFO("[evt] %s pts 前跳: %" PRId64 " -> %" PRId64
                         " (Δ=%+" PRId64 "ms)", nm, *last, pts_ms, d);
            }
        }
        *last = pts_ms;
    }

    if (codec == MOQT_CODEC_H264) {
        if (a->vdec) a->vdec->Decode(data, len, dts_ms, pts_ms, keyframe != 0);
    } else {
        if (!a->audio_pts_seeded) a->audio_first_pts = pts_ms;
        if (a->adec) a->adec->Decode(data, len, dts_ms, pts_ms);
    }
}

void OnClosed(moqt_client_t*, moqt_result_t status,
              const char* reason, void* user) {
    App* a = static_cast<App*>(user);
    LOG_INFO("会话关闭: %s (%s)",
             reason && reason[0] ? reason : "-", ResultName(status));
    if (!a->done) {
        a->done = true;
        uv_stop(&a->loop);
    }
}

/* ------------------------- 定时器 / 信号（后台线程） ------------------------- */

void OnStats(uv_timer_t* t) {
    App* a = static_cast<App*>(t->data);
    a->elapsed_sec++;

    const uint64_t dvo = a->video.objects - a->snap_video_objs;
    const uint64_t dao = a->audio.objects - a->snap_audio_objs;
    const uint64_t dcv = a->decoded_video - a->snap_dec_video;
    const uint64_t dca = a->decoded_audio - a->snap_dec_audio;

    a->snap_video_objs = a->video.objects;
    a->snap_audio_objs = a->audio.objects;
    a->snap_dec_video  = a->decoded_video;
    a->snap_dec_audio  = a->decoded_audio;

    if (dvo == 0 && dao == 0) {
        LOG_INFO("stat %3ds 空闲", a->elapsed_sec);
        return;
    }
    LOG_INFO("stat %3ds 收 v=%" PRIu64 "/s a=%" PRIu64 "/s  解 v=%" PRIu64
             "/s a=%" PRIu64 "/s",
             a->elapsed_sec, dvo, dao, dcv, dca);
}

void OnFinish(uv_timer_t* t) {
    uv_stop(static_cast<uv_loop_t*>(t->data));
}

void StopLoop(App* a) {
    if (a->done.exchange(true)) return;
    uv_timer_stop(&a->duration_timer);
    if (a->client) moqt_client_close(a->client);
    a->finish_timer.data = &a->loop;
    uv_timer_start(&a->finish_timer, OnFinish, 300, 0);
}

/* 主线程请求停止：往 loop 线程发个 async，避免跨线程直接动 uv handle */
void OnStopAsync(uv_async_t* h) {
    App* a = static_cast<App*>(h->data);
    StopLoop(a);
}

void OnDuration(uv_timer_t* t) {
    App* a = static_cast<App*>(t->data);
    LOG_INFO("到达设定时长 %.0f 秒，准备退出", a->duration_sec);
    StopLoop(a);
}

void OnSigint(uv_signal_t* h, int) {
    App* a = static_cast<App*>(h->data);
    LOG_INFO("收到 Ctrl-C，准备退出");
    StopLoop(a);
}

void OnResolved(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
    App* a = static_cast<App*>(req->data);
    if (a->done) {
        if (res) uv_freeaddrinfo(res);
        return;
    }

    char ip[64] = {0};
    if (status == 0 && res) {
        for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET && ai->ai_addr) {
                uv_ip4_name(reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr),
                            ip, sizeof(ip));
                break;
            }
        }
    }
    if (res) uv_freeaddrinfo(res);

    if (status != 0 || ip[0] == 0) {
        LOG_ERROR("DNS 解析 %s 失败: %s", a->host.c_str(),
                  status != 0 ? uv_strerror(status) : "没有 IPv4 A 记录");
        a->failed = true;
        uv_stop(&a->loop);
        return;
    }

    a->connect_ip = ip;
    LOG_INFO("解析 %s -> %s", a->host.c_str(), a->connect_ip.c_str());

    const moqt_result_t r = moqt_client_connect_ex(
        a->client, a->host.c_str(), a->connect_ip.c_str(), a->port,
        a->path.c_str(), a->appname.c_str(), a->stream.c_str(),
        MOQT_DIRECTION_SUBSCRIBE, 5000);

    if (r != MOQT_OK) {
        LOG_ERROR("connect_ex 失败: %s", ResultName(r));
        a->failed = true;
        uv_stop(&a->loop);
    }
}

/* ------------------------- 后台线程主函数 ------------------------- */

/* ------------------------- 后台线程主函数 ------------------------- */

void WorkerMain(App* a) {
    /* 打印本线程实际的栈范围，便于事后判断是否触发栈溢出 */
    {
        void*  base = pthread_get_stackaddr_np(pthread_self());
        size_t size = pthread_get_stacksize_np(pthread_self());
        LOG_INFO("后台线程栈: 大小=%.1f MB  顶=%p  底≈%p",
                 size / 1048576.0, base,
                 static_cast<void*>(static_cast<char*>(base) - size));
    }

    uv_run(&a->loop, UV_RUN_DEFAULT);

    /* 冲刷解码器与音频滤镜里残留的帧，让尾帧也能进队列 */
    if (a->vdec) a->vdec->Flush();
    if (a->adec) a->adec->Flush();

    /* 这里故意不调用 moqt_client_free()：
     * 上游实现只 free() 结构体，没有 uv_close() 它内部的 uv handle
     * （connect_timer 等），释放后 loop 里会留下悬空 handle。
     * 详见 main 里释放 client 处的注释。 */

    if (a->player) a->player->SetInputDone();
    LOG_INFO("后台线程结束（拉流+解码已停止）");
}

void* WorkerThreadEntry(void* arg) {
    WorkerMain(static_cast<App*>(arg));
    return nullptr;
}

void Usage(const char* argv0) {
    LOG_ERROR("用法: %s <host> <port> <app> <stream> [选项]", argv0);
    LOG_ERROR("  host 可为 IPv4 地址或域名（QUIC 栈仅支持 IPv4）");
    LOG_ERROR("选项:");
    LOG_ERROR("  --av-sync <ms>      0=直通（默认，最低延迟），>0=预缓冲后精确同步（如 2000）");
    LOG_ERROR("  --path <p>          WebTransport 路径，默认 /moq");
    LOG_ERROR("  --duration <sec>    运行 N 秒后退出（默认跑到关窗/ESC/Ctrl-C）");
    LOG_ERROR("  --out-rate <hz>     avfilter 音频输出采样率，默认 48000");
    LOG_ERROR("  --out-channels <n>  avfilter 音频输出声道数，默认 2");
    LOG_ERROR("  --log-packets <n>   每条轨前 n 个报文逐条打印，0 = 不打，默认 0");
    LOG_ERROR("  --log-frames <n>    前 n 个解码帧逐帧打印，0 = 全部，默认 5");
    LOG_ERROR("  --dump-yuv <file>   解码后 YUV420P 落盘");
    LOG_ERROR("  --dump-pcm <file>   avfilter 转换后 PCM 落盘（F32）");
    LOG_ERROR("  --snapshot <file>   渲染若干帧后抓一张 BMP（验证渲染用）");
    LOG_ERROR("  --log-level <l>     error|warn|info|debug，默认 info");
    LOG_ERROR("  --log-file <f>      额外把日志追加写入文件");
    LOG_ERROR("示例:");
    LOG_ERROR("  %s 127.0.0.1 4433 live demo", argv0);
    LOG_ERROR("  %s 127.0.0.1 4433 live demo --av-sync 2000", argv0);
}

bool ParseLogLevel(const char* s, LogLevel* out) {
    if (strcasecmp(s, "error") == 0) { *out = ERROR; return true; }
    if (strcasecmp(s, "warn")  == 0) { *out = WARN;  return true; }
    if (strcasecmp(s, "info")  == 0) { *out = INFO;  return true; }
    if (strcasecmp(s, "debug") == 0) { *out = DEBUG; return true; }
    return false;
}

} /* namespace */

int main(int argc, char** argv) {
    /* logger 先起来，后面所有输出都走它。异步 logger 退出前必须 shutdown */
    log_set_console(1);
    log_set_level(INFO);

    App a;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            LogLevel lv;
            if (ParseLogLevel(argv[i + 1], &lv)) log_set_level(lv);
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            log_set_file(argv[i + 1]);
        }
    }

    if (argc < 5) {
        Usage(argv[0]);
        log_shutdown();
        return 1;
    }

    a.host    = argv[1];
    a.port    = atoi(argv[2]);
    a.appname = argv[3];
    a.stream  = argv[4];

    for (int i = 5; i < argc; ++i) {
        if (strcmp(argv[i], "--av-sync") == 0 && i + 1 < argc) {
            a.av_sync_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            a.path = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            a.duration_sec = atof(argv[++i]);
        } else if (strcmp(argv[i], "--out-rate") == 0 && i + 1 < argc) {
            a.out_rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out-channels") == 0 && i + 1 < argc) {
            a.out_channels = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-packets") == 0 && i + 1 < argc) {
            a.log_packets = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-frames") == 0 && i + 1 < argc) {
            a.log_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dump-yuv") == 0 && i + 1 < argc) {
            const char* p = argv[++i];
            a.dump_yuv = fopen(p, "wb");
            if (!a.dump_yuv) {
                LOG_ERROR("无法打开 --dump-yuv 文件: %s", p);
                log_shutdown();
                return 1;
            }
        } else if (strcmp(argv[i], "--dump-pcm") == 0 && i + 1 < argc) {
            const char* p = argv[++i];
            a.dump_pcm = fopen(p, "wb");
            if (!a.dump_pcm) {
                LOG_ERROR("无法打开 --dump-pcm 文件: %s", p);
                log_shutdown();
                return 1;
            }
        } else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            a.snapshot_path = argv[++i];
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            ++i;
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            ++i;
        } else {
            LOG_ERROR("未知选项: %s", argv[i]);
            Usage(argv[0]);
            log_shutdown();
            return 1;
        }
    }

    if (a.port <= 0 || a.port > 65535) {
        LOG_ERROR("端口非法: %d", a.port);
        log_shutdown();
        return 1;
    }
    if (a.appname.empty() || a.stream.empty()) {
        LOG_ERROR("app 与 stream 不能为空");
        log_shutdown();
        return 1;
    }
    if (a.out_rate <= 0 || a.out_channels <= 0) {
        LOG_ERROR("--out-rate / --out-channels 非法: %d / %d",
                  a.out_rate, a.out_channels);
        log_shutdown();
        return 1;
    }

    LOG_INFO("========================================");
    LOG_INFO(" moq_player — 拉流 + 解码 + SDL3 播放");
    LOG_INFO("========================================");
    LOG_INFO(" 目标    : %s:%d%s", a.host.c_str(), a.port, a.path.c_str());
    LOG_INFO(" 命名空间: %s/%s", a.appname.c_str(), a.stream.c_str());
    LOG_INFO(" 音频输出: %dHz %dch F32（avfilter 转换）", a.out_rate, a.out_channels);
    LOG_INFO(" 同步    : %s", a.av_sync_ms > 0
                 ? "开启（预缓冲后按 pts 对齐）" : "关闭（直通，最低延迟）");
    LOG_INFO(" av_sync : %d ms（队列窗口 %d ms，重缓冲目标 %d ms）",
             a.av_sync_ms,
             a.av_sync_ms > 0 ? a.av_sync_ms * 2 : 500,
             a.av_sync_ms > 0 ? a.av_sync_ms : 0);
    LOG_INFO(" 构建    : %s %s", __DATE__, __TIME__);
    LOG_INFO("----------------------------------------");

    /* ---- 播放器必须在主线程初始化（SDL3 窗口/渲染/音频） ---- */
    Player player;
    {
        std::string err;
        if (!player.Init(a.av_sync_ms, a.out_rate, a.out_channels, &err)) {
            LOG_ERROR("播放器初始化失败: %s", err.c_str());
            log_shutdown();
            return 1;
        }
    }
    a.player = &player;
    if (!a.snapshot_path.empty()) {
        player.SetSnapshot(a.snapshot_path, 40);
    }

    /* ---- 后台线程：uv loop + 拉流 + 解码 ---- */
    uv_loop_init(&a.loop);

    moqt_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_connected    = OnConnected;
    cbs.on_stream_ready = OnStreamReady;
    cbs.on_data         = OnData;
    cbs.on_closed       = OnClosed;

    a.client = moqt_client_new(&a.loop, &cbs, &a);
    if (!a.client) {
        LOG_ERROR("moqt_client_new 失败");
        uv_loop_close(&a.loop);
        log_shutdown();
        return 1;
    }

    uv_async_init(&a.loop, &a.stop_async, OnStopAsync);
    uv_timer_init(&a.loop, &a.duration_timer);
    uv_timer_init(&a.loop, &a.finish_timer);
    uv_timer_init(&a.loop, &a.stats_timer);
    uv_signal_init(&a.loop, &a.sigint);

    a.stop_async.data     = &a;
    a.duration_timer.data = &a;
    a.stats_timer.data    = &a;
    a.sigint.data         = &a;

    uv_signal_start(&a.sigint, OnSigint, SIGINT);
    uv_timer_start(&a.stats_timer, OnStats, 1000, 1000);
    if (a.duration_sec > 0) {
        uv_timer_start(&a.duration_timer, OnDuration,
                       static_cast<uint64_t>(a.duration_sec * 1000), 0);
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", a.port);

    a.resolve.data = &a;
    const int rc = uv_getaddrinfo(&a.loop, &a.resolve, OnResolved,
                                  a.host.c_str(), portstr, &hints);
    if (rc != 0) {
        LOG_ERROR("uv_getaddrinfo(%s) 失败: %s", a.host.c_str(), uv_strerror(rc));
        /* 同 main 末尾的约定：free 之后不再碰 loop（见下方说明） */
        moqt_client_free(a.client);
        a.client = nullptr;
        log_shutdown();
        return 1;
    }

    /* 后台线程使用**平台默认栈**，不放大。
     *
     * 之前这里把栈设成 8MB，是为了绕开一个上游 bug：quic_recovery.c 的
     * compact_sent_packets() 里
     *     QuicRecoverySentPacket tmp[QUIC_RECOVERY_MAX_SENT_PACKETS];   // 4096
     * 是 4096 × 64B = 262,144 B（256KB）的**栈**数组。该 static 函数被内联进
     * 两个函数，而两者恰好在同一条 PTO 重传链上：
     *     on_pto_timer(262,288B) → send_quic_packet(2,272B)
     *                            → quic_recovery_on_packet_sent(262,304B)
     * 合计 526,864 B > macOS 非主线程默认 512KB，必崩（___chkstk_darwin
     * 踩 guard page，实测地址 = 栈底 - 8）。
     *
     * 调大栈只是掩盖「不该在栈上放 256KB」这个错误，所以真正修法是在
     * 上游把该临时数组改为按实际条目数堆分配（见 upstream/quic/quic_recovery.c
     * 的 compact_sent_packets）。栈保持默认，谁超了谁改。
     *
     * MOQ_WORKER_STACK_KB 仅作为调试开关保留（不设则用平台默认）。 */
    pthread_t worker;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (const char* env = getenv("MOQ_WORKER_STACK_KB")) {
        const long v = strtol(env, nullptr, 10);
        if (v >= 128) pthread_attr_setstacksize(&attr, static_cast<size_t>(v) * 1024);
    }
    const int prc = pthread_create(&worker, &attr, WorkerThreadEntry, &a);
    pthread_attr_destroy(&attr);
    if (prc != 0) {
        LOG_ERROR("创建后台线程失败: %s", strerror(prc));
        log_shutdown();
        return 1;
    }

    /* ---- 主线程：SDL 事件循环 + 渲染播放 ---- */
    uint64_t last_stat_tick = SDL_GetTicks();
    while (true) {
        if (!player.Iterate()) break;      /* 用户关窗 / ESC */
        if (player.Finished()) break;      /* 输入结束且队列已播完 */

        const uint64_t now = SDL_GetTicks();
        if (now - last_stat_tick >= 1000) {
            last_stat_tick = now;
            LOG_INFO("play 已渲染=%" PRIu64 " 已喂音频=%" PRIu64
                     " 丢弃=%" PRIu64 "  队列 v=%zu(%" PRId64 "ms) a=%zu(%" PRId64 "ms)"
                     "  | 到达跳变 v=%" PRIu64 " a=%" PRIu64 " 倒退 v=%" PRIu64,
                     player.rendered_frames(), player.fed_audio_frames(),
                     player.dropped_frames(),
                     player.video_queue_size(), player.video_buffered_ms(),
                     player.audio_queue_size(), player.audio_buffered_ms(),
                     a.in_v_jumps, a.in_a_jumps, a.in_v_backwards);
        }
    }

    /* ---- 收尾：先叫停后台线程，再等它退出 ---- */
    if (!a.done) uv_async_send(&a.stop_async);
    pthread_join(worker, nullptr);

    /* SDL 资源按正确顺序释放（先拆纹理/窗口/音频设备，最后 SDL_Quit） */
    player.Shutdown();

    a.vdec.reset();
    a.adec.reset();

    if (a.dump_yuv) { fflush(a.dump_yuv); fclose(a.dump_yuv); a.dump_yuv = nullptr; }
    if (a.dump_pcm) { fflush(a.dump_pcm); fclose(a.dump_pcm); a.dump_pcm = nullptr; }

    /* ============================================================
     * 释放 SDK client —— 必须是最后一次触碰 uv 的操作。
     *
     * ⚠️ 上游 moqt_client_free() 的实现是：
     *      uv_timer_stop(&c->connect_timer);
     *      ... free_stream(s) ...
     *      free(c);                       <-- 没有 uv_close()
     *   也就是说它的 uv handle 从未被 uv_close()，free 之后 loop 的
     *   handle 队列/定时器堆里仍留着指向已释放内存的悬空指针。
     *
     *   因此这里之后**绝对不能**再调用 uv_run() 或 uv_loop_close()，
     *   否则 libuv 会去遍历那个悬空 handle，在 uv_timer_stop 里踩空崩溃
     *   （这正是之前 Ctrl+C / --duration 退出时 SIGSEGV 的根因）。
     *
     *   进程随后立即退出，残留的 loop 句柄由内核回收，不影响正确性。
     * ============================================================ */
    if (a.client) {
        moqt_client_free(a.client);
        a.client = nullptr;
    }

    /* ------------------------- 汇总 ------------------------- */
    LOG_INFO("----------------------------------------");
    LOG_INFO(" 汇总");
    LOG_INFO("----------------------------------------");

    auto log_track = [](const char* name, const TrackStat& s) {
        if (s.first_dts >= 0) {
            LOG_INFO(" %-6s 报文=%" PRIu64 " 字节=%" PRIu64 " 关键帧=%" PRIu64
                     " dts=%" PRId64 "..%" PRId64 "ms",
                     name, s.objects, s.bytes, s.keyframes, s.first_dts, s.last_dts);
        } else {
            LOG_INFO(" %-6s 报文=%" PRIu64 " 字节=%" PRIu64, name, s.objects, s.bytes);
        }
    };
    log_track("video", a.video);
    log_track("audio", a.audio);

    LOG_INFO(" 解码 video: %" PRIu64 " 帧", a.decoded_video);
    LOG_INFO(" 解码 audio: %" PRIu64 " 帧  %" PRIu64 " 样本",
             a.decoded_audio, a.decoded_audio_samples);
    LOG_INFO(" 播放 video: %" PRIu64 " 帧  丢弃=%" PRIu64,
             player.rendered_frames(), player.dropped_frames());
    LOG_INFO(" 播放 audio: %" PRIu64 " 块", player.fed_audio_frames());
    LOG_INFO(" 诊断 到达跳变 v=%" PRIu64 " a=%" PRIu64 " 倒退 v=%" PRIu64
             " | 播放器 丢晚=%" PRIu64 " 丢满=%" PRIu64 " 跳转=%" PRIu64
             " 重缓冲=%" PRIu64,
             a.in_v_jumps, a.in_a_jumps, a.in_v_backwards,
             player.dropped_late(), player.dropped_push(),
             player.resync_count(), player.rebuffer_count());

    int ret = 0;
    if (!a.got_media) {
        LOG_ERROR("结果：未收到任何媒体报文（%s）",
                  a.failed ? "连接或协商失败" : "服务器无数据");
        ret = 1;
    } else if (player.rendered_frames() == 0 && player.fed_audio_frames() == 0) {
        LOG_ERROR("结果：收到报文但未能播放任何帧");
        ret = 1;
    } else {
        LOG_INFO("结果：播放成功");
    }

    log_shutdown();
    return ret;
}
