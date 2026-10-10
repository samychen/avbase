# avbase — 企业播放器基座(C++20)

**风格**：Google C++ Style Guide + Chromium 工程约定（`base/` · `media/` 分层、`DCHECK`、sequence 模型、`base::BindOnce`/`WeakPtr`/`scoped_refptr`）
**目标平台**：Linux（X11 / Wayland），SDL2 后端 + 原生 OpenGL 后端双实现
**定位**：面向二次开发者的 **SDK**（API 易用、文档完整、示例齐全、错误信息可操作）
**依赖**：系统 FFmpeg（`find_package`，不打 patch）

> **项目沿革**：起于 ijkplayer 的 C++20 重构(docs/05 迁移对照表为历史卷,
> golden 对拍 ijkplayer 保留为回归资产);现按 avbase 升级计划演进为
> 三平台、硬解、网络流完备的企业播放器基座。

> **当前状态：M0–M11 ✅ · Phase 0/1/3/4 与转码 E1–E5 已铺设 · 审查报告
> Critical/High/Medium/Low 全部清零（第四十三、四十四轮）· 清理与深修两轮（第四十五、四十六轮）**
>
> | 配置 | 结果 |
> |---|---|
> | `no-ffmpeg`（无 FFmpeg / SDL2 / X11） | ✅ **468/468** |
> | `ffmpeg`（FFmpeg + 解复用/解码/转码） | ✅ **585/585** |
> | `full`（FFmpeg + SDL2 + 示例，macOS 真窗口） | ✅ **588/588** |
> | `debug`（Debug + DCHECK + `-Werror`） | ✅ 全绿，零警告 |
> | `asan`（ASan + UBSan + **LSan**） | ✅ 全绿，**0 泄漏** |
> | `tsan`（**ThreadSanitizer**） | ✅ 全绿，**0 data race** |
> | `check_invariants.py` | ✅ 全规则通过（**364 文件**；C23 列宽棘轮 **35**） |
> | `extract_constants.py --selftest` | ✅ 24 个移植常量与 docs/05 表 7 一致 |
>
> ⚠️ **用例数以 [docs/PROGRESS.md](docs/PROGRESS.md) 为准**（活文档，第四十六轮实测值见上表）。
> **能播放了**：`examples/headless <url>` 完整播到 kCompleted，`examples/play_sdl2
> --url <url>` 真窗口带音频出画（SDL2 后端）。
> 覆盖率当前为**棘轮基线**（core line 75.5% / branch 58.8%，见 `coverage_baseline.json`），
> **不是**已达标值——门禁只拦下降，不要求及格线。
---

## 0. TL;DR

| 项 | 结论 |
|---|---|
| 语言标准 | C++20，**`-fno-exceptions -fno-rtti`**（Chromium 规定） |
| 代码风格 | Google C++ Style + Chromium 约定；`.cc` 后缀、`CamelCase()` 方法、`snake_case()` getter、`member_` 尾下划线、80 列、2 空格 |
| 分层 | `base/` → `media/` → `player/` → `platform/`，**镜像 Chromium 的 `base/` + `media/` 目录结构** |
| 线程模型 | **控制面走 `base::SequencedTaskRunner` + `BindOnce` + `WeakPtr`（Chromium 风格）**；数据面保留有界队列（ffplay 风格） |
| 解码器接口 | **异步回调式**，对齐 `media::VideoDecoder::Decode(scoped_refptr<DecoderBuffer>, DecodeCB)` |
| 错误处理 | `base::expected<T, MediaError>` + `RETURN_IF_ERROR` / `ASSIGN_OR_RETURN`，**零异常** |
| 引用计数 | `scoped_refptr<T>` + `base::RefCountedThreadSafe<T>`（不用 `std::shared_ptr`） |
| 构建 | CMake ≥ 3.20 + `CMakePresets.json`，全 target 化，无 Android.mk / 无 shell 脚本前置 |
| Linux 视频 | ① `platform/sdl2`（最快出画，已交付）② `platform/linux`（原生 OpenGL 3.3 + EGL/GLX + Wayland/X11 + ALSA/PulseAudio，零第三方）—— **M12 已顺延，方案见 docs/archive/09** |
| 窗口归属 | **嵌入模式为核心**（渲染到调用方给的 `X11 Window` / `wl_surface`），`examples/` 额外提供内建窗口 |
| 验收 demo | `examples/play_sdl2 --url x.mp4`（真窗口带音频）· `examples/headless`（无窗口播到 kCompleted）· `examples/pull_frames`（抽帧） |
| 质量验收 | **Golden Test**：与原版 ijkplayer 逐帧 pts / 音频样本校验和对齐 |
| 工期 | 核心 + Linux 双后端 **单人约 23 周 / 双人约 12 周**（M0–M13） |

---

## 1. 文档导航

| # | 文档 | 内容 |
|---|---|---|
| — | [STYLE.md](STYLE.md) | **代码风格细则**：Google Style 落地规则、Chromium 约定、命名表、注释模板、clang-tidy 配置、提交规范、**与 Chromium 的对照速查表** |
| — | [PROGRESS.md](docs/PROGRESS.md) | **实施进度活文档**：已完成/未完成清单、可执行验证命令、check_invariants 抓到的真实问题、移植中发现的 2 个缺陷 |
| 01 | [现状剖析与设计目标](docs/01-现状剖析与设计目标.md) | ijkplayer 的 12 条病灶（代码取证）、设计目标 G1–G12、10 条设计原则、"为什么参照 Chromium media" |
| 02 | [总体架构与模块划分](docs/02-总体架构与模块划分.md) | **Chromium 式四层架构**（base/media/player/platform）、完整目录树（镜像 `media/base` + `media/filters`）、CMake target 拓扑、依赖方向铁律 |
| 03 | [核心类与接口设计](docs/03-核心类与接口设计.md) | 头文件级 C++20 声明，**命名与签名对齐 Chromium**：`DecoderBuffer` / `VideoFrame` / `AudioBus` / `DemuxerStream` / `VideoDecoder` / `Renderer` / `Pipeline` / `VideoFrameCompositor` / `AudioRendererSink` / `MediaLog` / `Player` |
| 04 | [线程模型与数据流](docs/04-线程模型与数据流.md) | **Task Runner / Sequence 模型**、6 个线程与 sequence 归属、`PostTask` + `WeakPtr` 通信、`serial` 语义、seek/flush/stop 时序、锁与无锁边界、竞态清单 |
| 05 | [迁移对照表](docs/05-迁移对照表.md) | **三方对照**（ijkplayer ↔ avbase ↔ Chromium media）：文件级 / API 级 / 函数级 / 选项级 / 消息级 / 属性级 / 常量级 / 字段级 + 16 条故意差异 |
| 06 | [CMake 工程与构建体系](docs/06-CMake工程与构建体系.md) | `.cc` + Google flags + `-fno-exceptions`、`FindFFmpeg`（三级查找）、FFmpeg 4.4~7.x 兼容层、**Linux 依赖探测**（SDL2/OpenGL/EGL/X11/Wayland/ALSA/Pulse）、install/export、CI 矩阵、符号隐藏 |
| 07 | [测试策略与可观测性](docs/07-测试策略与可观测性.md) | **`base::test::TaskEnvironment`** 单测模型、Chromium 式 Mock（`MockVideoDecoder`/`MockDemuxerStream`）、合成数据源、Golden Test、Fuzz、Sanitizer、覆盖率门禁 |
| 08 | [实施路线图与风险](docs/08-实施路线图与风险.md) | M0–M12 里程碑与 DoD、双人并行方案、工作量估算、砍掉/后置清单、**16 条风险登记册**、"平替"验收标准 A1–A16 |
| ~~09~~ | [Linux 平台实现方案](docs/archive/09-Linux平台实现方案.md) | **已归档**（第四十六轮）：M12 原生 OpenGL 后端顺延为可选，`platform/linux/` 从未落地，Linux 出画由 SDL2 交付。原文为双后端（SDL2 + 原生 OpenGL）的详细设计，保留为决策记录 |
| 10 | [SDK 易用性设计](docs/10-SDK易用性设计.md) | **面向二次开发者**：10 行 quick-start、可操作错误信息规范、API 人体工学清单、文档体系、示例矩阵、打包与集成方式、常见任务 cookbook |
| 11 | [行为规范卷](docs/11-行为规范卷.md) | **验收依据**（自 avbase_design.md §5/§7/§8 并入）：线程与任务模型、背压级联与 seek 序列、三级水位表。Phase 1–2 的验收标准引用本章 |
| 12 | [剩余工作清单](docs/12-剩余工作清单.md) | **执行快照**：Phase 0–4 逐阶段完成度、剩余项（零拷贝显示 / Qt 门面 / soak / Windows CI）、环境事项与开放决策点 |

---

## 2. 架构一览（Chromium 式分层）

```
┌───────────────────────────────────────────────────────────────────────────┐
│ player/          SDK 门面层                                                │
│   public/player.h  player_config.h  player_event.h  media_info.h          │
│   player_impl.cc（+ _events/_stop）  state_machine.cc  event_hub.cc       │
│   seek_controller.cc  buffer_controller.cc  option_registry.cc            │
│   ↔ Chromium: media/mojo/clients + blink HTMLMediaElement 的库化版本        │
├───────────────────────────────────────────────────────────────────────────┤
│ media/           媒体框架层                                                │
│   base/     接口与核心类型（无具体实现）                                     │
│             decoder_buffer · video_frame · audio_bus · audio_buffer        │
│             video_decoder · audio_decoder · decoder_stream(接口)            │
│             demuxer · demuxer_stream · renderer · renderer_client          │
│             pipeline · pipeline_controller · time_source                   │
│             audio_renderer_sink · video_renderer_sink · media_log          │
│   ffmpeg/   ★全项目唯一 include libav*.h 的层（第三十八轮自 platform 迁入）  │
│             ffmpeg_demuxer · ffmpeg_video_decoder · ffmpeg_audio_decoder    │
│             ffmpeg_glue · ffmpeg_hw_video_decoder · hw_frame_readback      │
│             av_packet_storage · ffmpeg_{video,audio}_filter                │
│   filters/  具体实现                                                       │
│             decoder_stream<T,D> · decoder_selector · pipeline_impl         │
│             video_renderer_impl · audio_renderer_impl · renderer_impl      │
│             audio_renderer_algorithm(WSOLA) · null_{video,audio}_sink      │
│             legacy/  video_frame_compositor ★ · av_sync_controller · clock │
│                      （LGPL-2.1 隔离区，见该目录 README.md）                 │
│   transcode/ 转码流水线（编码器层 · 滤镜 · concat · remux · 异步任务）        │
│   renderers/ default_renderer_factory                                     │
│   ↔ Chromium: media/  （目录结构与类名一一对应）                             │
├───────────────────────────────────────────────────────────────────────────┤
│ base/            基础设施层（Chromium base/ 的同名同语义最小子集，自研）       │
│   types/expected · expected_macros · memory/{scoped_refptr,ref_counted,     │
│   weak_ptr,raw_ptr} · functional/{callback,bind,callback_helpers} ·         │
│   time/{time,tick_clock} · synchronization/{lock,condition_variable,        │
│   waitable_event,atomic_sequence_number} · sequence_checker ·               │
│   task/{task_runner,sequenced_task_runner,single_thread_task_runner} ·      │
│   threading/{thread,platform_thread,message_pump_epoll} · observer_list ·   │
│   check · logging · feature_list · trace_event · containers/circular_deque  │
│   test/{task_environment,mock_callback,scoped_feature_list}                 │
│   ↔ Chromium: base/                                                        │
├───────────────────────────────────────────────────────────────────────────┤
│ platform/        平台后端（唯一允许出现平台 SDK 头文件的地方）                  │
│   sdl2/       Sdl2VideoSink / Sdl2AudioSink / Sdl2Window（最快出画）          │
│   hwaccel/    VideoToolbox / VAAPI / D3D11 的硬件解码 spec                  │
│   linux/      ⬜ M12 顺延：GlVideoSink(OpenGL 3.3) + EGL/GLX/Wayland/X11     │
│               AlsaAudioSink / PulseAudioSink / PipeWireSink                 │
│               DmaBufVideoFrame（零拷贝）· PresentExtension（vsync）          │
│   ↔ Chromium: ui/gl + media/audio/linux + media/base/linux                 │
└───────────────────────────────────────────────────────────────────────────┘
依赖方向： player → media → base ；platform → media/base（实现接口，由 player 注入）
```

**为什么镜像 Chromium 的目录**：Chromium `media/` 是工业界最成熟的开源媒体管线实现，它的 `base/`（接口）+ `filters/`（实现）分离、`DecoderBuffer`/`VideoFrame`/`DemuxerStream`/`Renderer`/`Pipeline` 这套抽象经过 15 年和数十亿设备验证。命名与目录一一对应意味着：
1. 任何读过 Chromium media 代码的 C++ 开发者可以零成本上手（SDK 友好）
2. 遇到设计分歧时，Chromium 是现成的裁决依据
3. 未来若需要接 Mojo IPC、DRM（CDM）、WebCodecs 语义，路径已经铺好

---

## 3. 数据流（一图）

```
                     player::Player  (SDK 门面，thread-safe)
                              │ PostTask(BindOnce(&Pipeline::StartPlayingFrom))
                              ▼
                     media::PipelineController  ──► media::Pipeline (on "media" sequence)
                              │
              ┌───────────────┼────────────────────────────────┐
              ▼               ▼                                ▼
     media::FFmpegDemuxer   media::RendererImpl          media::MediaLog
     (own thread "demux")   (on "media" sequence)
              │               │
    DemuxerStream::Read   ┌───┴────────────────────┐
    (count, ReadCB)       ▼                        ▼
              │    VideoRendererImpl        AudioRendererImpl
              │    (on "video" sequence)    (on "audio" sequence)
              │           │                        │
              │    DecoderStream<VideoDecoder>  DecoderStream<AudioDecoder>
              │    + DecoderSelector (回退链)   + DecoderSelector
              │           │                        │
              │    FFmpegVideoDecoder         FFmpegAudioDecoder
              │    (async: Decode(buf, cb))   (async)
              │           │                        │
              │    VideoFrameCompositor ★    AudioRendererAlgorithm (WSOLA)
              │    (帧调度/丢帧/同步)          (重采样/变速/时钟)
              │           │                        │
              │    VideoRendererSink         AudioRendererSink::RenderCallback
              │    ::RenderCallback          ::Render(delay, delay_ts,
              │           │                       glitch, AudioBus* dest)
              ▼           ▼                        ▼
        platform::  Sdl2/Gl VideoSink        Sdl2/Alsa/Pulse AudioSink
                    → X11/Wayland 窗口         → 声卡
```

---

## 4. SDK 友好性：10 行代码播放视频

```cpp
#include "player/public/player.h"

int main() {
  avbase::Player player;                                  // 默认依赖：FFmpeg + SDL2
  player.SetVideoSurface(window.native_handle());        // 嵌入模式：给个 X11 Window 就行
  player.SetEventHandler([](const avbase::PlayerEvent& e) {
    if (e.type == avbase::EventType::kPrepared) player_ref->Start();
  });
  AVBASE_RETURN_IF_ERROR(player.SetDataSource("video.mp4"));
  AVBASE_RETURN_IF_ERROR(player.PrepareAsync());
  return 0;
}
```

对比原版 ijkplayer 需要 20+ 行（`ijkmp_create` → 若干 `ijkmp_set_option_int` → `set_data_source` → `set_native_window` → `prepare_async` → 起一个线程轮询 `ijkmp_get_msg`）。详见 [docs/10](docs/10-SDK易用性设计.md)。

**SDK 易用性的硬性要求**（进 CI/评审清单）：
- 零配置可用：`avbase::Player p; p.SetDataSource(url); p.PrepareAsync();` 就能出画出声（自动探测平台后端）
- 错误信息可操作：不只说"失败了"，要说**为什么 + 怎么办**
- 头文件自解释：每个 public 方法有"在哪个线程调、会不会阻塞、失败时怎样"的注释
- 无需理解 FFmpeg：公开头文件不出现任何 `AV*` 类型
- 无需管理线程：`Player` 析构自动清理，不卡死
- 文档有 cookbook：20 个"我想做 X"的即抄即用片段

---

## 5. 需要拍板的决策（v2 更新）

| # | 决策点 | 建议 | 详见 |
|---|---|---|---|
| D1 | `base/` 是自研最小子集还是 vendor Chromium base | **自研 ~28 个文件 / ~4000 行**，API 与 Chromium 同名同语义，文件头注明 mirror 来源 | 02 §3 |
| D2 | FFmpeg 类型是否出现在 `media/base/` 接口 | **不出现**。只出现在 `media/ffmpeg/`（唯一目录，见 D11） | 03 §3 |
| D3 | 解复用线程模型（FFmpeg `av_read_frame` 是阻塞的，与 task runner 模型冲突） | **专用 demux 线程 + `interrupt_callback`**（首期）；本地文件二期改 `base::File` 异步 IO | 04 §2.1 ⚠️这是与 Chromium 的**有意偏离**，需你确认 |
| D4 | 变速音频处理 | **自研 `AudioRendererAlgorithm`（WSOLA）**，对齐 Chromium，去掉 SoundTouch 依赖 | 03 §7 |
| D5 | Linux 视频后端 | **两套都做**：SDL2（1.5 周，快速可用）+ 原生 OpenGL（3 周，展示抽象正确性 + 零依赖） | 09 |
| D6 | 窗口归属 | **嵌入模式为核心**（`SetVideoSurface(NativeDisplay)`），`examples/` 提供内建窗口 | 09 §2 |
| D7 | `ijkio` 边下边播缓存 | **后置**（M12），首期只留 `DataSource` 装饰器接口 | 08 §4 |
| D8 | DRM / CDM | **不实现**，`CdmContext` 位置留空接口 | 08 §4 |
| D9 | 是否首期提供 C ABI 兼容层 | **不提供**（已选现代 C++ API），预留 `player/public/c/` 目录与符号清单 | 08 §4 |
| D10 | 许可证 | avbase 用 **BSD-3-Clause**（与 Chromium base 兼容），移植自 ijkplayer 的算法文件保留 **LGPL-2.1** 声明并单独归入 `media/filters/legacy/` | 08 §5 R8 ⚠️需法务确认 |
| D11 | FFmpeg 的 include 隔离强度 | **链接隔离 + 目录隔离，放弃 include 隔离**：所有需要 `libav*` 的代码（胶水层与 `ffmpeg_*` 实现）同在 **`media/ffmpeg/`** 一个目录、同属 `avbase_ffmpeg` 一个 target，目录内可互相 include。更严格（include 也隔离）成本约 +15% | 02 §8 |

---

## 6. 新旧对照（速览）

| ijkplayer | avbase（Chromium 对齐） |
|---|---|
| `ff_ffplay.c` 5400 行 | 拆成 `media/filters/` 下 9 个文件，最大 ~700 行 |
| `VideoState` ~200 字段上帝结构体 | `RendererImpl` + `VideoRendererImpl` + `AudioRendererImpl` + `VideoFrameCompositor`，各自 ≤ 15 私有字段 |
| `AVPacket` 满天飞 | `scoped_refptr<media::DecoderBuffer>` |
| `AVFrame` + `SDL_VoutOverlay` 两套 | `scoped_refptr<media::VideoFrame>` + `StorageType` 枚举 |
| `ffpipeline` / `ffpipenode` 手写 vtable | `media::VideoDecoder` / `AudioDecoder` 虚基类 + `DecoderSelector` |
| `SDL_Vout` / `SDL_Aout` 手写 vtable | `media::VideoRendererSink` / `AudioRendererSink`（Chromium 接口） |
| `video_refresh()` 400 行 + 3 处 `goto retry` | `VideoFrameCompositor::UpdateCurrentFrame()` 纯决策 + 可单测 |
| `sdl_audio_callback` 里做 swr_convert | `AudioRendererSink::RenderCallback::Render()` 只做拷贝，重活在 `AudioRendererImpl` sequence |
| `packet_queue_*` / `frame_queue_*` C 函数族 | `media::DecoderBufferQueue` / `VideoFrameQueue`（`scoped_refptr` 化） |
| `FFP_MSG_*` int 常量 + arg1/arg2 | `player::PlayerEvent` 强类型 `std::variant` |
| `ffp_context_options[]` 字符串表 + `offsetof` | `player::PlayerConfig` 强类型 + `OptionRegistry`（校验 + 可生成文档） |
| `ijkmp_get_property_int64(FFP_PROP_*)` × 20 | `player.GetPlaybackStats()` 一次返回结构体 |
| `abort_request` + 手工 `SDL_CondSignal` × 5 处 | `base::SequencedTaskRunner::PostTask` + `WeakPtr` + `WaitableEvent` |
| 全局 `g_ijkmp_*` | 无全局态，`Player` 实例独立 |
| `Android.mk` + ndk-build + shell 脚本 | CMake + `CMakePresets.json` |
| 需给 FFmpeg 打 patch | 用发行版 FFmpeg（`apt install libavformat-dev`） |
| 无测试 | `base::test::TaskEnvironment` + gmock + Golden Test，核心覆盖率 ≥ 85% |

---

## 7. 术语

| 术语 | 含义 |
|---|---|
| Sequence | Chromium 概念：一组必须串行执行的任务，不绑定具体线程（但可绑定）。用 `SEQUENCE_CHECKER` 断言 |
| Task Runner | 任务投递目标。`SequencedTaskRunner`（串行）/ `SingleThreadTaskRunner`（固定线程） |
| `DecoderBuffer` | 解复用后、解码前的压缩数据（= ijkplayer 的 `AVPacket` 封装） |
| `VideoFrame` / `AudioBuffer` | 解码后的数据（= `AVFrame` 封装），`scoped_refptr` 管理 |
| `DemuxerStream` | 单条流的包来源，`Read(count, ReadCB)` 异步取包 |
| `Renderer` | Chromium 概念：把 demuxer 的输出变成音视频输出的组件（≈ ijkplayer 的整个 `ff_ffplay.c`） |
| `VideoFrameCompositor` | 决定"下一帧何时显示/是否丢弃"的组件（≈ ijkplayer 的 `video_refresh`） |
| `Sink` | 输出端：`VideoRendererSink`（送显）/ `AudioRendererSink`（送声卡） |
| serial | seek 世代号（继承自 ffplay），区分 flush 前后的数据 |
| Golden Test | 与原版 ijkplayer 在相同输入下逐帧比对，作为"平替"的验收手段 |

---

## 8. 仓库布局

### 8.1 实际状态（第九轮实测，行数与文件数为脚本统计）

此前这一节只有一张"规划"树，把 `examples/`、`tools/gen_options.py`、`media/renderers/`
这些**还不存在**的东西画得像已经存在，读者无法判断真实进度。现改为实测树 +
规划树并列，实测树里 ⬜ 表示规划中但不存在。

```
avbase/
├── LICENSE                  ✅ BSD-3-Clause + 4 节第三方/衍生说明（第九轮补）
├── .clang-format            ✅   .gitignore ✅（第九轮补 __pycache__/ *.pyc）
├── .clang-tidy              ⬜ STYLE.md §8 有完整配置内容，文件本身不存在
├── .editorconfig            ⬜
├── STYLE.md · VERSION.txt(0.1.0) · CMakeLists.txt · CMakePresets.json ✅
├── .github/workflows/ci.yml ✅ 16 个 job（含 format / clang-tidy / cpplint / corpus /
│                               soak / fuzz-smoke / windows-msvc 等）；e2e-linux 仍 if:false
├── cmake/                   ✅ 10 个：AvbaseOptions · AvbaseCompilerFlags · AvbaseThirdParty
│                               FindFFmpeg · FindSDL2 · FindLibyuv · AvbaseCheckInvariants
│                               BuildConfig.h.in · Version.h.in · AvbaseSanitizers
│                            ⬜ FindLinuxMediaDeps(M12，已顺延) · avbase.map(R18)
├── base/                    ✅ 44 文件 / 3,987 行
│   ├── functional/ memory/ time/ synchronization/ task/ threading/ types/ test/
│   └── ⬜ containers/ files/ strings/ · feature_list.h
├── media/
│   ├── base/                ✅ 64 文件 / 6,882 行（值类型 + 接口头）
│   ├── ffmpeg/              ✅ 44 文件 / 6,559 行 —— **全项目唯一链接 libav\* 的层**
│   │                            （第三十八轮由 `platform/ffmpeg` 整体迁入）
│   ├── filters/             ✅ 44 文件 / 10,064 行（decoder_stream · pipeline_impl
│   │                            + pipeline_impl_host · null 双 sink ·
│   │                            renderer_impl + renderer_impl_controls ·
│   │                            audio_renderer_algorithm 等）
│   │   └── legacy/          ✅ 6 文件 —— LGPL-2.1 隔离区（第九轮建）：
│   │                            video_frame_compositor · av_sync_controller · clock
│   │                            + LICENSE.LGPL-2.1 + README.md（准入规则）
│   ├── transcode/           ✅ 18 文件 / 3,484 行（转码 E1–E5：编解码器层 · 滤镜 ·
│   │                            拼接 · remux · 异步任务）
│   └── renderers/           ✅ DefaultRendererFactory
├── player/
│   ├── public/              ✅ 11 个 SDK 头（`version.h` 第四十六轮删：只有声明无定义）
│   └── *.cc                 ✅ 17 文件 —— player_impl 拆 events/stop 等 TU 承载
│                               状态机、事件枢纽与 S1/S3/S4 三线程
├── platform/
│   ├── sdl2/                ✅ 10 文件 / 1,875 行 —— Linux 出画由 SDL2 后端交付
│   ├── hwaccel/             ✅ VideoToolbox / VAAPI / D3D11 的 hw spec 头
│   └── linux/               ⬜ M12 已顺延；开关在 platform/CMakeLists.txt 里是
│                               有意的 FATAL_ERROR（见 docs/archive/09）
├── tests/                   ✅ 91 文件 / 19,842 行 · 588 用例（`full` 预设）
│                               unit/ 66 个 .cc · support/ 夹具 · fuzz/ 2 个目标
│                            ⬜ contract/ integration/ golden/ e2e/ stress/ bench/
├── tools/                   ✅ `tools/*.py` 5,721 行
│                               check_invariants.py（C1–C27 / 364 文件）
│                               extract_constants.py · gen_options.py · sim_wsola.py
│                               inspect/ → avbase-inspect（probe · decode · sync）
│                            ⬜ golden_record.py · golden_diff.py · verify_e2e.py
├── examples/                ✅ 3 个 / 759 行（headless · play_sdl2 · pull_frames）
├── third_party/             ⬜ 按设计保持为空（不 vendor）
└── docs/                    ✅ 13 篇（01–08 + 10–12 + PROGRESS + BUILDING）
                             + archive/（1–9 轮进度 · 09 Linux 方案）+ reviews/
                             ⬜ API · COOKBOOK · MIGRATION · TROUBLESHOOTING
                                EXTENDING · PERFORMANCE · CHANGELOG（M13 发版 blocker）
```

合计：**364 个 `.h`/`.cc`** —— C++ **39,488 行实现 + 19,842 行测试**，
`tools/*.py` **5,721 行**。逐项进度以 [docs/PROGRESS.md](docs/PROGRESS.md) 为准。

> **路径变更通知（第九轮 / 第三十八轮）**：① 第九轮 `video_frame_compositor.{h,cc}`、
> `av_sync_controller.{h,cc}`、`clock.{h,cc}` 移入 `media/filters/legacy/`（LGPL-2.1 隔离），
> docs/01–08 中的**旧路径刻意保留不改**——事后改路径会让"当时决定了什么"失真。
> ② 第三十八轮 `platform/ffmpeg/*` + `media/filters/ffmpeg_*` 整体迁入 **`media/ffmpeg/`**
> （目录边界应等于 target 边界）；这一批是**目录本身已不存在**，故全套文档已回填，
> 不回填会让读者按图索骥到空目录。

---

*文档版本 v2.1 · 2026-10-10（第四十六轮）*
*v2.1 变更：状态、用例数与目录树按第四十六轮实测重写；`platform/ffmpeg/` 路径回填为
`media/ffmpeg/`；删除与实测树冲突的 §8.2 规划树；09 Linux 方案因 M12 顺延移入 archive；
删除自声明冻结的「项目架构与能力分析」快照。*
