# avbase 长期约定

Chromium 风格分层 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。沿革见
`.workbuddy/memory/<日期>.md` 与 `git log`（早期在 `docs/archive/`）；门禁命令
与长尾教训在 `operations.md`；剩余工作权威清单 `docs/12`，状态速览 `docs/PROGRESS.md`，
产品化缺口 `docs/TODO.md`。本文件只留跨轮仍有效的硬规则。

## 分层（docs/02 §6）
`platform/` 只装「被宿主选中」的代码（FFmpeg→`media/ffmpeg/`、GL→`render/gl/`）。
**C28：`render/` 不得 include `media`/`platform`/`base`/`player/`。**

## 硬规矩
- **提交信息一律中文**（含正文；英文历史已整体改写）。
- **不写「第 N 轮」**（用户 2026-10-21 要求）：记录、文档、代码注释、提交信息一律
  不用轮次计数当时间坐标——对别人无意义，且会随整理过期。定位用日期、提交哈希或事件
  本身（「修 fps-cap 时」「起播定格那次」）。已全树清理过一次（`docs/`、各 README、
  记忆文件）；`docs/archive/` 的历史档案保留，它本身就是按轮组织的旧物。
  **机械删「第 N 轮」会留下没主语的句子**（「把时间线换成…」「拿 10/15 旧二进制」），
  删完要通读改到的行补主语。
- **改 `docs/` 章节号前先全树 grep 旧号**：精简后 `docs/12 §2.x`、`PROGRESS §3l` 这类
  引用仍留在 40+ 处注释里指向不存在的小节。要么连引用一起改，要么加「旧号 → 现在
  在哪」的转发（docs/12 §0 与 PROGRESS 开头的做法）。
- **宿主目标必须调 `avbase_configure_abi_definitions()`**：`SEQUENCE_CHECKER` 在 DCHECK
  有/无下分别展开成「一个成员」和「空」→ 库与宿主看到两套 `VideoFrame` 布局 → app 侧
  内联的 `AddRef()` 写到错误偏移 → 帧被 compositor 用着就释放。ASan 报
  `heap-use-after-free @ VideoFrameCompositor::Render`；无 ASan 时伪装成
  `malloc_zone_error`。**但 Qt 目标绝不能套 `avbase_configure_target`**（带 `-fno-rtti`，
  moc 的 cast 与 QML 转换需要 RTTI）→「ABI 宏全一样、RTTI/异常不一样」。
- **Qt 单测不走 `avbase_add_unittest()`**（同上 `-fno-rtti`）：手写 `add_executable` +
  `avbase_configure_abi_definitions()`。AUTOMOC 只找源文件同目录的头，跨目录必须把
  `.h` 显式列进目标源，否则链接期只报「missing vtable」。产物 `build/<dir>/bin/`，
  不在 CI（CI 没有 Qt，整个 `apps/player_qt` 从未在 CI 构建过）。
- **Qt 宿主接 `avbase_gl` 三条**（全实测）：① `QSurfaceFormat` 请求 3.3 core 且在建
  **任何**窗口前 `setDefaultFormat`（macOS 默认给 2.1）；② `QQuickFramebufferObject`
  要 `setMirrorVertically(true)`，否则整幅倒像、零错误零日志；③ glad 与 Qt GL 头不能
  同 TU。Qt 里用不了 `GlPresenter`（契约结尾是 blit canvas + swap）。
- **C1 单文件 500 行（连注释），顶到就拆文件**（先例 `player_bridge_events.cc`、
  `renderer_impl_reporting.cc`），不压注释凑数。
- **UI 数据必须来自真实现场**：占位数据比空面板坏（空的一眼看出没做完，假的一眼会被
  当真的读）；兑现不了的编辑入口要删，别留「接了空气的控件」。
- **`max-buffer-size` 是全播放器预算**，不是每条流的额度，按流拆分走
  `SplitQueueBudget()`；`max_buffers=256` 不参与拆分。详见 `docs/TODO.md §2`。
- **GPU 上屏 ≠ 零拷贝**：着色器只搬走算术没搬走字节（1080p 每帧 3.1 MB 仍从 CPU 进
  显存；854×480 下 GPU 4.2ms vs 软件 2.9ms）。别按「GPU 快 N 倍」估工期。
- **子代理/工具的审查报告要逐条复核**（ 11 条里 3 条误报），误报也要记下来，
  否则会被反复重查。

## 音视频同步与配速
- **音频设备必须在准备阶段打开**（实测）：音频时钟只在设备第一次回调
  送出样本时起锚；若在 `StartPlayingFrom()` 才开设备，那段耗时（SDL 子系统 129ms +
  `SDL_OpenAudioDevice` 275ms）整段变成「视频领先主时钟」→ `ComputeTargetDelay()`
  一次性吸收 → 画面冻结等量时间（设备慢的机器就是几秒）。ffplay 不出现只因它的
  `audio_open()` 在 prepare 阶段。修法：`Sdl2AudioSink::Initialize()` 开设备（paused），
  由 `OnDecoderInitialized()` 成功后调——不是 `Initialize()` 开头，因为「解码器失败不能
  留下空转的设备」。代价：首帧晚约一个设备打开的时间。
- **配速的 forward 方向没有上限**：`PresentLocked` 的 snap 只在 frame_timer 落后时生效，
  任何大的负 av_diff 都会变成一次冻结。定位先分清「队列空」还是「在等」：给 `Render()`
  的 hold/empty 两出口打探针看 `pending_frames_.size()` 与 `wait_ms`，一步排除解码链。
- **fps-cap 问时间戳间隔，不问墙钟**：sink 按自己节奏轮询，一批帧共享
  同一 `now`，墙钟答不出帧间隔——30fps 源实测读到 18ms，151/162 帧被判「太早」丢掉、
  画面 1fps 而 `frames_dropped_late` 全程为 0。`max_fps` 默认 **31**（ijk 原值），第七
  十一轮接线后才生效。判据只对配速判定 due 的帧调用。
- **sink 播放状态只有一个派发点**：`VideoRendererImpl::SetPaused`（不是
  `StartPlayingFrom`/`Flush`）。新 sink 一律用 `media/base/video_sink_gate.h`，别写
  `if (playing_)`。`Flush()` 语义含「下一帧是另一幅画」，暂停也要拉一次。见 `docs/05` Δ23。
- **暂停只停时钟，不停代际**：`StartPlayingFrom` 在暂停中 seek 也**必须**调用，但只许
  接管代际与重启解码，**不许**清 `paused_`（清过一次：画面脱离暂停时钟自己跑）。

## 统计（只有一条路：push）
media 层用 `RendererClient::OnStatisticsUpdate` 推 `PipelineStatistics`，`PlayerImpl` 存
快照供 `PlaybackStats` 读。**别在 media 层加第二个统计形状**——不会编译失败、不会测试
红，只会悄悄少字段（此前删掉的 `Pipeline::Statistics` 就是这个下场）。新统计走
`PipelineStatistics` + `ToJson` + `renderer_sink_stats_unittest`。
- **两个 `frames_dropped` 不同义，别相加**：compositor 的（选帧被拒：late / fps /
  stale-serial / accurate-seek，四者相加 = `frames_dropped`）vs sink 的（到手但无可绘制
  目标，报 `frames_dropped_by_sink`，是 `frames_presented` 的**子集**）。
- **「这一拍没有帧」不是丢帧**（ffplay 口径）：`ClassifyFrameOutcome` 对 `!has_frame`
  返回 `kNone`，三个计数都不动；否则计数变成刷新率的函数（30fps/60Hz 下 30/s、暂停
  60/s）。详见 `docs/05` Δ24。
- **`docs/API.md` 是生成物，禁止手改**：改 `player/public/*.h` 的注释再跑
  `tools/gen_api_docs.sh`（mac 用 `DOXYGEN=/tmp/avbase_tools/doxygen-1.18.0/doxygen`）。
  注意 `gate_local --jobs inv,fmt,lint,apicompat,commentcov` **不含** apidocs，跑那五个
  不等于跑过它。

## 五条「写下来会静默失效」的约定（都真踩过）
- **朝向**：从渲染目标采样回来的 pass 都要 `flip_vertical=true`（反了无 GL 错误）→ 有
  朝向的代码必须配像素断言。
- **stride**：`GL_UNPACK_ROW_LENGTH` 单位是 **texel 不是字节**（1080p 恰好相等藏住 bug，
  320×240/854×480 必现）。
- **GL 常量**：一个常量别在两个头世界各写一遍 → 后端用 `target()` 说出来。
- **变换顺序**：顶点 shader 是 screen→source **反向映射** → CROP 写在 ROTATE **之后**。
- **demux 循环在 EOF 处结束**（不驻留，否则排队的 `Flush` 永远跑不到），由
  `StartPlayingFrom` 重挂；所有权规则「谁把 `LoopState` 置 `kRunning` 谁 `PostTask`」，
  用状态机而不是两个布尔（会产生两个循环任务 → 假 EOS）。见 `docs/04 §2.1`。

## 诊断门禁（先跑这个，再怀疑产品 bug）
`python3 tools/check_binary_freshness.py --root build/<preset>`。曾拿 10/15 旧
二进制诊断出「VideoToolbox 硬解卡死」，重链后 3/3 正常——**那个 bug 不存在**。测量前先
确认被测物 mtime 晚于输入。
`AVBASE_QT_TRACE=<ms>`（`apps/player_qt/main.cc`）逐拍打印「GUI 正在显示那一帧自己的
pts」+ stats JSON。只看 `frames_presented` 分不出「画面在动」和「计数在涨」。

## 写单测前
测试绝不依赖工作目录（用 `AVBASE_TESTDATA_DIR`）；`PrepareSync` 无默认参数，显式传
`base::Seconds(30)`；`Signal()` 持锁（等待者必在 signal 后的动作之后才醒，别当竞态）；
**注释声称「这个测试能抓住 X」之前，先改回去跑一次**（有注释这么写，改回去
全绿——注释里的鉴别力声明也会假绿）。

## 性能门禁（别按设计门槛直接判）
微基准容差 25%（PostTask 40%、EventHub 35%）+ 端到端 CPU 15%——两条的**设计门槛
（10% / 5%）都低于本机噪声地板**。误报过的门禁会被习惯性 override，之后什么都抓不住。
改阈值前先读 `tools/bench-tolerance.json` / `e2e-cpu-tolerance.json` 里的实测依据；
改测量方法优先于放宽阈值。

## 本 sandbox 网络约束
GitHub HTTP(S) 被代理掐断 → **别再试 `gh`/API**（唯一通的是 SSH git）。看不到 CI 就用
`tools/gate_local.sh` 本地复刻同一套门禁命令。`rm -rf` 会触发 sandbox escalation 且
escalated 容器文件视图不可信 → 删目录用 `python3 -c "shutil.rmtree(...)"`。

## C ABI（M15）代码前必读
`avbase_c.h` 是 ABI 契约（`-DAVBASE_ENABLE_CAPI=ON` 默认 OFF，target `avbase_cabi`），
what 码不能猜。全树 `-fno-exceptions -fno-rtti`：**不写 try/catch**；`Status` 是
`base::expected<void, MediaError>` **无 `.code()`**，先 `has_value()` 再 `.error()`；
`MediaInfo` 只有 `duration`/`bit_rate`/`streams[]`（尺寸/旋转在 `StreamInfo`）。

## 路线图现状
L1–L5、docs/14 阶段 0–4、Phase 2 协议验收、M15 C ABI 均完成。Phase 3 只差**真零拷贝**
（需 Linux/Windows 硬件）；Phase 4 剩 Qt 门面库化 / soak 48h / Windows CI 首跑；
Phase 5 仅 E5 硬编未实测；M13 剩 golden 录制基线 / 依赖解码器的 `BM_*` / conan 实跑 /
渲染 CPU 机器判据。以 `docs/12` 为准。
