# moq_player

**English** | [简体中文](README_cn.md)

An audio/video player built on **Media over QUIC (MoQ)**. It **pulls** a stream from a MoQ server (SUBSCRIBE / media pull), decodes **H.264 + AAC** with FFmpeg, and hands the result to **SDL3** for on-screen rendering and audio playback.

The protocol layer is the upstream C SDK [`runner365/moqlivecast`](https://github.com/runner365/moqlivecast) — specifically `moq_transport_client` (moq-transport draft-22 over WebTransport), vendored as a git submodule in `upstream/`.

```
┌──────────────── background thread ────────────────┐   ┌───────────── main thread ──────────────┐
│                                                   │   │                                        │
│  moq SDK ─► H.264 decode ─► video queue (by pts)  ├──►│  SDL3 render (YUV direct, no swscale)  │
│  (AVCC)    AAC decode ─► avfilter ─► audio queue  │   │  CoreAudio continuous feed             │
│                 (PCM 48k/2ch F32)                 │   │                                        │
└───────────────────────────────────────────────────┘   └────────────────────────────────────────┘
```

## Features

- **Pull**: SUBSCRIBE to `app/stream`; parses `avcC` / AudioSpecificConfig and logs the codec parameters
- **Decode**: FFmpeg 8.0.1 — **H.264 / H.265 (HEVC)** video and AAC audio, producing YUV420P and PCM; no Annex-B or ADTS conversion anywhere
- **Codec selection**: taken straight from the SDK's `moqt_stream_t::codec` (`MOQT_CODEC_H264` / `MOQT_CODEC_H265`); the config record is only used for log formatting and a consistency warning — no command-line switch needed
- **Audio resampling**: `avfilter` (`aresample` + `aformat`) converts 44.1kHz FLTP into device-ready 48kHz stereo F32
- **Rendering**: SDL3 `SDL_UpdateYUVTexture` consumes YUV420P directly — no swscale RGB conversion
- **A/V sync**: optional. Default is passthrough (lowest latency); with `--av-sync N` it pre-buffers N ms and aligns video to the master clock by pts
- **Observability**: all logging goes through moqlivecast's async logger, with per-second rate / queue depth / drop statistics; YUV/PCM dumping and render snapshots are available for regression checks

## Dependencies

| Dependency | Requirement | Source |
|---|---|---|
| CMake | ≥ 3.16 | System install |
| C++ compiler | C++14 support | System toolchain (Clang / GCC) |
| OpenSSL | **3.5+** (needs the QUIC TLS API such as `SSL_set_quic_tls_cbs`) | System install, e.g. `brew install openssl@3` |
| FFmpeg | 8.0.1 (H.264/H.265/AAC decoders, avfilter, swresample, swscale) | `3rdparty/ffmpeg.tar.gz`, built automatically by ExternalProject |
| SDL3 | 3.4.20 | `3rdparty/SDL3-3.4.20.tar.gz`, compiled statically via `add_subdirectory` |
| libuv | Bundled with upstream | `upstream/3rdparty/libuv` |
| moqlivecast SDK | Commit pinned by the submodule | `upstream/` (git submodule) |

FFmpeg is built with a minimal configuration (H.264/AAC decoders + avfilter + swresample only), which takes about 2 minutes; artifacts are installed to `build/ffmpeg/install`.

## Build

```bash
# 1. Fetch the upstream SDK (required after the first clone)
git submodule update --init --recursive

# 2. Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

Artifacts:

```
build/bin/moq_player              # the player
build/ffmpeg/install/             # FFmpeg static libs + headers
```

You can point to a different upstream copy with `-DMOQ_UPSTREAM_DIR=/path/to/moqlivecast` (defaults to `./upstream`).

## Run

```bash
./build/bin/moq_player <host> <port> <app> <stream> [options]
```

Examples:

```bash
# Default passthrough mode (lowest latency)
./build/bin/moq_player 127.0.0.1 4433 live demo

# Precise sync with a 500ms pre-buffer
./build/bin/moq_player 127.0.0.1 4433 live demo --av-sync 500

# 2 second pre-buffer
./build/bin/moq_player 127.0.0.1 4433 live demo --av-sync 2000
```

Press **ESC** in the window or close it to exit; **Ctrl-C** in the terminal also works (every exit path releases resources cleanly and prints a summary).

### Command-line options

| Option | Description | Default |
|---|---|---|
| `--av-sync <ms>` | `0` = passthrough (no pre-buffer, lowest latency); `>0` = pre-buffer that many milliseconds, then align by pts | `0` |
| `--path <p>` | WebTransport path | `/moq` |
| `--duration <sec>` | Exit after N seconds | Runs until window close / ESC / Ctrl-C |
| `--out-rate <hz>` | avfilter audio output sample rate | `48000` |
| `--out-channels <n>` | avfilter audio output channel count | `2` |
| `--log-packets <n>` | Print the first n packets of each track, `0` = off | `0` |
| `--log-frames <n>` | Print the first n decoded frames, `0` = all | `5` |
| `--dump-yuv <file>` | Dump decoded YUV420P (for regression checks) | — |
| `--dump-pcm <file>` | Dump post-avfilter PCM (raw F32) | — |
| `--snapshot <file>` | Save one BMP after a number of rendered frames (verifies the render path) | — |
| `--log-level <l>` | `error` / `warn` / `info` / `debug` | `info` |
| `--log-file <f>` | Additionally append logs to a file | — |

The `MOQ_WORKER_STACK_KB` environment variable overrides the worker thread stack size and is **debug-only** (by default the platform default is used; you normally don't need it).

## Diagnostic logs (troubleshooting stalls / A/V drift)

`--log-level debug` adds two kinds of lines aimed at "picture froze" and "audio and video out of sync" problems:

> Note: the log text itself is in Chinese. The tables below translate every field.

```bash
./build/bin/moq_player <host> <port> <app> <stream> --av-sync 2000 \
    --log-level debug --log-file diag.log
```

**① One `[diag]` line per second** — it doubles as a heartbeat: if these lines stop, the main thread is blocked.

```
[diag] t=3s 时钟=1488(audio 偏差+8) 墙钟=1480 | V 首=1500 尾=3467 缓冲=1967ms n=60
       | A 位置=1488 缓冲=0ms n=0 设备积压=1987ms | 音画差=-21ms
       | 渲染=45 喂音=149 丢晚=0 丢满=0 跳转=0 重缓冲=0
```

| Field | Meaning |
|---|---|
| `时钟` / `(来源 偏差)` | Current master clock value; source (`audio` = audio device, `wall` = wall clock); audio clock minus wall clock |
| `墙钟` | Playback anchor + elapsed wall time |
| `V 首/尾/缓冲/n` | Video queue head pts, tail pts, buffered span (ms), frame count |
| `A 位置/缓冲/n` | Audio media position *currently being played*, queue buffer, queue frame count |
| `设备积压` | Bytes still queued inside the SDL audio device, in ms |
| `音画差` | Last rendered video pts − audio playback position (positive = video ahead) |
| `丢晚` / `丢满` | Frames dropped for being late / dropped at the queue limit |
| `跳转` / `重缓冲` | Number of `ResyncTo` jumps / re-buffers |
| Trailing state | empty = playing, `[未起播]` = waiting for pre-buffer, `[缓冲中]` = re-buffering |

**② Event lines `[evt] …`** (printed only on state changes)

```
[evt] 主时钟切换 wall -> audio（音频-墙钟偏差 +16ms，阈值 ±250）
[evt] 等待队首到点: 领先主时钟 2680ms，已持续 1024ms（队首=9400 时钟=6720 时钟源=audio）
[evt] video pts 前跳: 4000 -> 9400 (Δ=+5400ms)
[evt] audio pts 倒退: 12130 -> 11980 (Δ=-150ms)
缓冲中……（目标 2000ms，当前 0ms）
缓冲完成，恢复播放（可用 2000ms）
```

What to look at first:

- `等待队首到点` repeating → the video queue head never catches up with the clock (the picture is frozen)
- `pts 前跳` / `pts 倒退` → the server's timestamps are discontinuous
- `主时钟切换` flip-flopping → the audio device's playback position reading is unstable
- `时钟` drifting away from `墙钟` → the clock is running away

## Data format conventions

These were confirmed by measurement, not assumed — you need them if you touch the decode layer.

| Item | What the SDK delivers | How we use it |
|---|---|---|
| Video codec | H.264 or **H.265 (HEVC)**, from `moqt_stream_t::codec` | Used directly to pick the decoder; a mismatch with the config record (`avcC`/`hvcC`) raises a WARN |
| Video bitstream | **Length-prefixed NALs** (4-byte length + NAL, not Annex-B), identical for both codecs | Passed straight to `avcodec_send_packet`, no conversion |
| Video config | `avcC` (H.264, SPS/PPS) or `hvcC` (H.265, VPS/SPS/PPS) | Used as `extradata`; FFmpeg detects length-prefix mode and splits NALs internally |
| Audio bitstream | **Raw AAC frames**, no ADTS header | Passed straight to the decoder |
| Audio config | AudioSpecificConfig (AAC-LC / sample rate / channels) | Used as `extradata`, and parsed locally for logging |
| Timestamps | `dts_ms` / `pts_ms`, milliseconds | Queue ordering and sync baseline, see below |

> **Note**: when the server replays a GOP it may rebase timestamps (possibly restarting from 0), so **never treat dts as absolute time**. The playback layer rebuilds the timeline relative to the queue head frame.

## Queues and A/V sync

Two independent queues (video / audio) hold decoded `AVFrame`s ordered by `pts_ms`:

- **Fast path O(1)**: append at the tail while pts increases
- **Out-of-order fallback**: binary insert only when ordering is violated (intra-track decoder output is already monotonic, so this practically never fires)
- **Two limits**: a time window plus a frame-count cap; **on overflow the newly arriving frame (tail) is dropped, never the head**
  - The head *is* the playback position. Dropping the head is a silent seek: the head moves ahead of the clock and never becomes "due" again → the picture freezes on one frame
  - Genuinely late frames are handled by `DropBefore(clock - 300ms)`, where the clock has already passed them, so dropping is safe

| | `av_sync = 0` (default, passthrough) | `av_sync > 0` (pre-buffered sync) |
|---|---|---|
| Start | Render as soon as decoded | Start after `av_sync` ms are buffered |
| Video | Render immediately; queue window 500ms, cap 30 frames | Align to master clock by pts; window `av_sync × 2`, cap 120 frames |
| Audio | Feed the device immediately; device backlog limit 200ms (queue window 800ms, cap 120 frames) | Device backlog limit becomes `av_sync` (queue window `av_sync × 2`, cap 300 frames) |
| A/V | Independent, may drift slowly over long runs | Tightly aligned |
| Latency | Lowest | ≈ buffer depth (after a recovery it may sit at the live edge, so latency drops) |

Implementation details:

- **Audio is the master clock**: the device drains at a constant rate, so "bytes sent − device backlog" gives the playback position. That value is only accepted within ±250ms of the wall clock; outside that it is treated as a bad reading and the wall clock is used
- **Monotonic clock**: the clock never moves backwards, avoiding the "drop a batch as late, then wait as not-yet-due" oscillation
- **Video catch-up**: frames more than **300ms** behind the master clock are dropped (`kLateDropMs`)
- **Timeline gap / underrun → re-buffer (stability first)**: when the head leads the clock by more than **500ms**, or the queue runs dry, playback pauses (audio is stopped and the device buffer cleared) and resumes from the head once `av_sync` ms are buffered again
  - Typical trigger: the server replays a cached GOP on subscribe; the burst fills the queue to its limit so new frames are dropped, and when the buffer is exhausted the next frame comes straight from the live edge
  - Hard-switching freezes the picture for the whole gap (measured 5s); waiting forever pins it on one frame. Re-buffering costs roughly `av_sync` ms of pause and restores a stable buffer depth
  - The log prints `缓冲中……（目标 2000ms，当前 0ms）` / `缓冲完成，恢复播放（可用 2000ms）`
  - If the input has already ended (stream closed / Ctrl-C), it does not wait for the buffer — it resumes with whatever is available so shutdown never hangs
- **Audio watermark**: feeding stops once the device backlog reaches its limit, so latency cannot grow without bound

## Directory layout

```
.
├── CMakeLists.txt                       # Top-level build: OpenSSL detection / libuv / webtransport_client
│                                        #                   / moq_transport_client / FFmpeg / SDL3
├── moq_player.cpp                       # Main program: arg parsing, pull, callbacks, threading, summary
├── moq_decoder.h / moq_decoder.cpp      # Decode layer: H264Decoder / AacDecoder (incl. avfilter conversion)
├── moq_render.h / moq_render.cpp        # Playback layer: FrameQueue / VideoSink / AudioSink / Player
├── README.md                            # This file (English)
├── README_cn.md                         # Chinese version
├── 3rdparty/
│   ├── ffmpeg.tar.gz                    # FFmpeg 8.0.1 source tarball (build input)
│   └── SDL3-3.4.20.tar.gz               # SDL3 source tarball (build input)
├── upstream/                            # git submodule: runner365/moqlivecast
└── requirement.md                       # Original requirements
```

`upstream/` is a git submodule (managed via `.gitmodules` + a gitlink). **Do not** add it to `.gitignore`, and don't edit or delete it in place; see the next section for how to bump the pinned commit.

## Upstream version

The submodule is pinned to `eaa6dd3` (moqlivecast `master`), which contains the quic-recovery fix for the
256KB stack array (originally shipped here as a stopgap patch) plus **upstream H.265 (HEVC) support**.
**No local patches are applied**, so the submodule is clean after `git submodule update --init`.

To move to a newer upstream commit:

```bash
git -C upstream fetch origin
git -C upstream checkout <commit>
git add upstream && git commit -m "Bump upstream to <commit>"
```


## Local end-to-end testing

The server and the pusher live in the upstream project (this project does not build them), so build them once. Prefer a separate build directory such as `build-upstream/` — don't generate build files inside the submodule:

```bash
cmake -S upstream -B build-upstream -DCMAKE_BUILD_TYPE=Release
cmake --build build-upstream -j8
# Artifacts: build-upstream/bin/{moqlivecast, flv2moqtransport_client, moqtransport_client_smoke}
```

> Upstream builds its own vendored OpenSSL 3.5.8 via ExternalProject by default, which is slow on the first run (our project uses the system OpenSSL and does not rebuild it).

Then:

```bash
# 1. Start the server (UDP 4433 by default, config in upstream/moqlivecast/etc/config.yml)
./build-upstream/bin/moqlivecast upstream/moqlivecast/etc/config.yml

# 2. Push a test stream (H.264 baseline + AAC 44.1kHz)
ffmpeg -f lavfi -i testsrc2=size=640x360:rate=30:duration=20 \
       -f lavfi -i sine=frequency=440:sample_rate=44100:duration=20 \
       -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 30 -b:v 600k \
       -c:a aac -b:a 96k -ar 44100 -ac 2 -y test.flv
./build-upstream/bin/flv2moqtransport_client test.flv 127.0.0.1 4433 live demo

# 3. Play
./build/bin/moq_player 127.0.0.1 4433 live demo --av-sync 500
```

The server needs about 3–4 seconds before it accepts connections; the pusher sends in **real time**, so a 20-second file is streamed over 20 seconds.

Typical output (log prefixes are uniformly `[file:line][time][level]`, trimmed below):

```
已发送 SUBSCRIBE，等待轨道就绪...
轨道就绪: H264 track=video config=40 字节
avcC: profile=66 level=31 nal_length=4 sps_count=1 sps_len=25 pps_count=1 | hex=01 42 c0 1f ff e1 ...
视频解码器已就绪（H.264 / avcC）
轨道就绪: AAC  track=audio config=5 字节
音频解码器已就绪（AAC 44100Hz 2ch → 48000Hz 2ch F32）
recv H264 #1  9277 B  dts=0ms  pts=0ms  KEY  [nal_len=9273 type=5 IDR]
dec  video #1  640x360 pix_fmt=yuv420p  linesize=[640,320,320]
dec  audio #1  1098 samples  rate=48000 fmt=flt layout=stereo
stat   2s 收 v=30/s a=43/s  解 v=30/s a=43/s
play 已渲染=38 已喂音频=95 丢弃=0  队列 v=29(933ms) a=0(0ms)

 解码 video: 199 帧  640x360 YUV420P
 解码 audio: 286 帧  317649 样本  44100Hz→48000Hz  2ch→2ch  F32
 播放 video: 166 帧  丢弃=0
 播放 audio: 238 块
结果：播放成功
```

### Regression checks

```bash
# Dump decoded output → verify duration from the byte count
./build/bin/moq_player 127.0.0.1 4433 live demo --duration 8 \
    --dump-yuv out.yuv --dump-pcm out.pcm
python3 -c "import os; print('frames', os.path.getsize('out.yuv')//(640*360*3//2))"

# Render a snapshot → compare pixel by pixel with the source frame (don't trust your eyes)
./build/bin/moq_player 127.0.0.1 4433 live demo --duration 6 --snapshot shot.bmp
```

## Known limitations

- **IPv4 only**: the upstream QUIC stack does not do IPv6; `host` must be an IPv4 literal or a name with an A record
- **Only tested on macOS**: Windows / Linux are untested (the TLS client skips certificate verification and connects to self-signed servers)
- **A 128KB stack frame remains on the receive path**: `FeedRaw` in `quic_connection.c` has `pkt_buf[65536] + pkt_orig[65536]`. Measured peak for the receive + decode chain is about 200KB (192KB crashes, 224KB passes), leaving roughly 2.5× headroom on the default 512KB stack, so it is left as is for now
- **10-bit H.265 goes through one pixel-format conversion**: the renderer only accepts YUV420P (`SDL_UpdateYUVTexture`, three 8-bit planes), so the common `yuv420p10le` output is converted to 8-bit with swscale before display — a per-frame CPU cost, and output is 8-bit
- **No seek / pause / volume control**: this is a pure live pull player
- **A timestamp rebase triggers one re-buffer**: when the server switches GOP or resets the timeline you see a short pause (logged as `缓冲中……`); that is expected, not a failure
- **Audio output lags by roughly `av_sync`**: in sync mode the audio device may hold up to `av_sync` ms of data (aligned with the video buffer); reduce `--av-sync` for lower latency
