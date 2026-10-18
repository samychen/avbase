# EXTENDING · 扩展 avbase

> 本篇回答「我要换掉/加进 avbase 的某一环，该实现什么、注入到哪」。
> 所有签名摘自 `player/public/` 与 `media/base/` 头文件原文（2026-10-18 校对）。
> 分层铁律先读这里：**C28 —— `render/` 不得 include `media`/`platform`/`base`/`player`**；
> `platform/` 只装「被宿主选中」的代码；可移植代码不进 `platform/`
> （FFmpeg→`media/ffmpeg/`，GL→`render/gl/`，docs/02 §6）。

---

## 1. 扩展点地图

| 你想换/加的 | 实现的接口 | 注入位置 |
|---|---|---|
| 字节来源（加密流/自带下载器/缓存） | `media::DataSource` | `Deps::data_source` |
| 视频解码器 | `media::VideoDecoder` + `VideoDecoderFactory` | `Deps::video_decoder_factories` |
| 音频解码器 | `media::AudioDecoder` + `AudioDecoderFactory` | `Deps::audio_decoder_factories` |
| 字幕解码器 | `media::TextDecoder` + `TextDecoderFactory` | `Deps::text_decoder_factory` |
| 视频输出 | `media::VideoRendererSink` + `VideoRendererSinkFactory` | `Deps::video_sink_factory` |
| 音频输出 | `media::AudioRendererSink` + `AudioRendererSinkFactory` | `Deps::audio_sink_factory` |
| 窗口/表面 | `NativeDisplay::From*/Wrap` | `Player::SetVideoSurface()` |
| 事件循环 | `EventDispatcher` | `Deps::event_dispatcher` |
| 日志 | `base::logging::LoggingDelegate` | `base::logging::SetDelegate()` / `Deps::logging_delegate` |
| 时钟 | `base::TickClock` | `Deps::tick_clock` |
| 选项 | `tools/gen_options.py` 重新生成 | 树内开发，非运行时 |

所有 `Deps` 字段都可以为 null——为 null 时用当前平台的自动探测默认值，
这就是 `Player player; player.SetDataSource(url);` 零配置能播的原因（E1）。
工厂是 `std::vector` 的，**顺序即优先级**：渲染器按 `SupportsCodec` 命中顺序选。

---

## 2. 自定义 DataSource

`media/base/data_source.h`（SDK 侧经 `deps.h` 前向声明使用）：

```cpp
class AVBASE_MEDIA_EXPORT DataSource
    : public base::RefCountedThreadSafe<DataSource> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();  // 用 base::MakeRefCounted 造

  // 宿主可以暂停/恢复 avbase 的读请求（弱网背压用）。
  class Host {
   public:
    virtual void RequestAdditionalBytes(size_t bytes) = 0;
    virtual void SetDeferring(bool deferred) = 0;
   protected: virtual ~Host() = default;
  };

  virtual void SetHost(Host* host) = 0;
  // 异步读：结果经 callback 返回（ReadResult = expected<int, MediaError>，
  // 值是读到的字节数，<0 是错误）。绝对偏移，调用方保证 [offset, offset+size)
  // 不越界（GetSize 界内）。
  virtual void Read(int64_t offset, size_t size, uint8_t* data,
                    base::OnceCallback<void(ReadResult)> callback) = 0;
  virtual ReadResult ReadBlocking(int64_t offset, size_t size,
                                  uint8_t* data) = 0;  // 可选实现
  virtual void Abort() = 0;
  virtual bool GetSize(int64_t* size_out) = 0;
  virtual bool IsStreaming() const = 0;
  virtual void SetBitrate(int bitrate) = 0;
  virtual bool IsSeekable() const = 0;
};
```

注入与装饰器模式：

```cpp
auto deps = std::make_unique<avbase::Deps>();
deps->data_source = base::MakeRefCounted<media::MyDecryptDataSource>(
    base::MakeRefCounted<media::FileDataSource>(path), key);
avbase::Player player(config, std::move(deps));
```

装饰器（包装另一个 `scoped_refptr<DataSource>`）是官方推荐的复合方式：
M9 的 `RetryDataSource`（重试/重连）就是这个形状；M18 的 `CacheDataSource`
（边下边播）后置于同一接口。**ijkplayer 的 `ijkurlhook` URL 重写在 0.1.0
没有对应组件**（见 MIGRATION Δ11），需要的业务按这个接口自己包一层。

## 3. 自定义解码器

三个解码器接口同形（异步完成 + 回调），以视频为例：

```cpp
class AVBASE_MEDIA_EXPORT VideoDecoder {
 public:
  using InitCB = base::OnceCallback<void(DecoderStatus)>;
  // 每解出一帧调一次；必须尽快调用，不要为它起线程跳转——高帧率素材上
  // 每帧多一跳会可测量地伤害送显时机。
  using OutputCB = base::RepeatingCallback<void(
      base::scoped_refptr<VideoFrame>)>;
  using DecodeCB = base::OnceCallback<void(DecoderStatus)>;

  // 初始化契约（与 Chromium 逐字一致）：重初始化丢弃内部缓冲帧；decode/reset
  // 未决时不得调用；init_cb 之前不得做任何调用；init_cb 可能在本方法返回前跑。
  virtual void Initialize(const VideoDecoderConfig& config, bool low_delay,
                          CdmContext* cdm_context, InitCB init_cb,
                          const OutputCB& output_cb,
                          const WaitingCB& waiting_cb) = 0;
  // decode_cb 保证不在 Decode() 内联调用、且即使不再 Decode 也会跑；
  // EOS 缓冲先冲刷完所有 output_cb 再跑 decode_cb。
  virtual void Decode(base::scoped_refptr<DecoderBuffer> buffer,
                      DecodeCB decode_cb) = 0;
  virtual void Reset(base::OnceClosure closure) = 0;      //  flush 后回调
  virtual bool NeedsBitstreamConversion() const;
  virtual bool CanReadWithoutStalling() const;
  virtual int GetMaxDecodeRequests() const;               // 0 = 不限
  virtual VideoDecoderType GetDecoderType() const = 0;    // 枚举进表
  virtual const char* name() const = 0;
};
```

配套工厂（`media/base/video_decoder_factory.h`）：

```cpp
virtual VideoDecoderCapability GetCapability() const = 0;  // 分辨率/码率上限
virtual bool SupportsCodec(VideoDecoderType type_hint) const = 0;
virtual std::unique_ptr<VideoDecoder>
CreateVideoDecoder(const VideoDecoderConfig& config) = 0;
virtual const char* name() const = 0;
```

要点：
- 工厂与解码器都是 `RefCountedThreadSafe`，构造用 `base::MakeRefCounted`，
  `Deps` 字段持 `scoped_refptr`（**不能**用 `shared_ptr`——双重计数会 double
  free，deps.h 的所有权注释有完整说明）。
- `Initialize`/`Decode`/`Reset` 全部异步：**不要在调用线程上阻塞等待**，
  完成必须走 callback（Chromium 同款契约）。
- 硬件解码平台表外置在 `platform/hwaccel/`（E5b）：加一种硬解 = 加一张平台表
  + 一个 `VideoDecoderType` 枚举值，不要改选择逻辑。
- 硬解输出无 CPU 像素的帧用 `StorageType::kStorageOpaque` /
  `kStorageDmaBufs` 标志（Δ21），不要试图塞 `AVFrame`——public 头里根本没有它。

## 4. 自定义 Sink

**视频**（`media/base/video_renderer_sink.h`）：

```cpp
class VideoRendererSink {
 public:
  class RenderCallback {
    // 返回要显示的帧；nullptr = 保持上一帧。deadline_min/max 括住送显窗口，
    // deadline_max 为空表示 sink 无 vsync 信息（Δ18）。
    virtual base::scoped_refptr<VideoFrame>
    Render(base::TimeTicks deadline_min, base::TimeTicks deadline_max) = 0;
    virtual void OnFrameSubmitFailure() = 0;
  };
  virtual void Initialize(RenderCallback* callback) = 0;
  virtual void Start() = 0;  virtual void Stop() = 0;
  virtual void Pause() = 0;  virtual void Play() = 0;
  virtual void Flush() = 0;  // 仅非播放态合法
  virtual void SetOutputTarget(base::scoped_refptr<NativeDisplay>) = 0;
  virtual bool IsRunning() const = 0;
  virtual bool GetDisplayInterval(base::TimeDelta*) const = 0;  // vsync 周期
  virtual VideoSinkStats GetStats() const = 0;
  virtual const char* name() const = 0;
};
```

**音频**（`media/base/audio_renderer_sink.h`）——回调契约最严的一条：

```cpp
// 运行在 sink 的音频线程：MUST NOT block / allocate / 取媒体序列持有的锁 /
// 回调 Player。预算 < 100 µs（docs/04 §8；无专属契约测试，见 §11）。返回写出
// 的帧数，0 = 播静音。
virtual int Render(base::TimeDelta delay, base::TimeTicks delay_timestamp,
                   const AudioGlitchInfo& glitch_info, AudioBus* dest) = 0;
```

注入：实现工厂（`Create(NativeDisplay)`）塞进 `Deps::video_sink_factory` /
`audio_sink_factory`。为 null 时按平台自动探测（Linux：Wayland > X11 > SDL2；
音频：ALSA/Pulse/PipeWire/SDL2）。`render.disable_video_output`（nodisp）会
**压过**宿主注入的 sink factory 选 `NullVideoSink`——想强制不出画用它。

窗口句柄用 `NativeDisplay`（类型擦除，media 层不 include X11/Wayland/SDL 头）：

```cpp
auto display = media::NativeDisplay::FromX11Window({xdisplay, window, false});
player.SetVideoSurface(display);            // 播放中可调
// 自有窗口抽象：Wrap(raw, kind, release)；release 在最后一个引用消失时于
// sink 渲染序列执行。
auto mine = media::NativeDisplay::Wrap(ptr, media::NativeDisplayKind::kGlfwWindow,
                                       [](void* p) { MyRelease(p); });
```

`NativeDisplay::None()` 是合法的「渲染到无处」目标（sink 进 discard 模式，
音频继续——后台播放用）。

## 5. 平台后端与 GL

- `render/gl/` 是**独立模块**（glad2 内嵌、零 media 依赖，C28），可以单独使用：
  见 [13-GL模块独立使用指南](13-GL模块独立使用指南.md)。
- 加一个新后端（比如 Win32）：`platform/win32/` 下实现 sink 工厂 + 后端选择，
  经 `render.linux_backend`/平台等价键接入自动探测链；**可移植代码不要放进
  `platform/`**（会被门禁按目录规则拦）。
- 硬解零拷贝导入（IOSurface/VAAPI dmabuf）的接缝在 `media/gl` 的 import 接口 +
  `platform/hwimport` 的 macOS 实现；加平台 = 在 `platform/hwimport/` 加一个
  后端，接口不动。

## 6. 选项系统：加一个新选项

字符串 key 与强类型字段由**同一个生成器**保证不漂移（规则 C13）：

```
docs/05 表 4（规格）  +  player/public/player_config.h（代码）
                \      /
         tools/option_map.py（映射与换算：KIND_VOLUME_PERCENT 等）
                /
   tools/gen_options.py --emit   → player/option_registry.inc
   tools/gen_options.py --check  → 三处对账，CI-safe，漂移即非零退出
```

加选项步骤：① `PlayerConfig` 加字段（带默认值与注释 key 名）→ ②
`tools/option_map.py` 登记（kind/范围/默认/换算）→ ③ `--emit` 重新生成 →
④ `--check` 对账。**不要手改 `option_registry.inc`**——它是生成物；
Δ2 之后未登记 key 直接报错，漏登记的后果是业务配置迁移时炸出来。

## 7. 事件：三种接入形态

1. `SetEventHandler(RepeatingCallback<void(const PlayerEvent&)>)`：单回调，
   S8 单线程串行派发，回调内可调 Player 任何方法（不持锁）。
2. `AddObserver(PlayerObserver*)` 返回 RAII `Subscription`：只覆写关心的事件
   （`OnPrepared`/`OnError`/…，未覆写走 `OnEvent` 兜底）。
3. `Deps::event_dispatcher`：把事件 post 到**宿主自己的 loop**（Android
   Looper / GLib main context / Qt）。默认是专用 FIFO 线程，保证顺序且不阻塞
   任何管线序列。切到宿主 loop 后，回调线程语义 = 宿主的 loop 线程。

旧消息 handler 不想改：`ToLegacyEvent(event, &out)` 输出 `(what, arg1, arg2,
obj)`，与 ijkplayer 的 `FFP_MSG_*` 数值一致（05 表 5 的映射即来源）。

## 8. 合成层（多源/多图层）

`player/public/scene.h`（docs/14 阶段 3 的产品化面）：

- `Player::scene()` 永不为 null、永不被失效：注册一次的 source 跨
  `Stop()/Reset()/re-Prepare()` 存活。
- `Player::AddSource(descriptor, name, z)`：注册**次源**（同一管线的第二个
  demuxer，共用一个时钟），返回 `shared_ptr<SceneSource>` 图层句柄。
  Source 0 是 `SetDataSource()` 的本源；`z` 是叠放序。
- per-frame 布置：注册 `SceneUpdateCallback`
  （`base::RepeatingCallback<void(SceneFrame&, const scoped_refptr<VideoFrame>&)>`），
  在 `frame.layer_of(source)` 拿 `SceneLayer` 设 rect/opacity/visible。回调在
  渲染线程、每帧一次；**只放布置，不做注册**（`AddSource` 随时可调，放帧回调里
  会把同一 source 注册上千次）。
- `Scene::Describe()` 用文字回答「现在能不能合成」——默认 headless sink 不能
  合成时它说清楚是哪回事，省得把「PiP 没显示」误判成层级放错。
- 只要 GL 不要播放器：直接用 `media::GlScene`/`GlSceneEngine`（docs/13）。

## 9. 日志

```cpp
class MyLog : public base::logging::LoggingDelegate { /* … */ };
avbase::base::logging::SetDelegate(std::make_unique<MyLog>());
```

FFmpeg 的 `av_log` 经 `media/ffmpeg/log_bridge.cc` 汇入同一委托（Δ22），
不需要两套钩子。`PlayerBuilder::SetLogLevel(severity)` 只调 avbase 自身的
严重度阈值。

## 10. C ABI（跨语言）

`player/public/c/avbase_c.h` 是**冻结契约**，独立 target `avbase_cabi`，
默认 `-DAVBASE_ENABLE_CAPI=OFF`。新增 C 函数的硬规则（M15 踩过的坑）：

- what 码（200/300/100/400/500-502/600/700/800/900/1000/10001）**不能猜**，
  对照现有表扩展；C++ 默认参数等 C 不支持的形态进不了这个头（CI 用
  `cc -std=c99` 编译 `capi_smoke_test.c` 把着关）。
- 全树 `-fno-exceptions -fno-rtti`：不要写 try/catch；绑定回调用 lambda 时
  显式区分「捕获」与「参数」。
- C1 单文件 500 行上限连注释都算；头里定义函数必须 `inline`。

Java/OC 侧迁移见 MIGRATION §7（0.1.0 只有 C ABI 这条跨语言路）。

## 11. 测试你的扩展

- 布局：`tests/unit/<dir>/*.cc` 按 glob 收集（新文件不用改 CMakeLists，
  但 invariants 的文件数基线会 +1，照常更新）。
- **测试绝不依赖工作目录**：测试数据路径用编译期宏 `AVBASE_TESTDATA_DIR`
  （tests/CMakeLists.txt 逐 target 定义）。同二进制直接跑绿、ctest 红 =
  先查 cwd。
- **鉴别力声明要先证明**：注释写「这个测试能抓住 X」之前，把 X 改回去跑一次
  确认它红（第八十八轮的教训：注释里的鉴别力声明也会假绿）。
- 门禁本地一套：`tools/gate_local.sh`（inv/fmt/lint/三预设 ctest + 可选
  sanitizer）；测二进制前先跑 `tools/check_binary_freshness.py --root build/<preset>`
  （第八十五轮：拿旧二进制诊断出过不存在的 bug）。
- 音频 sink 的线程契约（<100µs、不阻塞、不分配、不取媒体序列的锁）目前**没有
  专属契约测试**——头注释里曾写「Enforced by tests/contract/…」，那个文件从未
  存在，2026-10-18 已把注释改成实况（渲染路径由
  `tests/unit/media_filters/audio_renderer_impl_unittest.cc` 经 fake sink 覆盖）。
  自定义 sink 时这条预算靠 review 把着，别当成已有自动化。
