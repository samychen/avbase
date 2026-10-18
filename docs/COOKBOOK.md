# COOKBOOK · 常见任务配方

> 每条配方都是**可编译的真实代码**：签名逐个对照 `player/public/` 头文件核过
> （2026-10-18，M13 第八波）。目录引自 [10 §8](10-SDK易用性设计.md)，其中两条
> 与现实不符的已按代码改正并在文末「与设计目录的差异」登记。
>
> 每条格式：**场景 → 代码 → 注意 → 相关配置**。代码省略了 `#include
> "player/public/player.h"` 与 `main()` 的外壳，默认都在 `int main()` 里、
> `using namespace avbase;` 之下。

**通用骨架**（后面各条只写差异部分）：

```cpp
avbase::PlayerConfig config;
auto deps = std::make_unique<avbase::Deps>();
avbase::Player player(config, std::move(deps));
player.SetEventHandler(avbase::base::BindRepeating(
    [](const avbase::PlayerEvent& e) {
      if (e.type == avbase::EventType::kError) {
        if (const auto* p = avbase::AsError(e)) {
          std::fprintf(stderr, "error: %s\n", p->error.ToString().c_str());
        }
      }
    }));
if (const avbase::Status s = player.SetDataSource(url); !s) { /* 三段式错误 */ }
if (const avbase::Status s = player.PrepareAsync(); !s) { /* ... */ }
```

> `Status` 是 `base::expected<void, MediaError>`：`!s` 即失败，`s.error()` 取
> 三段式详情（summary / detail / suggestion，见 [V5](10-SDK易用性设计.md)）。
> 析构前用 `StopSync(timeout)` 而不是 `Stop()`：后者异步，拆窗口会竞态
> （examples/play_sdl2.cc 的 L6 注释有完整说明）。

---

## 1. 播放一个本地文件

```cpp
if (const avbase::Status s = player.SetDataSource("/path/video.mp4"); !s) {
  return 1;
}
player.PrepareAsync();   // kPrepared 事件到达即可 Start()
```

**注意**：`PrepareSync(base::Seconds(30))` 是阻塞版，`start_on_prepared`
默认 true——`PrepareSync` 返回时通常已经在播，再调 `Start()` 会收到
"Start in state Started is not legal"（examples/pull_frames.cc 因此加了
状态守卫）。事件驱动场景用 `PrepareAsync` + `kPrepared`。
**相关配置**：`demux.probe_size` / `demux.analyze_duration`。

## 2. 播放网络流（HLS/RTMP/RTSP/SRT）

```cpp
avbase::PlayerConfig config;
config.net.user_agent = "my-host/1.0";
config.net.headers = {{"Referer", "https://example.com/"}};
config.net.reconnect = true;
config.net.reconnect_max_retries = 5;
config.net.reconnect_delay = avbase::base::Milliseconds(200);
// 之后用同一 config 构造 player，SetDataSource("https://.../index.m3u8")
```

**注意**：协议由 URL scheme 选择（`ffmpeg` 层）。直播流默认不 chase 边缘，
见第 18 条。
**相关配置**：`demux.timeout`（单次 IO 超时）、`demux.analyze_duration`
（直播设小，如 `base::Milliseconds(100)`，否则起播慢）。

## 3. 从内存播放（已下载的 buffer）

```cpp
// data 需在播放期间有效：MemoryDataSource 不拷贝（与 libFuzzer 的
// fuzz_demuxer.cc 同一约定）。
const avbase::Status s = player.SetDataSource(
    avbase::DataSourceDescriptor::FromMemory(bytes.data(), bytes.size()));
```

**注意**：`FromMemory` 收 `(const uint8_t*, size_t)`，不是 span。
**相关配置**：同第 1 条。

## 4. 从自定义下载器播放

实现 `media::DataSource`（`media/base/data_source.h`，纯虚 8 个）并挂进 Deps：

```cpp
class MySource : public avbase::media::DataSource {
 public:
  void SetHost(Host* host) override { host_ = host; }
  void Read(int64_t offset, size_t size, uint8_t* data,
            avbase::base::scoped_refptr<avbase::base::TaskRunner> runner,
            ReadCB read_cb) override {
    // 异步读：read_cb 在 runner 上跑，绝不在线内联；0 == EOF
    runner->PostTask(FROM_HERE,
                     avbase::base::BindOnce(std::move(read_cb),
                                            ReadResult(0)));
  }
  ReadResult ReadBlocking(int64_t offset, size_t size,
                          uint8_t* data) override {
    return 0;  // demux 线程的阻塞版；0 = EOF
  }
  void Abort() override {}                      // 必须能唤醒阻塞读（有界 Stop）
  bool GetSize(int64_t* size_out) override { *size_out = 0; return true; }
  bool IsStreaming() const override { return false; }
  void SetBitrate(int bitrate) override { bitrate_ = bitrate; }
  bool IsSeekable() const override { return true; }

 private:
  Host* host_{nullptr};
  int bitrate_{0};
};

auto deps = std::make_unique<avbase::Deps>();
deps->data_source = avbase::base::MakeRefCounted<MySource>();
```

**注意**：`Deps` 持有一个实例而非工厂（一个 Player 一个源）。多源用
`Player::AddSource()`（第 6 条，docs/14）。`ReadResult` 是
`base::expected<int, MediaError>`——字节数或错误，**不是 bool**。
`Abort()` 必须能唤醒阻塞的读，这是 `Stop()` 有界的成因（docs/04 §5.4）。
**相关配置**：`demux.timeout`、`demux.forced_format`。

## 5. 渲染到自己的窗口

avbase 不建窗口，宿主建窗并把 handle 包成 `NativeDisplay`：

```cpp
// SDL2：
auto display = avbase::media::NativeDisplay::FromSdl2Window(sdl_window);
// GLFW：
auto display = avbase::media::NativeDisplay::FromGlfwWindow(glfw_window);
// EGL surface（Linux）：
auto display = avbase::media::NativeDisplay::FromEglSurface(egl_surface);
// DRM master fd（Linux 直出）：
auto display = avbase::media::NativeDisplay::FromDrmMaster(drm_fd, crtc_id);

player.SetVideoSurface(display);   // 运行期可换；nullptr 即停止上屏
```

**注意**：四种工厂就是全部——没有 X11/Wayland 专用工厂（设计目录里的
`FromX11Window`/`FromWaylandSurface` 未落地，见文末差异登记）；X11/Wayland
宿主走 SDL2/GLFW 后端或 DRM。`SetVideoSurface` 可重复调用换目标，
`nodisp`（第 7 条）是配置层决定，压得住晚到的 surface。
**相关配置**：`render.linux_backend`、`render.prefer_dmabuf_zero_copy`。

## 6. 画中画 / 多图层合成

```cpp
// 第二个源进同一条管线（docs/14 stage 3 之后没有第二个 Player）：
auto scene_source = player.AddSource(
    avbase::DataSourceDescriptor::FromUri(second_url), /*layer=*/1);
// 之后 scene_source->Publish(frame) 把自己的帧送进 SceneSource；
// 完整可运行形态见 examples/play_sdl2.cc --scene pip|fade 与
// examples/scene_demo.h。
```

**注意**：`AddSource` 必须在 `SetDataSource`/`PrepareAsync` **之前**调，
之后注册会被拒。所有源共用一个 pipeline 与一个时钟——这正是它与
「起第二个 Player」的区别。
**相关配置**：`render.gl_context_sharing`。

## 7. 只要音频不要画面

```cpp
avbase::PlayerConfig config;
config.render.disable_video_output = true;   // "nodisp"
```

**注意**：与 `video.disabled`（"vn"）不同——后者**不解码**视频，
`nodisp` 仍完整解码、维持 av sync 与统计，只是不上屏；presented 帧计 0、
全部计入 dropped。想运行期停止上屏用 `SetVideoSurface(nullptr)`。
**相关配置**：`video.disabled`、`render.disable_video_output`。

## 8. 抽帧 / 截图

```cpp
if (const avbase::Status s =
        player.TakeSnapshot(avbase::base::Seconds(3), "/tmp/frame.png");
    !s) { /* 失败也带三段式 */ }
// 完成事件：kSnapshotCompleted（携带 path 与 MediaError）
```

**注意**：`at` 是媒体时间轴上的位置；文件格式由扩展名决定。播放中调用，
内部走一次精确 seek + 解码一帧。
**相关配置**：`seek.accurate`（否决位，见第 9 条）。

## 9. 精确 seek 到某一帧

```cpp
// 返回值是请求 id（用于把结果匹配回请求），不是帧序号：
const avbase::Result<int64_t> id =
    player.SeekTo(avbase::base::Seconds(90), avbase::SeekMode::kAccurate,
                  avbase::base::BindOnce([](avbase::Status s) {
                    if (!s) {
                      // s.error() 三段式：summary / detail / suggestion
                    }
                  }));
// 落点信息走事件：kAccurateSeekCompleted 的 requested / actual
```

**注意**：`SeekTo(position)` 是即发即忘的关键帧版；三参版才是精确版，
回调必达且在事件序列上跑（从不在线内联）——EOF 之后的 seek 也会被应答
并重启 demux（docs/04 §2.1「EOF 与 seek 的握手」）。`config.seek.accurate`
（"enable-accurate-seek"）是**全局否决位**：false 时三参版不报错，落关键帧，
`requested`/`actual` 的差就是你看得出的降级。
**相关配置**：`seek.accurate`、`seek.accurate_timeout`、`seek.flush_on_seek`。

## 10. 逐帧步进

```cpp
player.Pause();
player.StepOnce();   // 每次一帧；仅暂停态合法
```

**注意**：`StepOnce` 期间时钟不走，音视频仍按各自泵推进。
**相关配置**：无。

## 11. 变速播放保持音调

```cpp
player.SetPlaybackRate(2.0);   // [0.25, 4.0]
avbase::PlayerConfig config;
config.audio.tempo_stretch = true;    // 变速时拉伸音频而非重采样
config.audio.preserves_pitch = true;  // 保音调（WSOLA 变基）
```

**注意**：`tempo_stretch` 关掉时变速会变调（重采样语义）。速率影响
媒体时钟，`GetMediaTime()` 仍报媒体时间。
**相关配置**：`audio.tempo_stretch`、`audio.preserves_pitch`。

## 12. 切换音轨 / 字幕轨

```cpp
if (const auto info = player.media_info()) {
  for (const auto& s : info->streams) {
    // s.kind == avbase::media::StreamKind::kAudio / kVideo / kText
    // s.index / s.codec_name / s.language / s.title / s.duration
  }
}
player.SelectTrack(avbase::media::DemuxerStreamType::kAudio, /*index=*/1);
// 生效事件：kTrackChanged
```

**注意**：`media_info()` 返回 `std::optional<MediaInfo>`，prepare 前为
`nullopt`。`SelectTrack` 传 `DemuxerStreamType` + `StreamInfo::index`
（不是「第几个音频轨」的位置序）。自动选中第一条字幕轨的范式见
examples/play_sdl2.cc 的 kStateChanged 分支。
**相关配置**：`audio.selected_stream`、`video.selected_stream`。

## 13. 监听卡顿与缓冲进度

```cpp
player.SetEventHandler([](const avbase::PlayerEvent& e) {
  switch (e.type) {
    case avbase::EventType::kBufferingStarted: /* 进入 kBuffering */ break;
    case avbase::EventType::kBufferingEnded:   /* 恢复播放 */ break;
    case avbase::EventType::kBufferingProgress: /* 进度 */ break;
    case avbase::EventType::kWaiting:           /* 等数据/等解码 */ break;
    default: break;
  }
});
```

**注意**：三级水位由 `buffer.first/next/last_high_water_mark` 推导，
`kBufferingProgress` 的百分比按当前档 mark 计算。
**相关配置**：见第 17 条。

## 14. 上报首帧耗时与 QoS

```cpp
const avbase::PlaybackStats st = player.GetPlaybackStats();
// 分段计时（prepare → 上屏）：
avbase::base::TimeDelta t = st.stages.total_first_frame;
// 或单独看某段：st.stages.open_input_to_stream_info 等
// 首帧延迟（另一口径，含起播决策）：
avbase::base::TimeDelta lat = st.first_frame_latency;
// JSON 一键上报：
std::string json = st.ToJson();
```

**注意**：`stages` 六段计时与 `first_frame_latency` 是两个口径（前者是
分段和，后者含起播门控），别混。`ToJson()` 适合直接进日志系统。
**相关配置**：`buffer.first_high_water_mark` 直接影响首帧 latency。

## 15. 检测爆音

```cpp
const avbase::PlaybackStats st = player.GetPlaybackStats();
const avbase::media::AudioGlitchInfo& g = st.audio_glitches;
// g 内含 underrun/glitch 计数（音频 sink 直供，1 Hz 随 kStats 更新）
```

**注意**：爆音 = underrun 数字，不是「听起来有啪声」的主观判断；
把它做成告警比让用户报障便宜。
**相关配置**：`audio.frame_queue_size`（队列深了爆音少、延迟高）。

## 16. 循环播放

```cpp
player.SetLoopCount(-1);   // 无限循环；0 = 不循环；n = n 次
```

**注意**：循环边界会重新走 seek 握手（EOF → `StartPlayingFrom` 重挂），
与第 9 条同一机制。
**相关配置**：无。

## 17. 弱网调优

```cpp
avbase::PlayerConfig config;
config.buffer.enabled = true;                 // 总开关（"packet-buffering"）
config.buffer.max_bytes = 30 * 1024 * 1024;   // 全播放器字节预算（按流拆分）
config.buffer.first_high_water_mark = avbase::base::Milliseconds(100);
config.buffer.next_high_water_mark = avbase::base::Seconds(1);
config.buffer.last_high_water_mark = avbase::base::Seconds(5);
config.buffer.unlimited = false;              // true = "infbuf"：不设字节上限
config.net.reconnect_max_retries = 8;
```

**注意**：`max_bytes` 是**整个播放器**的预算（ijk `MAX_QUEUE_SIZE` 同义），
由 `SplitQueueBudget()` 按流数拆分——直接当每流额度会让 A/V 文件默认内存
翻倍（docs/TODO §2 记着这件事）。`min_frames` 是预缓冲门控的包数下限，
按**全管线包数总和**判定（docs/12 §3 附近的 PipelineImpl 注释）。
**相关配置**：`buffer.*` 全部、`net.reconnect*`。

## 18. 直播追帧

```cpp
config.net.live_max_latency = avbase::base::Seconds(3);  // 0 = 不追边缘
```

**注意**：非零时播放器会主动 seek 到「当前边缘减 latency」，代价是
可能的轻微回跳。HLS/RTMP/RTSP/SRT 实测起播到稳定均约 3s（docs/TODO §3），
追帧不改变协议本身的最小延迟。
**相关配置**：`net.live_max_latency`、`buffer.sync_av_start`。

## 19. 强制软解 / 强制硬解

```cpp
avbase::PlayerConfig config;
config.video.decoder_preference = avbase::DecoderPreference::kSoftware;
// kAuto（默认，硬解优先回落）/ kHardwareFirst（回落时告警）/
// kHardwareOnly（失败即报错，不回落）
config.video.hw_codecs = static_cast<avbase::HwCodecMask>(
    avbase::HwCodecFlag::kHevc);   // 只允许 HEVC 走硬解
// 监听回落：kDecoderFallback 事件（携带原因）
```

**注意**：`hw_codecs` 是位掩码；`kAll` 为默认。回落不是错误，但
`kHardwareOnly` 场景下它会变成错误——用 `kDecoderFallback` 事件
区分「静默回落」与「按配置失败」。
**相关配置**：`video.decoder_preference`、`video.hw_codecs`、
`video.hw_decoder_name`。

## 20. 把日志接到自己的日志系统

```cpp
class MyLog : public avbase::base::logging::LoggingDelegate {
 public:
  void OnLogMessage(avbase::base::logging::LogSeverity severity,
                    const char* file, int line,
                    std::string_view message) override {
    my_log(severity, file, line, message);   // 你的落点
  }
};

auto deps = std::make_unique<avbase::Deps>();
deps->logging_delegate = std::make_unique<MyLog>();
```

**注意**：`OnLogMessage` 会在多个线程被调，实现需线程安全；不要在回调里
再打 avbase 日志。`base::logging::SetMinLogLevel()` 可全局调级别。
**相关配置**：无。

## 21. 线上问题排查

```cpp
std::string dump = player.DumpDiagnostics();   // 播放中随时可取
// 命令行离线诊断（不需要写宿主）：
//   avbase-inspect probe  <url>    容器/流信息
//   avbase-inspect decode <url>    解码链路 smoke
//   avbase-inspect dump   <url>    全量诊断
```

**注意**：`DumpDiagnostics()` 含各队列水位、线程状态与统计快照，是
「用户说什么都卡」类工单的第一手材料；先 dump 再猜。
**相关配置**：无。

## 22. 从 ijkplayer 迁移配置

```cpp
avbase::PlayerBuilder builder;   // SetOption 在 builder 上，不在 Player 上
builder.SetConfig(config);
// 四个 category：kPlayer / kFormat / kCodec / kScaler
builder.SetOption(avbase::OptionCategory::kFormat, "analyzeduration",
                  int64_t{500});
builder.SetOption(avbase::OptionCategory::kPlayer, "max-buffer-size",
                  int64_t{30 * 1024 * 1024});
builder.SetOption(avbase::OptionCategory::kPlayer, "nodisp", true);
auto built = builder.Build();    // Result<std::unique_ptr<Player>>
if (!built) {
  return 1;  // 未知 key 在此报错（带 "did you mean" 建议，Δ2）
}
std::unique_ptr<avbase::Player> player = std::move(*built);
```

**注意**：注册表内 9 个 key 全部有真实读取者（docs/TODO §2 列表），
不接受的 key 会**报错**而不是静默忽略——这是有意的：静默 no-op 选项
比没有选项更贵。`OptionValue` 是 `variant<bool, int64_t, double, string>`。
**相关配置**：即 `PlayerConfig` 各字段，见 docs/05 的逐项对照表。

---

## 与设计目录（docs/10 §8）的差异登记

| 目录条目 | 实际 | 处置 |
|---|---|---|
| 5/6. `NativeDisplay::FromX11Window` / `FromWaylandSurface` | 不存在；实际工厂为 `FromSdl2Window` / `FromGlfwWindow` / `FromEglSurface` / `FromDrmMaster` | 第 5 条按实际工厂改写 |
| 4. `Deps::data_source_factory`（工厂） | `Deps::data_source`（单实例）；`DataSource::Read` 为异步回调式（`ReadResult` = 字节数或错误） | 第 4 条按实际改写 |
| 3. `FromMemory(span)` | `FromMemory(const uint8_t*, size_t)` | 第 3 条按实际改写 |
| 9. 回调携带「落点帧序号」 | 回调只带 `Status`；`Result<int64_t>` 返回的是**请求 id**，落点看事件的 `requested`/`actual` | 第 9 条按实际改写 |
| 22. `PlayerBuilder::SetOption` | 就在 `PlayerBuilder` 上——**目录原本是对的**，M13 第一版曾据行号臆断为 `Player::SetOption`，被 doc_snippets 编译纠正 | 第 22 条恢复目录原样；此错登记为教训 |

> 登记而非悄悄改掉：设计目录是「意图」，本文件是「现状」，两者的差就是
> 待办。V8 要求每条可编译——本文件代码块由 `examples/doc_snippets/`
> 逐条编译（已建目标 `doc_snippets`，编译并运行通过；剩余条目的 snippet
> 与 CI 接入见 M13 第九波）。

返回：[README](README.md) ｜ 相关：[10 SDK 易用性设计](10-SDK易用性设计.md) ·
[11 行为规范卷](11-行为规范卷.md) · [TROUBLESHOOTING](TROUBLESHOOTING.md)
