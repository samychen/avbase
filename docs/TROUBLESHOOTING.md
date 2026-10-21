# TROUBLESHOOTING · 常见症状与排查

> 每条症状的修复手段都来自本仓真实踩过的坑（轮次记录见 git log）或真实
> 代码路径（2026-10-18，M13 第八波整理）。排查三件套：
> `player.DumpDiagnostics()`（运行中快照）、`avbase-inspect probe <url>`
> （离线容器诊断）、`kError` 事件的三段式错误
> （summary / detail / suggestion，[V5](10-SDK易用性设计.md)）。
>
> **先看错误本身**：avbase 的每个 `MediaError` 都带 suggestion，别跳过它
> 直接猜。

---

## T1. `PrepareAsync` 后长时间不进 `kPrepared`（起播慢）

**现象**：直播源 3–10 秒才出画；VOD 也明显慢于 ffprobe。
**原因**：默认 `demux.analyze_duration = 5s`、`probe_size = 5 MiB`——
探测要读满额度才判定流。ijkplayer 同款默认，但它的宿主多数本地文件。
**修复**：

```cpp
config.demux.analyze_duration = avbase::base::Milliseconds(100);  // 直播
config.demux.probe_size = 1024 * 1024;                            // 1 MiB 够探
```

**诊断**：`avbase-inspect probe <url>` 会打印各阶段耗时；`stats.stages`
里 `open_input_to_stream_info` 段即探测耗时。

## T2. 播放中频繁 `kBufferingStarted`（卡顿）

**现象**：进度走走停停，`buffering_count` 持续上涨。
**原因**：带宽不足或水位设太高。三级水位
`first/next/last_high_water_mark`（默认 100ms/1s/5s）是「缓冲多久才认为
够播」，最后一级 5s 意味着每次缓冲要攒 5 秒内容。
**修复**：

```cpp
config.buffer.last_high_water_mark = avbase::base::Seconds(2);
config.buffer.max_bytes = 30 * 1024 * 1024;   // 全播放器预算，按流拆分
config.net.reconnect_max_retries = 8;
```

**诊断**：`stats.buffering.total_buffering_time` 累计卡顿时长；
`current_hwm_step` 当前在第几档。

## T3. 声音爆音、咔哒声

**现象**：周期性杂音，尤其在变速或 seek 之后。
**原因**：音频队列下溢（underrun）——解码供不上消费；或设备层 xrun。
**诊断**：

```cpp
const avbase::PlaybackStats st = player.GetPlaybackStats();
// st.audio_glitches.total_glitches     播放器侧下溢计数
// st.audio_glitches.total_glitch_duration
// st.audio_glitches.xruns              设备层 xrun（ALSA EPIPE 等）
```

**修复**：`config.audio.frame_queue_size` 调大（默认 9，深了爆音少、
延迟高）；确认 `audio.tempo_stretch` 在变速时开启（否则重采样会引入
可闻失真）。

## T4. 音视频不同步，且随时间漂移

**诊断**：

```cpp
const avbase::PlaybackStats st = player.GetPlaybackStats();
st.av_delay;  // 当前音画延迟
st.av_diff;   // 相对基准的差
```

**原因与修复**：
- 漂移单调增大 → `buffer.sync_av_start` 语义或时钟基准问题，先确认不是
  变速播放（`playback_rate != 1` 时以音频时钟为准是设计行为）。
- 突变一次后恢复 → seek 之后的 `flush_on_seek` 行为，属正常。
- 直播源 → 可能是 `net.live_max_latency` 追帧造成，设 0 观察是否消失。

## T5. seek 之后没有回调，像卡死

**现象**：`SeekTo` 三参版迟迟不回调，`StopSync` 超时。
**历史事实**：EOF 之后到达的 seek 曾经永久挂起——demux 循环在
`AVERROR_EOF` 处结束，而 seek 请求只被发布无人消费（已修：
`StartPlayingFrom` 现在负责重挂循环，`AVERROR_EXIT` 不再当致命错）。
**现在的排查**：若仍复现，`sample <pid> 1`（macOS）抓栈——**两个线程都
空闲在 `TaskQueue::Run()` 是「没有人做这件事」，不是死锁**；这个判断法
省过一个错误的锁序分析（docs/TODO §4 记着这次误判）。

## T6. 硬解不生效，或悄悄回落到软解

**诊断**：监听 `kDecoderFallback` 事件；`stats.video_decoder` 显示实际
在用的解码器类型。
**原因与修复**：

```cpp
config.video.decoder_preference = avbase::DecoderPreference::kHardwareFirst;
// kAuto：硬解优先，静默回落（默认）
// kHardwareFirst：回落时告警
// kSoftware：永不硬解
// kHardwareOnly：失败即报错，不回落
config.video.hw_codecs = static_cast<avbase::HwCodecMask>(
    avbase::HwCodecFlag::kHevc);   // 只放行 HEVC 走硬解
```

容器/codec 不在放行掩码、或平台无对应后端时回落；`hw_decoder_name` 可指名
具体实现。

## T7. 有声音没画面（或反之）

**逐项排除**：
1. `config.video.disabled`（"vn"）为 true → **故意**不解码视频，检查配置。
2. `config.render.disable_video_output`（"nodisp"）为 true → 仍解码但不上屏，
   presented 计 0、全部计入 dropped——这是该模式的**正确**统计，不是 bug。
3. `SetVideoSurface(display)` 从未调用或传了无效 display → 检查
   `NativeDisplay` 工厂返回值与调用时序（Prepare 前设置最稳）。
4. `--gl` 路径下 GL 运行时坏 → 示例会自动回落 SDL_Renderer 并打印
   "GL runtime broken on this host"（examples/sdl2_demo.h 的探测逻辑）。
5. 窗口建好了但尺寸 0 → 等 `kVideoSizeChanged` 再布局，别用初始窗口尺寸。

## T8. 字幕不显示

**逐项排除**：
1. **字体**：示例用 `FindDemoFont()` 找三个系统路径，都没有就静默不渲染。
   宿主必须指用户的真实字体。
2. **轨道没选中**：avbase 不自动选字幕轨（示例里 play_sdl2 自己在
   kPrepared 时选第一条）。用 `SelectTrack(kText, index)`。
3. **直播字幕过期**：cue 超过 media 时钟的 end 即被丢弃（设计如此），
   1 Hz 的过期检查有至多 1 秒粗粒度。
4. **分辨率**：`kTimedText` 事件的 `w/h` 非零时按视频像素比例摆放，
   否则底部居中。

## T9. 内存持续上涨

**诊断**：`stats.video.cached_bytes` / `stats.audio.cached_bytes` 与
`stats.heap_bytes_estimate`。
**口径**（这里错过两次，值得记）：
- `buffer.max_bytes`（"max-buffer-size"）是**整个播放器**的字节预算，
  ijk `MAX_QUEUE_SIZE` 同义；按流数拆分给 audio/video/text 各队列。
  直接当每流额度会让 A/V 文件默认内存翻倍（15 → 30 MiB）。
- **包数上限 256 不参与拆分**——每条队列各自全额。
- `buffer.unlimited`（"infbuf"）只解除字节上限，包数仍封顶。
**修复**：按观测的 `cached_bytes` 设 `max_bytes`；持续上涨先看是不是
每起播一次就新建 Player 而不 Stop（析构前的 StopSync 契约见 T10）。

## T10. 退出时卡死或崩溃

**事实**：`Stop()` 是异步的；对着还在 flush 的管线拆窗口/销毁 display
会竞态。**用 `StopSync(timeout)`**（有界、幂等），见 examples/play_sdl2.cc
的 L6 注释与 docs/04 §5.4（Δ15）。崩溃多在 `~PipelineImpl` 的 CHECK——
第二十三/二十四轮的关闭竞态，修法是等 `!IsRunning()` 而非等队列排空。

## T11. `SetOption` 报「unknown key」

**这是故意的**：注册表内 9 个 key（`max-fps` / `framedrop` /
`start-on-prepared` / `packet-buffering` / `max-buffer-size` / `infbuf` /
`min-frames` / `nodisp` / `enable-accurate-seek`）全部有真实读取者；
其余 ijk key **未接线**，报错并带 "did you mean" 建议，而不是静默接受
一个什么都不做的选项。要的行为要么改配置，要么等该 key 接线。
ijk 风格的布尔写法（`1`/`0`）同样被接受。

## T12. 链接报找不到符号 / 多 definition

1. 确认链接的是 `avbase::avbase`（总门面）而不是单模块 target。
2. dlopen 模式下 `libavbase.so` 只硬链 FFmpeg；SDL2/GL/X11/ALSA 由后端
   在运行期加载（V3）——**别把 `-lSDL2` 加进最终产品链接**，那是把可选
   后端变刚需。
3. `player/public/` 头全部可独立 include；若你的构建报缺
   `AVBASE_*_EXPORT`，说明包含了内部路径（如 `media/base/*.h`）——
   公开面只有 `player/public/` 与 `avbase_c.h`。

## T13. 日志太多 / 看不到日志

```cpp
// 全局级别
avbase::base::logging::SetMinLogLevel(avbase::base::logging::SEVERITY_WARNING);
// 或整个接管（Deps::logging_delegate，COOKBOOK 第 20 条）
auto deps = std::make_unique<avbase::Deps>();
deps->logging_delegate = std::make_unique<MyLog>();
```

media 层的阶段日志（`open-input` / `find-stream-info` 等）也走同一出口；
`DumpDiagnostics()` 不受日志级别影响，随时可取。

---

## 症状速查表

| 症状 | 第一诊断 | 条目 |
|---|---|---|
| 起播慢 | `stats.stages.open_input_to_stream_info` | T1 |
| 卡顿 | `stats.buffering.total_buffering_time` | T2 |
| 爆音 | `stats.audio_glitches.total_glitches` | T3 |
| 音画差 | `stats.av_delay` / `av_diff` | T4 |
| seek 无回调 | `sample <pid> 1` 看两线程是否都空闲 | T5 |
| 硬解没生效 | `kDecoderFallback` 事件 | T6 |
| 无画面 | `nodisp` vs `vn` vs surface 时序 | T7 |
| 无字幕 | 字体 / 选轨 / cue 过期 | T8 |
| 内存涨 | `stats.video.cached_bytes` | T9 |
| 退出竞态 | 换 `StopSync` | T10 |
| SetOption 报错 | 9 key 白名单 | T11 |
| 链接符号 | 只链 `avbase::avbase`；dlopen 模式别链 `-lSDL2` | T12 |
| 日志 | `SetMinLogLevel` / `logging_delegate` | T13 |

返回：[README](README.md) ｜ 配方见 [COOKBOOK](COOKBOOK.md) ｜ 错误约定见
[10 §4](10-SDK易用性设计.md)
