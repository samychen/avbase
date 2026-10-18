# PERFORMANCE · 性能指南

> 三件事：**怎么测**（方法论与纪律）、**预算与门槛**（什么算回退）、
> **怎么调**（旋钮与口径）。文中每个数字都标了出处；凡是 0.1.0 没有的东西
> （微基准、golden 对拍、48h soak）如实写在 §6，不拿计划当事实。

---

## 1. 测量纪律（先读这节，再谈数字）

1. **先证明被测二进制是新的**：
   `python3 tools/check_binary_freshness.py --root build/<preset>`（exit 0/1/2）。
   第八十五轮拿 10/15 的旧二进制诊断出过「VideoToolbox 硬解卡死」——重链后
   3/3 正常，**那个 bug 不存在**。ninja 不跟踪静态库内容，`cmake --build`
   对部分目标会报成功却留旧可执行文件；判新鲜度读 `.ninja_log`，不是 `stat`。
2. **同一素材、同一机器、同一负载**。`tools/bench_render.sh` 的注释记着三条
   踩过的坑：短片段会把启动窗口平均进结果（60fps 被测成 53）——所以每段
   12s、只取**稳态尾部的中位数**；负载下的机器移动同样的数字——回退怀疑先
   看负载消了数字是否回来；`max` 是进程启动以来的累计值，只能读作「有没有
    Stall 过」，不是尾延迟。
3. **本机干净 ≠ CI 干净**。沙箱看不到 CI；本地全套用
   `tools/gate_local.sh` 复刻（inv/fmt/lint/三预设 ctest + 可选
   sanitizer/coverage）。

## 2. 现成的测量工具（0.1.0 实况）

| 工具 | 测什么 | 依赖 |
|---|---|---|
| `tools/bench_render.sh [build] [秒/段] [canvas WxH]` | GL 渲染路径端到端：upload / compose / blit / present 四段 + 输出帧率 + 上传路径 | `play_glfw` 构建 + 显示屏 + ffmpeg（造素材） |
| `SCENE=pip\|fade SECOND=<url> tools/bench_render.sh` | 多源/多图层路径（第二层可喂**真第二解码器**，多一次解码不止多一次上传） | 同上 |
| `tools/check_weaknet.py` / `.sh` | 弱网/限速/断网下的缓冲与恢复行为 | 本地回放 |
| `tools/run_soak.py` | 长稳：内存曲线斜率门禁 | **48h 未跑**（沙箱禁读他进程 RSS，需无沙箱/CI 长跑机） |
| `tools/check_binary_freshness.py` | 被测物是否比输入新（测量前置门禁） | — |

`bench_render.sh` 的输出读法（脚本注释即文档）：`present` 贴近刷新周期是
正常的（它在等 vsync），不是问题；四段里 `upload` 在 4K 开始不免费；
传 `--canvas` 走缩放路径，`blit` 从此包含重采样。

**0.1.0 没有的**：Google Benchmark 微基准（docs/04 §8 底部那份 `BM_*` 清单
是计划，仓库里没有 benchmark target）、golden 对拍（M10 的「15 份 golden
JSONL」未落成，回归靠单测 + 素材端到端 + sanitizer + corpus）。

## 3. 性能预算与回归门槛

基准负载：**1080p30 + 48kHz stereo**（帧间隔 33.3ms）。表摘自
[docs/04 §8](04-线程模型与数据流.md)：

| 路径 | 预算 |
|---|---|
| `DecoderBufferQueue::Push/Pop` | < 5 µs |
| `VideoFrameQueue::Reserve/Commit` | < 5 µs |
| `VideoFrameCompositor::DecideNextFrame()` | **< 1 µs**（纯函数：无分配、无锁、无日志） |
| `VideoFrameCompositor::Render()`（含选帧） | < 20 µs（持 `lock_` < 5 µs） |
| `AvSyncController::GetMasterClock()` | **< 200 ns**（seqlock 读，每帧 2–4 次） |
| `AudioRendererSink::RenderCallback::Render()` | **< 100 µs**（只 `FillBuffer` + `Scale`，Δ13） |
| `AudioRendererAlgorithm::FillBuffer()`（含 WSOLA） | < 60 µs / 1024 帧（rate==1.0 走纯 memcpy 快路） |
| `EventHub::Post()` | < 3 µs |
| `PostTask` | < 2 µs |
| **相对 FFmpeg 裸调的总额外开销** | **< 3% CPU** |

CI 门槛（docs/08 R3）：**微基准回退 > 10% 或端到端 CPU 回退 > 5% 即 fail**。
微基准这一半现在有机器判据了：`bench/` 自包含 harness 跑出数字，
`tools/check_bench_regression.py` 与入库基线 `tools/bench-baseline.jsonl`
对比，`gate_local --jobs bench` 本地硬 fail（开发机安静，结果可行动）。
CI 的 `bench` job **不硬 fail**，只记录并上传 artifact：共享 runner 噪声大，
10% 门槛会假红；而且入库基线测于 mac、CI 跑在 linux，跨平台绝对值本就不可
比（见 §6「跨机器数字不可比」）。端到端 CPU 回退那 5% 仍无机器判据，仍靠
`bench_render.sh` 人工。

## 4. 调优旋钮（按代价从高到低）

### 4.1 内存预算——先搞清口径再调

`max-buffer-size`（`buffer.max_bytes`，默认 **15 MiB**）是**全播放器预算**，
不是每条流的额度：ijkplayer/avbase 比的是 audio+video+subtitle **三队列之和**。
按流拆分走 `SplitQueueBudget()`（`media/base/decoder_buffer_queue.h:57`）：

```cpp
if (per_stream.max_bytes > 0 && stream_count > 1) {
  const size_t share = limits.max_bytes / stream_count;
  per_stream.max_bytes = share > 0 ? share : 1;
}
```

所以 A+V 文件默认每队列 7.5 MiB。**不要**按「每流 15 MiB」心算——第九十四轮
修过这个口径：此前按每流全额传递，A/V 文件默认内存包络从 15 MiB 静默翻倍到
30 MiB，而且没有测试读回上限。改这块之前先看
`ByteBudgetIsSplitAcrossStreams` 测试。

- `max_buffers`（默认 **256/队列**，`kDefaultQueueMaxBuffers`）是**包数**
  硬上限，**不参与**拆分——每条队列各自的全额。字节超限丢包，包数超限同样丢。
- `infbuf`（`buffer.unlimited=true`）：`max_bytes=0` 字节不设上限，包数仍受
  256 保护。直播/边下边播想少重缓冲时才开。
- `max_cached_duration`（默认 3s）：另一种封顶，时间维度。
- 观测：`GetPlaybackStats().heap_bytes_estimate` + `video/audio.cached_bytes`
  + `DumpDiagnostics()`。

### 4.2 CPU

- `video.max_fps`（默认 **31**）：60fps 源会被降到 31fps。这是接通时的**有意
  行为变更**（docs/05 B 类教训：media 层默认 0 是不封顶），golden 录制要显式
  `--option max-fps=-1` 关掉。降帧计数在 `frames_dropped_fps`。
- `video.max_frame_drop`（framedrop）：合成器主动丢帧换时序。
- `DecoderConfig::thread_count` / `thread_type`（`threads`/`thread_type`
  key）：解码线程数；多核收益随分辨率上升，低分辨率可能负收益。
- `video.decoder_preference`：硬解优先（macOS VideoToolbox 实测可用；
  Linux VAAPI 有 spec 与选择链，显示侧零拷贝未实测）。硬解失败自动回落软解
  + `kDecoderFallback` 事件（Δ12）。
- `video.skip_loop_filter` / `skip_frame` / `skip_idct`：FFmpeg 侧解码降级，
  透传，代价是画质。

### 4.3 起播延迟

- `StageTimings`（`stats.stages`）把首帧延迟拆成 open_input / find_stream_info /
  decoder_open / first_packet / first_frame / presented 六段——**先看分解再调**，
  90% 的「起播慢」是 `find_stream_info` 在探测，不是解码。
- `demux.probe_size` / `analyzeduration`：探测深度，直接换起播时间。
- `buffer.first_high_water_mark`（默认 100ms）：首次起播要攒够的水位；
  调小 = 起播快但更容易起播后立即卡。
- `start_on_prepared`（默认 true）：prepared 即起播；配合 `PrepareSync` 有
  「autostart 在返回前跑完」的陷阱（MIGRATION §1）。
- `seek.seek_at_start`：从指定位置起播，跳过片头。

### 4.4 渲染路径

四段成本在 `stats.render`（upload/compose/blit/present，GL 渲染线程实测，
换 sink 不丢）：
- **upload**：平面像素→GPU。4K 开始不免费；硬解 dmabuf 零拷贝
  （`render.prefer_dmabuf_zero_copy` 默认 true）在这一段省掉一次读回。
- **compose**：清屏 + 绘制pass，进离屏画布。多层场景（PiP）成本 ≈ 层数 ×
  单层绘制，`bench_render.sh SCENE=pip` 能量出来。
- **blit**：合成结果→窗口（带缩放）。传 `--canvas` 走重采样，成本上升。
- **present**：SwapBuffers，开 vsync 时阻塞等待——贴近刷新周期是正常的。

多源注意（docs/14 阶段 3）：`AddSource` 的次源是**同一管线里的第二个
demuxer + 第二对渲染腿**，共用一个时钟——多一次完整解码，不只是多一次上传。

### 4.5 音频

回调线程只做 `FillBuffer` + `Scale`（Δ13），预算 < 100µs；变速
（`SetPlaybackRate`）默认走内置 WSOLA（`audio.tempo_stretch=false` 回退简单
重采样，音质换 CPU）。爆音观测：`audio_glitches`（count/total_duration），
音频 underrun 一次就是一个 glitch——「偶尔爆音」变成可告警的数字。

## 5. 可观测面（定位性能问题先看这些）

- `GetPlaybackStats()`：`video_decode_fps` / `video_output_fps`（媒体时间戳
  推导）/ `av_delay` / `av_diff` / `drop_frame_rate` / `frames_{presented,
  dropped,dropped_late,dropped_fps,repeated}` / `render` 四段 / `stages` /
  `first_frame_latency` / `heap_bytes_estimate` / `events_dropped_by_overflow`
  （事件队列溢出计数——宿主回调慢会在这里显形）。
- `DumpDiagnostics()`：全量内部状态 JSON，~1ms，任何时刻可调；**bug 报告的
  第一附件**。
- `kStats` 事件随统计 tick（1s）推送，替代 ijkplayer 的 20+ 次属性轮询。
- 日志线程名前缀：`avbase-media` / `avbase-demux`（background 优先级）/
  `avbase-video` / `avbase-audio` / `avbase-event`（Δ16）。

## 6. 已知边界（0.1.0 性能面的未项）

- **微基准已落地（base 层）**：`bench/` 是自包含 harness——无 Google
  Benchmark（沙箱取不到，CI 也不该装），接 `AVBASE_BUILD_BENCH`，只链
  `avbase::base`。实跑两项：`BM_PostTask_RoundTrip` ≈ 8.6 µs、
  `BM_BindOnce_Invoke` ≈ 118 ns（mac 实测，入 `tools/bench-baseline.jsonl`）；
  趋势守护 `tools/check_bench_regression.py`（>10% 回归告警），
  `gate_local --jobs bench` 本地硬判定，CI 的 `bench` job 只记录并上传
  artifact。**仍缺**：media/player 层的 `BM_*`（`DecideNextFrame` /
  `GetMasterClock` / `EventHub::Post`）与端到端 CPU 门槛的机器判据——后者仍
  靠 `bench_render.sh` 人工。
- **golden 对拍：diff 件已建，录制基线仍缺**：`tools/golden_diff.py` 按
  docs/07 §7.4 容差分级判定（帧数 / pts ±0、视频 hash 汉明 ≤5、事件序列
  类型与顺序一致、`late_us` ±5000 µs、`stat` 跳过）并支持 `allowlist.yaml`
  豁免，自带 selftest 与突变反证。一处如实收窄：`audio.checksum` 按**全等**
  判定——录制格式里它是 CRC32，表达不了「逐样本差 ≤1 LSB」，需豁免时走
  allowlist。**录制基线不在仓库**，需 ijkplayer 原版录制（docs/07 §7.3），
  被开放问题 Q8（锁哪个 ijkplayer 版本）卡住且沙箱无原版，故 CI 不跑对拍：
  没有基线可对比，跑出来等于没测。
- **soak 48h 未跑**：脚本就绪（`tools/run_soak.py`，内存曲线斜率门禁），
  沙箱禁读他进程 RSS，需 CI 长跑机。
- **跨机器数字不可比**：换机器/换负载重测，别拿两个环境的绝对值相减。
- 显示侧零拷贝（Linux VAAPI-EGL / Windows D3D11-GL）未实测；E5 硬件编码器
  本体未实测——见 CHANGELOG 的 Known limitations。
