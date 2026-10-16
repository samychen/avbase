# TODO · 未完成工作（按建议排序）

> 本文件是**待办清单**，与 [12-剩余工作清单](12-剩余工作清单.md)（执行快照，含历史
> 决策与已闭合项）和 [PROGRESS.md](PROGRESS.md)（轮次活文档）互补。
> 排序依据是**性价比 × 用户可见性**：先修正确性 bug，再补结构性缺口，
> 最后做「等需求出现才启动」的能力。每条给出现状证据与验收判据。

---

## 0. 背景：差距的位置已经变了

L1–L5（可观测性 / FBO 合成地基 / 多源场景层 / 硬件零拷贝 / 多源编排）全部完成后，
**渲染层不再是短板**。剩余差距集中在下两层：

| 层 | 状态 |
|---|---|
| `render/gl/`（零依赖 GL） | ✅ 已闭合 |
| `media/gl/`（渲染编排） | ✅ 已闭合（第六十八轮 §4：属性 + 转场）。未做的四项（混合模式 / 镜像 / 色彩调整 / 图层名）属「未要」，见 §4 末尾 |
| `media/filters/`（播放器管线） | ✅ 已闭合（第七十轮 点名 + 第七十四～七十七轮**原生多源落地**）：一条管线现在能吃 N 个 URL —— 见下方说明 |
| `player/public/`（SDK 门面） | ✅ 已闭合（第六十九轮，§2） |

### 为什么「单源」不算缺口（第七十轮的判定）

> **第七十七轮补充**：上面的判定没过时，但「单源」已不再描述现状 —— docs/14
> 阶段 0–3 把多源做进了管线本身。保留这段是因为它解释的那件事（`AddVideoStream(int)`
> 为什么是死接口）到今天仍然成立，而它当初给出的「绕过」方案已经退役，见本节末尾。

表里这一格一直是 🔶「单源硬限制」，第六十九轮 §2 又用「第二路完整 Player +
邮箱」绕过去，读起来像是两轮都在回避。**查证之后结论相反：那条边界是真的，
绕过去的姿势是对的。**

1. **根因比管线更低一层。** `media/base/demuxer.h` 的
   `DemuxerStream* GetStream(DemuxerStreamType)` **按类型返回单个流**，没有
   index、没有 `GetAllStreams`。一个 demuxer 在抽象上就是每类型一路。
2. **PiP 要的根本不是「同一 demuxer 的两路流」。** 两路画面是两个 URL，也就是
   两个 Demuxer 实例。所以「给管线加多路支持」这个说法本身指错了地方。
3. **`Add/Remove{Video,Audio,Text}Stream(int id)` 是死接口。** 它们照抄
   Chromium `MediaResource` 的「第 id 路」语义，而 avbase 没有这个 axis；
   轨切换走 `SelectXxxTrack(stream_index)`。全树**零调用者**，原实现是
   no-op + `LOG(WARNING)`，且注释里「facade 会先拒绝」的说法是假的。

所以闭合方式是**点名**而不是实现（与第六十七轮「静默退化改成点名拒绝」同一
手法）：接口与实现写清它为什么不可能存在、6 条 no-op 改成明确 rejected 措辞、
3 条测试钉住「调用后零副作用」（突变反证 3/3 红）、`docs/05` 里两条把落点写成
`AddVideoStream` / `AddTextStream` 的迁移对照（后者还标了 ✅）已更正。

**多画面的正解是 §2 那条**：第六十九轮用 N 个 Player + `GlSceneEngine` 邮箱实测
可用（224 帧发布 / 113 消费 / 111 取代），**第七十七轮（docs/14 阶段 3）已下沉到
管线本身** —— `Player::AddSource(descriptor, name, z)` 注册次源，一条管线开 N 个
demuxer，第二幅画面不再需要第二个 Player。`SceneSource::MakePublisherFactory()`
与其驱动的「第二路完整 Player」一并删除（见下方 §2 的表格更正）。上面的论证没有
被推翻，变的是它的适用范围：`AddVideoStream(int)` 那 6 个仍是死接口，因为多源的
轴是 `GetStream(source_id, type)`，**不是**「同一资源的第 id 路同类型流」。

---

## 1. 【P1】✅ 已完成（第六十六轮）渲染侧消费 rotation 元数据

**原状**：rotation 被一路解析、上报，但**渲染完全不看它**。

```
media/base/decoder_config.h:69      int rotation{0}
media/base/video_frame.h:128        int rotation() const
media/ffmpeg/ffmpeg_demuxer_configs.cc:60-67   从 display matrix 解析
player/player_impl.cc:362           video_rotation_ = v->rotation
player/public/player.h:117          int video_rotation() const
player_event.h:50                   事件 kRotationChanged
```

而 `media/gl/`、`render/gl/`、`platform/` 三个目录里 grep `rotation` **零命中**。

**后果**：手机竖拍素材（编码为横向像素 + rotation=90 标记）会**横着显示**。
这是用户可见的正确性 bug，不是缺失功能。

**验收**（已完成）：
- ✅ `VideoFrame::NaturalSizeForRotation()`：90/270 时返回 `(h, w)`，0/180 时原样。
- ✅ 图层可显式设 rotation（含「恢复跟随帧」）。
- ✅ 像素测试：同一帧分别在 0 / 90 / 180 / 270 下出画，白色半边每次落在不同边。

**落地要点**

`VideoFrame::rotation()` **此前全仓没有任何写入者**，永远读回 0 —— 所以光让渲染
看它是没用的，得先把值送上帧。三处补齐：

| 层 | 改动 |
|---|---|
| `media/base/video_frame.h` | `set_rotation()`（与既有 `set_color_space()` 同风格）+ `NaturalSizeForRotation()` |
| `media/ffmpeg/ffmpeg_video_decoder.cc` / `ffmpeg_hw_video_decoder.cc` | 出帧时 `out->set_rotation(config_.rotation)` —— 软硬两条路都要，否则硬解素材丢了旋转 |
| `render/gl/gl_shaders.cc` | `kQuadVertexShaderWithRectBody` 加 `#ifdef ROTATE`，`uRotation` = (cos, sin) |
| `media/gl/gl_video_layer.cc` | `CreateLayerProgram` 拼 `kVertexRotationDefine`，并把新 program 留在「不旋转」的默认状态 |
| `media/gl/gl_scene.{h,cc}` | `SetLayerRotation()` / `LayerRotation()`；`Upload()` 时捕获帧的旋转 |
| `media/gl/gl_present.cc` | 单帧路径按帧的旋转设 uniform |

两个不显然的决定：

1. **旋转纹理坐标，不是旋转 quad。** 两者只在 rect 为正方形时等价；动 quad 会把
   图层自己的矩形从画面底下转走，于是「图层相对自己的 rect 转了」而不是
   「画面转了」。
2. **`uRotation` 必须在建 program 时设成 (1, 0)。** 没人设过的 `vec2` 读回
   (0, 0)，而 (0, 0) 不是单位元 —— 它把四个顶点全送去中心，结果是一帧
   全黑，没有错误也没有日志。这跟「翻转写反」是同一类静默失败。

**已知限制（诚实写明）**：旋转后的画面仍按 stretch-to-fill 铺进 rect，所以
竖拍素材转正后会被横向拉伸。要真正正确显示，需要 **aspect-fit**
（见 §4）—— 渲染层现在完全不读 `natural_size`，这是独立的第四项。

---

## 2. 【P2】✅ 已完成（第六十九轮）SDK 层暴露场景 API

**原状**：`player/public/player.h`（47 个公开方法）与 `player/public/deps.h` 里
grep `scene|layer|pip` **零命中**。`GlScene` / `GlSceneEngine` / `GlLayerSource`
全是 `media/gl/` 内部类型，SDK 用户摸不到。

**后果**：多图层合成与多源编排**只有宿主自己绕过播放器才能用** —— 旧
`examples/scene_demo.h` 的 `--second` 正是这么绕的：第二个 `Player`，加上一个把
工厂造出来的 sink `static_cast` 成 `GlVideoSink` 才能拿到引擎。库有这个能力，
SDK 用户没有。

### 已落地

`player/public/scene.h`（新，随 SDK 一起安装）：

| 接口 | 作用 |
|---|---|
| `Player::scene()` | 场景句柄。跟随 Player，跨 Prepare 自动重绑 |
| `Scene::AddSource(name, z)` / `RemoveSource` / `FindSource` | 注册一个「外部推帧」的输入：不经解码器，谁 `Publish()` 谁负责画面来源 |
| `SceneSource::Publish(frame)` | 任何线程推帧；`frames_published/consumed/superseded` 三计数 |
| `Player::AddSource(descriptor, name, z)` | **第七十七轮新增**：注册一个次 URL，由管线自己开第二个 demuxer 解码并喂进对应图层。必须在 `PrepareAsync` 之前调用 |
| ~~`SceneSource::MakePublisherFactory()`~~ | **已删除（第七十七轮）**。它把一整个 Player 的输出变成场景输入，是「第二路 Player」绕行的产物；`PublisherSink` 本身保留作管线内适配器 |
| `Scene::SetUpdateCallback(hook)` | 每显示帧一次的摆放回调 |
| `SceneFrame::main_layer()` / `AddLayer` / `layer_of` / `layer_with_id` | 回调内取图层句柄 |
| `SceneLayer::SetRect` / `SetZ` / `SetOpacity` / `SetVisible` / `SetRotation` / `SetSourceRect` / `SetContentMode` | rect / opacity / z 等属性 |
| `Scene::SetBackgroundColor` / `Describe()` | 背景色，与一行自述诊断 |

### 三个不显然的决定

1. **句柄跟随 Player，不是跟随 pipeline。** sink 是每次 `PrepareAsync` 新建一个，
   引擎跟着 sink 走；`Scene` 若绑在某个 pipeline 上，宿主就得在每个 Prepare
   周期重新注册一切 —— 那是它不该写的生命周期代码。所以 `Scene` 只记「有哪些源、
   有什么 hook、什么背景色」，由 `Player` 在新 sink 造好时 `Bind(engine)`，
   在 `Stop()` 时 `Bind(nullptr)`。

2. **「这个 sink 能不能合成」由 sink 自己回答。** 全仓 `-fno-rtti`，没有
   `dynamic_cast`；而默认 `Deps` 不设 `video_sink_factory` 时 fallback 到
   `NullVideoSink`，在那个对象上 `static_cast<GlVideoSink&>` 是**未定义行为**
   —— 示例原来就是这么写的。改为 `VideoRendererSink::scene_engine()` 虚接口，
   不能合成的返回 nullptr，`Scene` 据此进入「未绑定」状态并由 `Describe()`
   说出来。

3. **引擎自己报死亡，不等 Player 通知。** 管线还会被错误处理、轨切换拆掉，那些
   路径 Player 不一定有机会说话。所以加 `GlSceneEngine::SetTeardownCallback()`：
   析构时主动回调，`Scene` 把 `engine_` 置空并 Retire 所有源。析构函数在**锁外**
   跑它，否则回调撞上自己的成员析构就是死锁。

### 顺手修掉的一处数据竞争

`GlSceneEngine::update_` 原本是类里**唯一没有 `GUARDED_BY` 的共享状态**，GL 线程
在锁外读它 —— 「hook 必须在 `Start()` 之前设」这个契约只是给数据竞争穿了件外衣。
现在 `SetUpdateCallback()` 加锁、`Sync()` 在锁内取快照、锁外跑快照（hook 里回调
`AddSource` 是合法的，持锁会死锁）。**没有照抄这条契约到 SDK 上**。

### 验收

- ✅ 示例不再绕过 `Player`：`g_scene->Install(player)`，工厂只写
  `std::make_shared<Sdl2VideoSinkFactory>()`。
- ✅ `examples/publisher_sink.h` **已删除**（被当时的 `MakePublisherFactory()` 取代，
  后者在第七十七轮也删了，见上表）。
- ✅ 实跑：`--scene pip` → `scene: hook placed 48 frame(s), scene now bound,
  0 source(s), hook installed`；`--scene fade --second <url>` →
  `scene now bound, 1 source(s), hook installed` +
  `--second: published=108 consumed=108 superseded=0`。
  **第七十七轮重跑同一条命令行为不变**，但底下已不是第二个 Player，而是同一条
  管线的第二个 demuxer（`Player::AddSource()`）。
- ✅ `tests/unit/player/scene_unittest.cc`（10 条）+ `gl_multi_source_unittest.cc`
  补 2 条 hook 替换测试；**8 条突变反证真跑过、全红**。

### 没做的

`Scene` 刻意不暴露 `GlScene` 本身（SDK 用户的 include 路径里不该出现
`media/gl`），也没有「导出到纹理 / 落盘」出口 —— 那属于 §5 的产品化能力，
当前全部不存在。

---

## 3. 【P3】✅ 已完成（第六十七轮）CPU 滤镜与硬件零拷贝互斥，且是静默退化 —— 内部矛盾

**现状**：滤镜挂在 `media/filters/video_renderer_impl.cc:370-403`，位置是
**解码之后、`compositor_.PutCurrentFrame()` 之前**。

- `ffmpeg_video_filter.cc:151` 要求帧格式与 ctx 匹配；
- `VideoFrame::visible_data()` 对不可映射的 GPU 原生帧返回**空 span**
  （`media/base/video_frame.cc:150-155`）。
- 而 `GlVideoLayer::Upload()` 的零拷贝路线只认**不可映射**的原生帧
  （`gl_video_layer.cc:117-120, 182-222`）。

**后果**：两条路**不能同时走**。开了 `filter_graph`，硬解帧就得先读回或退回软解，
`upload_path` 从 `hw_zero_copy` 落到 `cpu_sync` —— **没有任何日志说明这不是配置错误，
也没有测试钉住这条退化**。这是「两个功能各自都对、合起来互相拆台」的形状。

最坏的一种表现：`Process()` 返回 false 时老代码**直接 return 而不重投 pump**，
于是 video 腿停死、**画面冻结且什么都不报**。

**验收**（已完成）：

- ✅ **拒绝发生在建图之前**，而不是把空 plane 指针喂给 avfilter 再等它失败。
- ✅ 拒绝**有名字**：新增 `VideoFilterState`（`kOff` / `kRunning` /
  `kRefusedHardwareFrame` / `kRefusedNoFactory` / `kRefusedGraphFailed`），
  五个取值一眼分得清「没要滤镜」和「要了但静默没跑」。
- ✅ **统计里可见**：`RenderPerfStats::filter_state` 进 `ToJson()`
  （`"filter_state":"refused_hardware_frame"`），而 `playback_stats.cc:31`
  正是用 `render.ToJson()` 出 JSON —— 宿主不用改代码就能看到。
- ✅ **日志说清怎么修**："the decoder is producing hardware frames
  (zero-copy); showing the picture unfiltered. Disable hwaccel to apply the
  graph."，且**每流一代只说一次**（按帧率重复打的日志没人读）。

| 层 | 改动 |
| --- | --- |
| `media/filters/video_filter_stage.{h,cc}`（新） | `VideoFilterStageRunner` 拥有 stage 的整个生命周期 |
| `media/base/render_perf_stats.{h,cc}` | `VideoFilterState` + `VideoFilterStateName()` + `RenderPerfStats::filter_state` 进 JSON |
| `media/filters/video_renderer_impl.{h,cc}` | 三条 `filter_*` 成员 → 一个 `filter_runner_`；返回值不再被丢弃 |
| `media/filters/renderer_impl_reporting.cc` | 统计采样时把 `filter_state` 补进去（sink 看不到这个字段） |
| `media/ffmpeg/ffmpeg_video_filter.{h,cc}` | 拒绝不可映射帧；`last_rejection()` 给出理由 |
| `examples/play_glfw.cc` | `--vf0 <graph>` 命令行入口 |

**仍保留的行为**：拒绝的代价是**滤镜图**，不是画面 —— 所有 refusal 分支都把
解码帧原样送去显示。刻意**不**在 runner 里 `ToI420()` 兜底：那会把每一帧拖回系统
内存、悄悄撤销零拷贝，是用最贵的方式搬一张本来就不需要动的图。

---

## 4. 【P4】图层级属性与多转场 —— ✅ 完成（第六十八轮）

**原状**：`GlScene::Layer` 只有 5 个属性：`rect` / `z` / `opacity` /
`visible` / `id`。`rotation` 由 §1 单独处理（第六十六轮完成）。

### 已落地

| 项 | 接口 | 备注 |
|---|---|---|
| 背景色 | `GlScene::SetBackgroundColor(r,g,b,a)` | **场景级**，取代硬编码 `ClearColor(0,0,0,1)`；非有限值拒绝 |
| 源矩形裁剪 crop | `GlScene::SetLayerSourceRect(id,x,y,w,h)` | 归一化源矩形；越界 / 非有限 / 零尺寸一律**拒绝而非 clamp** |
| aspect-fit | `SetLayerContentMode(id, kFit\|kStretch)` | 此前渲染层完全不读 `natural_size()`（零命中），一律拉满铺 |
| 多转场 | `TransitionKind` × 5 + `GlPresenter::SetTransitionKind()` | cross-fade + 四向 wipe |

**没做的**（仍属未要）：混合模式（`GL_SRC_ALPHA` 固定）、镜像、色彩调整、
图层名。36 个转场 GLSL 依旧**刻意不移植**
（[12 §4 5.5](12-剩余工作清单.md) 决策：avbase 无此产品形态），不算欠账。

### 两处会咬人的约束（写进代码注释了）

- **`Compose(int dst_width, int dst_height)`。** aspect-fit 是**像素网格**
  上的关系，纯归一化坐标表达不了（1920 的一半 ≠ 1080 的一半）。传尺寸不是
  传 canvas，所以同一个调用照样能合成进导出目标。
- **`uSrcRect` / `uRotation` 建 program 时必须设成单位元** `(0,0,1,1)` /
  `(1,0)`。没设过的 `vec2` 读回 `(0,0)` —— 那不是单位元，是把四顶点塌缩到
  中心：一帧全黑，无错误无日志。单帧路径不裁剪，但跟场景共用同一个 program，
  uniform 属于 program，所以也得明确设。

### 验收

`tests/unit/gl/gl_layer_attributes_unittest.cc`（8 条）+ `gl_scene_present_unittest.cc`
补 7 条 wipe 用例；几何与朝向一律像素断言。**13 条突变反证全部真跑过、全红**，
其中一条复现的正是本轮自己写反的 bug（裁剪框绕图片中心转，只在四分之一转
素材上分得开）。

---

## 5. 【P4】确认没有的产品化能力

| 能力 | 现状 |
|---|---|
| 录制（播放侧落盘） | ❌ 无。只有 `media/transcode/` 的离线转码（输入是文件，不是实时输出） |
| 推流 / RTMP publish | ❌ 无。`rtmp` 仅作**输入**协议（`ffmpeg_demuxer.cc:300-301`） |
| 缩略图 / seek preview | ❌ 无。`get_frame_mode` 是**死选项**：`player_config.h:218` 有配置、
  `option_registry.inc:212` 有注册，`media/` 里**零命中** |
| HDR / tone mapping | ❌ 无实现。`player_config.h:51` 有枚举、`:117` 有字段，
  但 `hdr_tone_mapping` 在 `media/`/`render/` 里**零命中**；
  事件 `kHdrUnsupported` **从未被 emit** |
| PBO 上传 | ❌ 未实现。`render_perf_stats.h:28` 的 `kPbo` 注明 "not built" |
| 真色彩管理 | ❌ 无。只有 YUV→RGB 矩阵（`gl_video_layer.cc:69-113`），
  无 gamut 转换 / EOTF / ICC / HDR→SDR |

### 5b. 已配置但未生效的选项 —— 第七十轮核验出 16 个

`docs/05-迁移对照表.md` 里一度有 22 个 ✅，逐项回到代码追「谁读了这个字段」
之后 **16 个不成立**。它们的共同形状是：**配置有字段、registry 有条目，`SetOption`
不报错、`GetOption` 还能原样读回 —— 但没有任何一处把它变成行为**。这比「不存在
的选项」更难发现，因为它不会报错。

| 类 | 数量 | 选项 | 性质 |
| --- | --- | --- | --- |
| A 无读取者 | 14 | `fast` `rdftspeed` `min-frames` `packet-buffering` `sync-av-start` `infbuf` `video-pictq-size` `no-time-adjust` `preset-5-1-center-mix-level` `get-frame-mode` `video-mime-type` `ijkmeta-delay-init` `nodisp` `render-wait-start` | 除定义与 registry 外**零命中**；`video-pictq-size` 只被 `player_config.cc` 的区间校验读到 |
| B 末环无调用者 | 2 | `max-fps` `framedrop` | ~~几乎通了~~ → **第七十一轮已接通**（见下） |

**B 类已闭合（第七十一轮）。** 之所以冒充 ✅ 这么久，是因为算法本体在
`video_frame_compositor_unittest.cc` 里有充分测试（测试直接给 `in.max_fps`
赋值），而「选项 → 算法」那一段既无调用者也无测试 —— **「算法正确」不等于「选项
生效」**。接通的过程与由此发现的三件事（轨切换同样丢配置、`frames_dropped_fps`
不上报、默认 31 是行为变更）见 `docs/05` 文末。

### 5c. `player/` 层零测试覆盖（第七十一轮 mutation 跑出来的）

**没有任何测试构造 `Player`** —— tests/ 里 grep `PlayerBuilder` 零命中。后果不是
抽象的：接通 `max-fps` 时误删了 `player_impl.cc` 里 `factory_deps.video_disabled =
config_.video.disabled;` 这一行（`vn` 选项），**528 个测试全绿，无一发现**；是提交前
自查 diff 时才抓到。

`PlayerImpl::PrepareAsync` 那段 deps 组装（把 `PlayerConfig` 翻译成
`RendererImpl::Deps`）因此**没有任何断言**。
`renderer_option_wiring_unittest.cc` 覆盖的是 `Deps → 内部状态` 这一跳，它明确记录
了这个边界，而不是假装测到了 player 层。

- **要做的事**：给 `PlayerBuilder → PrepareAsync` 补一个能观测 deps 的测试。这需要
  能拦截 `DefaultRendererFactory` 或其 Deps（工厂由 `PrepareAsync` 内部构造，目前没有
  注入点），所以先要开一个缝。
- **在那之前**：这一跳的每次改动都必须靠 diff 自查 —— 这正是本轮的实际做法，也是它
  唯一的保障。

**已闭合（第七十二轮）。** 缝、11 条测试、以及两个顺带挖出的拆除竞态：

- **先拆文件才能开缝。** `player_impl.cc` 当时 499 行，C1 上限 500 —— 缝本身
  （一个 setter）放不进去。按代码里已有的 seam 把"准备的一半"（`CreateDemuxer` /
  `SetDataSource` / `PrepareAsync` / `PrepareSync` / `OnPipelineReady` 及两个
  helper）整体移到 `player_impl_prepare.cc`，移完 208 行；两段 diff 逐字节比对一致。
- **缝的形状**：`set_deps_observer_for_testing(FactoryDepsObserver)`，在
  `std::move(factory_deps)` **之前**触发。观察者受 `state_lock_` 保护，调用在
  **锁外**（观察者是测试代码，沿用 `GlSceneEngine` 的 update-hook 纪律）。
  deps 组装是同步的 —— `pipeline->Start()` 只是 post，观察点在调用线程上必然
  触发，测试无需等待。
- **测试**：`player_deps_wiring_unittest.cc` 11 条，从 `PlayerBuilder` 一路走到
  `PrepareAsync` 交出去的 `Deps`：默认配置 / `vn` / `an` / 帧率 / 解码器偏好 /
  硬解掩码 / 滤镜图 / 同步主钟映射 / 注入时钟 / 注入 sink 工厂 / 解码器回退链。
  **14 个突变全部变红** —— 包括删 `video_disabled` 赋值，即第七十一轮让 528 个
  测试全绿的那一个。
- **顺带修掉两个真实拆除竞态**（写"prepare 后立刻析构"的测试时撞见的，不是设计
  出来的）：① demuxer 的 open 完成回调绑着裸指针，`Stop()` join demux 线程会
  drain 那个 task，而 `Stop()` 返回后 owner 就可以销毁 demuxer —— UAF；改为绑
  `WeakPtrFactory`（docs/04 §7 R11）。② `PipelineImpl::Start()` 把状态留在 `kNew`
  直到 media sequence 跑 `DoStart`，而 `IsRunning()` 这个"safe to destroy"谓词
  **不数 `kNew`** —— 拆除方被误告安全，管线 mid-`kStarting` 被析构；改为 `Start()`
  同步做 `kNew → kStarting` CAS。两者由 `player_prepare_teardown_unittest.cc`
  循环 20 次钉住（红色方向是时序相关的，单次复现不保证）。

### 5d. 本机 clang-tidy 对 `media/ffmpeg` 静默零发现（第七十二轮查出，已修）

`avbase_ffmpeg` 是全仓**唯一**用 `target_precompile_headers` 的 target，而本机
PyPI `clang-tidy==18.1.8` 读不了 Xcode 的新 PCH 格式：每个 `avbase_ffmpeg` TU 报
一句 `PCH file uses a newer PCH format`，然后 **exit 0、零 finding**。于是第七十轮
"142 TU 剩 1 条"的记录里，**21 个 `media/ffmpeg` TU 全部静默没被分析**（那 1 条
EGL 噪声来自没有 PCH 的 `platform/egl`，所以才看得见）。CI 在 Ubuntu 上由
clang-18 自己生成、自己读 PCH，**没有这个问题** —— 这 11 条 CI 一定看得见。

已修：本机跑 tidy 必须配 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` 的 scratch
数据库（`operations.md` 有命令）；用它重扫全 scope 得 11 条（3×
`modernize-pass-by-value`、3× `bugprone-branch-clone`、2×
`bugprone-implicit-widening-of-multiplication-result`、1×
`bugprone-multi-level-implicit-pointer-conversion`、1×
`modernize-use-starts-ends-with`、1×
`performance-inefficient-string-concatenation`），逐条修完后重扫 **139 文件 0 条**。
另修掉一个连带发现：`color_space_bridge.h` 用 `AVFrame` 却不包含任何定义它的头，
一直靠 PCH 兜底 —— 改为前向声明（与 `hw_frame_readback.h` 同形）。

### 5e. strict/debug 配置不可构建（第七十二轮查出，已修）

`debug` preset（Debug + DCHECK + `-Wconversion`/`-Wsign-conversion` + `-Werror`）
在 HEAD 上编不出来：6 处隐式转换，全是"已经发生、只差命名"。其中 3 处
（`{live,retry}_data_source_unittest.cc`、`audio_convert_unittest.cc`）在
origin/main 里就有 —— **CI 的 strict job 自 10 月 3 日起就是红的**（本地 86 个提交
未推送，没人看得见）。代价不只是门禁：debug 是 DCHECK 的家，编不出来就没人能带着
DCHECK 跑测试。6 处显式化后 debug 全量 ctest 663/663。

---

## 6. 环境相关（非代码，需特定机器/权限）

沿用 [12 §1 2.4 / 2.5 / §2](12-剩余工作清单.md) 的登记：

| 项 | 阻塞原因 |
|---|---|
| RTMP / RTSP / SRT 播放验收 | 无法从文件伺服，需真实 peer |
| 真 `tc netem` 弱网 | 需 root；macOS 无 |
| Linux 显示侧零拷贝（VAAPI→EGL） | 需 Linux + VAAPI 设备 |
| Windows 显示侧零拷贝（D3D11-GL） | 需 Windows + D3D11 环境 |
| 硬解性能对照（4K CPU < 15%） | 需 4K 样本入库 |
| Qt 门面 / soak 48h | Phase 4 剩余 |

---

## 7. 进度记录

| 项 | 状态 | 落地 |
|---|---|---|
| §1 旋转（渲染侧消费 rotation） | ✅ 完成 | 第六十六轮 |
| §2 SDK 暴露场景 API | ✅ 完成 | 第六十九轮 |
| §3 滤镜 × 零拷贝（改明确拒绝） | ✅ 完成 | 第六十七轮 |
| §4 图层属性（背景色 / crop / aspect-fit）与多转场 | ✅ 完成 | 第六十八轮 |
| CI 风格门禁存量红灯（clang-tidy 800 条 / cpplint 4 条） | ✅ 完成 | 第七十轮 |
| §0 播放器层多源（docs/14 阶段 0–3：一条管线吃 N 个 URL） | ✅ 完成（第二路 Player 绕行退役） | 第七十四～七十七轮 |

---

## 8. 记录更正

- **第六十六轮的门禁数字是抄的、没重跑**（沿用第六十五轮的 504/625/672）。
  第六十七轮当场实测当前基线是 **511 / 688**。门禁表的数字必须当场跑出来。
- **「测试写了」不等于「性质被钉住」。** 第六十七轮的两条测试在加强之前都是空转：
  一条在断言构造函数（新对象本来就没有理由），一条断言的性质从公开 API
  根本观测不到。项目约定因此补一条：**反证必须真跑一遍，看有没有红的。**
- **门禁的「绿」可能是「没跑」，而且这一条在第七十轮又被证实了一次。**
  `check-clang-tidy` 三处失效同时存在：`-header-filter` 用了 POSIX ERE 不支持的
  负向前瞻（头文件从未被检查，17 条 vs 1095 条）、`platform/` 的可选后端不在
  编译数据库里（142 个 scope 文件里 27 个被静默跳过）、`HeaderFilterRegex` 未锚定
  （vendored 的 glad 全中）。修完之后存量是 **800 条**——所以「绿灯」这三个字
  在修好之前**没有任何信息量**。同类条款见 `operations.md`：「exit=127 会让门禁
  看起来绿」「2 秒跑完的全树检查一定没跑」。
- **抑制（NOLINT）会静默失效，比不写更危险**，因为它读起来像是已经处理过了。
  第七十轮两种：诊断行与 `NOLINTNEXTLINE` 差一行（模板头 / 宏的反斜杠续行）；
  以及**散文注释里出现 NOLINT 这个词会被当成真指令**。详见第七十轮。
