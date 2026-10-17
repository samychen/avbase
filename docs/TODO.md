# TODO · 未完成工作

> 与 [12-剩余工作清单](12-剩余工作清单.md)（剩余工作权威清单）和
> [PROGRESS.md](PROGRESS.md)（当前状态速览）互补。
> 本文件只收**未完成**工作；已完成项已移出，详见 12 / PROGRESS。

---

## 0. 现状

渲染 / 管线 / SDK 门面三层均已闭合（第六十六～七十九轮）。`AddVideoStream(int)` 等
6 个「第 id 路」接口经证实是死接口——多源轴是 `GetStream(source_id, type)`，不是
「同一资源多同类型流」——已点名拒绝，不再占用篇幅。

---

## 1. 产品化能力（全部未做）

| 能力 | 现状 |
|---|---|
| 录制（播放侧落盘） | ❌ 无。仅 `media/transcode/` 离线转码（输入是文件） |
| 推流 / RTMP publish | ❌ 无。`rtmp` 仅作**输入**协议 |
| 缩略图 / seek preview | ❌ 无。`get_frame_mode` 未接线（config 与 DRAFT `.inc` 有，构建注册表无，media 零命中——见 §2） |
| HDR / tone mapping | ❌ 无。`hdr_tone_mapping` 在 media/render 零命中；`kHdrUnsupported` 从未 emit |
| PBO 上传 | ❌ 未实现（`render_perf_stats.h:28` 标 "not built"） |
| 真色彩管理 | ❌ 无。仅 YUV→RGB 矩阵，无 gamut/EOTF/ICC/HDR→SDR |

---

## 2. 已配置但未生效的选项

`OptionRegistry` 的可配置面是手写的 `player/option_registry.cc`，不是
`option_registry.inc`（后者是 DRAFT，未进构建，由 `tools/gen_options.py` 从
`tools/option_map.py` 的 63 个 spec key 生成）。所以「已配置」指的是注册表里的条目；其余
54 个 spec key 从未进过构建，属「未接线」而非「构建内死选项」。

**2026-10-17 现状：9 个 key 全部接通，构建内死选项为 0**（3 个原生效 + 4 个 buffer key
第九十一轮接回 + 2 个渲染/seek key 第九十二轮接回）。

| key | 写入字段 | 读取者 |
|---|---|---|
| `max-fps` | `video.max_fps` | renderer deps |
| `framedrop` | `video.max_frame_drop` | renderer deps |
| `start-on-prepared` | `start_on_prepared` | `player_impl_prepare.cc` 自动起播 |
| `packet-buffering` | `buffer.enabled` | `BufferController`（时间域三级门控的开关） |
| `max-buffer-size` | `buffer.max_bytes` | `DecoderBufferQueue`（`BufferQueueLimits::max_bytes`） |
| `infbuf` | `buffer.unlimited` | 同上（`max_bytes=0` 即无限） |
| `min-frames` | `buffer.min_frames` | `PipelineImpl` 预缓冲门控的包数下限 |
| `nodisp` | `render.disable_video_output` | `DefaultRendererFactory::Deps::disable_video_output` → 选 `NullVideoSink` |
| `enable-accurate-seek` | `seek.accurate` | `PlayerImpl::SeekTo` 对 `SeekMode::kAccurate` 的否决位 |

后 4 个 buffer key 早前因「`BufferController` 从不读 `config.buffer.*`」被判为静默 no-op 并
移除；第九十一轮把 `BufferConfig` 真正接进三条下游后重新注册，语义对齐 ijkplayer：
- 时间域：`BufferController(config.buffer)` 由 `first/next/last_high_water_mark` 推导三级
  mark，`enabled` 来自 `packet-buffering`，对外供出 `current_mark()` 与 `progress_percent()`。
  **门控本身在 `PipelineImpl`**；`BufferController::Evaluate` 是同一策略的纯函数模型，
  生产路径上无人调用（只有单测），别把它当成门控——第九十四轮核对时把这句话改准了。
- 字节域：`DecoderBufferQueue` 走 `BufferQueueLimits`（`max_buffers` 包数上限 / `max_bytes`
  字节上限，0 = **字节**不设上限而包数仍封顶 256）。`max-buffer-size` 是**全播放器**预算，
  由 `SplitQueueBudget()` 按流数拆分——直接传给每条队列会让 A/V 文件的默认内存翻倍
  （第九十一轮留下的口径，第九十四轮修正并有单测钉住）。
- 门控：`PipelineImpl` 在 `OnBufferingStateChange(kHaveEnough)` 处按「已缓冲时长 ≥ mark
  且 包数 ≥ min-frames」推迟上报；`min-frames` 报 0（未跟踪的源）时以时长 mark 为准，避免卡死。

最后 2 个（第九十二轮）的接法，以及它们为什么不是另一个已有开关的别名：

| key | 接在哪 | 与近邻的区别 |
|---|---|---|
| `nodisp` | `DefaultRendererFactory::CreateVideoRendererSink` 在 `disable_video_output` 时无视注入的 sink factory，直接造 `NullVideoSink` 并传 **nullptr** 目标（进 discard 模式，帧计入 dropped 而非 presented） | 不是 `vn`（`video.disabled`）：那条腿**仍解码**，av sync / 节奏 / 统计照旧，只是不上屏。也不是 `SetVideoSurface(nullptr)`：那是运行期换目标，而 `nodisp` 是配置层决定，宿主传了后端 sink 也压得住 |
| `enable-accurate-seek` | `SeekTo` 里 `accurate = (mode == kAccurate && config.seek.accurate)`；被否决时降级为关键帧落点，`LOG(WARNING)` 只报一次（`accurate_seek_veto_logged_` 原子闩） | 不是 Δ19 里「`SeekMode` 取代 option」那种替代关系：`kAccurate` 仍是 per-call 的一等 API，这个位是**全局否决**。`PlayerConfig::SeekConfig::accurate` 默认 `true`（与 ijk 的 0 相反），因为 opt-in 已经在调用点发生；默认播放行为仍是关键帧，那由 `SeekMode` 的默认值决定 |

> **`accurate=false` 时的诚实性**：降级不是错误，`SeekCompletedPayload` 的
> `requested` / `actual` 会显示没落到目标帧。测试
> `tests/unit/player/player_seek_config_unittest.cc` 双向钉住：否决时
> `kAccurateSeekCompleted` 计数为 0 且回调成功，允许时该事件恰好 1 次（到达或超时都会发）。

**spec 里「未接线」的 key（不在构建注册表，仅 DRAFT `.inc` / `docs/05 §4`）**：`fast` ·
`rdftspeed` · `sync-av-start` · `video-pictq-size` · `no-time-adjust` ·
`preset-5-1-center-mix-level` · `get-frame-mode` · `video-mime-type` · `ijkmeta-delay-init` ·
`render-wait-start` · 以及全部 `mediacodec-*` / `videotoolbox-*` / `opensles` / `soundtouch` /
FORMAT/CODEC 透传项。它们的 `PlayerConfig` 字段同样多数无人读；接线时每个都要先写读取者，
不要只加注册表条目。

> 处置原则（沿用 TODO 原意）：注册表条目要么有下游读取者（生效），要么移除并让 `SetOption`
> 报错。不保留「接受但不做事」的 key。第九十一/九十二轮就是照这条原则收的尾：先写读取者，
> 再把 key 注册回来 —— 顺序反了就会得到「能读能写但什么都不影响」的假选项。

---

## 3. 环境相关（非代码，需特定机器/权限）

| 项 | 阻塞原因 |
|---|---|
| 真 `tc netem` 弱网 | 需 root；macOS 无 |
| Linux 显示侧零拷贝（VAAPI→EGL） | 需 Linux + VAAPI 设备 |
| Windows 显示侧零拷贝（D3D11-GL） | 需 Windows + D3D11 环境 |
| 硬解性能对照（4K CPU < 15%） | 需 4K 样本入库 |
| Qt 门面 / soak 48h | Phase 4 剩余（见 12 §2） |

> **已从本节移出**：RTMP / RTSP / SRT 播放验收（第八十二轮实测全绿，MediaMTX v1.21.2
> loopback、exit 0，RTMP L≈3.97s / RTSP L≈2.99s / SRT L≈3.54s；工具
> `tools/check_live_protocols.py`，数字与判据见 [12 §2.4](12-剩余工作清单.md)）。它搬走前
> 指向的 `operations.md` 从未记过这件事，是行文里的假指针。

---

## 4. 记录更正（evergreen 教训）

- **门禁「绿」可能是「没跑」**：check-clang-tidy 三处失效（header-filter 负向前瞻不支持 /
  platform 可选后端不在编译库 / HeaderFilterRegex 未锚定 vendored glad）；exit=127 让门禁
  看起来绿；2 秒跑完的全树检查一定没跑；CMake **多输出边**会让 ctest 二进制被静默丢弃
  （`tools/check_binary_freshness.py` 已修）；`add_test()` 写在 `enable_testing()` 前被
  静默丢弃。数字必须当场跑出来。
- **测试写了 ≠ 性质被钉住**：反证必须真跑一遍看有没有红的。
- **「两个线程都空闲」不是死锁，是没有人做这件事**：挂起时先 `sample <pid> 1` 抓栈，比读
  锁代码便宜一个数量级。第九十二轮拿「队列满、Flush 排在背压等待后面」解释了一个 seek 挂起，
  采样一秒钟就把它推翻了 —— 两个线程都在 `TaskQueue::Run()` 的 `wait()` 上。修完的现场记录
  在 [04](04-线程模型与数据流.md) §2.1「EOF 与 seek 的握手」。
- **日志含 NUL 字节时 `grep -n` 会静默无输出**（不等于「无匹配」）：用 `grep -a` 或 `sed -n`。
  同一类错误还有 `rg`/`grep` 把 `->Flush` 当成选项解析，需要 `-e`。
- **抑制（NOLINT）会静默失效**，比不写更危险：诊断行与 `NOLINTNEXTLINE` 差一行、散文注释里
  出现 NOLINT 被当真指令。
- **门禁「干净 ≠ CI 干净」**：本机 clang-tidy 读不了 Xcode PCH 会静默零发现；strict 在发布前
  编不过会一直红而无人见（sandbox 看不到 CI）；预设 `targets:` 收窄会让 `--preset` 只编部分
  目标，CI 的 ctest 步找不到可执行文件（CMakePresets.json 已修）。
