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
| 缩略图 / seek preview | ❌ 无。`get_frame_mode` 是死选项（config 有、registry 有，media 零命中） |
| HDR / tone mapping | ❌ 无。`hdr_tone_mapping` 在 media/render 零命中；`kHdrUnsupported` 从未 emit |
| PBO 上传 | ❌ 未实现（`render_perf_stats.h:28` 标 "not built"） |
| 真色彩管理 | ❌ 无。仅 YUV→RGB 矩阵，无 gamut/EOTF/ICC/HDR→SDR |

---

## 2. 已配置但未生效的选项

`OptionRegistry` 的可配置面是手写的 `player/option_registry.cc`，不是
`option_registry.inc`（后者是 DRAFT，未进构建，由 `tools/gen_options.py` 从
`tools/option_map.py` 的 63 个 spec key 生成）。所以「已配置」指的是注册表里的条目；其余
54 个 spec key 从未进过构建，属「未接线」而非「构建内死选项」。

**2026-10-17 现状：7 个 key 全部接通**（3 个原生效 + 4 个 buffer key 本轮接回）。

| key | 写入字段 | 读取者 |
|---|---|---|
| `max-fps` | `video.max_fps` | renderer deps |
| `framedrop` | `video.max_frame_drop` | renderer deps |
| `start-on-prepared` | `start_on_prepared` | `player_impl_prepare.cc` 自动起播 |
| `packet-buffering` | `buffer.enabled` | `BufferController`（时间域三级门控的开关） |
| `max-buffer-size` | `buffer.max_bytes` | `DecoderBufferQueue`（`BufferQueueLimits::max_bytes`） |
| `infbuf` | `buffer.unlimited` | 同上（`max_bytes=0` 即无限） |
| `min-frames` | `buffer.min_frames` | `PipelineImpl` 预缓冲门控的包数下限 |

后 4 个 buffer key 早前因「`BufferController` 从不读 `config.buffer.*`」被判为静默 no-op 并
移除；本轮把 `BufferConfig` 真正接进三条下游后重新注册，语义对齐 ijkplayer：
- 时间域：`BufferController(config.buffer)` 由 `first/next/last_high_water_mark` 推导三级
  mark，`enabled` 来自 `packet-buffering`；`Evaluate` 首次成为真实门控。
- 字节域：`DecoderBufferQueue` 走 `BufferQueueLimits`（`max_bytes` / 0=无限 / `min_buffers`）。
- 门控：`PipelineImpl` 在 `OnBufferingStateChange(kHaveEnough)` 处按「已缓冲时长 ≥ mark
  且 包数 ≥ min-frames」推迟上报；`min-frames` 报 0（未跟踪的源）时以时长 mark 为准，避免卡死。

**仍为死选项（2 个，未注册，`SetOption` 直接报错）**：

| 已移除的 key | 写入字段 | 为何仍 no-op | 接线的真实代价 |
|---|---|---|---|
| `nodisp` | `render.disable_video_output` | 无 sink 选择逻辑读它（`pipeline_status.cc:155` 文案承诺了它，但代码没接） | 在 renderer factory 选 `NullVideoSink`（小改动，待做） |
| `enable-accurate-seek` | `seek.accurate` | 无 seek 路径 gate 它（`player_impl_accurate_seek.cc` 只读 `accurate_timeout`） | 把精确 seek 改成可关（但当前是默认常开，关掉会变行为） |

**spec 里「未接线」的 key（不在构建注册表，仅 DRAFT `.inc` / `docs/05 §4`）**：`fast` ·
`rdftspeed` · `sync-av-start` · `video-pictq-size` · `no-time-adjust` ·
`preset-5-1-center-mix-level` · `get-frame-mode` · `video-mime-type` · `ijkmeta-delay-init` ·
`render-wait-start` · 以及全部 `mediacodec-*` / `videotoolbox-*` / `opensles` / `soundtouch` /
FORMAT/CODEC 透传项。它们的 `PlayerConfig` 字段同样多数无人读；接线时每个都要先写读取者，
不要只加注册表条目。

> 处置原则（沿用 TODO 原意）：注册表条目要么有下游读取者（生效），要么移除并让 `SetOption`
> 报错。不保留「接受但不做事」的 key。缓冲区类如果真要做，应让 `BufferController` 消费
> `BufferConfig` 并把 key 接回，而不是现在这样假装。

---

## 3. 环境相关（非代码，需特定机器/权限）

| 项 | 阻塞原因 |
|---|---|
| RTMP / RTSP / SRT 播放验收 | 需真实 peer（本机 MediaMTX 已可跑，见 operations.md） |
| 真 `tc netem` 弱网 | 需 root；macOS 无 |
| Linux 显示侧零拷贝（VAAPI→EGL） | 需 Linux + VAAPI 设备 |
| Windows 显示侧零拷贝（D3D11-GL） | 需 Windows + D3D11 环境 |
| 硬解性能对照（4K CPU < 15%） | 需 4K 样本入库 |
| Qt 门面 / soak 48h | Phase 4 剩余（见 12 §2） |

---

## 4. 记录更正（evergreen 教训）

- **门禁「绿」可能是「没跑」**：check-clang-tidy 三处失效（header-filter 负向前瞻不支持 /
  platform 可选后端不在编译库 / HeaderFilterRegex 未锚定 vendored glad）；exit=127 让门禁
  看起来绿；2 秒跑完的全树检查一定没跑；CMake **多输出边**会让 ctest 二进制被静默丢弃
  （`tools/check_binary_freshness.py` 已修）；`add_test()` 写在 `enable_testing()` 前被
  静默丢弃。数字必须当场跑出来。
- **测试写了 ≠ 性质被钉住**：反证必须真跑一遍看有没有红的。
- **抑制（NOLINT）会静默失效**，比不写更危险：诊断行与 `NOLINTNEXTLINE` 差一行、散文注释里
  出现 NOLINT 被当真指令。
- **门禁「干净 ≠ CI 干净」**：本机 clang-tidy 读不了 Xcode PCH 会静默零发现；strict 在发布前
  编不过会一直红而无人见（sandbox 看不到 CI）；预设 `targets:` 收窄会让 `--preset` 只编部分
  目标，CI 的 ctest 步找不到可执行文件（CMakePresets.json 已修）。
