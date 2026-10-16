# avbase 长期约定

Chromium 风格分层 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。
详细轮次记录见 `docs/PROGRESS.md`，GL 模块契约见 `docs/13`。
门禁命令与操作教训见 `operations.md`。

## 分层（docs/02 §6，D11）
`platform/` 装「被宿主选中」的代码；可移植、各宿主一致的不进（FFmpeg→`media/ffmpeg/`，
GL→`render/gl/`）。**图形 API 轴 × 窗口系统轴**别压平，否则加第二个 API 就 N×M 个目录。
不设 `graphics/`。`media/gl/`（被 VideoFrame 塑形）/ `media/renderers/`（仅工厂）/
`render/gl/`（零依赖）不重复。
**C28：`render/` 不得 include `media`/`platform`/`base`/`player/`。**

## avbase_gl（render/gl/）零依赖可分发模块
`find_package(avbase_gl)` + `avbase::gl`（靠 `EXPORT_NAME gl`）。`GlApi` 一 context
一实例，入口点 `gl.context().Clear(...)`。装载器 = glad 2.0.8 vendored 生成物
（`--mx` 开、`--loader`/`--alias` 不开）；唯一漏网 `glEGLImageTargetTexture2DOES`
手工解。**不手写 GL 常量表**（手抄 = 造一个写错也不报错的失败模式）。glad 枚举是
`#define` int → 三元窄化进 GLenum 要 `static_cast`。隐患：`GLAPIENTRY` 为空，
32 位 Win 会崩。

## 四条「写下来会静默失效」的约定（都真踩过）

**朝向。** 图像第 0 行落在视口顶 = framebuffer 最后一行 → 纹理第 0 行装的是图像底 →
**凡从渲染目标采样回来的 pass 都要 `flip_vertical=true`**（管 `GlBlitter` 与
`GlTransition`）。搞反 = 上下颠倒，**无返回值、无 GL 错误**。这条推导被独立写反过
两次 → **有朝向的代码必须配像素断言**。

**stride。** `GL_UNPACK_ROW_LENGTH` 单位是 **texel 不是字节**：上传前设
`stride(plane)`、用完还原 0；NV12 交错色度用 `GL_RG` 所以传 `stride/2`。1080p 恰好
相等把 bug 藏住，320x240/854x480 必现（**周期 8 行绿色横条**）。

**GL 常量。** 一个常量别在两个头世界里各写一遍（`GL_TEXTURE_RECTANGLE` 不在 Apple 的
`gl.h`）→ 后端用 `target()` 说出来。

**变换顺序（第六十八轮第三次栽在同一类上）。** 顶点 shader 做的是
**screen → source 的反向映射**，所以**观众最后看到的那一步在计算上是最先做的**。
于是 CROP 必须写在 ROTATE **之后**（算术上），`uSrcRect` 才落在「解码器存下来的
那张图」的坐标系里。写成先 crop 再 rotate = 把裁剪框**绕整幅图的中心**又转一次；
且**只在四分之一转素材上分得开**（不旋转时两种顺序逐像素相同）。
**别用算术顺序去描述视觉顺序** —— 反向映射里两者天然相反，`gl_scene.h` 里那句
"Applied BEFORE the display rotation" 就是这么埋的雷。

## 多源编排（L5）
`media/base/latest_frame_queue` → `media/gl/gl_layer_source` →
`media/gl/gl_scene_engine`；`gl_scene_driver.{h,cc}` 已删（职责被引擎完整吸收）。
- **单槽邮箱，不是队列**：生产者超前 → **取代**槽里那帧。`Publish()` 交回被取代的帧，
  因为生产者是**唯一**看见它的一方，否则「解码了没显示」根本无从知晓。
  恒等式 `published == consumed + superseded + cleared + (pending?1:0)`，四项分开是因为
  「没被显示」的三种原因在排查时是不同的诊断。
- **两个方向都保留**：播放器的流被 `Render()` 拉（A/V 同步基准）；其它源推到各自邮箱
  （有自己的时钟）。压成一个形状会让显示等外来解码器，或给播放器的帧套一个仅为满足
  抽象而存在的邮箱。
- **图层持有它上传的那一帧**：拷贝路线看不出差别，但**导入纹理指向解码器的 surface，
  释放它的正是帧的 release 回调** → 不专门测则本机全绿、有导入后端的机器崩。

### 已知未钉住
（第六十七轮又撞见两次同类：一条测试在断言**构造函数**——新对象本来就没有理由；
一条的性质从公开 API **根本观测不到**。约定：**反证必须真跑一遍看有没有红的**，
详见 `operations.md`。）
引擎的早退（无源、无钩子时走单帧路径）**没有反证**：去掉它 5 条测试全绿。原因是从公开
API 观测不到 —— 两条路径逐像素输出相同，差别只在「有没有分配一个场景」。钉住它要加
`scene()` 查询 = 扩大导出类接口，第六十五轮没擅自做。

## 旋转（第六十六轮补齐）

`VideoFrame::rotation()` **曾全仓无写入者** —— 有 getter、被 demuxer 从 display
matrix 解析、被 `Player::video_rotation()` 上报，但**没有一帧真正带上它**。
于是「渲染看 rotation」与「渲染忽略 rotation」是同一份代码。
**修消费端之前先确认这个值真的有人在写**，否则再正确的渲染也是在消费一个常量。

- 写入点：`FFmpegVideoDecoder` / `FFmpegHwVideoDecoder` 出帧时
  `set_rotation(config_.rotation)` —— **软硬两条路都要**，只补一条 = 硬解素材静默丢旋转。
- **旋转纹理坐标，不是旋转 quad。** 两者只在 rect 为正方形时等价；动 quad 会把图层的
  矩形从它本该框住的那张画面底下转走，于是「图层相对自己的 rect 转了」而不是「画面转了」。
- **`uRotation` 建 program 时就要设成 (1, 0)。** 未设过的 `vec2` 读回 (0, 0)，
  那不是单位元而是把四顶点塌缩到中心 —— 一帧全黑，无错误无日志。
- 旋转后仍按 stretch-to-fill 铺进 rect，所以竖拍转正会被横向拉伸；
  **aspect-fit 是独立项**：渲染层完全不读 `natural_size()`（零命中）。

## 图层属性与转场（第六十八轮补齐，TODO §4）

`GlScene` 现在有：背景色（**场景级** `SetBackgroundColor`）、源矩形裁剪
（`SetLayerSourceRect`，越界/非有限/零尺寸**拒绝而非 clamp**）、
`LayerContentMode{kStretch,kFit}`。转场五种（`TransitionKind`：cross-fade +
四向 wipe）。

- **`Compose(int dst_width, int dst_height)`**：aspect-fit 是**像素网格**上的
  关系，纯归一化坐标表达不了（1920 的一半 ≠ 1080 的一半）。传尺寸不是传 canvas
  —— 尺寸不是 canvas，同一调用照样能合成进导出目标。
- **`source_aspect()` = `NaturalSizeForRotation()` × `frame.sar()`。** sar 属于
  **存储下来的**采样网格、沿它的宽度缩放，所以四分之一转不抵消它、只把它挪到
  另一根轴：720x576 PAL 横放竖放每采样都宽 16/15。
- **`uSrcRect` 同样要设单位元 `(0,0,1,1)`**，理由同 `uRotation`。单帧路径不裁剪
  但**跟场景共用同一个 program**，uniform 属于 program 不属于调用它的路径。
- **转场刻意只要五种，不移植 FFmpeg 那 36 个**：那是视频编辑器的产品功能，这里
  是渲染原语。要的是能证明管线可推广的最小集（一个空间恒定的权重 + 一个不恒定
  的），不是目录。方向命名的**边缘行进方向**，不是 `to` 在哪一侧。
- `SetTransitionKind()` 只记录，`EnsureTransition()` 在渲染路径懒重建 program：
  编译着色器需要当前 context，setter 没资格假定自己有（调用方可能在 UI 线程）。

**仍未做**：混合模式（`GL_SRC_ALPHA` 固定）、镜像、色彩调整、图层名。

## SDK 场景门面（第六十九轮，TODO §2）

`player/public/scene.h`：`Player::scene()` → `Scene` / `SceneSource` /
`SceneFrame` / `SceneLayer`。媒体层类型**全部前向声明**，SDK 用户的 include
路径里不出现 `media/gl`。实现在 `player/scene.cc`（绘制面）+ `player/scene_source.cc`
（生产者面，C1 逼出的拆分：原 522 行超 500 行上限）。

- **句柄跟着 Player，不跟着 pipeline。** sink 每次 `PrepareAsync` 新建一个、引擎
  跟着 sink 走；绑在 pipeline 上 = 让宿主每个周期重新注册一切。`Scene` 只记「有哪些
  源 / 什么 hook / 什么背景色」，Player 在新 sink 造好时 `Bind(engine)`、`Stop()` 时
  `Bind(nullptr)`。
- **「能不能合成」由 sink 自己回答**（`VideoRendererSink::scene_engine()` 虚接口，
  默认 nullptr）。**不能 `static_cast<GlVideoSink&>`**：全仓 `-fno-rtti`，且默认
  `Deps` 给 `NullVideoSink` —— 在那里是 UB，示例原来就是这么写的。
- **引擎自报死亡**（`GlSceneEngine::SetTeardownCallback`，析构时**锁外**跑）。不能等
  Player 通知：管线还被自己的错误处理和轨切换拆掉，那些路径 Player 不一定有机会说话。
- **`SceneSource::MakePublisherFactory()`** 把一整个 Player 的输出变成场景的输入。
  第二路管线仍必须 present 到某处（那是它的帧被拉出来的唯一途径）；此前唯一做法是
  宿主手写 sink（`examples/publisher_sink.h`，本轮删除）。
- **`update_` 曾是全类唯一没有 `GUARDED_BY` 的共享状态** —— 「hook 必须 before
  Start()」= 给数据竞争穿的外衣，真代价是 `RepeatingCallback` 的引用计数。
  SDK **没有**照抄这条契约。

**单源硬限制的修复已开工（第七十四轮，docs/14 阶段 0 落地）**：
`MediaResource`/`Demuxer` 有 `source_id` 寻址轴（无轴版=source 0，子类零改动），
`CompositeDemuxer` 聚合 N 源，`Pipeline::Start` 收 `vector<unique_ptr<Demuxer>>`
（PipelineImpl 持 composite 而非裸 vector——与设计书 §3.4 的有意偏差，逐源访问走
`source_count()`/`source(id)`）。**RendererImpl 仍只消费 source 0**——阶段 1–3
（渲染多路 / 时钟多轴 / GlSceneEngine 升格退役 L5 绕行）待做。§2 的 SDK 场景门面
（第二路 Player + 邮箱）在阶段 3 才废弃。

## player 层可观测性与 clang-tidy 环境陷阱（第七十二轮，TODO §5c/§5d）

- **`PrepareAsync` 的 deps 组装可观测了**：`set_deps_observer_for_testing`
  （`player_impl.h`），`std::move(factory_deps)` 之前、锁保护、**锁外**调用；
  11 条测试在 `player_deps_wiring_unittest.cc`。拆分先例：
  `player_impl_prepare.cc`（C1 500 行上限逼出来的 seam 拆分）。
- **`DoStart` 先造 renderer 再初始化 demuxer** —— sink 工厂在通往失败的路上一定
  会被调用（fake 工厂返回 nullptr 会 SEGV，别假设"失败得早所以不会调到我"）。
- **拆除竞态两形态**（20 次循环钉住）：① 完成回调 posted 到 media sequence，
  而 `Stop()` join 会 drain 该 task、返回后即可析构接收者 → 绑 WeakPtr；
  ② `IsRunning()`（safe-to-destroy 谓词）**不数 `kNew`** → "已 post 未跑"被误判
  安全 → `Start()` 同步做 `kNew→kStarting` CAS。谓词必须覆盖"已排队未执行"。
- **本机 clang-tidy 对 `media/ffmpeg` 静默零发现**：`avbase_ffmpeg` 是全仓**唯一**
  开 PCH 的 target，Xcode 新 PCH 格式 18.1.8 读不了 → 每 TU 一句
  `PCH file uses a newer PCH format` 然后 **exit 0、零 finding**。本机跑 tidy
  必须配 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` 的 scratch 库（命令在
  `operations.md`）；CI 在 Ubuntu 上没这个问题 —— **本机的"干净"不等于 CI 的**。

## 性能数据唯一入口
三宿主示例均支持 `--stats`；`tools/bench_render.sh`，`SCENE=pip|fade` 测双层，
`SECOND=<url>` 测真第二路解码。
- **判断回归先看四段耗时，不要只看 fps。**
- `present ≈ 16.7ms` 是设计意图；真 bug 是 `present ≈ 1ms` 却 30fps。
- `blit_ms` 独立成字段；恒 0 读作「全免费」而不是「没测」。
- **「第二层让 upload 翻倍」是 L4 之前的旧结论**（当时 CPU 拷贝）。硬件导入只是重指
  指针，1080p60 单帧约 0.03~0.09 ms，**低于本机逐轮噪声** ——
  数字没变不等于没生效（第六十五轮实测确认）。
- 节流锚点是「上次计划的时间」不是「现在」（`media/base/render_cadence.h`）。

## 路线图
L1 可观测性 / L2 FBO 合成 / L3 多源场景层 / L4 硬件零拷贝 / L5 多源编排 —— **全部完成**。
播放器层「管线多源」改造（docs/14）：**阶段 1–2 已落地（第七十五～七十六轮）**，
阶段 3 待做；L5 的「第二路 Player + 邮箱」绕行在阶段 3 退役。

## RendererImpl 多源（第七十五轮，docs/14 阶段 1）
`video_renderers_` 按源索引（视频缺失的源留空项占位），`video_initialized_`/
`video_ended_` 同步索引；**audio 腿仍仅源 0**（§4.2：次源音频「看见但不选」）。
源 0 主层特权：宿主 sink、`frame_presented_cb` 仅源 0 接线（喂视频时钟）、
`TakeSnapshot` 与轨道切换只作用源 0。次源 sink 走 `extra_video_sink_factory`
（null→`NullVideoSink`——选 Null 而非跳过整腿，「两条解码链」才可观测）。
- **bind.h `UnwrapArg` 解包绑定参数里的 WeakPtr**：失活检查只在 `BindState::Run`。
  跨线程完成回调要**捕获** WeakPtr、内层 `BindOnce(方法, weak)`，形参直接写
  `WeakPtr` 会静静收到裸指针（Flush 屏障就这么修的）。
- **外来流守卫实时求证**（`IsPrimaryStream` 向 resource 现场问，不用 Initialize
  快照）：流的集合会长，快照守卫把 `TheCapSurvivesAVideoTrackSwitch` 挂满 5s。
- **fake 间流传的身份字段 = 路由的可观测证据**：两源视频流给不同 serial，sink 里
  帧的 serial 证明「宿主 sink 给源 0」是行为断言（突变 M2 红在 serial 上）。
- 新 TU：`renderer_impl_sources.cc`（构造+接线）/ `renderer_impl_text.cc`（文本腿）；
  allowlist 上限 600→590、700→690。双源套件 `renderer_multi_source_unittest.cc`。
