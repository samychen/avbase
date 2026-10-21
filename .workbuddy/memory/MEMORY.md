# avbase 长期约定

Chromium 风格分层 C++ 媒体播放器，目录即 target（`docs/06 §7.6`）。剩余工作权威清单
`docs/12`，状态速览 `docs/PROGRESS.md`，产品化缺口 `docs/TODO.md`。本文件只留最硬的规则
（有大小上限）；门禁命令、工具路径、写单测与性能阈值等长尾见 `operations.md`，逐日沿革
见 `<日期>.md` 与 `git log`。

## 分层
`platform/` 只装「被宿主选中」的代码。**C28：`render/` 不得 include
`media`/`platform`/`base`/`player/`。**

## 硬规矩
- **提交信息一律中文**（含正文）。
- **不写「第 N 轮」**：记录/文档/注释/提交信息不用轮次计数，定位用日期、哈希或事件。
  `docs/archive/` 例外。**机械删会留下没主语的句子**，删完要通读补主语；批量脚本只做
  目标替换、不碰格式（顺手规整空格曾把 API.md 压平 368 行，整份回滚）。
- **改 `docs/` 章节号前先全树 grep 旧号**（精简后旧引用仍散在 40+ 处注释）：要么连引用
  改，要么加「旧号 → 现在在哪」转发。
- **C1 单文件 500 行（连注释），顶到就拆文件**，不压注释凑数。
- **UI 数据必须来自真实现场**；兑现不了的编辑入口要删（占位数据比空面板坏）。
- **文档里带数字的旧观察要与修复同步更正**（Qt README 曾留「本机 ~2fps」，那是 fps-cap
  bug 的症状，比没写更危险）。
- **子代理审查报告逐条复核**（11 条里 3 条误报），误报也要记下来。
- **怀疑产品 bug 前先跑 `tools/check_binary_freshness.py`**（曾拿旧二进制诊断出「硬解
  卡死」，重链后 3/3 正常——那个 bug 不存在）。

## 宿主 / Qt 目标
- 宿主目标必须调 `avbase_configure_abi_definitions()`：`SEQUENCE_CHECKER` 在 DCHECK 有/无
  下展开不同 → 库与宿主两套 `VideoFrame` 布局 → 帧被用着就释放（无 ASan 时伪装成
  `malloc_zone_error`）。
- **Qt 目标绝不能套 `avbase_configure_target`**（`-fno-rtti`，moc/QML 要 RTTI）→ ABI 宏
  全一样、RTTI 不同。Qt 单测同此，手写 `add_executable`；AUTOMOC 跨目录须把 `.h` 显式
  列进目标源（否则只报 missing vtable）。CI 的 `qt` job 与 `gate_local --jobs qt`
  建这两个目标（preset `qt`，不开 FFmpeg；Homebrew Qt 需 `AVBASE_QT_PREFIX`）。
- Qt 接 `avbase_gl` 三条（实测）：① 3.3 core 且在建**任何**窗口前 `setDefaultFormat`；
  ② `QQuickFramebufferObject` 要 `setMirrorVertically(true)`（否则倒像、零错误）；③ glad
  与 Qt GL 头不能同 TU。Qt 里用不了 `GlPresenter`。

## 音视频同步与配速
- **音频设备必须在准备阶段打开**：时钟只在设备首次回调送出样本时起锚；若
  `StartPlayingFrom()` 才开（SDL 129+275ms），那段整段变成「视频领先」→ 被
  `ComputeTargetDelay()` 吸收成冻结。修法：`Initialize()` 开设备（paused），由
  `OnDecoderInitialized()` **成功后**调。
- **配速 forward 没有上限**：大的负 av_diff 会变成一次冻结。先分清「队列空」还是
  「在等」——给 `Render()` 的 hold/empty 出口打探针看 `pending_frames_.size()`/`wait_ms`；
  `AVBASE_QT_TRACE=<ms>` 打印屏上那一帧自己的 pts。
- **fps-cap 问时间戳间隔，不问墙钟**（sink 轮询使一批帧共享 `now`，墙钟答不出帧间隔 →
  30fps 源实测 1fps）。`max_fps` 默认 31。
- sink 播放状态只有 `VideoRendererImpl::SetPaused` 一个派发点；新 sink 用
  `media/base/video_sink_gate.h`。**暂停只停时钟不停代际**：`StartPlayingFrom` 不许清
  `paused_`。

## 统计（只有 push 一条路）
media 层用 `RendererClient::OnStatisticsUpdate` 推 `PipelineStatistics`；**别加第二个统计
形状**（不编译失败、不测试红，只会少字段）。两个 `frames_dropped` 不同义、别相加
（compositor 选帧被拒 vs sink 无可绘制目标）。「这一拍没有帧」不是丢帧（`kNone`）。
`docs/API.md` 是生成物，禁止手改。

## 五条「写下来会静默失效」的约定
朝向（采样回来的 pass 要 `flip_vertical=true`，配像素断言）· stride（`GL_UNPACK_ROW_LENGTH`
单位是 texel）· GL 常量（后端用 `target()` 说）· 变换顺序（screen→source 反向映射，CROP
写在 ROTATE 之后）· demux 在 EOF 结束并由 `StartPlayingFrom` 重挂（两个布尔 → 假 EOS）。

## 路线图
L1–L5、docs/14 阶段 0–4、Phase 2 协议验收、M15 C ABI 均完成。Phase 3 只差真零拷贝（需
硬件）；Phase 4 剩 Qt 门面库化 / Qt 进 CI / soak 48h / Windows CI 首跑；Phase 5 仅 E5 硬编
未实测；M13 剩 golden 基线 / `BM_*` / conan / 渲染 CPU 判据。以 `docs/12` 为准。
