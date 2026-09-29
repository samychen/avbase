# 09 · Linux 平台实现方案

> 上一篇：[08 实施路线图与风险](08-实施路线图与风险.md) ｜ 返回：[README](../README.md)

> **实现状态**：本篇是设计文档，描述目标形态。**当前已落地到哪一步以
> [PROGRESS.md](PROGRESS.md) 为唯一真相源**；两者的差异（已实现 / 计划中）在 PROGRESS.md 里逐项标注。

本篇是"**最终要实现 Linux 平台播放视频**"这一目标的落地方案：双后端（SDL2 + 原生 OpenGL）、嵌入模式优先、dlopen 弱依赖、xvfb 可验收。

---

## 1. 目标与验收

| 目标 | 验收方式 |
|---|---|
| G-L1 在 X11 与 Wayland 桌面上流畅播放 1080p30 H.264/H.265 | `examples/play_sdl2` 与 `examples/play_native` 人工 + `tools/verify_e2e.py` 自动 |
| G-L2 支持**嵌入模式**：渲染到调用方给的 `Window`(X11) / `wl_surface`(Wayland) | `examples/play_embed --wid 0x...` |
| G-L3 零第三方也能出画（原生后端只依赖 GL/EGL/X11/Wayland/ALSA，且全部 dlopen） | `ldd libijkpp.so` 不含 `libSDL2` `libGL` `libasound` |
| G-L4 CI 无显卡也能验证（xvfb + llvmpipe） | GitHub Actions `e2e-linux` job |
| G-L5 音画同步 av_diff 稳态 < 20ms | `verify_e2e.py --max-av-diff-ms 20` |
| G-L6 硬解（VAAPI）输出零拷贝送显 | dmabuf 路径 + `stats().zero_copy_frames == total` |
| G-L7 CPU 占用 ≤ ffplay 同场景 × 1.1 | `tests/bench` |
| G-L8 窗口 resize / 切全屏 / 切显示器不崩溃不黑屏 | 压力测试 + 契约测试 |

---

## 2. 双后端策略

| | `platform/sdl2/` | `platform/linux/`（原生） |
|---|---|---|
| 定位 | **快速可用**，开发调试首选，demo 最快出画 | **展示抽象正确性 + 零第三方 + 生产级** |
| 窗口 | SDL2 内建（也可 `SDL_CreateWindowFrom` 嵌入） | X11 / Wayland 原生，**嵌入模式为核心** |
| 视频 | SDL_Renderer（software/GLES）或自己拿 GL context | 自建 OpenGL 3.3 / GLES 3.0 渲染器 |
| 音频 | `SDL_AudioStream` 回调 | ALSA `snd_pcm` / PulseAudio / PipeWire |
| vsync | `SDL_RENDERER_PRESENTVSYNC` | X11 Present 扩展 / Wayland `wp_presentation_feedback` |
| 依赖 | `libSDL2.so` | GL/EGL/X11/Wayland/ALSA/Pulse，**全部 dlopen** |
| 工期 | 1.5 周 | 3 周 |
| 里程碑 | **M11**（第一个可见成果） | **M12** |

**为什么两套都做**（你选的 `both`）：
1. SDL2 后端 1.5 周就能出画，把"能播视频"这个验收点提前，风险前置暴露。
2. 原生后端证明 `VideoRendererSink` / `AudioRendererSink` 抽象是真的通用（只有一个实现时，接口很可能是错的）。
3. 原生后端零第三方依赖，是 SDK 分发友好性的关键（G-L3）。
4. 两个后端跑同一套 `tests/contract/` 契约测试 —— 契约测试能同时通过两个实现，才说明接口设计对了。

### 2.1 后端自动探测（`LinuxBackend::Detect()`）

```cpp
// platform/linux/linux_backend.cc
scoped_refptr<PlatformBackend> LinuxBackend::Detect(const PlayerConfig& config) {
  // 视频
  auto video = [&]() -> scoped_refptr<VideoRendererSinkFactory> {
    switch (config.render.linux_backend) {
      case LinuxVideoBackend::kSdl2: return CreateSdl2Factory();
      case LinuxVideoBackend::kGl:   return CreateGlFactory();
      case LinuxVideoBackend::kAuto: break;
    }
    // Auto: Wayland 会话 > X11 > SDL2
    if (base::Environment::Has("WAYLAND_DISPLAY") && LoadWaylandLibs())
      return CreateGlFactory(WindowSystem::kWayland);
    if (base::Environment::Has("DISPLAY") && LoadX11Libs() && LoadGlLibs())
      return CreateGlFactory(WindowSystem::kX11);
    if (LoadSdl2()) return CreateSdl2Factory();
    return nullptr;   // → headless（NullVideoRendererSink），记 LOG(WARNING)
  }();

  // 音频：PipeWire(通过 Pulse 兼容层) > PulseAudio > ALSA > SDL2 > Null
  auto audio = DetectAudioBackend(config.render.linux_audio);

  if (!video && !audio) return nullptr;
  return base::MakeRefCounted<LinuxPlatformBackend>(std::move(video), std::move(audio));
}
```

**探测失败不报错，而是降级并记日志** —— SDK 的第一原则是"能跑就先跑起来"（见 [docs/10](10-SDK易用性设计.md)）。

---

## 3. `NativeDisplay`：嵌入模式的核心协议

```cpp
// player/public/native_display.h
namespace ijkpp {

enum class NativeDisplayKind {
  kNone,
  kX11Window,          // 载荷：X11WindowHandle{Display*, Window}
  kWaylandSurface,     // 载荷：WaylandSurfaceHandle{wl_display*, wl_surface*}
  kWaylandSurfaceWithDisplay,  // 同上 + 由 ijkpp 拥有 wl_display（自建窗口）
  kSdl2Window,         // 载荷：SDL_Window*
  kDrmMaster,          // 载荷：gbm_device* + drm fd（kiosk 模式）
  kGbmDevice,
  kAndroidNativeWindow, kAndroidSurface,   // 预留
  kCametalLayer, kCaEaglLayer,             // 预留
  kHwnd,                                   // 预留
};

struct X11WindowHandle {
  void* display{nullptr};      // Display*，调用方拥有；nullptr 表示让 ijkpp 自己 XOpenDisplay
  uint64_t window{0};          // Window
  bool ijkpp_owns_display{false};
};

struct WaylandSurfaceHandle {
  void* display{nullptr};      // wl_display*
  void* surface{nullptr};      // wl_surface*
  bool ijkpp_owns_display{false};
  int32_t width{0}, height{0}; // Wayland 无法查询 surface 尺寸，必须由调用方给
};

class IJKPP_PLAYER_EXPORT NativeDisplay {
 public:
  static scoped_refptr<NativeDisplay> FromX11Window(X11WindowHandle handle);
  static scoped_refptr<NativeDisplay> FromWaylandSurface(WaylandSurfaceHandle handle);
  static scoped_refptr<NativeDisplay> FromSdl2Window(void* sdl_window);
  static scoped_refptr<NativeDisplay> FromDrmMaster(int drm_fd, void* gbm_device);
  // 通用逃生舱：自定义类型 + 释放回调
  template <typename T>
  static scoped_refptr<NativeDisplay> Wrap(T* raw, NativeDisplayKind kind,
                                          base::OnceCallback<void(T*)> release);

  NativeDisplayKind kind() const;
  bool valid() const;
  template <typename T> const T* as() const;   // kind 不匹配返回 nullptr
  std::string AsDebugString() const;
};

}  // namespace ijkpp
```

### 3.1 嵌入模式的三条铁律

| # | 规则 | 理由 |
|---|---|---|
| E1 | ijkpp **绝不拥有**调用方的 `wl_display` / `Display`，除非 `ijkpp_owns_display == true` | 避免双重 `wl_display_disconnect` 崩溃 |
| E2 | ijkpp **绝不**创建自己的事件循环去 poll 调用方的 display fd；它只在 render sequence 上按需 `wl_display_dispatch_queue_pending(queue)`（用自己独立的 `wl_event_queue`） | 与宿主事件循环共存的关键 |
| E3 | 所有窗口系统调用都在 sink 的 render sequence 上执行，`SetOutputTarget()` 通过 `PostTask` 转发 | 避免 X11/Wayland 客户端库的线程限制（`wl_proxy` 不是线程安全的） |

**Wayland 嵌入的实现要点**：为 ijkpp 创建独立的 `wl_event_queue`（`wl_display_create_queue`），把 ijkpp 用到的所有 proxy（`wl_surface`、`xdg_surface`、`wp_viewport`、`zwp_linux_dmabuf_v1`…）用 `wl_proxy_set_queue` 绑到这个队列，然后 render sequence 上循环 `wl_display_dispatch_queue_pending(display, queue)`。这样宿主 App 的事件循环与 ijkpp 完全不干扰 —— 这是 Wayland 嵌入最容易做错的地方。

**X11 嵌入的实现要点**：如果 `display` 是调用方的，X11 客户端库默认非线程安全，必须 `XInitThreads()`（且要在**任何** X11 调用之前，只能由调用方或 `GlobalInit()` 调）。ijkpp 提供 `player::GlobalInit()` 里调 `XInitThreads()`，并在文档明确要求"若你自己也用 X11，请确保 `GlobalInit()` 先于你的第一次 X 调用"。

---

## 4. 视频输出：原生 OpenGL 渲染器

### 4.1 目录

```
platform/linux/gl/
├── gl_video_renderer_sink.h/.cc     实现 media::VideoRendererSink
├── gl_video_renderer.h/.cc          实际的绘制逻辑（shader program、VAO、纹理）
├── gl_shaders.h/.cc                 GLSL 源码（内嵌字符串常量）+ 编译/缓存
├── egl_context.h/.cc                EGL display/context/surface（Wayland、GBM、X11-EGL）
├── glx_context.h/.cc                GLX context（X11 传统路径）
├── gl_loader.h/.cc                  dlopen("libGL.so.1"/"libEGL.so.1") + 函数指针表
├── texture_uploader.h/.cc           三条上传路径的选择与实现
└── color_space_shader.h/.cc         YUV→RGB 矩阵与 transfer function 生成

platform/linux/window/
├── window_surface.h                 抽象：CreateContext / SwapBuffers / GetVsyncInterval / Resize
├── x11_surface.h/.cc                X11 + GLX/EGL + Present 扩展 + XShm
├── wayland_surface.h/.cc            wl_surface + xdg-shell + viewporter + presentation-time
│                                    + linux-dmabuf + fractional-scale
└── present_extension.h/.cc          X11 Present / Wayland wp_presentation_feedback 统一封装

platform/linux/zero_copy/
├── dmabuf_video_frame.h/.cc         media::VideoFrame 的 dmabuf StorageType 实现
└── egl_image_importer.h/.cc         dmabuf → EGLImage → GL texture
```

### 4.2 纹理上传的三条路径（按优先级）

| 路径 | 触发条件 | 开销 | 说明 |
|---|---|---|---|
| **① dmabuf 零拷贝** | `frame->storage_type() == kStorageDmaBufs`（VAAPI/硬解输出）且 EGL 有 `EGL_EXT_image_dma_buf_import`（+ `MODIFIERS` 扩展更佳） | **~0** | `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` → `glEGLImageTargetTexture2DOES`。需要处理 DRM fourcc modifier（NVIDIA/Intel 各不相同） |
| **② PBO 异步上传** | CPU 帧 + GL 3.0+ | 一次 DMA | 双缓冲 PBO：帧 N 上传时用 PBO A，帧 N+1 用 PBO B，`glMapBufferRange(GL_MAP_WRITE_BIT\|GL_MAP_INVALIDATE_BUFFER_BIT\|GL_MAP_UNSYNCHRONIZED_BIT)` 避免同步停顿 |
| **③ 直接 `glTexSubImage2D`** | 兜底 | 一次拷贝 + 可能同步 | 逐平面上传；I420 三张 `GL_R8` 纹理，NV12 两张（`GL_R8` + `GL_RG8`） |

X11 上还有 **④ XShm** 路径（不走 GL，直接 `XShmPutImage`），用于软件渲染/低端机，但会牺牲缩放质量，默认关闭（`config.render.prefer_xshm = false`）。

### 4.3 Shader 与色彩空间

```glsl
// 内嵌 GLSL（platform/linux/gl/gl_shaders.cc）
// YUV → RGB：矩阵由 VideoColorSpace 在 CPU 侧算好，作为 uniform 传入
#version 330 core
in  vec2 a_tex_coord;
out vec4 frag_color;
uniform sampler2D u_y;
uniform sampler2D u_u;   // NV12 时为 u_uv（RG）
uniform sampler2D u_v;
uniform mat3      u_yuv_to_rgb;
uniform vec3      u_yuv_offset;
uniform int       u_is_nv12;
uniform vec2      u_chroma_scale;   // 通常 0.5，4:2:2 时 (1.0, 0.5)
uniform float     u_alpha;

void main() {
  vec3 yuv;
  yuv.x = texture(u_y, a_tex_coord).r;
  if (u_is_nv12 == 1) {
    vec2 uv = texture(u_u, a_tex_coord * u_chroma_scale).rg;
    yuv.yz = uv;
  } else {
    yuv.y = texture(u_u, a_tex_coord * u_chroma_scale).r;
    yuv.z = texture(u_v, a_tex_coord * u_chroma_scale).r;
  }
  vec3 rgb = u_yuv_to_rgb * (yuv - u_yuv_offset);
  frag_color = vec4(rgb, u_alpha);
}
```

`u_yuv_to_rgb` 由 `ColorSpaceShader::BuildMatrix(const VideoColorSpace&)` 生成，支持：

| primaries | transfer | matrix | range |
|---|---|---|---|
| BT.601 (SD) | BT.709 / sRGB | BT.601 (625/525) | Limited / Full |
| BT.709 (HD) | BT.709 / sRGB | BT.709 | Limited / Full |
| BT.2020 (HDR) | PQ (SMPTE 2084) / HLG / BT.709 | BT.2020 ncl | Limited / Full |

**HDR 处理策略**：首期不做完整 HDR tone mapping（那需要显示器 EDID + `GL_EXT_output_DRR` 之类，工程量大）。做法是：
- 检测到 PQ/HLG transfer → `LOG(WARNING)` + 发 `kDecoderFallback`? 不，改为发一个 `PlayerEvent::kHdrUnsupported`（🆕），并按 BT.2020→BT.709 做简易 tone mapping（Hable/ACES 近似），保证画面不惨白
- `config.video.hdr_tone_mapping = kNone | kSimple | kAuto`，默认 `kSimple`
- 完整 HDR 列为 M14+ 的可选增强

### 4.4 布局与 letterbox

`DisplayGeometry::ComputeLayout(natural_size, sar, rotation, target_rect, scale_mode)` 是**纯函数**，返回纹理坐标 + 顶点位置，可穷举单测（对应原版 `calculate_display_rect`）：

```cpp
enum class ScaleMode { kContain, kCover, kFill, kNone };
struct Layout {
  std::array<std::array<float, 2>, 4> vertices;     // NDC
  std::array<std::array<float, 2>, 4> tex_coords;   // 已含 rotation/flip
  gfx::Rect viewport;
};
Layout ComputeLayout(gfx::Size natural_size, Rational sar, int rotation_degrees,
                     gfx::Rect target, ScaleMode mode);
```

### 4.5 vsync 与 `Render(deadline_min, deadline_max)`

`VideoRendererSink::RenderCallback::Render()` 需要 sink 给出显示时间窗口。三条实现路径：

| 窗口系统 | 机制 | 精度 |
|---|---|---|
| **X11** | `XPresentQueryVersion` + `XPresentSelectInput(window, PresentNotifyMSC)` → 收 `PresentCompleteNotify` 拿 MSC/UST；用 `XPresentNotifyMSC` 请求在指定 MSC 显示 | µs 级（有 Present 扩展）；无扩展时退化为 `glXSwapIntervalEXT(1)` + 定时器估算 |
| **Wayland** | `wp_presentation_feedback` 的 `sync_output` + `presented` 回调给出精确的 `tv_sec/tv_nsec/refresh`；`wl_surface.frame` 回调作为节奏源 | µs 级（有 `presentation-time` 协议） |
| **SDL2** | `SDL_RENDERER_PRESENTVSYNC` + `SDL_GetWindowDisplayMode().refresh_rate` | ms 级 |

`deadline_min = 上一个 vsync 时刻`，`deadline_max = 下一个 vsync 时刻`。`VideoFrameCompositor::Render()` 在这两个时刻之间选 `reference_time` 落入窗口的帧，否则返回上一帧（`kRepeat`）。这正是 Chromium 的做法，也是 Δ18 的落地。

**`GetDisplayInterval()`** 返回刷新周期（60Hz → 16.67ms），供 `VideoFrameCompositor` 做 fps cap（`config.video.max_fps`）。

### 4.6 Wayland 特有事项（易错清单）

| 事项 | 处理 |
|---|---|
| 无法查询 surface 尺寸 | 由 `wp_viewport` + `wl_surface.commit` 后的 `xdg_toplevel.configure(w,h)` 回调更新；`WaylandSurfaceHandle.width/height` 作为初始值 |
| 高分屏缩放 | `wp_fractional_scale_v1`（新）或 `wl_output.scale`（旧，整数）→ 更新 viewport 目标尺寸 |
| 服务器端装饰 | `xdg-decoration-unstable-v1` 请求 SSD；失败则不画标题栏（嵌入模式本来就没有） |
| dmabuf 格式协商 | `zwp_linux_dmabuf_v1.get_formats` + `get_modifiers`；老版本只有 `get_formats` 无 modifier → 用 `DRM_FORMAT_MOD_INVALID` |
| 事件队列隔离 | 见 §3.1 E2 |
| 没有全局坐标 | 不需要（我们只画自己的 surface） |
| `wl_display_roundtrip` 阻塞 | 只在初始化和 resize 时用，render 循环里用 `dispatch_queue_pending` |

---

## 5. 音频输出

### 5.1 后端矩阵

| 后端 | 实现 | 延迟查询 | 优先级 |
|---|---|---|---|
| **PipeWire** | 通过 PulseAudio 兼容层（`libpulse`） | `pa_stream_get_latency` | 1（现代发行版默认） |
| **PulseAudio** | `libpulse-simple`（`pa_simple_write`）或 async `pa_stream` | `pa_stream_get_latency` | 2 |
| **ALSA** | `snd_pcm_*` 直接 | `snd_pcm_status_get_delay` | 3 |
| **SDL2** | `SDL_OpenAudioDevice` + 回调 | `SDL_GetQueuedAudioSize` 换算 | 4（若已链 SDL2） |
| **Null** | 静音消费，按墙钟推进 | 固定 `buffer_duration` | 兜底 |

**实现决策**：PulseAudio 用 **async API（`pa_stream`）而非 simple API**，因为 simple API 无法查询硬件延迟，而 `AudioRendererSink::RenderCallback::Render(delay, delay_timestamp, ...)` 的 `delay` 参数是音频时钟校正的关键（缺了它 av_diff 会有 20–80ms 的系统性偏差）。

### 5.2 `AudioRendererSink` 实现骨架

```cpp
// platform/linux/audio/alsa_audio_renderer_sink.cc
class AlsaAudioRendererSink final : public RestartableAudioRendererSink {
 public:
  // AudioRendererSink:
  void Initialize(const AudioParameters& params, RenderCallback* callback) override;
  void Start() override;      // 启动 "ijkpp-alsa" 线程
  void Stop() override;       // join，保证返回后不再调 Render()（Chromium 契约 R13）
  void Pause() override;      // snd_pcm_pause
  void Play() override;
  void Flush() override;      // snd_pcm_drop + prepare（★seek 必须，否则听到旧声音）
  bool SetVolume(double volume) override;   // 软件音量：交给 AudioRendererImpl 做
  bool IsOptimizedForHardwareParameters() override { return false; }
  bool CurrentThreadIsRenderingThread() override;

 private:
  void RenderThread();
  // ALSA 要求 period_size 为 2 的幂或特定值；协商后可能与请求不同
  AudioParameters NegotiateParameters(const AudioParameters& requested);
  base::TimeDelta QueryHardwareLatency() const;   // snd_pcm_status_get_delay

  snd_pcm_t* pcm_{nullptr};
  AudioParameters params_;
  raw_ptr<RenderCallback> callback_;              // 非拥有
  base::Thread render_thread_{"ijkpp-alsa"};
  base::AtomicFlag stop_flag_;
  // ...
};

void AlsaAudioRendererSink::RenderThread() {
  std::vector<float> interleaved(params_.frames_per_buffer() * params_.channels());
  auto bus = AudioBus::Create(params_.channels(), params_.frames_per_buffer());
  while (!stop_flag_.IsSet()) {
    const base::TimeDelta delay = QueryHardwareLatency();
    const base::TimeTicks delay_ts = tick_clock_->NowTicks();
    bus->Zero();
    const int filled = callback_->Render(delay, delay_ts, glitch_info_, bus.get());
    if (filled < bus->frames()) {
      ++glitch_info_.total_glitches;      // ★爆音可观测（对齐 Chromium）
      glitch_info_.total_glitch_duration += base::Microseconds(
          (bus->frames() - filled) * 1'000'000 / params_.sample_rate());
    }
    // float planar → interleaved s16/s32f
    AudioBus::Interleave(bus.get(), interleaved.data(), params_.sample_format());
    snd_pcm_sframes_t written = snd_pcm_writei(pcm_, interleaved.data(), filled);
    if (written == -EPIPE) { snd_pcm_prepare(pcm_); ++glitch_info_.xruns; }
    else if (written < 0)  { OnError(written); break; }
  }
}
```

**关键点**：`Render()` 回调在 `ijkpp-alsa` 线程上执行，`AudioRendererImpl` 的实现只做 `AudioRendererAlgorithm::FillBuffer` + `Scale`（见 [04 §6.3](04-线程模型与数据流.md)），预算 < 100µs。契约测试 `AudioSinkContract.RenderCallbackBudget` 会实测并断言。

### 5.3 采样率协商

`AudioParameters` 请求值 vs 设备实际值可能不同（尤其 ALSA 直连时）。规则：
1. 请求 `config.audio` 推导出的参数（通常 = 流的采样率，或 48000）
2. 后端 `NegotiateParameters()` 尝试打开；失败则按 48000 → 44100 → 设备默认 依次重试
3. 最终参数回传给 `AudioRendererImpl`，由 `FFmpegAudioConverter`（swr）做重采样适配
4. 若 `IsOptimizedForHardwareParameters()` 返回 true（SDL2 后端），则直接用设备参数，省一次重采样

---

## 6. dlopen 弱依赖（G-L3 的实现）

```cpp
// platform/linux/gl/gl_loader.cc
namespace {
struct GlApi {
  decltype(&glGenTextures) glGenTextures{};
  decltype(&glTexSubImage2D) glTexSubImage2D{};
  // ... 约 40 个函数指针
};
GlApi g_gl;
base::ScopedLibrary g_lib_gl;
}  // namespace

bool LoadGl() {
  static const char* kCandidates[] = {"libGL.so.1", "libGL.so", "libGLESv2.so.2"};
  for (const char* name : kCandidates) {
    if (g_lib_gl.Open(name)) break;
  }
  if (!g_lib_gl.is_open()) return false;
  #define LOAD(sym) g_gl.sym = reinterpret_cast<decltype(g_gl.sym)>(g_lib_gl.Symbol(#sym)); \
                    if (!g_gl.sym) return false;
  LOAD(glGenTextures) LOAD(glTexSubImage2D) /* ... */
  #undef LOAD
  return true;
}
```

对每个平台库都做同样处理：`libEGL.so.1`、`libX11.so.6`、`libXext.so.6`、`libXpresent.so.1`、`libwayland-client.so.0`、`libwayland-egl.so.1`、`libxkbcommon.so.0`、`libasound.so.2`、`libpulse-simple.so.0`、`libpulse.so.0`、`libSDL2-2.0.so.0`。

**收益**：
- 单个 `libijkpp.so` 在 minimal 容器（无 X11）里也能加载运行（headless 模式），不会因 `cannot open shared object file` 直接挂掉
- SDK 分发时不需要声明一堆 `Depends:`
- `ldd libijkpp.so` 只剩 `libc / libstdc++ / libm / libdl / libpthread`（+ 若静态链 FFmpeg 则无 FFmpeg）

**代价**：约 600 行 loader 样板代码 + 每个函数调用多一次间接跳转（可忽略，且都是冷路径）。

`IJKPP_LINUX_LINK_RUNTIME=OFF` 时改为直接链接，适合发行版打包（依赖明确、启动稍快）。

---

## 7. 零拷贝路径（G-L6）

```
VAAPI 解码（可选，M14+）
  av_hwframe_transfer_data 不再调用
  → AVFrame 持有 AVDRMFrameDescriptor（AV_PIX_FMT_DRM_PRIME）
  → platform/ffmpeg 包成 VideoFrame{storage_type = kStorageDmaBufs,
                                    dmabuf_descriptor = {fd[4], offset[4], stride[4], modifier[4], fourcc}}
  → VideoRendererSink::RenderCallback::Render()
  → eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, ...) → glEGLImageTargetTexture2DOES
  → 绘制
  → 帧被 drop 时 dmabuf fd 随 VideoFrame 析构关闭（scoped fd）

结果：1080p NV12 每帧省掉 ~3MB 的 GPU→CPU→GPU 往返，CPU 占用降 30–50%
```

首期（M11/M12）**不启用 VAAPI**（软解 + PBO 上传已经够 1080p60），但 `VideoFrame::StorageType::kStorageDmaBufs` 与 `dmabuf_video_frame.cc` 的骨架要留好，M14 加 `VaapiVideoDecoder` 时不用改接口。

---

## 8. `examples/` 设计（Linux 验收载体）

### 8.1 `examples/play_sdl2/`（M11 交付）

```bash
./play_sdl2 --url video.mp4 [选项]
  --duration <t>          播放 t 后退出（CI 用）
  --stats-json <path>     退出时写 PlaybackStats JSON（verify_e2e.py 用）
  --sink null             强制 headless
  --backend sdl2|gl|auto  强制后端
  --audio alsa|pulse|sdl2|null
  --loop <n>
  --rate <f>
  --log-level trace|debug|info|warn|error
  --dump-diagnostics      Ctrl-C 时输出诊断 JSON
  --exit-code-on-error    播放出错时返回非 0（CI 用）

交互键（内建窗口模式）：
  Space 暂停/继续   ←/→ seek ∓10s   ↑/↓ 音量   [ ] 变速   f 全屏   s 截图
  i     在 stdout 打印 PlaybackStats   d 打印 DumpDiagnostics()   q 退出
```

`main.cc` 约 250 行（含 SDL2 窗口与事件循环）。这是**验证 SDK 易用性的活文档** —— 如果 `main.cc` 超过 400 行，说明 API 不够好用，要回头改 `player/public/`。

### 8.2 `examples/play_native/`（M12 交付）

同 CLI，但用原生 GL 后端 + 自建 X11/Wayland 窗口。约 400 行（窗口创建比 SDL2 繁琐，这部分代码本身就是"为什么要有 SDL2 后端"的说明）。

### 8.3 `examples/play_embed/`（★嵌入模式验收）

```bash
# 模式 A：宿主是 X11 程序
./play_embed --wid 0x04600007 --url video.mp4
# 模式 B：宿主是 Wayland 程序（通过 zwp_xwayland 或自己传 wl_surface 指针）
./play_embed --wl-surface 0x5583a2b1c400 --wl-display 0x5583a2b0e200 \
             --width 640 --height 360 --url video.mp4
```

`main.cc` 里演示了三种宿主形态：
1. 纯 C 宿主（`extern "C"` 调 ijkpp，验证头文件在 C++ 之外不炸 —— 实际是 `--wid` 传入，ijkpp 只当句柄用）
2. 宿主自己有 X11 事件循环（验证 E1/E2/E3 铁律）
3. 宿主是 Qt/GTK（用一个最小 GTK4 窗口演示，可选编译）

### 8.4 `examples/headless/` + `examples/stats_dump/` + `examples/ijkpp_inspect/`

见 [07 §11.3](07-测试策略与可观测性.md) 与 [10 §7](10-SDK易用性设计.md)。

---

## 9. Linux 验收自动化

`tools/verify_e2e.py`（CI 用）：

```python
#!/usr/bin/env python3
"""Verify a playback run from its --stats-json output."""
# 断言项：
#   frames_presented >= --min-frames
#   |av_diff| p99 <= --max-av-diff-ms
#   audio_glitches.count <= --max-glitches
#   drop_frame_rate <= --max-drop-rate
#   exit_code == 0
#   no LOG(ERROR) lines matching --forbid-log-regex
#   first_frame_latency <= --max-first-frame-ms
#   rss_peak <= --max-rss-mb
```

CI 里跑：

```bash
xvfb-run -a -s "-screen 0 1280x720x24" ./play_sdl2 --url t.mp4 --duration 5s \
  --stats-json /tmp/a.json --exit-code-on-error
python3 tools/verify_e2e.py /tmp/a.json --min-frames 140 --max-av-diff-ms 40 \
  --max-glitches 2 --max-first-frame-ms 1500

export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
xvfb-run -a -s "-screen 0 1280x720x24" ./play_native --url t.mp4 --duration 5s \
  --stats-json /tmp/b.json --exit-code-on-error
python3 tools/verify_e2e.py /tmp/b.json --min-frames 100 --max-av-diff-ms 60
```

llvmpipe 软渲染下阈值放宽（`--min-frames 100`、`--max-av-diff-ms 60`），因为 CPU 光栅化会拖慢 present。**关键是"能跑通且不崩"，性能阈值在真机基准里另测。**

---

## 10. 已知 Linux 坑与对策

| # | 坑 | 对策 |
|---|---|---|
| L1 | X11 客户端库非线程安全，宿主也调 X11 → 崩溃 | `GlobalInit()` 调 `XInitThreads()`；文档明确要求先于宿主第一次 X 调用；提供 `config.render.x11_thread_safe_already_init` 让宿主声明已初始化 |
| L2 | Wayland `wl_proxy` 不是线程安全 | §3.1 E2：独立 `wl_event_queue` |
| L3 | NVIDIA 专有驱动的 GLX 与 Mesa 行为差异（`glXSwapIntervalEXT` vs `MESA_swap_control`） | 用 `GLX_EXT_swap_control` → `GLX_MESA_swap_control` → `GLX_SGI_swap_control` 三级回退 |
| L4 | dmabuf modifier 各家不同（Intel/Igalia/NVIDIA） | `eglQueryDmaBufModifiersEXT` 枚举，不硬编码；无 modifier 扩展时用 `DRM_FORMAT_MOD_INVALID` |
| L5 | PipeWire 通过 Pulse 兼容层时延迟查询不准（多一跳） | 检测 `XDG_SESSION_TYPE` + `pipewire --version`；若走 pw-pulse 则额外补偿 ~10ms，或直接用 PipeWire 原生 API（`IJKPP_LINUX_USE_PIPEWIRE`） |
| L6 | ALSA `snd_pcm_writei` 返回 `-EPIPE`（xrun） | `snd_pcm_prepare()` 重开 + 计入 `glitch_info_.xruns` + 通知 `AvSyncController` 重置音频时钟 |
| L7 | 显示器刷新率变化（省电模式 60→30Hz、多显示器不同刷新率） | 监听 X11 `RRScreenChangeNotify` / Wayland `wl_output.mode`，更新 `GetDisplayInterval()` 并通知 compositor |
| L8 | DPI 缩放（X11 分数缩放不存在、Wayland fractional-scale） | `DisplayGeometry` 用像素坐标；Wayland 用 `wp_fractional_scale_v1`，X11 用 `Xft.dpi` 资源提示（仅影响内建窗口的初始大小） |
| L9 | 容器/CI 无 GPU、无 X server | xvfb + llvmpipe（§9）；`NullVideoRendererSink` 兜底 |
| L10 | `libGL.so.1` 在某些 minimal 镜像里叫 `libGL.so` 或不存在 | dlopen 多候选名 + 优雅降级到 NullSink（§6） |
| L11 | 宿主进程 fork 后 GL context 失效 | 文档明确：不支持 fork-after-init；`GlobalInit()` 注册 `pthread_atfork` 在子进程标记 backend 失效并 LOG(FATAL) |
| L12 | 长时间播放后 fd 泄漏（dmabuf / wl_buffer / XShm） | `FdGuard` 类 + 压力测试采样 `/proc/self/fd` 数量（[07 §9](07-测试策略与可观测性.md)） |
| L13 | Wayland 下 `wl_display_dispatch` 与 render 循环竞争导致 100% CPU | 用 `wl_display_prepare_read_queue` / `read_events` / `dispatch_queue_pending` 三段式，配合 `poll()` 超时而非忙等 |
| L14 | SDL2 后端在 Wayland 上默认走 XWayland，模糊 | 检测并 `SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR, ...)`；文档说明可用原生后端获得清晰渲染 |

---

## 11. Linux 后端契约测试

两个后端跑同一套（见 [07 §3](07-测试策略与可观测性.md)）：

```cpp
INSTANTIATE_TEST_SUITE_P(AllVideoSinks, VideoRendererSinkContract,
    ::testing::Values(
        MakeNullSink,
        MakeSdl2Sink,
        MakeGlX11Sink,          // GTEST_SKIP() if !getenv("DISPLAY")
        MakeGlWaylandSink));    // GTEST_SKIP() if !getenv("WAYLAND_DISPLAY")

INSTANTIATE_TEST_SUITE_P(AllAudioSinks, AudioRendererSinkContract,
    ::testing::Values(MakeNullSink, MakeSdl2Sink, MakeAlsaSink, MakePulseSink));
```

CI 的 `e2e-linux` job 用 `xvfb-run` + 真实 ALSA（`snd-dummy` 内核模块或 `apulse`）跑完整契约套件 —— **契约测试同时通过 4 个视频后端实现，才算 `VideoRendererSink` 抽象是对的**。

---

## 12. 工期分解

| 子任务 | 工期 | 里程碑 |
|---|---|---|
| `platform/sdl2/` 全部（video sink + audio sink + window + GL renderer） | 1.5 周 | **M11** |
| `examples/play_sdl2` + `verify_e2e.py` + CI e2e job | 0.5 周 | M11 |
| `platform/linux/gl/`（loader + context + renderer + shaders + uploader） | 1.5 周 | **M12** |
| `platform/linux/window/x11_surface` + Present 扩展 | 0.7 周 | M12 |
| `platform/linux/window/wayland_surface` + 协议生成 + 4 个扩展 | 1.0 周 | M12 |
| `platform/linux/audio/`（ALSA + Pulse + Null） | 0.8 周 | M12 |
| `examples/play_native` + `play_embed` | 0.5 周 | M12 |
| dmabuf 零拷贝骨架（不含 VAAPI 解码器） | 0.3 周 | M12 |
| 契约测试适配 + 真机验收 | 0.5 周 | M12 |
| **小计** | **7.3 周** | |

后续（不在核心范围）：`VaapiVideoDecoder` + dmabuf 全链路（1.5 周，M14）、PipeWire 原生（0.5 周）、完整 HDR（1.5 周）。

---

返回：[README](../README.md)
