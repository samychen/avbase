# player_qt — Qt Quick 播放器 / 编辑器骨架

一个能真正跑起来的 Qt 6 应用：视频有画面、有声音、能拖进度、能切轨道，
下面挂一条 Shotcut 风格的多轨时间线。它同时是**两个可借用的模板**：

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
  -DAVBASE_BUILD_QT_PLAYER=ON \
  -DCMAKE_PREFIX_PATH=/usr/local/opt/qt      # 换成你的 Qt 6.4+ 前缀
cmake --build build/qt-player --target avbase_player_qt
./build/qt-player/bin/avbase_player_qt /path/to/video.mp4
```

`AVBASE_BUILD_QT_PLAYER` 默认 **OFF**，所以不带 Qt 的机器照常构建整个树。
`AVBASE_ENABLE_SDL2=ON` 时链接 `Sdl2AudioSinkFactory`（**有声音**）；关掉也能编，
只是退回 avbase 的 null sink（静音），和 `play_glfw` 一样。

## 目录

| 文件 | 职责 | 行数量级 |
|---|---|---|
| `main.cc` | 组装根：QML 引擎 → bridge → mailbox 的销毁顺序 | ~100 |
| `player_bridge.{h,cc}` | **唯一**让 avbase 与 Qt 相遇的地方：事件跨线程投递 + Q_PROPERTY | ~450 |
| `qt_video_sink.{h,cc}` | 自定义 `VideoRendererSink`，拉帧丢进 mailbox | ~150 |
| `frame_mailbox.h` | 渲染线程 → GUI 线程的单槽帧交换 | ~60 |
| `video_item.{h,cc}` | `QQuickPaintedItem`，I420 → RGB → QPainter | ~250 |
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

## 视频画面是怎么上屏的

avbase 已有的三个后置 sink（SDL2 / GLFW / EGL）都是"把宿主的 GL context 交给
`GlVideoSink` 去画"。**Qt 走不了这条路**：`QQuickWindow` 的 GL context 不允许
从 avbase 的渲染线程 `makeCurrent()`。

所以这里用了 avbase 早就留好的另一个缝：**自定义 `VideoRendererSink`**。
`VideoRendererSink` 是 pull 模型（自己起线程调 `upstream_->Render()` 拿
`VideoFrame`），于是宿主可以把帧直接取到自己手里：

```
avbase 渲染线程                     GUI 线程
  QtVideoSink::PollOnce()
    upstream_->Render() ──► FrameMailbox ──► VideoItem::refresh()
                                                   │ ToI420()（硬件帧才需要）
                                                   │ I420 → QImage
                                                   ▼ QPainter::drawImage
```

代价是每帧一次 YUV→RGB 转换 + 一次纹理上传，在 CPU 上做。1080p30 够用；
要零拷贝就得往下走一步。

### 往零拷贝走的那一步（未实现）

`render/gl/native_gl_context.h` 的接口只有 5 个方法
（`GetProcAddress` / `MakeCurrent` / `ReleaseCurrent` / `SwapBuffers` /
`GetDrawableSize`）。照 `platform/glfw/glfw_gl_context.cc`（45 行）的样子写一个
Qt 版，再仿 `platform/glfw/glfw_video_sink.cc`（47 行）加一个 backend，
avbase 就能直接画进 Qt 的 FBO，画面零拷贝、色彩/旋转/overlay 全由 avbase 处理。

难点不在代码量，在**所有权**：Qt 要求在自己的渲染线程里画，而 avbase 的 sink
也要自己的线程绑 context。可行方案是 `QQuickFramebufferObject`（或 Qt 6 的
`QQuickRhiItem`）+ 把 avbase 的 `GlVideoSink` 改成"画到调用方给的 FBO"。
这是一次真实的重构，不是加个文件。

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
| `AVBASE_QT_GRAB=/tmp/x.png` | 抓窗口内容后退出；配合 `QT_QPA_PLATFORM=offscreen` 可在无显示环境验证 |
| `AVBASE_QT_GRAB_DELAY=1500` | 抓图前的等待毫秒数（默认 1500） |
| `AVBASE_QT_NULL_VIDEO=1` | 用 avbase 的 null 视频 sink，隔离「是不是我的 sink」 |
| `AVBASE_QT_NULL_AUDIO=1` | 用 null 音频 sink，隔离音频设备 |
| `AVBASE_QT_NO_PIXELS=1` | 收帧但不做 YUV→RGB，隔离「是不是我的转换」 |

后四个是本轮定位崩溃时加的，留着是因为下次崩在别处还得用同一套二分。

## 已验证到什么程度

- 本机 Qt 6.9.3 编译通过（`AppleClang 17`，macOS）。
- 打开 `tests/testdata/two_video_tracks.mkv` 能播放、有画面、时间线播放头跟随；
  该文件是 H.264 High 4:4:4（profile 244），**触发了 VideoToolbox 的拒绝门禁并回退软解**，
  日志里能看到 `'VideoToolboxDecoder' declined config` → `using VideoDecoderStream`。
- 15 秒连续播放后自行退出 `EXIT=0`，6 条 avbase 线程全部干净停止。
- **ASan 全插桩构建下 0 error**（这也是最后定位到 ABI 问题的方式）。
- 未验证：音频是否真的出声（沙箱里没有可用听感验证手段，只能确认
  `Sdl2AudioSinkFactory` 被链接、音频解码器就绪）；多显示器/全屏切换；Windows/Linux。

