# player_qt — Qt Quick 播放器 / 编辑器骨架

一个能真正跑起来的 Qt 6 应用：视频有画面、有声音、能拖进度、能切轨道，
下面挂一条 Shotcut 风格的多轨时间线。视频有**两条上屏路径**：软件（CPU 转换，
默认）和 GPU（`QQuickFramebufferObject` + avbase 的 GL 模块，`AVBASE_QT_GL=1`）。

它同时是**两个可借用的模板**：

- `qml/ControlBar.qml` — Haruna 风格的控制栏（悬浮覆盖层）
- `qml/TimelinePanel.qml` — Shotcut 风格的时间线面板（多轨 + 播放头 + 缩放）

两者都只借用**布局与交互思路**（Haruna 与 Shotcut 都是 GPL-3.0，代码不可拷贝
进闭源产品），所有实现是从 avbase 的 `Player` API 重新写的。

## 编译

```bash
cmake -S . -B build/qt-player -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAVBASE_ENABLE_FFMPEG=ON \
  -DAVBASE_ENABLE_SDL2=ON \
  -DAVBASE_ENABLE_QT=ON \
  -DAVBASE_BUILD_QT_PLAYER=ON \
  -DCMAKE_PREFIX_PATH=/usr/local/opt/qt      # 换成你的 Qt 6.4+ 前缀
cmake --build build/qt-player --target avbase_player_qt
./build/qt-player/bin/avbase_player_qt /path/to/video.mp4
AVBASE_QT_GL=1 ./build/qt-player/bin/avbase_player_qt /path/to/video.mp4
```

`AVBASE_BUILD_QT_PLAYER` 默认 **OFF**，所以不带 Qt 的机器照常构建整个树。
`AVBASE_ENABLE_QT=ON` 建 `platform/qt/`（第四个窗口系统适配器），应用**硬依赖**它：
没有 GPU 路径的播放器骨架会误导人，所以缺这个开关时 CMake 直接报错并点名。
`AVBASE_ENABLE_SDL2=ON` 时链接 `Sdl2AudioSinkFactory`（**有声音**）；关掉也能编，
只是退回 avbase 的 null sink（静音），和 `play_glfw` 一样。

## 目录

| 文件 | 职责 | 行数量级 |
|---|---|---|
| `main.cc` | 组装根：QML 引擎 → bridge → mailbox 的销毁顺序 | ~290 |
| `player_bridge.{h,cc}` | **唯一**让 avbase 与 Qt 相遇的地方：事件跨线程投递 + Q_PROPERTY | ~450 |
| `qt_video_sink.{h,cc}` | 自定义 `VideoRendererSink`，拉帧丢进 mailbox | ~150 |
| `frame_mailbox.h` | 渲染线程 → GUI 线程的单槽帧交换 | ~80 |
| `video_item.{h,cc}` | **软件路径**：`QQuickPaintedItem`，I420 → RGB → QPainter | ~270 |
| `gl_video_item.{h,cc}` | **GPU 路径的 Qt 半边**：`QQuickFramebufferObject` + FBO + mailbox | ~300 |
| `gl_video_renderer.{h,cc}` | **GPU 路径的 GL 半边**：GlApi + GlVideoLayer + YUV 程序 + 四边形 | ~200 |
| `timeline_model.{h,cc}` | 时间线数据模型（多轨 clip、播放头、缩放） | ~250 |
| `qml/ControlBar.qml` | Haruna 风格控制栏 | ~280 |
| `qml/TimelinePanel.qml` | Shotcut 风格时间线 | ~270 |
| `qml/Main.qml` | 应用壳：视频面 + 控制栏 + 时间线 + 拖放 | ~330 |

## UI 元素 → Player API 映射

这是这份骨架最有价值的部分：每个控件都对应一个 SDK 调用，没有一处绕过 `Player`。

| 控件 / 交互 | Player API | 备注 |
|---|---|---|
| 打开文件 / 拖放 | `SetDataSource(uri)` → `PrepareAsync()` | 二次打开前先 `StopSync()` + `Reset()`，否则 `kInvalidState` |
| 播放 / 暂停按钮 | `state()` 判分支 → `Start()` / `Pause()` | 用 `start_on_prepared=false`，让按钮说了算 |
| 停止 | `Stop()` | |
| 进度条**拖动中** | `SeekTo(pos)`（无回调） | 落关键帧，快；拖动会产生几十次，只有最后一次有意义 |
| 进度条**释放** | `SeekTo(pos, SeekMode::kAccurate, cb)` | 精确到帧 |
| 时间显示 `00:12 / 03:45` | `GetMediaTime()` / `GetDuration()` | `formatTime()` 把秒转成时间串 |
| 缓冲区（进度条浅色段） | `GetBufferedTime()` | |
| 音量滑杆 | `SetVolume(0..1)` / `volume()` | 越界自动 clamp |
| 静音按钮 | `SetMuted(bool)` / `muted()` | |
| 倍速下拉 | `SetPlaybackRate()` / `playback_rate()` | SDK clamp 到 `[0.25, 4.0]` |
| 轨道菜单 | `media_info().streams` + `SelectTrack(kAudio/kVideo, index)` | 切换后等 `kTrackChanged` 重建列表 |
| 逐帧按钮 | `StepOnce()` | 仅在 `kPaused` 生效 |
| 截图菜单项 | `TakeSnapshot(at, path)` | 完成事件 `kSnapshotCompleted` |
| 统计弹窗 | `GetPlaybackStats().ToJson()` | 也可用 `DumpDiagnostics()` 拿全量内部状态 |
| 状态文本 | `state()` + `GetPlayerStateName()` | 由 `kStateChanged` 事件驱动 |
| 错误提示 | `kError` 事件 → `ErrorPayload.error.ToString()` | |
| 时间线播放头 | `GetMediaTime()` 每 100 ms 写入模型 | 播放头与播放器是**同一个时钟** |
| 时间线点击定位 | `SeekTo()` | |
| 直播源（无时长） | `is_live()` | 进度条变只读，时间显示 `LIVE` |
| 视频尺寸 / 旋转 | `video_natural_size()` + `kVideoSizeChanged` | 旋转在转 RGB 时一次性应用 |

## 视频画面是怎么上屏的（两条路径）

avbase 已有的三个后置 sink（SDL2 / GLFW / EGL）都是"把宿主的 GL context 交给
`GlVideoSink` 去画"。**Qt 走不了这条路**：`QQuickWindow` 的 GL context 由 Qt 的
场景图线程持有，不允许从 avbase 的渲染线程 `makeCurrent()`。

所以这里用了 avbase 早就留好的另一个缝：**自定义 `VideoRendererSink`**。
`VideoRendererSink` 是 pull 模型（自己起线程调 `upstream_->Render()` 拿
`VideoFrame`），于是宿主可以把帧直接取到自己手里。两条路径共用这一个取帧通道，
只在上屏方式上分叉：

```
avbase 渲染线程                      Qt 侧
  QtVideoSink::PollOnce()
    upstream_->Render() ──► FrameMailbox ─┬─► VideoItem::refresh()          [软件]
                                           │     I420 → QImage → QPainter
                                           │     转换在 GUI 线程上做
                                           │
                                           └─► GlVideoRenderer::Upload()    [GPU]
                                                 glTexSubImage2D ×3
                                                 └─► 场景图渲染线程 render()
                                                       YUV→RGB 片元着色器
                                                       画进 Qt 给的 FBO
```

用 `AVBASE_QT_GL=1` 切到 GPU 路径，不设就是软件路径。软件路径留着不是怀旧：
它是 GPU 路径的**判据**（见下面"像素判据"）。

### 实测成本（本机 Intel Iris Plus 655，1080p，2026-10-18）

| | 854x480 | 1920x1080 |
|---|---|---|
| 软件路径：`I420ToImage`（GUI 线程） | 2.86 ms/帧 | 10.6 ms/帧 |
| GPU 路径：`Upload`（三次 `glTexSubImage2D`） | 3.73 ms/帧 | 8.2 ms/帧 |
| GPU 路径：`Paint`（提交绘制调用） | 0.47 ms/帧 | 0.45 ms/帧 |

**结论和预期不一样，值得单独写下来**：GPU 路径把算术搬走了，但没把**字节搬走**。
1080p 每帧仍有 3.1 MB 要从 CPU 内存进显存，而在这块核显上，这次拷贝的代价（8.2 ms）
和整段定点 YUV→RGB 的代价（10.6 ms）几乎一样大。854x480 下 GPU 路径甚至**更慢**
（4.2 ms vs 2.9 ms）。

所以"接上 `NativeGLContext` 就能把这段消掉"是我之前的误判。真正能消掉它的是让**解码器
直接交出 GPU 帧**（`GlHwTextureImporter` + `CVPixelBuffer`/`IOSurface` 那条路，
`platform/hwimport` 里已经有 CGL 实现，`GlVideoLayer::Upload` 也已经分好三条路线）。
在那之前，这条 GPU 路径买到的是：GUI 线程不再被像素算术占住、旋转/letterbox 由着色器
做、以及一条能承接硬件帧的管线——而不是 10 倍的帧率。

（帧率本身在这台沙箱机上一直只有 ~2 fps，软件/GPU 两条路径都一样，且与本轮改动无关：
日志里能看到队列反复 `DRY`。上面的数字是**每帧**成本，不受那个问题影响；但"能不能跑满
30 fps"这个问题本轮没有答案。）

### 像素判据：怎么证明两条路径画的是同一幅画

`AVBASE_QT_GRAB_REF=<另一条路径的抓图>` 会把抓图与参考图逐像素比较，打印平均/最大
绝对差。两条路径渲染同一个 QML 场景，所以差的就是渲染本身。

判据要成立，**画面内容必须与时间无关**：拿会动的片子抓两次，差的是"两个不同的帧"，
不是"两条路径"。`testsrc2` 也不行——它每帧烧一个时间码。可用的做法是自己合成：

```bash
ffmpeg -f lavfi -i "smptebars=size=854x480" \
  -vf "drawbox=x=0:y=0:w=90:h=70:color=white@1:t=fill,\
drawbox=x=764:y=410:w=90:h=70:color=black@1:t=fill" \
  -frames:v 1 pattern.png
ffmpeg -loop 1 -i pattern.png -t 10 -r 30 -c:v libx264 -pix_fmt yuv420p still.mp4
```

（`smptebars` 本身上下对称，所以两个对角标记是**必须的**——否则上下颠倒也测不出来。
854 宽是故意的：非 16 对齐宽度才能压出 stride 相关的花屏。）

实测：`mean_abs=0.808`，`max_abs=207`（孤立边缘像素，来自文字抗锯齿）。修朝向前是
`mean_abs=34.9`——这就是它抓到的第一个真 bug，见下。

### 四个只有接上 Qt 才会遇到的坑

1. **Qt 默认给的是 OpenGL 2.1 兼容 profile**（macOS 上实测 `2.1 INTEL-23.7.4`），
   而 `avbase_gl` 要求 **3.3 core**。症状是 `GlApi::Load` 拒绝、GPU 项显示
   "gl unavailable" 并画一块纯色背景，其余 Qt 测试全绿。修法是在创建任何窗口**之前**
   `QSurfaceFormat::setDefaultFormat()` 请求 3.3 core（macOS 随后给 4.1 core）。
1. **上下颠倒**。GL 的原点在左下、Qt 合成这个 FBO 时假设行从上往下，所以
   `QQuickFramebufferObject` 要 `setMirrorVertically(true)`。搞反了没有任何 GL 错误、
   没有任何日志，只是一幅完全合法的倒像——本项目记忆里"从渲染目标采样要 flip"
   那条规矩第一次被 Qt 这边独立撞到。
1. **glad 与 Qt 的 GL 头不能共存于同一个 TU**。macOS 上任何 Qt OpenGL 头
   （`QOpenGLContext`/`QOpenGLFunctions`/`QOpenGLFramebufferObject`）都会经
   `<QtGui/qopengl.h>` 拉进 `<OpenGL/gl3.h>`，与 `glad/gl.h` 在
   `PFNGLTEXIMAGE2DMULTISAMPLEPROC` 等类型上直接冲突（十几条 error，不是警告）。
   所以代码按"需要哪一边"拆成两个 TU：`gl_video_item.cc`（Qt，无 glad）与
   `gl_video_renderer.cc`（glad，无 Qt GL 头）。接缝是 `void*`，让编译器帮你守住这条线。
1. **`GlPresenter` 用不上**。它的契约最后一步是"把 canvas blit 到默认帧缓冲并 swap"，
   而 Qt 项里这两样都不归你：FBO 是 Qt 绑的，swap 是 Qt 合成整个窗口时做的。
   所以这里自己驱动 `GlApi` + `GlVideoLayer` + 程序 + 四边形，停在 present 之前——
   这正是 `GlPresenter` 内部同一条分界线，差别只在谁调用。

### `platform/qt/` 是第四个窗口系统适配器

`QtGLContext` 只有 45 行，和 `SdlGLContext` / `GlfwGLContext` / `EglGLContext`
是同一角色：把宿主 context 包成 `gl::NativeGLContext`。但它**头一次有两个方法是空的**：

- `MakeCurrent()` 空 —— Qt 调 renderer 之前已经绑好了；
- `SwapBuffers()` 空 —— 没有"我们的后缓冲"可以换，Qt 合成时统一呈现。

这两个空方法是"Qt 是场景图宿主、不是可嵌入窗口"这件事的全部技术后果，
也是这个 backend 和前三个不能共用 `GlVideoSink` 的原因。

## 时间线是真的模型，不是画出来的图

`TimelineModel` 是 `QAbstractListModel`：clip 有 track / start / duration，
`moveClip()` 会 clamp 并触发 `dataChanged`，`clipAt(seconds, track)` 能回答
"这个时刻这一轨上是什么"。QML 只负责把秒映射成像素。

这意味着换成 QWidget 时间线、或者做撤销/重做、或者序列化成工程文件时，
**没有任何逻辑要搬**——它们都在模型里。

还**没有**的（真编辑器需要，但和 UI 框架选型正交）：undo/redo、涟漪编辑、
吸附、每秒缩略图、音频波形、工程文件读写、导出面板（导出本身复用
`avbase::transcode`，地基是现成的）。

## 一个必须知道的 ABI 陷阱（实测踩过）

**宿主目标必须调用 `avbase_configure_abi_definitions()`，不能只链接库就完事。**

`SEQUENCE_CHECKER(name)` 这个宏在定义了 `AVBASE_ENABLE_DCHECK` 时展开成**一个成员变量**，
没定义时展开成**空**：

```cpp
// base/sequence_checker.h
#if defined(AVBASE_ENABLE_DCHECK)
#define SEQUENCE_CHECKER(name) mutable ::avbase::base::SequenceChecker name
#else
#define SEQUENCE_CHECKER(name)
#endif
```

于是库和宿主看到的是**两套类布局**。`scoped_refptr<VideoFrame>::AddRef()` 是头文件里的
内联模板，在宿主侧编译 → 按宿主的偏移写引用计数 → 写到别的字段上 → **引用计数不增长**
→ 帧被提前释放 → compositor 还在用。

实测现象（ASan）：

```
heap-use-after-free, 40 bytes inside a 232-byte region
  READ  VideoFrameCompositor::Render  (video_frame_compositor.cc:615)
  freed by  VideoItem::refresh()      (video_item.cc:236)
  allocated VideoFrame::CreateBlackFrame
```

崩溃看起来像「Qt 的 QRegion 分配失败」或「malloc_zone_error」，和真凶完全不像。
修法就是补宏，`apps/player_qt/CMakeLists.txt` 里一行：

```cmake
avbase_configure_abi_definitions(avbase_player_qt)
```

## 另一个陷阱：不要把 Qt 目标套进 `avbase_configure_target()`

那个 helper 会加 `-fno-exceptions -fno-rtti`——那是核心库的契约。Qt 的元对象系统
（`qobject_cast`、QML 运行时类型转换、moc 生成的 cast）依赖 RTTI，套上去的失败发生在
moc 生成的代码里，而不是你写的任何一行。

所以 Qt 目标走的是「**宏全一样、RTTI/异常不一样**」的组合：ABI 由
`avbase_configure_abi_definitions()` 保证，编译契约按 UI 应用来。

## 诊断开关（不设就没有任何开销）

| 环境变量 | 作用 |
|---|---|
| `AVBASE_QT_GL=1` | 用 GPU 路径（`QQuickFramebufferObject`）而不是软件路径 |
| `AVBASE_QT_GRAB=/tmp/x.png` | 抓窗口内容后退出；配合 `QT_QPA_PLATFORM=offscreen` 可在无显示环境验证（GPU 路径不行，它需要真实 GL context） |
| `AVBASE_QT_GRAB_DELAY=1500` | 抓图前的等待毫秒数（默认 1500） |
| `AVBASE_QT_GRAB_REF=/tmp/other.png` | 与参考图逐像素比较，打印 `mean_abs` / `max_abs` |
| `AVBASE_QT_GRAB_SEEK=30` | 抓图前先暂停并 seek 到该秒数。注意：avbase 的 sink 在暂停时**不再拉帧**（`GlVideoSink::PresentOne` 也是同一条件），所以暂停后的画面其实是最后一帧，这个开关只在"暂停前的画面恰好是想要的"时有用；像素判据靠的是内容与时间无关的素材，不是它 |
| `AVBASE_QT_TIMING=1` | 打印每条路径的每帧成本（见上面实测表） |
| `AVBASE_QT_NULL_VIDEO=1` | 用 avbase 的 null 视频 sink，隔离「是不是我的 sink」 |
| `AVBASE_QT_NULL_AUDIO=1` | 用 null 音频 sink，隔离音频设备 |
| `AVBASE_QT_NO_PIXELS=1` | 收帧但不做 YUV→RGB，隔离「是不是我的转换」 |

后三个是定位 ABI 那个崩溃时加的，留着是因为下次崩在别处还得用同一套二分。

## 已验证到什么程度

- 本机 Qt 6.9.3 编译通过（`AppleClang 17`，macOS），软件 / GPU 两条路径都能出画面。
- 打开 `tests/testdata/two_video_tracks.mkv` 能播放、有画面、时间线播放头跟随；
  该文件是 H.264 High 4:4:4（profile 244），**触发了 VideoToolbox 的拒绝门禁并回退软解**，
  日志里能看到 `'VideoToolboxDecoder' declined config` → `using VideoDecoderStream`。
- **像素判据**：854x480 的合成静止片（自带对角标记）上，GPU 路径相对软件路径
  `mean_abs = 0.82`、`max_abs = 207`（孤立边缘像素）。改成错误朝向后立刻变成
  `mean_abs = 34.9`，所以这个数字是有鉴别力的，不是装饰。
- 15 秒连续播放后自行退出 `EXIT=0`，6 条 avbase 线程全部干净停止。
- **ASan 全插桩构建下 0 error**（GPU 路径初始化 + 绘制 + 退出）。
- 未验证：带 display-matrix 的旋转素材（手头没有合适的；`LayerRotationToCosSin`
  本身有 `tests/unit/gl/gl_rotation_unittest.cc` 覆盖，未验的是 Qt 路径把
  `frame.rotation()` 传进去这一段）；音频是否真的出声（沙箱无听感手段）；多显示器 /
  全屏；Windows/Linux；`QQuickRhiItem`（Qt 6.7+ 的现代写法，它走 RHI 管线，
  等于放弃 `render/gl`，与本项目的目的相反）。
- 一个与本轮改动无关但值得记的观察：这台沙箱机上播放速率只有 ~2 fps
  （10 秒里只产生 18~21 帧，日志里队列反复 `DRY`），两条路径一样。上面的每帧成本
  不受影响，但"能不能跑满 30 fps"本轮没有答案。

