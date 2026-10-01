# ijkpp — ijkplayer 核心播放器的 C++20 重构

**风格**：Google C++ Style Guide + Chromium 工程约定（`base/` · `media/` 分层、`DCHECK`、sequence 模型、`base::BindOnce`/`WeakPtr`/`scoped_refptr`）
**目标平台**：Linux（X11 / Wayland），SDL2 后端 + 原生 OpenGL 后端双实现
**定位**：面向二次开发者的 **SDK**（API 易用、文档完整、示例齐全、错误信息可操作）
**依赖**：系统 FFmpeg（`find_package`，不打 patch）

> **当前状态：M0–M6 ✅ · M7 渲染层代码完成 · M8 播放链路已接线 ✅ · 端到端播放 ✅（headless + SDL2 真窗口）**
>
> | 配置 | 结果 |
> |---|---|
> | `no-ffmpeg`（无 FFmpeg / SDL2 / X11） | ✅ 323/323 |
> | mac 配置（FFmpeg 7.1.1 + SDL2 + 示例） | ✅ 358/358，`headless`/`play_sdl2` 真实播放到 kCompleted |
> | `debug`（Debug + DCHECK + `-Werror`） | ✅ 全绿，零警告 |
> | `asan`（ASan + UBSan + **LSan**） | ✅ 全绿，**0 泄漏** |
> | `tsan`（**ThreadSanitizer**） | ✅ 全绿，**0 data race** |
> | `check_invariants.py` | ✅ 全规则通过（220 文件；C23 列宽棘轮 323→311） |
> | `extract_constants.py --selftest` | ✅ 24 个移植常量与 docs/05 表 7 一致 |
>
> ⚠️ **用例数以 [docs/PROGRESS.md](docs/PROGRESS.md) 为准**（活文档，第十轮为
> 358/358 与 323/323，macOS / AppleClang 21 / Homebrew FFmpeg 7.1.1 实测）。
> **能播放了**：`examples/headless <url>` 完整播到 kCompleted，`examples/play_sdl2
> --url <url>` 真窗口带音频出画（SDL2 后端，M11 代码提前落地）。
> Sanitizer 历史战果与第十轮抓到的 12 个真 bug → [docs/PROGRESS.md](docs/PROGRESS.md)
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
| Linux 视频 | ① `platform/sdl2`（最快出画）② `platform/linux`（原生 OpenGL 3.3 + EGL/GLX + Wayland/X11 + ALSA/PulseAudio，零第三方） |
| 窗口归属 | **嵌入模式为核心**（渲染到调用方给的 `X11 Window` / `wl_surface`），`examples/` 额外提供内建窗口 |
| 验收 demo | `examples/play_sdl2 --url x.mp4` 与 `examples/play_native` 在 Linux 上流畅播放 1080p |
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
| 05 | [迁移对照表](docs/05-迁移对照表.md) | **三方对照**（ijkplayer ↔ ijkpp ↔ Chromium media）：文件级 / API 级 / 函数级 / 选项级 / 消息级 / 属性级 / 常量级 / 字段级 + 16 条故意差异 |
| 06 | [CMake 工程与构建体系](docs/06-CMake工程与构建体系.md) | `.cc` + Google flags + `-fno-exceptions`、`FindFFmpeg`（三级查找）、FFmpeg 4.4~7.x 兼容层、**Linux 依赖探测**（SDL2/OpenGL/EGL/X11/Wayland/ALSA/Pulse）、install/export、CI 矩阵、符号隐藏 |
| 07 | [测试策略与可观测性](docs/07-测试策略与可观测性.md) | **`base::test::TaskEnvironment`** 单测模型、Chromium 式 Mock（`MockVideoDecoder`/`MockDemuxerStream`）、合成数据源、Golden Test、Fuzz、Sanitizer、覆盖率门禁 |
| 08 | [实施路线图与风险](docs/08-实施路线图与风险.md) | M0–M12 里程碑与 DoD、双人并行方案、工作量估算、砍掉/后置清单、**16 条风险登记册**、"平替"验收标准 A1–A16 |
| 09 | [Linux 平台实现方案](docs/09-Linux平台实现方案.md) | **双后端详细设计**：SDL2 后端、原生 OpenGL 后端（GLX/EGL + X11/Wayland）、ALSA/PulseAudio/PipeWire、vsync 与 Present 扩展、零拷贝 dmabuf、色彩空间、嵌入模式窗口协议 |
| 10 | [SDK 易用性设计](docs/10-SDK易用性设计.md) | **面向二次开发者**：10 行 quick-start、可操作错误信息规范、API 人体工学清单、文档体系、示例矩阵、打包与集成方式、常见任务 cookbook |

---

## 2. 架构一览（Chromium 式分层）

```
┌───────────────────────────────────────────────────────────────────────────┐
│ player/          SDK 门面层                                                │
│   public/player.h  player_config.h  player_events.h  media_info.h         │
│   player_impl.cc   state_machine.cc  event_hub.cc  seek_controller.cc     │
│   buffer_controller.cc  option_registry.cc  diagnostics.cc                │
│   ↔ Chromium: media/mojo/clients + blink HTMLMediaElement 的库化版本        │
├───────────────────────────────────────────────────────────────────────────┤
│ media/           媒体框架层                                                │
│   base/     接口与核心类型（无具体实现）                                     │
│             decoder_buffer · video_frame · audio_bus · audio_buffer        │
│             video_decoder · audio_decoder · decoder_stream(接口)            │
│             demuxer · demuxer_stream · renderer · renderer_client          │
│             pipeline · pipeline_controller · time_source                   │
│             audio_renderer_sink · video_renderer_sink · media_log          │
│   filters/  具体实现                                                       │
│             ffmpeg_demuxer · ffmpeg_video_decoder · ffmpeg_audio_decoder    │
│             ffmpeg_glue · decoder_stream<T,D> · decoder_selector           │
│             video_renderer_impl · audio_renderer_impl                      │
│             video_frame_compositor ★ · audio_renderer_algorithm(WSOLA)     │
│             renderer_impl · default_decoder_factory                        │
│   renderers/ default_renderer_factory                                     │
│   audio/     audio_manager · audio_output_device · null_audio_output        │
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
│ platform/        平台后端（唯一允许出现平台 SDK 与 FFmpeg 头之外的东西）       │
│   null/       NullVideoSink / NullAudioSink（headless、CI）                  │
│   sdl2/       Sdl2VideoSink / Sdl2AudioSink / Sdl2Window（最快出画）          │
│   linux/      GlVideoSink（OpenGL 3.3）+ EGL/GLX/Wayland/X11                │
│               AlsaAudioSink / PulseAudioSink / PipeWireSink                 │
│               DmaBufVideoFrame（零拷贝）· PresentExtension（vsync）          │
│   ffmpeg/     FFmpeg 适配（AvIncludes.h · Compat.h · LogBridge）             │
│               ★全项目唯一 include libav*.h 的地方                            │
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
  ijkpp::Player player;                                  // 默认依赖：FFmpeg + SDL2
  player.SetVideoSurface(window.native_handle());        // 嵌入模式：给个 X11 Window 就行
  player.SetEventHandler([](const ijkpp::PlayerEvent& e) {
    if (e.type == ijkpp::EventType::kPrepared) player_ref->Start();
  });
  IJKPP_RETURN_IF_ERROR(player.SetDataSource("video.mp4"));
  IJKPP_RETURN_IF_ERROR(player.PrepareAsync());
  return 0;
}
```

对比原版 ijkplayer 需要 20+ 行（`ijkmp_create` → 若干 `ijkmp_set_option_int` → `set_data_source` → `set_native_window` → `prepare_async` → 起一个线程轮询 `ijkmp_get_msg`）。详见 [docs/10](docs/10-SDK易用性设计.md)。

**SDK 易用性的硬性要求**（进 CI/评审清单）：
- 零配置可用：`ijkpp::Player p; p.SetDataSource(url); p.PrepareAsync();` 就能出画出声（自动探测平台后端）
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
| D2 | FFmpeg 类型是否出现在 `media/base/` 接口 | **不出现**。只在 `platform/ffmpeg/` 与 `media/filters/ffmpeg_*` | 03 §3 |
| D3 | 解复用线程模型（FFmpeg `av_read_frame` 是阻塞的，与 task runner 模型冲突） | **专用 demux 线程 + `interrupt_callback`**（首期）；本地文件二期改 `base::File` 异步 IO | 04 §2.1 ⚠️这是与 Chromium 的**有意偏离**，需你确认 |
| D4 | 变速音频处理 | **自研 `AudioRendererAlgorithm`（WSOLA）**，对齐 Chromium，去掉 SoundTouch 依赖 | 03 §7 |
| D5 | Linux 视频后端 | **两套都做**：SDL2（1.5 周，快速可用）+ 原生 OpenGL（3 周，展示抽象正确性 + 零依赖） | 09 |
| D6 | 窗口归属 | **嵌入模式为核心**（`SetVideoSurface(NativeDisplay)`），`examples/` 提供内建窗口 | 09 §2 |
| D7 | `ijkio` 边下边播缓存 | **后置**（M12），首期只留 `DataSource` 装饰器接口 | 08 §4 |
| D8 | DRM / CDM | **不实现**，`CdmContext` 位置留空接口 | 08 §4 |
| D9 | 是否首期提供 C ABI 兼容层 | **不提供**（已选现代 C++ API），预留 `player/public/c/` 目录与符号清单 | 08 §4 |
| D10 | 许可证 | ijkpp 用 **BSD-3-Clause**（与 Chromium base 兼容），移植自 ijkplayer 的算法文件保留 **LGPL-2.1** 声明并单独归入 `media/filters/legacy/` | 08 §5 R8 ⚠️需法务确认 |
| D11 | FFmpeg 的 include 隔离强度 | **链接隔离 + 目录隔离，放弃 include 隔离**：`media/filters/ffmpeg_*.cc` 可 include `platform/ffmpeg/av_includes.h`，但只有 `ijkpp_platform_ffmpeg` 这个 target 链接 FFmpeg。更严格（include 也隔离）成本约 +15% | 02 §8 |

---

## 6. 新旧对照（速览）

| ijkplayer | ijkpp（Chromium 对齐） |
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
ijkpp/
├── LICENSE                  ✅ BSD-3-Clause + 4 节第三方/衍生说明（第九轮补）
├── .clang-format            ✅   .gitignore ✅（第九轮补 __pycache__/ *.pyc）
├── .clang-tidy              ⬜ STYLE.md §8 有完整配置内容，文件本身不存在
├── .editorconfig            ⬜
├── STYLE.md · VERSION(0.1.0) · CMakeLists.txt · CMakePresets.json ✅
├── .github/workflows/ci.yml ✅ quick + full 矩阵；ffmpeg-matrix 与 e2e-linux 仍是 if:false
├── cmake/                   ✅ 7 个：IjkppOptions · IjkppCompilerFlags · IjkppThirdParty
│                               FindFFmpeg · IjkppCheckInvariants · BuildConfig.h.in · Version.h.in
│                            ⬜ FindLinuxMediaDeps(M12) · IjkppInstall(M8) · ijkpp.map(R18)
├── base/                    ✅ 44 文件 / 3,763 行
│   ├── functional/ memory/ time/ synchronization/ task/ threading/ types/ test/
│   └── ⬜ containers/ files/ strings/ trace_event/ · feature_list.h
│         threading/message_pump_epoll.cc（R2 的 L2 降级：现为 TaskQueue 驱动）
├── media/
│   ├── base/                ✅ 54 文件 / 5,301 行
│   │     其中 7 个是第九轮冻结的 DRAFT 接口头（17 行注释级缺口清单，未进构建）：
│   │     pipeline_status · media_resource · renderer_client · renderer ·
│   │     renderer_factory · pipeline · pipeline_controller
│   ├── filters/             ✅ 12 文件 / 3,031 行（ffmpeg_* · decoder_stream ·
│   │                            decoder_selector · ffmpeg_glue）
│   │   └── legacy/          ✅ 6 文件 / 1,716 行 —— LGPL-2.1 隔离区（第九轮建）：
│   │                            video_frame_compositor · av_sync_controller · clock
│   │                            + LICENSE.LGPL-2.1(501 行) + README.md（准入规则）
│   ├── renderers/           ⬜ M7
│   └── audio/               ⬜ M7
├── player/
│   ├── public/              ✅ 12 个 SDK 头 / 997 行（M8 契约冻结）
│   └── *.cc                 ⚠️ 7 文件 / 909 行，全是 skeleton：player.cc 的 10 个方法
│                               返回 kNotImplemented；option_registry 只覆盖 8/60+ 个 key
│                            ⬜ player_impl · state_machine · event_hub · seek_controller
│                               buffer_controller · diagnostics · public/c/
├── platform/
│   ├── ffmpeg/              ✅ 9 文件 / 730 行（全项目唯一链接 FFmpeg 的 target）
│   └── null/ sdl2/ linux/   ⬜ M10 / M11 / M12 —— 后两个开关目前在
│                               platform/CMakeLists.txt 里是有意的 FATAL_ERROR
├── tests/                   ✅ unit/ 23 文件 / 5,940 行 · 322 用例 · testdata/ 5 个样本
│                               （含 1 个 DRAFT 测试文件，10 用例，未计入 322）
│                            ⬜ contract/ integration/ golden/ e2e/ stress/ fuzz/
│                               bench/ support/
├── tools/                   ✅ check_invariants.py(374，14 条规则/174 文件)
│                               extract_constants.py(547) + ported_constants.py(222)
│                               inspect/ → ijkpp-inspect（probe · decode · sync）
│                               setup_ffmpeg.sh（FindFFmpeg.cmake 引用，非死代码）
│                            ⬜ gen_options.py · golden_record.py · golden_diff.py
│                               verify_e2e.py · ijkplayer-recorder/ · build_linux.sh
│                               inspect 的 doctor/play/dump/golden 子命令
├── examples/                ⬜ 12 个示例全部未建（M8/M11/M12）
├── third_party/             ⬜ 按设计保持为空（不 vendor）
└── docs/                    ✅ 11 篇（01–10 + PROGRESS，约 41.7 万字符）
                             ⬜ BUILDING · API · COOKBOOK · MIGRATION · TROUBLESHOOTING
                                EXTENDING · PERFORMANCE · CHANGELOG（M13 发版 blocker）
```

合计：**174 个 `.h`/`.cc`**（其中 8 个 DRAFT）、C++ **22,975 行**（实现 17,203 / 测试 5,772）、
`tools/*.py` **1,143 行**。逐项进度以 [docs/PROGRESS.md](docs/PROGRESS.md) 的
"未完成（按里程碑）"与"工具与门禁现状"两张表为准。

### 8.2 规划（目标布局，docs/02 §2 的完整版）

```
ijkpp/
├── .clang-format  .clang-tidy  .editorconfig  STYLE.md  VERSION  LICENSE
├── CMakeLists.txt  CMakePresets.json
├── cmake/            IjkppOptions · CompilerFlags · FindFFmpeg · FindLinuxMediaDeps
│                     IjkppInstall · IjkppCheckInvariants · ijkpp.map
├── base/             ← Chromium base/ 同名同语义子集（~28 文件）
│   ├── functional/ memory/ time/ synchronization/ task/ threading/
│   ├── containers/ files/ types/ trace_event/ test/
│   ├── check.h logging.h observer_list.h feature_list.h sequence_checker.h
├── media/
│   ├── base/         接口与核心类型（~30 文件）
│   ├── filters/      具体实现（~18 文件，含 ffmpeg_* 与 legacy/ 的三个移植件）
│   ├── renderers/    default_renderer_factory
│   └── audio/        audio_manager · audio_output_device · null/
├── player/
│   ├── public/       ★SDK 唯一对外头文件目录（12 个）
│   └── *.cc          实现
├── platform/
│   ├── null/  sdl2/  linux/  ffmpeg/
│   └── (android/ ios/ 预留)
├── examples/         play_sdl2 · play_native · play_embed · headless · ijkpp_inspect
├── tests/            unit/ contract/ integration/ golden/ stress/ fuzz/ bench/ testdata/
├── tools/            check_invariants.py · extract_constants.py · gen_options.py
│                     golden_record.py · golden_diff.py · verify_e2e.py · build_linux.sh
├── third_party/      （空或仅 vendored 单头）
└── docs/             01–10（本套设计文档）+ BUILDING/API/COOKBOOK/MIGRATION/...
```

> **路径变更通知（第九轮）**：`video_frame_compositor.{h,cc}`、`av_sync_controller.{h,cc}`、
> `clock.{h,cc}` 已从 `media/filters/` 移入 **`media/filters/legacy/`**（LGPL-2.1 隔离，
> 见该目录的 README.md）。docs/01–08 是 v2.0 已评审的设计文档，**其中的旧路径刻意保留不改**
> ——事后改路径会让"当时决定了什么"失真。当前布局以本节与 PROGRESS 为准。

---

*文档版本 v2.0 · 2026-09-29 · 状态：待评审*
*v2.0 变更：全面对齐 Google C++ Style 与 Chromium base/media 分层；Linux 双后端纳入核心范围；线程模型改为 task runner + sequence；解码器接口改为异步回调式；新增 09/10 两篇。*
