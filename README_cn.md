# moq_player

[English](README.md) | **简体中文**

基于 **Media over QUIC (MoQ)** 的音视频播放器。从 moq 服务器**拉流**（SUBSCRIBE / media pull），用 FFmpeg 解码 **H.264 + AAC**，再交给 **SDL3** 渲染上屏、音频设备出声。

协议侧直接使用上游 C SDK [`runner365/moqlivecast`](https://github.com/runner365/moqlivecast) 的 `moq_transport_client`（moq-transport draft-22 over WebTransport），以 git submodule 形式放在 `upstream/`。

```
┌────────────────────── 后台线程 ──────────────────────┐   ┌───────────── 主线程 ──────────────┐
│                                                      │   │                                   │
│  moq SDK ──► H.264 解码 ──► 视频队列（按 pts 有序）  ├──►│  SDL3 渲染（YUV 直传，不转 RGB）  │
│  (AVCC)     AAC 解码 ──► avfilter ──► 音频队列       │   │  CoreAudio 连续喂数据             │
│                  (PCM 48k/2ch F32)                   │   │                                   │
└──────────────────────────────────────────────────────┘   └───────────────────────────────────┘
```

## 特性

- **拉流**：SUBSCRIBE 到 `app/stream`，自动解析 `avcC` / AudioSpecificConfig 并打印编解码参数
- **解码**：FFmpeg 8.0.1，支持 **H.264 / H.265(HEVC)** 视频与 AAC 音频，输出 YUV420P + PCM；无需 Annex-B / ADTS 转换
- **编码自适应**：直接采用 SDK 上报的 `moqt_stream_t::codec`（`MOQT_CODEC_H264` / `MOQT_CODEC_H265`）选择解码器，无需命令行开关；配置记录仅用于日志与一致性校验
- **音频重采样**：`avfilter`（`aresample` + `aformat`）从 44.1kHz FLTP 转为设备可播的 48kHz 立体声 F32
- **渲染**：SDL3 `SDL_UpdateYUVTexture` 直传 YUV420P，不经过 swscale 转 RGB
- **音视频同步**：默认 500ms 预缓冲后按 pts 精确对齐；`--av-sync 0` 可切成直通（最低延迟）
- **可观测**：统一走 moqlivecast 的异步 logger，带每秒速率/队列深度/丢弃统计；支持落盘 YUV/PCM 和渲染快照做回归比对

## 依赖

| 依赖 | 版本要求 | 来源 |
|---|---|---|
| CMake | ≥ 3.16 | 系统安装 |
| C++ 编译器 | 支持 C++14 | 系统自带（Clang / GCC） |
| OpenSSL | **3.5+**（需 `SSL_set_quic_tls_cbs` 等 QUIC TLS API） | 系统安装，如 `brew install openssl@3` |
| FFmpeg | 8.0.1 | `3rdparty/ffmpeg.tar.gz`，构建时由 ExternalProject 自动编译 |
| SDL3 | 3.4.20 | `3rdparty/SDL3-3.4.20.tar.gz`，`add_subdirectory` 静态编译 |
| libuv | 上游自带 | `upstream/3rdparty/libuv` |
| moqlivecast SDK | commit 见 submodule | `upstream/`（git submodule） |

FFmpeg 以最小配置编译（H.264/H.265/AAC 解码 + avfilter + swresample + swscale），约 2 分钟；产物安装到 `build/ffmpeg/install`。

## 构建

```bash
# 1. 拉取上游 SDK（首次 clone 后必须执行）
git submodule update --init --recursive

# 2. 配置 + 编译
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

产物：

```
build/bin/moq_player              # 播放器
build/ffmpeg/install/             # FFmpeg 静态库 + 头文件
```

可用 `-DMOQ_UPSTREAM_DIR=/path/to/moqlivecast` 指定别的上游副本（默认 `./upstream`）。

## 运行

```bash
./build/bin/moq_player <host> <port> <app> <stream> [选项]
```

示例：

```bash
# 默认：500ms 同步缓冲
./build/bin/moq_player 127.0.0.1 4433 live demo

# 直通模式（不预缓冲，最低延迟）
./build/bin/moq_player 127.0.0.1 4433 live demo --av-sync 0

# 更大缓冲，抗抖动更强
./build/bin/moq_player 127.0.0.1 4433 live demo --av-sync 2000

# 只听音频（不开窗口），或只看视频（不开音频设备）
./build/bin/moq_player 127.0.0.1 4433 live demo --audio-only
./build/bin/moq_player 127.0.0.1 4433 live demo --video-only
```

窗口内按 **ESC** 或关闭窗口退出；终端里 **Ctrl-C** 也可（所有退出路径都会干净释放并打印汇总）。

### 命令行参数

| 参数 | 说明 | 默认 |
|---|---|---|
| `--av-sync <ms>` | 预缓冲该毫秒数后按 pts 精确同步；`0` = 直通（不预缓冲，最低延迟） | `500` |
| `--video-only` | 只播放视频：不解 AAC、不打开音频设备 | — |
| `--audio-only` | 只播放音频：不解视频、不创建窗口 | — |
| `--path <p>` | WebTransport 路径 | `/moq` |
| `--duration <sec>` | 运行 N 秒后自动退出 | 跑到关窗/ESC/Ctrl-C |
| `--out-rate <hz>` | avfilter 音频输出采样率 | `48000` |
| `--out-channels <n>` | avfilter 音频输出声道数 | `2` |
| `--log-packets <n>` | 每条轨前 n 个报文逐条打印，`0` = 不打 | `0` |
| `--log-frames <n>` | 前 n 个解码帧逐帧打印，`0` = 全部 | `5` |
| `--dump-yuv <file>` | 解码后 YUV420P 落盘（回归校验用） | — |
| `--dump-pcm <file>` | avfilter 转换后 PCM 落盘（F32 裸流） | — |
| `--snapshot <file>` | 渲染若干帧后抓一张 BMP（验证渲染管线） | — |
| `--log-level <l>` | `error` / `warn` / `info` / `debug` | `info` |
| `--log-file <f>` | 额外把日志追加写入文件 | — |

环境变量 `MOQ_WORKER_STACK_KB` 可覆盖后台线程栈大小，**仅供调试**（默认走平台默认值，不设即可）。

## 诊断日志（排查卡顿/不同步）

`--log-level debug` 会额外输出两类行，专门用来定位“画面卡住”“音画不同步”这类问题：

```bash
./build/bin/moq_player <host> <port> <app> <stream> --av-sync 2000 \
    --log-level debug --log-file diag.log
```

**① 每秒一行 `[diag]`**（也是主循环的心跳：如果它不打了，说明主线程被卡住）

```
[diag] t=3s 时钟=1488(audio 偏差+8) 墙钟=1480 | V 首=1500 尾=3467 缓冲=1967ms n=60
       | A 位置=1488 缓冲=0ms n=0 设备积压=1987ms | 音画差=-21ms
       | 渲染=45 喂音=149 丢晚=0 丢满=0 跳转=0 重缓冲=0
```

| 字段 | 含义 |
|---|---|
| `时钟` / `(来源 偏差)` | 当前主时钟值、来源（`audio` 音频 / `wall` 墙钟）、音频时钟减墙钟的偏差 |
| `墙钟` | 起播锚点 + 实际流逝时间 |
| `V 首/尾/缓冲/n` | 视频队列队首 pts、队尾 pts、缓冲跨度（毫秒）、帧数 |
| `A 位置/缓冲/n` | 音频*正在播放*的媒体位置、队列缓冲、队列帧数 |
| `设备积压` | SDL 音频设备里尚未播完的数据（毫秒） |
| `音画差` | 最近渲染视频帧 pts − 音频播放位置（正=画面超前） |
| `丢晚` / `丢满` | 因落后时钟丢弃 / 因队列超限丢弃的帧数 |
| `跳转` / `重缓冲` | `ResyncTo` 次数 / 重新缓冲次数 |
| 末位状态 | 空 = 正常播放，`[未起播]` = 等预缓冲，`[缓冲中]` = 重新缓冲 |

**② 事件行 `[evt] …`**（只在状态变化时打印）

```
[evt] 主时钟切换 wall -> audio（音频-墙钟偏差 +16ms，阈值 ±250）
[evt] 等待队首到点: 领先主时钟 2680ms，已持续 1024ms（队首=9400 时钟=6720 时钟源=audio）
[evt] video pts 前跳: 4000 -> 9400 (Δ=+5400ms)
[evt] audio pts 倒退: 12130 -> 11980 (Δ=-150ms)
缓冲中……（目标 2000ms，当前 0ms）
缓冲完成，恢复播放（可用 2000ms）
```

排查时优先看这几类：
- `等待队首到点` 持续出现 → 视频队列的 pts 一直追不上时钟（画面会冻住）
- `pts 前跳` / `pts 倒退` → 服务端时间戳不连续
- `主时钟切换` 频繁来回 → 音频设备的播放位置读数不稳
- `时钟` 与 `墙钟` 差值持续变大 → 时钟跑飞

## 数据格式约定

这些是实测确认的，不是推测 —— 理解它们才能改解码层。

| 项 | SDK 给什么 | 我们怎么用 |
|---|---|---|
| 视频编码 | H.264 或 **H.265(HEVC)**，SDK 通过 `moqt_stream_t::codec` 上报 | 直接据此选择解码器；配置记录（`avcC`/`hvcC`）不一致时打一条 WARN 便于排查 |
| 视频码流 | **长度前缀 NAL**（4 字节长度 + NAL，非 Annex-B），两种编码一致 | 原样 `avcodec_send_packet`，不带转换 |
| 视频配置 | `avcC`（H.264，含 SPS/PPS）或 `hvcC`（H.265，含 VPS/SPS/PPS） | 直接作为 `extradata`，FFmpeg 自动识别长度前缀模式并内置拆包 |
| 音频码流 | **裸 AAC 帧**，无 ADTS 头 | 原样送解码器 |
| 音频配置 | AudioSpecificConfig（AAC-LC / 采样率 / 声道） | 作为 `extradata`，并自解析用于日志 |
| 时间戳 | `dts_ms` / `pts_ms`，单位毫秒 | 队列排序与同步基线，见下 |

> **注意**：服务端重放 GOP 时会 rebase 时间戳（可能从 0 重新开始），所以**不能把 dts 当绝对时间**。播放层会以队首帧为原点重建时间轴。

## 同步与队列

两条独立队列（视频 / 音频），元素是解码后的 `AVFrame`，按 `pts_ms` 有序：

- **正常路径 O(1)**：pts 递增时直接尾插
- **乱序兜底**：出现乱序才二分插入（同轨内解码器输出本就递增，几乎不触发）
- **双重上限**：时间窗口 + 帧数上限，**超限丢新到的帧（队尾），不丢队首**
  - 队首就是播放位置。丢队首等于在不通知时钟的情况下来一次 seek，会让队首永久领先时钟 → 画面定格在某一帧
  - 真正“已迟到”的帧由 `DropBefore(clock - 300ms)` 处理：那是时钟已经越过它们，丢弃不会让播放位置失控

| | `av_sync = 0`（直通） | `av_sync > 0`（预缓冲同步，**默认 500**） |
|---|---|---|
| 起播 | 解码完立刻渲染 | 攒满 `av_sync` 毫秒后起播 |
| 视频 | 解码即上屏；队列窗口 500ms，上限 30 帧 | 按 pts 对齐主时钟；队列窗口 `av_sync × 2`，上限 120 帧 |
| 音频 | 解码即喂设备；设备积压上限 200ms（队列窗口 800ms，上限 120 帧） | 设备积压上限提为 `av_sync`（队列窗口 `av_sync × 2`，上限 300 帧） |
| 音画 | 各自独立，长跑可能缓慢漂移 | 精确对齐 |
| 延迟 | 最低 | ≈ 缓冲深度（出问题恢复后可能停在直播沿，延迟自动变小） |

实现细节：

- **音频为主时钟**：设备匀速抽干，用「已发送字节 − 设备积压字节」换算出播放位置，最稳；该值只在墙钟 ±250ms 内被采纳，超出视为读数异常退回墙钟
- **时钟单调**：绝不回退，避免「先判迟到丢一批、再判未到点等着」的抖动
- **视频追赶**：落后主时钟超过 **300ms** 的帧直接丢弃（`kLateDropMs`）
- **时间轴断层/欠载 → 重新缓冲（稳定优先）**：队首领先时钟超过 **500ms**，或队列被播空时，不硬接也不干等，而是暂停播放（同时停掉音频、清空设备缓冲），攒够 `av_sync` 毫秒后从队首恢复
  - 触发场景：服务端订阅瞬间补发缓存 GOP，突发数据把队列顶到上限后新帧被丢弃；缓存放完后，下一帧直接来自直播最新位置
  - 硬接会冻结整个空档（实测 5 秒）；干等会永久停在某一帧。重新缓冲的代价是约 `av_sync` 毫秒的停顿，换来恢复后缓冲深度稳定
  - 日志会打印 `缓冲中……（目标 2000ms，当前 0ms）` / `缓冲完成，恢复播放（可用 2000ms）`
  - 输入已结束（流断/Ctrl-C）时不等缓冲，按现有数据直接恢复，避免退出卡死
- **音频喂水线**：设备积压达到上限就暂停投喂，防止延迟无限增长

## 目录结构

```
.
├── CMakeLists.txt                       # 顶层构建：OpenSSL 探测 / libuv / webtransport_client
│                                        #             / moq_transport_client / FFmpeg / SDL3
├── moq_player.cpp                       # 主程序：参数解析、拉流、回调、线程编排、统计汇总
├── moq_decoder.h / moq_decoder.cpp      # 解码层：H264Decoder / AacDecoder（含 avfilter 音频转换）
├── moq_render.h / moq_render.cpp        # 播放层：FrameQueue / VideoSink / AudioSink / Player
├── README.md                            # 英文版
├── README_cn.md                         # 本文件（中文版）
├── 3rdparty/
│   ├── ffmpeg.tar.gz                    # FFmpeg 8.0.1 源码包（构建输入）
│   └── SDL3-3.4.20.tar.gz               # SDL3 源码包（构建输入）
├── upstream/                            # git submodule: runner365/moqlivecast
└── requirement.md                       # 原始需求
```

`upstream/` 是 git submodule（`.gitmodules` + gitlink 管理），**不要**把它加进 `.gitignore`，也不要手动删改；需要升级上游版本时见下节。

## 上游版本

submodule 固定在 `eaa6dd3`（moqlivecast `master`），包含 quic-recovery 的 256KB 栈数组修复，以及 **H.265(HEVC) 的上游支持**
（最初在本仓库以临时补丁形式存在，现已合入上游）。**本仓库不再打任何补丁**，执行
`git submodule update --init` 后子模块工作区是干净的。

需要升级到更新的上游提交时：

```bash
git -C upstream fetch origin
git -C upstream checkout <commit>
git add upstream && git commit -m "Bump upstream to <commit>"
```


## 本地端到端联调

服务端和推流器都在上游工程里（本工程不构建它们），先单独编一次。**建议编到 `build-upstream/`**，别在 submodule 里生成构建目录：

```bash
cmake -S upstream -B build-upstream -DCMAKE_BUILD_TYPE=Release
cmake --build build-upstream -j8
# 产物：build-upstream/bin/{moqlivecast, flv2moqtransport_client, moqtransport_client_smoke}
```

> 上游默认会用 ExternalProject 编译它自带的 OpenSSL 3.5.8，首次构建比较慢（我们自己的工程走系统 OpenSSL，不重复编）。

然后：

```bash
# 1. 起服务端（默认 UDP 4433，配置见 upstream/moqlivecast/etc/config.yml）
./build-upstream/bin/moqlivecast upstream/moqlivecast/etc/config.yml

# 2. 推一路测试流（H.264 baseline + AAC 44.1kHz）
ffmpeg -f lavfi -i testsrc2=size=640x360:rate=30:duration=20 \
       -f lavfi -i sine=frequency=440:sample_rate=44100:duration=20 \
       -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 30 -b:v 600k \
       -c:a aac -b:a 96k -ar 44100 -ac 2 -y test.flv
./build-upstream/bin/flv2moqtransport_client test.flv 127.0.0.1 4433 live demo

# 3. 播放
./build/bin/moq_player 127.0.0.1 4433 live demo --av-sync 500
```

服务端启动后约 3~4 秒才可连接；推流器是 **1x 实时**推送，20 秒的片子就只推 20 秒。

典型输出（日志前缀统一为 `[文件:行号][时间][等级]`，下面截掉了）：

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

### 回归校验

```bash
# 解码结果落盘 → 按字节数反推时长是否与源一致
./build/bin/moq_player 127.0.0.1 4433 live demo --duration 8 \
    --dump-yuv out.yuv --dump-pcm out.pcm
python3 -c "import os; print('帧数', os.path.getsize('out.yuv')//(640*360*3//2))"

# 渲染结果抓图 → 与源帧逐像素比对（别靠肉眼看颜色）
./build/bin/moq_player 127.0.0.1 4433 live demo --duration 6 --snapshot shot.bmp
```

## 已知限制

- **仅支持 IPv4**：上游 QUIC 栈不做 IPv6，host 需为 IPv4 字面量或能解析出 A 记录的域名
- **只验证过 macOS**：Windows / Linux 未测（TLS 客户端跳过证书校验，自签证书直连）
- **接收路径仍有 128KB 栈帧**：`quic_connection.c` 的 `FeedRaw` 有 `pkt_buf[65536] + pkt_orig[65536]`；实测接收+解码链峰值约 200KB（192KB 栈崩、224KB 通过），默认 512KB 下约 2.5× 余量，暂未改
- **10bit H.265 会做一次像素格式转换**：渲染层只吃 YUV420P（`SDL_UpdateYUVTexture` 三平面 8bit），HEVC 常见的 `yuv420p10le` 会用 swscale 转成 8bit 再上屏，有一次性 CPU 开销，画质按 8bit 呈现
- **`--video-only` / `--audio-only` 只影响播放侧**：SDK 在 SUBSCRIBE 时会同时订阅 video 与 audio 两路（`moq_transport_client.c` 里固定发两条 SUBSCRIBE），所以另一路的带宽照旧消耗，只是不解码、不播放
- **无 seek / 暂停 / 音量控制**：当前是纯直播拉流播放器
- **时间戳 rebase 会触发一次重新缓冲**：服务端切换 GOP/时间轴重置时，表现为短暂停顿（日志有 `缓冲中……`），不是故障
- **音频有 `av_sync` 量级的出声延迟**：同步模式下音频设备最多积压 `av_sync` 毫秒（与视频缓冲对齐），要更低延迟请减小 `--av-sync`
