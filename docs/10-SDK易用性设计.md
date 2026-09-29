# 10 · SDK 易用性设计

> 上一篇：[09 Linux 平台实现方案](09-Linux平台实现方案.md) ｜ 返回：[README](../README.md)

> **实现状态**：本篇是设计文档，描述目标形态。**当前已落地到哪一步以
> [PROGRESS.md](PROGRESS.md) 为唯一真相源**；两者的差异（已实现 / 计划中）在 PROGRESS.md 里逐项标注。

用户选定本项目定位为 **面向二次开发者的 SDK**。本篇定义"好用"的可执行标准 —— 不是形容词，而是能进 CI、能评审、能量化的具体条款。

---

## 1. 定位与验收

一个 C++ 播放 SDK 好不好用，最终体现在：**一个没用过它的 C++ 开发者，从 `git clone` 到播出一段视频，需要多少分钟、查多少次文档、遇到多少条看不懂的错误。**

| 验收项 | 目标 |
|---|---|
| V1 冷启动到出画 | **≤ 15 分钟**（含编译），且全程只需读 README |
| V2 README 里的第一个代码块 | **可直接复制粘贴编译通过**（CI 用 `examples/quickstart.cc` 逐字验证） |
| V3 依赖数量 | 核心 0 个；完整 Linux 播放 1 个（FFmpeg）；SDL2 后端 +1 |
| V4 集成方式 | CMake `find_package` / `pkg-config` / vcpkg / conan / 手工拷贝，5 种全支持 |
| V5 错误信息可操作率 | **100%**：每条错误都含"发生了什么 + 为什么 + 怎么办" |
| V6 无需理解 FFmpeg | 公开头文件 0 个 `AV*` 类型；文档 0 处要求用户配 `AVDictionary` |
| V7 无需管理线程 | 用户代码里 0 个 `std::thread`；`Player` 析构不卡死 |
| V8 文档覆盖 | 20 个 cookbook 条目覆盖 90% 的常见任务 |
| V9 头文件自解释 | 每个 public 方法注明：在哪个线程调、是否阻塞、失败时返回什么 |
| V10 版本兼容 | SemVer；minor 版本升级不改用户代码即可编译通过 |

---

## 2. 十条 API 人体工学规则

写进 code review checklist，违反必须给出理由。

**E1 · 零配置可用（Batteries Included）**
```cpp
ijkpp::Player player;                                  // 自动探测平台后端
player.SetDataSource("video.mp4");                     // 就这样
player.PrepareAsync();
```
所有依赖都能自动探测：FFmpeg 解码器、SDL2/GL 视频后端、ALSA/Pulse 音频后端。**`Deps` 是可选的高级用法，不是必经之路。**

**E2 · 默认值就是最佳实践**
`PlayerConfig` 的每个默认值都应是"大多数人想要的"。例：`seek.accurate` 默认 `false`（快）但 `video.handle_resolution_change` 默认 `true`（不花屏）。有争议的默认值必须在文档里解释理由。

**E3 · 构造即有效，不存在的状态无法表达**
`Player` 没有"未初始化"的裸构造 + `Init()` 二段式。`SetDataSource` 之前调 `Start()` 返回 `kInvalidState` 而不是崩溃或静默忽略。

**E4 · 阻塞语义显式命名**
`PrepareAsync()` vs `PrepareSync(timeout)`、`Stop()` vs `StopSync(timeout)`。凡是可能阻塞的方法名里必须有 `Sync` 或 `timeout` 参数。默认全部非阻塞。

**E5 · 错误可操作，不是错误码字典**
见 §4。返回 `base::expected<void, MediaError>`，`MediaError::message()` 是一句人话 + 修复建议，`code()` 供程序分支，`native_code()` 供深挖。

**E6 · 时间用 `base::TimeDelta`，禁止裸整数**
```cpp
player.SeekTo(base::Seconds(90));          // ✅ 意图明确
player.SeekTo(base::Milliseconds(90000));  // ✅
// player.SeekTo(90000);                   // ❌ 编译不过：90000 是 ms 还是 µs？
```
ijkplayer 的 `ijkmp_seek_to(mp, msec)` 与内部 `pts`（µs）、`audio_clock`（秒 double）三种单位混用是 bug 高发区，SDK 层彻底消除。

**E7 · 事件用强类型 + 便捷访问器，不强迫用户写 `std::visit`**
```cpp
player.SetEventHandler([](const ijkpp::PlayerEvent& e) {
  // 方式一：switch（最简单）
  switch (e.type) {
    case ijkpp::EventType::kPrepared: player.Start(); break;
    case ijkpp::EventType::kError:
      LOG(ERROR) << ijkpp::AsError(e)->error.ToString();
      break;
    default: break;
  }
});
// 方式二：只关心某几种（编译期过滤）
player.Observe<ijkpp::ErrorEvent, ijkpp::CompletedEvent>(
    [](const auto& ev) { ... });
// 方式三：观察者接口（大项目推荐）
class MyObserver : public ijkpp::PlayerObserver {
  void OnPrepared(const PreparedPayload&) override { ... }
};
```
三种方式并存，用户按项目规模选。

**E8 · 一次调用拿全部信息，不强迫轮询**
`GetPlaybackStats()` 返回一个结构体，替代原版 20+ 次 `ijkmp_get_property_int64`。`DumpDiagnostics()` 返回完整 JSON。

**E9 · 析构安全，不需要用户显式 `Release()`**
```cpp
{ ijkpp::Player p; ... }        // 出作用域自动停止，有超时兜底，绝不卡死
```
不需要 `ijkmp_dec_ref` / `ijkmp_shutdown` 的成对调用。

**E10 · 渐进式复杂度**
```
Level 0  Player p; p.SetDataSource(url); p.PrepareAsync();          // 3 行
Level 1  + SetEventHandler / SetVideoSurface / SeekTo                // 常用
Level 2  + PlayerConfig 调参                                          // 进阶
Level 3  + PlayerBuilder + OptionRegistry（字符串配置迁移）            // 从 ijkplayer 迁移
Level 4  + Deps 注入自定义 VideoRendererSink / VideoDecoder / DataSource  // 深度定制
```
文档与示例按 Level 组织，Level 0–1 的内容占 README 的 80%。

---

## 3. Quick Start（README 里的第一个代码块，CI 逐字验证）

### Level 0 — 10 行出画

```cpp
// quickstart.cc —— 这段代码由 CI 编译验证（examples/quickstart.cc）
#include "player/public/player.h"
#include <cstdio>

int main() {
  ijkpp::Player player;
  player.SetEventHandler([&player](const ijkpp::PlayerEvent& e) {
    if (e.type == ijkpp::EventType::kPrepared) player.Start();
    if (e.type == ijkpp::EventType::kCompleted) player.Stop();
  });
  if (auto r = player.SetDataSource("video.mp4"); !r) {
    std::fprintf(stderr, "%s\n", r.error().ToString().c_str());
    return 1;
  }
  if (auto r = player.PrepareAsync(); !r) {
    std::fprintf(stderr, "%s\n", r.error().ToString().c_str());
    return 1;
  }
  player.RunUntilIdle();   // 内建窗口模式下的简易事件循环，见下
  return 0;
}
```

编译：
```bash
g++ -std=c++20 -fno-exceptions -fno-rtti quickstart.cc \
    $(pkg-config --cflags --libs ijkpp) -o quickstart
./quickstart
```

> **`RunUntilIdle()` 的说明**：SDK 不该强迫用户写事件循环。`Player::RunUntilIdle()` 是一个便捷方法，内部阻塞直到播放结束或 `Stop()` 被调用（等价于 `base::WaitableEvent` 等待终态）。生产代码通常不需要它（用 `SetEventHandler` + 自己的主循环）。这个方法是 **Level 0 体验的关键**，v1 设计里缺失。

### Level 1 — 嵌入到你自己的窗口（30 行）

```cpp
#include "player/public/player.h"
#include <X11/Xlib.h>

int main() {
  Display* dpy = XOpenDisplay(nullptr);
  Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                   0, 0, 1280, 720, 0, 0, 0);
  XMapWindow(dpy, win);
  XFlush(dpy);

  ijkpp::Player player;
  player.SetVideoSurface(ijkpp::NativeDisplay::FromX11Window({dpy, win}));
  player.SetEventHandler([&](const ijkpp::PlayerEvent& e) {
    switch (e.type) {
      case ijkpp::EventType::kPrepared:   player.Start(); break;
      case ijkpp::EventType::kError:      HandleError(*ijkpp::AsError(e)); break;
      case ijkpp::EventType::kCompleted:  Quit(); break;
      default: break;
    }
  });

  IJKPP_RETURN_IF_ERROR(player.SetDataSource("video.mp4"));
  IJKPP_RETURN_IF_ERROR(player.PrepareAsync());

  // 你自己的事件循环；ijkpp 不碰你的 Display（见 docs/09 §3.1 铁律）
  XEvent ev;
  while (!ShouldQuit()) {
    while (XPending(dpy)) { XNextEvent(dpy, &ev); HandleEvent(ev); }
    // ...
  }
  player.StopSync(base::Seconds(1));
  return 0;
}
```

### Level 2 — 调参

```cpp
ijkpp::PlayerConfig config;
config.buffer.max_bytes            = 8 * 1024 * 1024;
config.buffer.max_cached_duration  = base::Seconds(10);
config.video.decoder_preference    = ijkpp::DecoderPreference::kHardwareFirst;
config.video.max_frame_drop        = 8;
config.seek.accurate               = true;
config.render.linux_backend        = ijkpp::LinuxVideoBackend::kGl;
config.audio.backend               = ijkpp::AudioBackend::kPulse;

ijkpp::Player player(config);
```

### Level 3 — 从 ijkplayer 配置迁移（字符串选项）

```cpp
ijkpp::PlayerBuilder builder;
// 你原来的 ijkmp_set_option_int 调用可以几乎原样搬过来
for (const auto& [cat, key, value] : legacy_options) {
  auto r = builder.SetOption(cat, key, value);
  if (!r) LOG(WARNING) << "option rejected: " << r.error().message();
}
auto player = std::move(builder.Build().value());
```

### Level 4 — 注入自定义后端

```cpp
auto deps = std::make_unique<ijkpp::Deps>();
deps->video_renderer_sink_factory = base::MakeRefCounted<MyOpenGLSinkFactory>();
deps->audio_renderer_sink_factory = base::MakeRefCounted<MyAudioSinkFactory>();
deps->video_decoder_factories.push_back(std::make_unique<MyVaapiDecoderFactory>());
deps->data_source_factory = std::make_shared<MyEncryptedDataSourceFactory>();
deps->logging_delegate = std::make_unique<MyLogger>();
deps->event_dispatcher = MyUiThreadDispatcher::Create();

ijkpp::Player player(config, std::move(deps));
```

---

## 4. ★可操作错误信息规范

这是 SDK 友好度最容易做差、也最容易做出差异化的地方。

### 4.1 `MediaError` 结构

```cpp
// player/public/error.h
namespace ijkpp {

enum class ErrorCode : uint16_t { /* 见 03 §2.1 */ };

class IJKPP_PLAYER_EXPORT MediaError {
 public:
  MediaError() = default;
  MediaError(ErrorCode code, std::string summary, std::string detail,
             std::string suggestion, int native_code = 0,
             std::string context = {});

  ErrorCode code() const;
  int native_code() const;              // AVERROR / errno / 平台错误码
  const std::string& context() const;   // "FFmpegDemuxer::Initialize"

  // 三段式人类可读消息，见 §4.2
  const std::string& summary() const;    // 一行：发生了什么
  const std::string& detail() const;     // 若干行：为什么（含关键上下文值）
  const std::string& suggestion() const; // 一行：怎么办

  std::string ToString() const;          // summary + detail + suggestion 拼接
  std::string ToJson() const;            // 供 DumpDiagnostics / 上报

  bool ok() const { return code_ == ErrorCode::kOk; }
  static MediaError Ok();
};

}  // namespace ijkpp
```

### 4.2 三段式格式（强制）

```
<ErrorCode>: <一句话说明发生了什么>
  context: <出错位置>
  detail:  <关键上下文值，每行一条>
  hint:    <具体的下一步动作>
```

**规范**：
- `summary` ≤ 80 字符，不含换行，主语是"ijkpp 做了什么失败了"而非"你错了"
- `detail` 必须含**实际值**（文件路径、分辨率、codec 名、超时时长），不能只说"失败"
- `suggestion` 必须是**可执行动作**，指明具体的 API / 配置项 / 命令
- `native_code` 附上原始错误码并翻译（`av_strerror` / `strerror`）
- 不使用"internal error"/"unknown error"作为最终消息 —— 若真无法归因，明确说"无法归因"并给出诊断命令

### 4.3 20 个实例（作为实现的验收样例）

```
SourceOpenFailed: cannot open the media source
  context: FFmpegDemuxer::Initialize
  detail:  uri = "https://cdn.example.com/v.mp4"
           native = AVERROR_HTTP_NOT_FOUND (404) — "Server returned 404 Not Found"
  hint:    verify the URL is reachable (curl -I "<uri>"). For URLs that expire,
           refresh them and call SetDataSource() again.

SourceUnsupported: no demuxer can parse this container
  context: FFmpegDemuxer::Initialize
  detail:  uri = "/tmp/blob"   size = 1048576 bytes
           probe read 5242880 bytes, best format score 12/100 (threshold 30)
           native = AVERROR_INVALIDDATA — "Invalid data found when processing input"
  hint:    the file may be truncated or not a media container. If it is a valid
           container FFmpeg cannot auto-detect, set config.demux.forced_format
           (e.g. "h264", "mpegts") or pass demux.probe_size a larger value.

DecoderNotFound: no decoder for codec
  context: DecoderSelector::SelectCandidates
  detail:  codec = AV1 (avcodec id 226)   stream = video#0   1920x1080
           tried: [FFmpegVideoDecoder]
           FFmpeg build has no AV1 decoder (libdav1d/libaom not compiled in)
  hint:    install an FFmpeg build with AV1 support (e.g. `apt install
           libavcodec-extra`), or register a custom VideoDecoderFactory via
           Deps::video_decoder_factories.

DecoderOpenFailed: hardware decoder rejected the stream
  context: VaapiVideoDecoder::Initialize
  detail:  codec = HEVC   profile = Main10   3840x2160   device = /dev/dri/renderD128
           native = AVERROR(ENODEV) — vaCreateContext failed
  hint:    your GPU/driver may not support 10-bit HEVC at this resolution.
           Set config.video.decoder_preference = DecoderPreference::kSoftware
           to force software decoding, or kAuto to fall back automatically
           (a kDecoderFallback event will be emitted).

SinkConfigureFailed: video sink rejected the output configuration
  context: GlVideoRendererSink::Configure
  detail:  requested format = P010 (10-bit)   GL version = 3.3 (Mesa, llvmpipe)
           GL_R16 texture upload requires GL 3.0 + GL_ARB_texture_rg — available
           but the driver reported GL_OUT_OF_MEMORY on a 3840x2160 buffer
  hint:    reduce config.video.hw_max_frame_width, or use
           config.video.overlay_format = OverlayFormat::kI420 to halve the
           upload size. On software rasterizers (llvmpipe) 4K10bit is not viable.

InvalidState: Start() called before the player is prepared
  context: PlayerImpl::Start
  detail:  current state = kInitialized   required state = kPrepared or kStarted
           state history: kIdle -> kInitialized (SetDataSource, 12 ms ago)
  hint:    call PrepareAsync() and wait for the kPrepared event, or use
           PrepareSync() for a blocking variant. Set
           config.start_on_prepared = true to start automatically.

InvalidArgument: unknown option key
  context: OptionRegistry::SetInt
  detail:  category = kPlayer   key = "max-buffer-sizes"   value = 8388608
           did you mean "max-buffer-size"?  (edit distance 1)
  hint:    call OptionRegistry::Describe() for the full list of supported keys,
           or use the typed field config.buffer.max_bytes instead.

InvalidArgument: option value out of range
  context: OptionRegistry::SetInt
  detail:  key = "max-fps"   value = 1000   allowed = [-1, 121]
  hint:    -1 disables the fps cap. Typical values are 30 or 60.

Timeout: prepare timed out
  context: PlayerImpl::PrepareSync
  detail:  timeout = 30 s   last stage reached = kFindStreamInfo (28.4 s ago)
           uri = "rtmp://slow.example/live"
           stages: kOpenInput = 210 ms, kFindStreamInfo = <in progress>
  hint:    the server is not delivering enough data for stream probing. Increase
           config.demux.timeout, or reduce config.demux.analyze_duration /
           config.demux.probe_size to probe faster.

SinkNotAttached: cannot present video without an output target
  context: GlVideoRendererSink::Render
  detail:  SetVideoSurface() was never called, or was called with an invalid
           NativeDisplay (kind = kNone)
  hint:    call player.SetVideoSurface(NativeDisplay::FromX11Window({dpy, win}))
           (or FromWaylandSurface / FromSdl2Window). To play audio only, set
           config.render.disable_video_output = true.

SourceReadFailed: network read error while demuxing
  context: FFmpegDemuxer::DemuxLoop
  detail:  uri = "https://cdn.example.com/v.mp4"   position = 47.2 s (byte 81234567)
           native = AVERROR(ETIMEDOUT) — "Connection timed out"
           reconnect attempts = 3/3 (config.net.reconnect_max_retries)
  hint:    increase config.net.reconnect_max_retries, or call ReconnectNow()
           to retry on demand. For live streams set config.net.live_max_latency
           so the player catches up instead of stalling.

SeekFailed: seek not possible on this stream
  context: SeekController::Request
  detail:  target = 30 s   stream seekable = false   format = "hls"
           media_info().seekable = false
  hint:    live and non-seekable streams cannot seek. Check player.is_live();
           for VOD-over-HLS the playlist must contain #EXT-X-ENDLIST.

OutOfMemory: frame pool exhausted
  context: VideoFramePool::AllocateFrame
  detail:  requested 1920x1080 I420 (3110400 bytes)   pool in use = 62208000 bytes
           pending frames = 20   config.video.frame_queue_size = 3
           heap estimate = 412 MB
  hint:    your VideoRendererSink may be holding frames too long (each
           scoped_refptr<VideoFrame> keeps its memory alive). Release frames
           promptly in Render(). Also check config.video.frame_queue_size and
           config.buffer.max_bytes.

Aborted: operation cancelled by Stop()
  context: DecoderStream::ReadFromDemuxerStream
  detail:  pending decode requests = 2   state = kStopped
  hint:    this is expected during shutdown; treat ErrorCode::kAborted as a
           normal completion, not a failure.

InvalidState: config field cannot change while playing
  context: PlayerImpl::UpdateConfig
  detail:  field = "video.overlay_format"   current state = kStarted
           runtime-mutable fields: volume, muted, playback_rate, loop_count,
           buffer.enabled, net.reconnect*, render.disable_video_output
  hint:    call Stop() (or Reset()) before changing this field, or set it in
           the PlayerConfig passed to the constructor.

SinkPresentFailed: OpenGL swap failed
  context: GlVideoRendererSink::Present
  detail:  window system = X11 (GLX)   eglSwapBuffers returned EGL_BAD_SURFACE
           display = 0x5583a2b0e200   surface = 0x0 (destroyed by the host?)
  hint:    the host window was destroyed while playback was active. Call
           SetVideoSurface(nullptr) before destroying your window, or subscribe
           to kError and call Stop().

ConfigInvalid: 3 configuration problems found
  context: PlayerConfig::Validate
  detail:  [1] buffer.first_high_water_mark (2000 ms) must be <=
               buffer.next_high_water_mark (1000 ms)
           [2] video.frame_queue_size = 1 is below the minimum 2
           [3] audio.startup_volume = 1.4 is outside [0.0, 1.0]
  hint:    fix the fields above, or start from PlayerConfig{} defaults and only
           change what you need. PlayerBuilder::Build() reports all problems at
           once so you can fix them in one pass.

DecoderHwFallback: hardware decoding failed, falling back to software
  context: DecoderStream::OnDecoderFallback
  detail:  failed = VaapiVideoDecoder (codec HEVC 3840x2160, status kDecodeError)
           fallback = FFmpegVideoDecoder (threads = 8)
  hint:    playback continues in software; expect higher CPU usage. To avoid
           the fallback cost on every start, set
           config.video.decoder_preference = DecoderPreference::kSoftware.

Timeout: shutdown did not complete in time
  context: Player::~Player
  detail:  timeout = 500 ms (config.shutdown_timeout)
           still running: ijkpp-demux (blocked in av_read_frame)
           diagnostics: { "state": "kStopping", "seek": {"phase":"kWaitingDemux"}, ... }
  hint:    the demuxer thread was blocked in network IO and did not honour the
           interrupt callback in time. Increase config.shutdown_timeout, or
           reduce config.demux.timeout so av_read_frame aborts faster. The
           thread has been detached; no resource is leaked beyond it.

TrackNotFound: requested track does not exist
  context: PlayerImpl::SelectTrack
  detail:  type = kText   index = 3   available text tracks = [0 (chi), 2 (eng)]
  hint:    use media_info().streams to enumerate valid indices.
```

**验收**：`tests/unit/player/error_messages_unittest.cc` 对每个 `ErrorCode` 断言 `summary` / `detail` / `suggestion` 三段都非空、`suggestion` 含至少一个 API 名或配置项名（正则匹配 `[A-Za-z]+\(\)|config\.[a-z_.]+`）。

### 4.4 日志同样遵循可操作原则

```cpp
// 不好
LOG(ERROR) << "failed";
// 好
LOG(ERROR) << "VideoDecoderFactory returned no candidates for HEVC Main10 "
           << "3840x2160; tried 2 factories. Falling back to software.";
```

`MediaLog` 事件（对齐 Chromium `media::MediaLog`）会同时进入 `base::logging` 与 `DumpDiagnostics()` 的 `recent_logs` 字段，用户报 bug 时一份 JSON 就够。

---

## 5. 头文件自解释规范

每个 public 方法的注释必须回答四个问题（缺一不可，CI 用脚本抽查 `player/public/` 的注释覆盖率）：

```cpp
// (1) 做什么 —— 一句话
// (2) 在哪个线程/sequence 调，会不会阻塞
// (3) 什么时候失败，返回什么
// (4) 有什么副作用 / 触发什么事件
//
// Requests an asynchronous seek to |position|.
//
// Thread safety: may be called from any thread. Returns immediately; the
// actual seek happens on the media sequence.
//
// Errors: kInvalidState if the player is not in kPrepared/kStarted/kPaused/
// kCompleted; kMediaUnseekable if media_info().seekable is false.
//
// Side effects: emits kSeekCompleted (and kAccurateSeekCompleted when
// config.seek.accurate is set) on the event sequence, then runs |cb| there.
// A pending seek is superseded by a newer one; the superseded request's |cb|
// still runs, with result kAborted.
void SeekTo(base::TimeDelta position, SeekMode mode, SeekCB cb);
```

类级注释必须包含：**用途、所有权、sequence 归属、典型用法片段**（对齐 Chromium 头文件风格）。

---

## 6. 文档体系

| 文档 | 受众 | 内容 | 位置 |
|---|---|---|---|
| `README.md` | 所有人 | 30 秒了解 + Level 0/1 quickstart + 安装 | 仓库根 |
| `docs/BUILDING.md` | 集成者 | 各平台依赖与构建命令 | `docs/` |
| `docs/API.md` | 集成者 | **Doxygen 生成**，全部 public 头文件 | 构建产物 + GitHub Pages |
| `docs/COOKBOOK.md` | 集成者 | 20 个任务导向配方（§8） | `docs/` |
| `docs/MIGRATION.md` | 从 ijkplayer 迁移者 | 基于 [05 迁移对照表](05-迁移对照表.md) + 代码 diff 示例 | `docs/` |
| `docs/TROUBLESHOOTING.md` | 集成者/运维 | 症状 → 诊断命令 → 常见原因 → 修复（§10） | `docs/` |
| `docs/EXTENDING.md` | 高级用户 | 如何写自定义 `VideoRendererSink` / `VideoDecoder` / `DataSource`（Level 4） | `docs/` |
| `docs/ARCHITECTURE.md` | 贡献者 | 本套设计文档 01–09 的入口 | `docs/` |
| `docs/PERFORMANCE.md` | 性能调优者 | 性能预算、基准结果、调参指南 | `docs/` |
| `CHANGELOG.md` | 所有人 | 每版变更 + 破坏性变更高亮 + Δ 编号索引 | 仓库根 |
| `STYLE.md` | 贡献者 | Google Style + Chromium 约定落地 | 仓库根 |

**文档 CI 检查**：
- `README.md` 里的每个代码块被抽取到 `examples/doc_snippets/*.cc` 并编译（V2）
- `docs/API.md` 每次 release 重新生成，diff 出公开 API 变化并写进 `CHANGELOG.md`
- `player/public/*.h` 的 public 方法注释覆盖率 ≥ 95%（脚本检查）

---

## 7. 示例矩阵

| 示例 | Level | 演示 | 行数目标 |
|---|---|---|---|
| `examples/quickstart.cc` | 0 | README 的代码块，逐字验证 | ≤ 25 |
| `examples/play_sdl2` | 0–1 | 内建窗口播放 + 键盘交互 + stats | ≤ 300 |
| `examples/play_native` | 1 | 原生 GL 后端 + 自建 X11/Wayland 窗口 | ≤ 400 |
| `examples/play_embed` | 1 | ★嵌入模式：X11 `--wid` / Wayland `--wl-surface` / GTK4 宿主 | ≤ 350 |
| `examples/headless` | 0 | 无显示播放到 NullSink（CI、转码前处理） | ≤ 80 |
| `examples/stats_dump` | 1 | 每秒打印 `PlaybackStats`（监控集成范式） | ≤ 100 |
| `examples/seek_stress` | 1 | seek 风暴 + 状态断言（稳定性范式） | ≤ 120 |
| `examples/custom_sink` | 4 | 实现一个把 YUV 写到文件的 `VideoRendererSink` | ≤ 200 |
| `examples/custom_decoder` | 4 | 包装一个假的 `VideoDecoder`（演示接口） | ≤ 200 |
| `examples/custom_data_source` | 4 | 从内存/自定义下载器播放 | ≤ 200 |
| `examples/snapshot` | 1 | `TakeSnapshot()` 抽帧 | ≤ 80 |
| `examples/ijkpp_inspect` | 2 | 诊断 CLI（dump / play / golden） | ≤ 600 |

**每个 example 都有 `--help` 和 `README` 段落**，且都在 CI 里编译（`play_native` / `play_embed` 在 xvfb 下实跑）。

**`main.cc` 行数是 API 易用性的度量**：如果 `play_sdl2/main.cc` 超过 300 行，说明 API 缺东西，回头改 `player/public/`。这条写进 [08](08-实施路线图与风险.md) 的 M11 DoD。

---

## 8. Cookbook（`docs/COOKBOOK.md` 的 20 条目录）

| # | 任务 | 关键 API |
|---|---|---|
| 1 | 播放一个本地文件 | `SetDataSource` + `PrepareAsync` + `kPrepared` → `Start` |
| 2 | 播放网络流（HLS/RTMP） | 同上 + `config.net.*` |
| 3 | 从内存播放（已下载的 buffer） | `DataSourceDescriptor::FromMemory(span)` |
| 4 | 从自定义下载器播放 | 实现 `DataSource` + `Deps::data_source_factory` |
| 5 | 渲染到自己的 X11 窗口 | `NativeDisplay::FromX11Window` |
| 6 | 渲染到自己的 Wayland surface | `NativeDisplay::FromWaylandSurface` |
| 7 | 只要音频不要画面 | `config.render.disable_video_output = true` |
| 8 | 抽帧 / 截图 | `TakeSnapshot(at, path)` |
| 9 | 精确 seek 到某一帧 | `SeekTo(t, SeekMode::kAccurate, cb)` |
| 10 | 逐帧步进 | `Pause()` + `StepOnce()` |
| 11 | 变速播放（0.5x / 2x）保持音调 | `SetPlaybackRate` + `config.audio.tempo_stretch` |
| 12 | 切换音轨 / 字幕轨 | `SelectTrack(type, index)` + `media_info().streams` |
| 13 | 监听卡顿与缓冲进度 | `kBufferingStarted/Ended/Progress` |
| 14 | 上报首帧耗时与 QoS | `stats().stages` + `stats().first_frame_latency` |
| 15 | 检测爆音 | `stats().audio_glitches` |
| 16 | 循环播放 | `SetLoopCount(-1)` |
| 17 | 弱网调优 | `config.buffer.*` 三级 HWM + `net.reconnect*` |
| 18 | 直播追帧 | `config.net.live_max_latency` |
| 19 | 强制软解 / 强制硬解 | `config.video.decoder_preference` + 监听 `kDecoderFallback` |
| 20 | 把日志接到自己的日志系统 | `Deps::logging_delegate` |
| 21 | 线上问题排查 | `DumpDiagnostics()` + `ijkpp-inspect dump` |
| 22 | 从 ijkplayer 迁移配置 | `PlayerBuilder::SetOption(cat, key, value)` |

每条格式：**问题描述 → 完整可编译代码 → 注意事项 → 相关配置项**。

---

## 9. 打包与集成

| 方式 | 支持 | 说明 |
|---|---|---|
| **CMake `find_package`** | ✅ 首选 | `find_package(ijkpp 0.1 REQUIRED)` + `target_link_libraries(app PRIVATE ijkpp::ijkpp)` |
| **pkg-config** | ✅ | `pkg-config --cflags --libs ijkpp`（非 CMake 项目） |
| **vcpkg** | ✅ M13 | 提交 portfile 到 vcpkg 官方 registry |
| **conan** | ✅ M13 | `conanfile.py` + conancenter 提交 |
| **系统包**（deb/rpm） | 🔧 提供 `cpack` 配置 | `cpack -G DEB` / `-G RPM` |
| **手工拷贝** | ✅ | `libijkpp.so` + `include/ijkpp/`，`ldd` 只有 libc/libstdc++（dlopen 模式） |
| **源码内嵌**（`add_subdirectory`） | ✅ | 所有 option 默认值在作为子项目时自动关闭 tests/examples/install |
| **单头文件** | ❌ 不提供 | 有 FFmpeg 与 GL 依赖，单头不现实；但公开头只有 ~60 个且分层清晰 |

`cpack` 配置要点：
- `CPACK_DEBIAN_PACKAGE_DEPENDS`：`libc6 (>= 2.31), libstdc++6 (>= 10), libavformat59 | libavformat60 | libavformat61`
- FFmpeg 用**动态链接**打包发行版包（依赖系统 FFmpeg，符合发行版规范）
- 自研分发用**静态链接 FFmpeg + `--exclude-libs,ALL`**（见 [06 §3.2](06-CMake工程与构建体系.md)），零依赖

---

## 10. 自助排障

### 10.1 `DumpDiagnostics()` 一份 JSON 定位 90% 的问题

见 [07 §11.3](07-测试策略与可观测性.md) 的完整样例。SDK 使用者遇到问题的标准动作：

```cpp
LOG(ERROR) << "ijkpp diagnostics:\n" << player.DumpDiagnostics();
```

粘贴这份 JSON 到 issue 里，维护者不需要复现就能定位大部分问题。

### 10.2 `ijkpp-inspect` CLI

```bash
ijkpp-inspect probe   video.mp4                       # 打印 MediaInfo（不播放）
ijkpp-inspect play    video.mp4 --stats 1s            # 播放 + 每秒打印 stats
ijkpp-inspect play    video.mp4 --trace sync,sched    # 打开热路径 trace
ijkpp-inspect play    video.mp4 --backend gl --log debug
ijkpp-inspect dump    --url video.mp4 --duration 30s  # headless 跑 30s 后输出 diagnostics
ijkpp-inspect doctor                                  # ★环境自检
ijkpp-inspect golden record video.mp4 -o out.jsonl
ijkpp-inspect golden diff golden/x.jsonl actual/x.jsonl
```

`doctor` 子命令输出（**集成失败时的第一站**）：

```
ijkpp 0.1.0 doctor
──────────────────────────────────────────────────────────
build            : release, shared, C++20, exceptions=OFF rtti=OFF
ffmpeg           : 7.1 (libavcodec 61.13.100) ............. OK
  decoders       : h264 ✓  hevc ✓  av1 ✓  vp9 ✓  aac ✓  mp3 ✓  opus ✓
  protocols      : file ✓  http ✓  https ✓  hls ✓  rtmp ✗ (librtmp not built)
  hwaccel        : vaapi ✓ (renderD128)   vdpau ✗   nvdec ✗
video backends   :
  sdl2           : libSDL2-2.0.so.0 ...................... OK  (2.30.0)
  gl/x11         : libGL.so.1 + libEGL.so.1 + libX11.so.6  OK  (GL 4.6 Mesa 24.0)
  gl/wayland     : libwayland-client.so.0 ................. MISSING (headless session)
  dmabuf zero-copy: EGL_EXT_image_dma_buf_import_modifiers OK
audio backends   :
  pipewire(pulse): libpulse-simple.so.0 .................. OK  (latency 21 ms)
  alsa           : libasound.so.2 ........................ OK  (default: hw:0,0)
  sdl2           : OK
session          : XDG_SESSION_TYPE=x11  DISPLAY=:0  WAYLAND_DISPLAY=(unset)
display          : 1920x1080 @ 60.00 Hz  scale 1.0
permissions      : /dev/dri/renderD128 rw ✓   audio group ✓
issues           : none

Try: ijkpp-inspect play <file> --backend auto --log debug
```

### 10.3 `docs/TROUBLESHOOTING.md` 结构

| 症状 | 一键诊断 | 常见原因 | 修复 |
|---|---|---|---|
| 黑屏但有声音 | `stats().frames_presented == 0`？`DumpDiagnostics().sink.video` | 没调 `SetVideoSurface` / Surface 已销毁 / GL context 丢失 | Cookbook #5/#6；错误 `SinkNotAttached` |
| 有画面无声音 | `stats().audio.cached_buffers` 增长但 `audio_glitches` 全是 underrun | 音频后端探测失败降级到 Null | `ijkpp-inspect doctor`；`config.audio.backend` |
| 音画不同步 | `stats().av_diff` | 音频延迟未上报（sink 未实现 `hardwareLatency`） | 用官方后端；或检查自定义 sink 的 delay 参数 |
| seek 后卡住几秒 | `DumpDiagnostics().seek.phase` | serial 处理问题 | 报 bug，附 diagnostics |
| 首帧慢 | `stats().stages` | `analyze_duration` 太大 / 网络慢 | `config.demux.*` |
| 播放中崩溃 | `DumpDiagnostics()` 的最后 `stateHistory` | Surface 生命周期 / 回调重入 | Cookbook；检查 E1–E3 铁律 |
| 停止时卡住 | 日志里的 "shutdown did not complete" | `demux.timeout` 太大 | 调小 timeout 或调大 `shutdown_timeout` |
| CPU 占用高 | `stats().video_decode_fps` vs `video_output_fps` | 软解 4K / 无零拷贝 | 开硬解；`prefer_dmabuf_zero_copy` |
| 内存持续增长 | `stats().heap_bytes_estimate` 趋势 | 自定义 sink 持有 `VideoFrame` 不放 | `EXTENDING.md` 的帧生命周期章节 |
| 符号冲突（与自带 FFmpeg 的 App） | `nm -D libijkpp.so \| grep ' T av'` | 用了非隐藏符号的构建 | 用 release preset（含 `--exclude-libs,ALL`） |

---

## 11. API / ABI 稳定性承诺

| 层 | 承诺 |
|---|---|
| `player/public/*.h` | **SemVer**。minor 升级：源码兼容 + 二进制兼容（Pimpl）。major 升级：允许破坏，`CHANGELOG.md` 逐条列出 |
| `media/base/*.h` `base/*.h` | **SemVer**（供 Level 4 用户写扩展）。镜像 Chromium API，Chromium 改了才跟着改 |
| `media/filters/*.h` `platform/*/*.h` | **不承诺**。不安装、不导出 |
| `PlayerConfig` 字段 | minor 只增不改不删；新增字段必须有默认值且放在结构体尾部 |
| `EventType` / `PlayerEvent` | 只增不减；用户的 `switch` 必须有 `default`（文档明确要求，且编译期 `-Wswitch` 提示新增枚举） |
| `ErrorCode` | 只增不减 |
| CMake target 名 `ijkpp::*` | 稳定 |

**破坏性变更流程**：先加 deprecated 版本（`IJKPP_DEPRECATED("use SeekTo(TimeDelta, SeekMode, SeekCB)")`）保留 2 个 minor 版本，再删。

---

## 12. SDK 易用性验收清单（进 CI / 评审）

发布 1.0 前必须全绿：

```
[  ] V1  新人冷启动 ≤ 15 分钟（找 3 个未参与项目的人实测，记录用时与卡点）
[  ] V2  README 每个代码块由 CI 编译（examples/doc_snippets/）
[  ] V3  最小集成只需 FFmpeg；ldd libijkpp.so 无 SDL2/GL/X11/ALSA（dlopen 模式）
[  ] V4  CMake / pkg-config / 手工拷贝 三种集成方式各有 CI job 验证
[  ] V5  每个 ErrorCode 都有 summary + detail + suggestion，单测断言三段非空
[  ] V6  player/public/*.h 中 grep 'AV[A-Z]' 结果为空
[  ] V7  examples 中 grep 'std::thread' 结果为空
[  ] V8  COOKBOOK.md 有 ≥ 20 条，每条代码可编译
[  ] V9  player/public/*.h 的 public 方法注释覆盖率 ≥ 95%（含线程/阻塞/错误/副作用四项）
[  ] V10 API diff 工具（abi-compliance-checker）在 minor 版本间无破坏
[  ] examples/play_sdl2/main.cc ≤ 300 行
[  ] examples/play_embed/main.cc ≤ 350 行
[  ] ijkpp-inspect doctor 在干净 Ubuntu 24.04 容器里输出可读的诊断
[  ] TROUBLESHOOTING.md 覆盖 ≥ 10 个常见症状
[  ] MIGRATION.md 覆盖 05 文档的全部 Δ 项
[  ] API.md（Doxygen）无 warning 生成
[  ] CHANGELOG.md 每条变更关联 commit 与 Δ 编号
```

---

返回：[README](../README.md)
