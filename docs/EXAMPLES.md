# EXAMPLES · `examples/` 逐文件说明

> 更新：**2026-10-23**。9 个受版本控制的文件，**6 个可执行目标**：5 个是给人读的
> 例子，第 6 个（`doc_snippets`）是 [COOKBOOK.md](COOKBOOK.md) 里代码块的可编译形式，
> 它不是 demo 而是一道门禁，所以放在 §6 单独说。
> 姊妹篇：[TOOLS.md](TOOLS.md)（`tools/` 逐文件说明）。
>
> 这个目录是**一把尺子，不是一批 demo**：[10 §6](10-SDK易用性设计.md) 把「最短出画
> 的 `main.cc` 有多少行」当作 API 易用性的度量（G13：clone 到第一帧 15 分钟），
> [10 §12](10-SDK易用性设计.md) 进一步把验收例子的主文件压在 300 行以内。所以这里
> 加代码的门槛是"它对新读者有没有用"，不是"它能不能演示一个功能"。

---

## 0. 速查表

| 文件 | 目标 | 构建开关 | 行数 |
|---|---|---|---|
| `CMakeLists.txt` | — | — | 54 |
| `headless.cc` | `headless` | `AVBASE_ENABLE_FFMPEG` | 156 |
| `play_sdl2.cc` | `play_sdl2` | `AVBASE_ENABLE_FFMPEG` **且** `AVBASE_ENABLE_SDL2` | 161 |
| `play_glfw.cc` | `play_glfw` | `AVBASE_ENABLE_GLFW` **且**找到 glfw3 | 352 |
| `play_egl.cc` | `play_egl` | `AVBASE_ENABLE_EGL` **且** `avbase::platform_egl` 存在（仅 Linux） | 288 |
| `pull_frames.cc` | `pull_frames` | `AVBASE_ENABLE_FFMPEG` | 246 |
| `doc_snippets/cookbook_smoke.cc` | `doc_snippets` | `AVBASE_ENABLE_FFMPEG` | 241 |
| `sdl2_demo.h` | （头文件，被 `play_sdl2` 用） | — | 390 |
| `scene_demo.h` | （头文件，被 GL 宿主例子用） | — | 237 |

**源清单是逐个列出的，不是 glob**（[06 §7.6](06-CMake工程与构建体系.md)）：五个可执行
文件同在一个目录、却分属四组不同的开关，**目录成员 ≠ 目标成员**，glob 表达不了。

---

## 1. `headless.cc` — 不需要窗口系统的整链路冒烟

整个 `decode → sync → render` 链路对 **null sink** 跑完，屏幕上什么都不出现，播放器
报 `kCompleted` 时进程退出 0。这是 CI 的 `e2e-headless` 与 `gate_local --jobs e2e`
跑的东西，也是 `check_protocols.py` / `check_live_protocols.py` / `check_weaknet.py`
判断播放是否真的发生了所依据的二进制。

156 行，是这个目录里最短的完整程序——**API 易用性的度量就落在这个数字上**。

```bash
./build/<cfg>/bin/headless tests/testdata/small_h264_aac_3s.mp4
# → "completed at media time 2.99s"，退出码 0
# 可选 --seek 1.5 / --rate 2.0 / --timeout n
```

注意它**吃位置参数而不是 `--url`**（三个窗口例子才是 `--url`），而且没有 `--help`：
传 `--help` 会被当成一个 URI 去打开，然后你会看到一条很有信息量的 `SourceNotFound`
错误事件——不是用法输出。

## 2. `play_sdl2.cc` — M11 验收例子

**嵌入模式**（[09](archive/09-Linux平台实现方案.md) §2）：宿主拥有窗口和 renderer，
avbase 往里画，主线程只干 `SDL_PollEvent` 和退出两件事。

```bash
./build/<cfg>/bin/play_sdl2 --url video.mp4 [--max-seconds n] [--gl] [--stats]
                            [--canvas WxH]
```

退出码 0 = 播完（或到 `--max-seconds` 上限），1 = 出错。

这个文件的特殊性写在 `sdl2_demo.h` 头里：docs/10 §12 要求验收例子的主文件在 300 行
以内——"新读者能把整个程序读完"就是 G13 那条 15 分钟的判据。它**曾经**超了，超的是
**不是 SDK 故事的那部分**：选项解析、事件日志、字幕光栅化、GL 运行时探测、窗口创建。
那些搬进了 `sdl2_demo.h`，留下的才是"程序"：建窗口 → 把 deps 交给 SDK → prepare →
泵事件 → stop。**现在 161 行。**

## 3. `play_glfw.cc` — GLFW 后端验收例子

同样是嵌入模式：宿主拥有 `GLFWwindow` 和它的 GL 上下文，avbase 通过 `GlPresenter`
画进它。音频走 null sink（GLFW 自己不带音频 API）。

```bash
./play_glfw --url video.mp4 [--max-seconds n] [--stats] [--canvas WxH]
            [--scene pip|fade] [--second video2.mp4]
```

`--canvas` 打开离屏画布路径（画到画布 FBO 再 blit 上屏），`--scene` 走多源场景，
`--second` 让第二层来自**第二个解码器**而不是同一流的延迟副本。这三个开关正是
`tools/bench_render.sh` 测量的组合。

352 行——它比 `play_sdl2` 长，因为它承担的场景开关多出三条轴；它不是 M11 的验收
例子（那是 `play_sdl2`），docs/10 §12 那条 300 行约束针对的是后者。

## 4. `play_egl.cc` — EGL 后端验收例子（仅 Linux）

宿主拥有 `EGLDisplay` / `EGLSurface` / `EGLContext`，经
`NativeDisplay::FromEglSurface()` 交给 avbase。

**macOS 上编不了**（没有 EGL 开发库），所以它被 `-DAVBASE_ENABLE_EGL=ON` 门控，且
只在该后端存在时才建。真宿主会从自己的窗口系统造 `EGLSurface`；这里用一个 **pbuffer
surface** 让例子自包含、不依赖窗口系统。

```bash
./play_egl --url video.mp4 [--max-seconds n] [--stats]
```

## 5. `pull_frames.cc` — 零拷贝取帧（Phase 3）

注入一个自定义 `VideoRendererSink`，它的渲染循环**留住**合成器递给它的帧——正是 GPU
合成器用的那个接缝——然后逐帧报告像素**留在 GPU 上**（`VideoFrame::NativeHandle`）还是
已经在 CPU 内存里。

**这里刻意不做回读**：调 `ToI420()` 会让这个例子失去意义。

它的输出在两类机器上都对，这是设计的一部分：

- 有可用硬解的机器上打印 `handle=cvpixelbuffer` 这类行，端到端证明 GPU 路径；
- 没有的机器上回退链静默走软解，行是 `storage=cpu`——**这本身就是回退契约在按设计
  工作**。

退出码 0 = 抓到了请求的帧数。

## 6. `doc_snippets/cookbook_smoke.cc` — 文档的可编译形式

[10 §12](10-SDK易用性设计.md) 的 V8：`COOKBOOK.md 有 ≥20 条，每条都能编译`。这个文件
是那些**别的例子没有覆盖到**的条目的可执行形式（窗口、事件循环、自定义 sink 由
`play_sdl2` / `pull_frames` / `headless` 证明）。

每个代码块**从它标注的那条 cookbook 条目逐字复制**下来。签名一漂，**红的是这个文件，
不是用户的构建**。

```bash
cmake --build build/ffmpeg --target doc_snippets
```

## 7. 两个头文件：宿主策略不属于库

| 文件 | 装什么 | 为什么在 `examples/` |
|---|---|---|
| `sdl2_demo.h` | SDL2 宿主的样板：选项解析、事件处理器、字幕光栅化、GL 探测、窗口创建 | 它是**宿主策略**，不是库代码。SDK 给宿主的是窗口与渲染器，怎么造窗口是应用的事。搬出来是为了让 `play_sdl2.cc` 剩下"程序" |
| `scene_demo.h` | `--scene` 演示：`pip`（延迟副本画成画中画）与 `fade`（同一延迟副本铺满画布、透明度循环） | 同样：**往场景里放什么是应用的决定**。两种布局足够跑遍场景的全部能力——第二层、画布位置、z 序、混合权重 |

两者都不属于任何库目标，因此**库永远不需要知道一个 demo 开关是什么**。

---

## 8. 三条压在这个目录上的规则

| 规则 | 内容 | 谁执行 |
|---|---|---|
| C30 | 不许起裸 `std::thread`——例子是宿主抄的模板，所以它必须说 SDK 的线程词汇（`base::Thread`） | `check_invariants.py` |
| C1 | 单文件 500 行（连注释） | `check_invariants.py` |
| §12 约束 | 验收例子的主文件 < 300 行（G13：clone 到第一帧 15 分钟） | 人工；`sdl2_demo.h` 头注释记录了这条约束当初是怎么把 play_sdl2 压回去的 |

---

## 9. 跑一遍

```bash
# 最短：不需要窗口系统
cmake --build build/ffmpeg --target headless
./build/ffmpeg/bin/headless tests/testdata/small_h264_aac_3s.mp4

# 真窗口（需要 SDL2 后端）
cmake --preset macos-sdl2 && cmake --build build/macos-sdl2 --target play_sdl2
./build/macos-sdl2/bin/play_sdl2 --url tests/testdata/small_h264_aac_3s.mp4

# 文档里的代码块能编
cmake --build build/ffmpeg --target doc_snippets
```

GLFW / EGL 两个例子要先有后端：`tools/setup_glfw.sh` 可以把 GLFW 装到仓库内
（免 sudo），见 [BUILDING §3.7](BUILDING.md)。
