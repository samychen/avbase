# avbase 长期约定

Chromium 风格分层 C++ 媒体播放器，目录即 target（`docs/06 §7.6`）。剩余工作看 `docs/12`，
状态速览 `docs/PROGRESS.md`。本文件只留最硬的规则；门禁命令与工具路径等长尾见
`operations.md`，逐日沿革见 `<日期>.md`。

## 分层
`platform/` 只装「被宿主选中」的代码，可移植代码不进。**C28：`render/` 不得 include
`media`/`platform`/`base`/`player/`。**

## 硬规矩
- **提交信息一律中文**（含正文）；**不写「第 N 轮」**（`docs/archive/` 例外）；机械删会
  留下没主语的句子，删完通读补主语。
- **改 `docs/` 章节号前先全树 grep 旧号**：要么连引用一起改，要么加「旧号 → 现在在哪」转发。
- **C1 单文件 500 行（连注释），顶到就拆文件**，不压注释凑数。
- **UI 数据必须来自真实现场**；兑现不了的编辑入口要删（占位数据比空面板坏）。
- **文档里带数字的旧观察与修复同步更正**（旧「~2fps」是 bug 症状，比没写更危险）。
- **子代理/工具的审查报告逐条复核**（曾 11 条里 3 条误报）；误报也要记。
- **怀疑产品 bug 前先跑 `tools/check_binary_freshness.py`**：两次「bug 不存在」都是旧二进制
  诊断出来的（硬解卡死、ctest 736 vs 784）。它除「二进制 vs 链接的库」外还比「源码 vs
  二进制」，`--binary` 可收窄到单个可执行文件（`bench_render.sh` 只问 play_glfw）。

## 宿主 / Qt 目标
- 宿主目标必须调 `avbase_configure_abi_definitions()`：DCHECK 开关改变 `VideoFrame` 布局
  → 帧被用着就释放（无 ASan 时伪装成 `malloc_zone_error`）。
- **Qt 目标绝不能套 `avbase_configure_target`**（带 `-fno-rtti`，moc/QML 要 RTTI）→「ABI
  宏全一样、RTTI 不同」。Qt 单测同此：手写 `add_executable`，AUTOMOC 跨目录要把 `.h`
  显式列进目标源（否则只报 missing vtable）。
- Qt 接 `avbase_gl` 三条（实测）：① 请求 3.3 core 且在建**任何**窗口前
  `setDefaultFormat`；② `QQuickFramebufferObject` 要 `setMirrorVertically(true)`；
  ③ glad 与 Qt GL 头不能同 TU。Qt 里用不了 `GlPresenter`。
- 门面库 `player/qt/` 已抽出。**链接一个库 = 求值它的 INTERFACE**：`find_package(Qt6 Core)`
  找不齐会在 generate 阶段红。
- **cpplint 把 `<QFoo>` 当 C 系统头**（无 `.h`）→ 必须排在 C++ 标准库之前；`<SDL.h>` 有
  `.h` 照常在前。别为「看着该在后面」挪回去。

## 音视频同步与配速
- **音频设备必须在准备阶段打开**：时钟只在设备首次回调送出样本时起锚；`StartPlayingFrom()`
  才开（SDL 129+275ms）会让那段整段变成「视频领先」→ 被吸收成冻结。修法：`Initialize()`
  开设备（paused），由 `OnDecoderInitialized()` **成功后**调。
- **配速 forward 没有上限**：大负 av_diff = 一次冻结。定位先分清「队列空」还是「在等」。
- **fps-cap 问时间戳间隔，不问墙钟**（sink 轮询使一批帧共享 `now`）。`max_fps` 默认 31。
- sink 播放状态只有 `VideoRendererImpl::SetPaused` 一个派发点，新 sink 用
  `video_sink_gate.h`；**暂停只停时钟不停代际**（`StartPlayingFrom` 不许清 `paused_`）。

## 统计与文档生成物
- 统计只有 push 一条路（`OnStatisticsUpdate` 推 `PipelineStatistics`），**别加第二个统计
  形状**（不编译失败、不测试红，只会少字段）。两个 `frames_dropped` 不同义、别相加
  （选帧被拒 vs 无可绘制目标）；「这一拍没有帧」不是丢帧。
- **`docs/API.md` 是生成物，禁止手改**：改头注释后跑 `tools/gen_api_docs.sh`，门禁判据是
  重生成零 diff。

## 五条「写下来会静默失效」的约定
朝向（采样回来的 pass 要 `flip_vertical=true`，配像素断言）· stride
（`GL_UNPACK_ROW_LENGTH` 单位是 texel）· GL 常量（后端用 `target()` 说）· 变换顺序
（screen→source 反向映射，CROP 写在 ROTATE 之后）· demux 在 EOF 结束并由
`StartPlayingFrom` 重挂（两个布尔 → 假 EOS）。

## 编码器与转码（media/transcode）
- **`avcodec_open2` 会 free 你传进去的字典**（成功/失败都这样），`*dict` 被换成装着未认
  选项的新对象 → `DictPtr` 持有再析构 = 双重释放（SIGABRT）。正解：`release()` 出去，
  `av_dict_free(&raw)` 收尾。
- **「工厂说出编码器名字」≠「这个构建真有」**：`setup_ffmpeg.sh` 是白名单构建，
  `h264_videotoolbox` 曾不在里面 → 测试全绿而 `Initialize()` 必失败。硬件编码器几乎都
  需要私有选项才开（VT `allow_sw`、VAAPI device）；Intel Mac 无可用的 VT H.264 会话。
- `media/transcode/` 已有生产入口 `avbase-inspect transcode`（默认 mjpeg+aac）。

## 路线图
L1–L5、docs/14 阶段 0–4、Phase 2 协议验收、M15 C ABI 均完成。剩：golden 基线录制、
soak 48h、Windows CI 首跑、真零拷贝（VAAPI/D3D11）、E5 硬编本体、conan 实跑。
**以 `docs/12` 为准。**
