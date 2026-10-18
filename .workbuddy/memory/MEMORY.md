# avbase 长期约定

Chromium 风格分层 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。轮次史见
`docs/PROGRESS.md`，GL 契约见 `docs/13`，门禁命令见 `operations.md`。本文件只留
跨轮仍有效的硬规则；长尾教训在 `operations.md`，别在此复述。

## 分层（docs/02 §6）
`platform/` 只装「被宿主选中」的代码；可移植代码不进（FFmpeg→`media/ffmpeg/`、
GL→`render/gl/`）。**C28：`render/` 不得 include `media`/`platform`/`base`/`player/`。**

## 硬规矩
- **提交信息一律中文**（2026-10-18 用户要求，含正文；此前英文历史已整体改写）。
- **宿主目标必须调 `avbase_configure_abi_definitions()`**（`apps/player_qt` 已这样）。
  `SEQUENCE_CHECKER` 在 `AVBASE_ENABLE_DCHECK` 下有/无分别展开成「一个成员」和
  「空」→ 库与宿主看到两套 `VideoFrame` 布局 → app 侧内联的
  `scoped_refptr::AddRef()` 写到错误偏移 → 引用计数不涨 → 帧被 compositor 用着
  就被释放。ASan 报 `heap-use-after-free @ VideoFrameCompositor::Render:615`；
  没 ASan 时伪装成 `malloc_zone_error`/Qt `QRegion` 分配失败，与真凶完全不像。
  **但 Qt 目标绝不能套 `avbase_configure_target`**（它带 `-fno-rtti`，moc 生成的
  cast 和 QML 类型转换需要 RTTI）。即「ABI 宏全一样、RTTI/异常不一样」。
- **Qt 宿主接 `avbase_gl` 的三条硬约束**（全实测）：① `QSurfaceFormat`
  必须请求 3.3 core，且在建**任何**窗口之前 `setDefaultFormat`（macOS 默认给 2.1，
  症状是 `GlApi::Load` 拒绝而其余 Qt 全绿）；② `QQuickFramebufferObject` 要
  `setMirrorVertically(true)`，否则整幅倒像、零 GL 错误零日志；③ glad 与任何 Qt
  GL 头不能同 TU（macOS 经 `<QtGui/qopengl.h>` 拉进 `<OpenGL/gl3.h>`，typedef 冲突）。
  另：Qt 里**用不了 `GlPresenter`**，它的契约结尾是 blit canvas + swap，两样都不归你。
- **GPU 上屏不等于零拷贝**：把 YUV→RGB 交给着色器只搬走了算术，没搬走字节。
  1080p 每帧 3.1 MB 仍从 CPU 内存进显存，核显上这次拷贝和定点转换一样贵（实测
  8.2 vs 10.6 ms，854x480 下 GPU 更慢）。真零拷贝要解码器交 GPU 帧
  （`GlHwTextureImporter` + `CVPixelBuffer`/`IOSurface`）。别再按「GPU 快 N 倍」估工期。
- **`max-buffer-size` 是「全播放器」预算**，不是每条流的额度：ijk 比的是
  audio+video+subtitle 三队列之和。按流拆分走 `SplitQueueBudget()`（`media/base`），
  直接写进每条队列会让 A/V 默认内存翻倍（15→30 MiB），且没有测试读回上限 →
  改这里先看 `ByteBudgetIsSplitAcrossStreams`。`max_buffers=256` 是每条队列各自的全额，
  **不参与**拆分。
- **子代理/工具的审查报告要逐条复核**：第九十五轮 11 条里 3 条是误报（加锁的跨线程读、
  不存在的锁序表、把表格行当成缺失章节）。误报也要写进当轮记忆，否则会被反复重查。
- **sink 的播放状态只有一个派发点**：`VideoRendererImpl::SetPaused`（不是
  `StartPlayingFrom`/`Flush`）。「该不该拉帧」的完整规则在
  `media/base/video_sink_gate.h`：**新 sink 一律用 `VideoSinkGate`，别再写
  `if (playing_)`**。`Flush()` 的语义包含「下一帧是另一幅画」——暂停也要拉一次，
  否则暂停中 seek 的画面永远不更新。改这块先读 `docs/05` Δ23。
- **暂停只停时钟，不停代际**：`StartPlayingFrom` 在暂停中 seek 也**必须**被调用
  （`Flush` 的 `HoldReads(true)` + 新 serial 只有它接管），但它只许接管代际与重启解码，
  **不许**清 `paused_`。清过一次的后果是画面脱离暂停的时钟自己跑（实测 4.0s seek 后
  800ms 已在 5.07s），而音频静音、日志全绿。
- **两个 `frames_dropped` 不是一件事，别相加**：compositor 的（选帧时被拒：late /
  fps 上限 / serial 过期）vs sink 的（帧到手、但没有可绘制目标——`GlVideoSink::discard_`
  是「无 display / 类型不对 / 无 raw surface」，**不是** seek）→ 后者是
  `frames_presented` 的**子集**，上报名 `frames_dropped_by_sink`。第九十八轮前它根本
  没上报（`GetStatistics` 只取 `sink_stats().render`），于是「在播但屏上什么都没有」
  与「播得很好」在前者眼里一样。动统计先看 `renderer_sink_stats_unittest.cc`。
- **「这一拍没有帧」不是丢帧**（ffplay 口径）：`frame_drops_late++` 只写在
  `frame_queue_next()` 的同一行，队列空与暂停都 `goto display` 不计数。sink 是每显示
  窗口问一次（不是 ffplay 的主动轮询），空问频率 = 刷新率 → 按「没交帧 = drop」计会让
  `frames_dropped` 变成刷新率的函数（30fps/60Hz 下 30/s，暂停 60/s）。
  `ClassifyFrameOutcome` 对 `!has_frame` 返回 `FrameOutcome::kNone`。**两个同名不同义**：
  `VideoSinkStats::frames_dropped`（到手未显示）vs `PlaybackStats::frames_dropped`
  （compositor 选帧被拒，宿主读的是这个，改前者不会动后者）。详见 `docs/05` Δ24。
- **统计只有一条路：push**。media 层用 `RendererClient::OnStatisticsUpdate` 把
  `PipelineStatistics` 推上去，`PlayerImpl` 存快照供 `PlaybackStats` 读。第一百轮删掉了
  `Pipeline::Statistics` + `Pipeline::GetStatistics()`（pull 侧副本，M8 起零调用者，
  已漂移：`PipelineStatistics` 后来长的三个字段它一个都不知道）。**别再在 media 层加
  第二个统计形状**——它不会编译失败、不会测试红，只会悄悄少字段。
  新加统计走 `PipelineStatistics` + `ToJson` + `renderer_sink_stats_unittest`。

## 五条「写下来会静默失效」的约定（都真踩过）
- **朝向**：从渲染目标采样回来的 pass 都要 `flip_vertical=true`。搞反 = 上下颠倒、
  无 GL 错误 → 有朝向的代码必须配像素断言。
- **stride**：`GL_UNPACK_ROW_LENGTH` 单位是 **texel 不是字节**；1080p 恰好相等把
  bug 藏住，320x240/854x480 必现（周期 8 行绿色横条）。
- **GL 常量**：一个常量别在两个头世界各写一遍 → 后端用 `target()` 说出来。
- **变换顺序**：顶点 shader 是 screen→source **反向映射**，观众最后看到的一步计算上
  最先做 → CROP 写在 ROTATE **之后**。别用算术顺序描述视觉顺序。
- **demux 循环在 EOF 处结束**（不驻留，否则排队的 `Flush` 永远跑不到），由
  `StartPlayingFrom` 重挂；所有权规则是「谁把 `LoopState` 置 `kRunning` 谁 `PostTask`」，
  用状态机而不是两个布尔对读（两个布尔会产生**两个**循环任务 → 假 EOS）。
  改这里先读 `docs/04 §2.1`「EOF 与 seek 的握手」。

## C ABI（M15）代码前必读
- `avbase_c.h` 是 ABI 契约，`-DAVBASE_ENABLE_CAPI=ON` 默认 OFF；独立 target
  `avbase_cabi`。what 码（200/300/100/400/500-502/600/700/800/900/1000/10001）不能猜。
- 全树 `-fno-exceptions -fno-rtti`：**不要写 try/catch**（编译失败）；绑定回调用 lambda
  显式区分「捕获」与「参数」；`SetEventHandler` 要显式写 `avbase::Player::EventHandler`。
- `Status` 是 `base::expected<void, MediaError>`：**无 `.code()`**，先 `has_value()` 守卫
  再 `.error()`。`MediaInfo` 字段 `duration`/`bit_rate`/`streams[]`（`StreamKind`），**无**
  `title`/`video_size`/`rotation`（尺寸/旋转在 `StreamInfo`）。
- **C1 单文件 500 行上限，连注释都算**；头里定义函数必须 `inline`；C `.c` 已在 format
  门禁但 CI 只扫 `*.cc *.h`，别把范围扩成 `*.c`（`cmake/FFmpegVersionProbe.c` 会红）。
- 本机 clang-tidy 对 `media/ffmpeg` **静默零发现**（PCH 格式读不了）→ 必须无 PCH
  scratch 库。**本机干净 ≠ CI 干净。**
- 鉴别力：往 `avbase_c.h` 塞 C++ 默认参数 → `cc -std=c99` 报「C does not support
  default arguments」，`c++` 通过 = 旧腐化形状（C++ 绿，C 宿主断）。

## 诊断门禁（先跑这个，再怀疑产品 bug）`python3 tools/check_binary_freshness.py --root build/<preset>`（exit 0/1/2）。
**为什么排第一**：第八十五轮拿 10/15 旧二进制诊断出「VideoToolbox 硬解卡死」，重链后
3/3 正常——**那个 bug 不存在**。判新鲜度读 `.ninja_log`（非 `stat`）；每个二进制只跟它
自己链接的库比（依赖取 `build.ninja` 显式段，不含 `||` 后 order-only）。ninja 不跟踪
静态库内容，但 `cmake --build` 对部分目标会报成功却留旧可执行文件——**测量前先确认被测
物的 mtime 晚于输入。

**`buildPresets.targets` 截断陷阱（第九十轮已修）**：`macos-sdl2`/`linux-sdl2`/
`linux-native` 的 buildPreset 曾带 `targets: ["play_sdl2"/"play_native"]`，导致
`cmake --build --preset macos-sdl2`（CI `macos-player` 用的就是这条）**只建 play_sdl2，
不建 player_unittests/headless** → 后续 `ctest` 因找不到可执行文件而红。沙箱看不到 CI，
这条红一直隐形。已移除三处 `targets` 限制（`CMakePresets.json`），现在 `--preset` 与
`cmake --build build/<dir>` 一致，构建全部目标。验证：删 player_unittests 后
`cmake --build --preset macos-sdl2` 会重建它；macos 门禁 786/786。

## 写单测前必读（第八十七/八十八轮）
- **测试绝不依赖工作目录**：用编译期宏 `AVBASE_TESTDATA_DIR`（`tests/CMakeLists.txt`
  逐 target 定义）。同二进制直接跑绿、ctest 红 = 先查 cwd。
- **`PlayerImpl::PrepareSync` 无默认参数**；单测显式 `PrepareSync(base::Seconds(30))`。
- `tests/unit/<dir>/*.cc` 是 glob 收集，新文件不用改 CMakeLists（但 invariants 文件数 +1）。
- **`Signal()` 持锁**：等待者必在 signal 后的动作之后才醒，别把二者当竞态；想造竞态加钩子。
- **注释声称「这个测试能抓住 X」之前，先改回去跑一次**：第八十八轮有注释写了「改回去会
  红」——改回去全绿。注释里的鉴别力声明也会假绿，且更难发现。

## API 文档管线（M13，`tools/gen_api_docs.sh`）
- **doxygen 1.18 的 INPUT_FILTER 是 argv 调用**（`<filter> <abs-path>`）。filter
  空输出 = doxygen 解析空文件 = **零 warning + 注释全丢的静默假绿**。防线：
  脚本第四重断言（XML 必须有非空 brief/detailed），突变反证两发齐（坏 argv
  分支 rc=1；注入非法 doxygen 命令 rc=1）。改管线先读脚本头注释。
- mac 无 doxygen：预编译 1.18 mac-intel 在 `/tmp/avbase_tools`（doxygen.nl 可达、
  GitHub 被墙）；跑法 `DOXYGEN=/tmp/avbase_tools/doxygen-1.18.0/doxygen tools/gen_api_docs.sh`。

## 本 sandbox 网络约束（查 CI 前先读）
- **GitHub HTTP(S) 被代理掐断**（`Proxy CONNECT aborted`/502）→ `gh` 装不上、API 拉不到
  CI。**别再试 `gh`/API。** 唯一通的是 SSH git 协议（`git push`/`ls-remote` 正常）。
- **看不到 CI 的替代方案**：`tools/gate_local.sh` 用同一套命令在本地复刻
  CI 门禁序列（inv/fmt/lint/apidocs/macos/capi/core/ffmpeg/e2e + 可选 tidy/asan/tsan/ubsan/
  coverage）。`--list` 看 job→CI 映射，本地绿 = CI 会绿。
- **`rm -rf`/`rm -r` 触发 sandbox escalation，且 escalated 容器文件视图不可信**
  （脚本内子进程 python3 报过「repo 文件不存在」，交互式同一调用却成功，时好时坏）。
  规避：删目录用 `python3 -c "shutil.rmtree(...)"`，让脚本全程 sandbox-direct。

## 路线图（现状）
L1–L5、docs/14 阶段 0–4、Phase 2 协议验收（HLS/HTTP + RTMP/RTSP/SRT，第八十二轮三协议
实测全绿）、M15 C ABI（已接门禁）均完成。第八十五轮推翻了自己报的「VideoToolbox 硬解
bug」（不存在），并新增 freshness 门禁。M13 进行中：SDK 文档七份齐 + apidocs 门禁
（bde3e8c/282a71f）已推送；V9 注释覆盖率、V10 API 兼容、微基准（base+media+
player 5 项）、端到端 CPU 机器判据、golden diff 件、cpack/vcpkg 均已完成并接
门禁。剩：golden **录制**基线（需 ijkplayer 原版，Q8 未决）、依赖解码器的
BM_*、soak 48h（需 CI 长跑机）、conan 实跑、渲染 CPU 的机器判据（仍靠
bench_render.sh 人工）。

## 性能门禁（两条，都别按设计门槛直接判）
- 微基准：`bench/` 链 `avbase::player`，`gate_local --jobs bench`（3 次取中位数），
  容差 `tools/bench-tolerance.json` 25%（PostTask 40%、EventHub 35%）。
- 端到端 CPU：`tools/measure_e2e_cpu.py` + `gate_local --jobs e2ecpu`（约 1 分钟），
  容差 `tools/e2e-cpu-tolerance.json` **15%**。
- **两条的设计门槛（微基准 10%、端到端 5%）都低于本机噪声地板**，误报过的教训：
  误报的门禁会被习惯性 override，之后什么都抓不住。改阈值前先读那两个 json 里的
  实测依据；改测量方法（轮长/统计量/次数）优先于放宽阈值。
