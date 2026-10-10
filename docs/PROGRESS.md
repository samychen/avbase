# 实施进度（活文档，每个里程碑结束时更新）

> 设计文档：[README](../README.md) ｜ 里程碑定义：[08 实施路线图](08-实施路线图与风险.md)

## 当前状态：**M0–M11 ✅ · Phase 0/1/3 ✅ · Phase 2 追帧/水位/重试/桥 ✅ · Phase 4 音·视频·字幕轨切换/fuzz/corpus 门禁/弱网与协议回归/覆盖率与 format 门禁 ✅ · macOS 窗口播放器 ✅ · 编码器层 E1/E2 ✅ · 转码 E3/E4/E5 ✅（铺设完毕，见第二十八轮）· C1 门禁五项违规全清（见第二十九轮）· 音频编码器通用化（见第三十轮）· 转码异步入口 E3b（见第三十一轮）· 全树 format 扫尾（见第三十二轮）· cpplint 修树轮 + 门禁（见第三十三轮）· corpus 加宽到 315 字条、揪出三个真崩溃并完成第二次 C1 拆分（见第三十四轮）· 4.6 覆盖率链路本机闭环复现（见第三十五轮）· docs/07 §5 计数三例两例转绿、顺线修掉一个真死锁（见第三十六轮）· docs/07 §5 计数三例**全部转绿**、顺线修掉解码器丢包（见第三十七轮）**

最后更新：2026-10-09（第三十四轮）—— **corpus 加宽到 315 字条，而它当场揪出三个真崩溃**：
先给矩阵补上一个**组合维度**（tier 2，`full` 独有：12 recipe × 4 scale × 5 fps = 240 条编码，
覆盖 HEVC/VP9/mpeg4/mjpeg × MP4/MKV/TS/FLV/AVI/WebM），`full` 从 74 条推到 **315 条**
（本机 15m50s，属周常不属 PR，故 `corpus-full.yml` 另配 `corpus_baseline_full.json`）。
加宽**立刻**把一类此前从未在 full 档跑过的样本变成红：`codec_mpeg2.mpg` 撞
`Check failed: params.is_valid()`。顺这条线挖出**三个真崩溃**，都是同一句话的三种进入方式
——**玩家把"这个流播不了"变成了进程 abort**：① 容器**没声明采样格式**（裸 `.ac3`、
Ogg/Vorbis、MPEG-PS 里的 mp2）→ libavformat 给 `AV_SAMPLE_FMT_NONE` → `SampleFormat::kUnknown`
→ `bytes_per_frame()==0`；② **声道数没有具名布局**（3/4/5/7 声道）→ 解复用器只能标
`ChannelLayout::kDiscrete` → `ChannelLayoutToChannelCount()` 答 **0**；③ **容器压根没量出这条流**
（libavformat 当场警告 "Could not find codec parameters"）→ 0 声道、0 采样率。①②修在
`AudioParameters`：改为**显式携带声道数**，且 `is_valid()` **不再要求采样格式已知**
（`AudioDecoderConfig::IsValidConfig()` 本来就不要求，播放路径也不读它）；③修在解复用器：
`HasUsableParameters()` 为假的音频流**按"播不了"丢弃**（与既有的 data/attachment 丢弃同一条
理由），但**不**丢弃"本套 build 没有解码器"的流——那是机器的性质，不是文件的性质，corpus
把它记为 SKIPPED 而不是失败。**本机复核**：full **315 样本 · 打开 308/308(100%) · 首帧 1.026 s ·
seek 302/308(98.1%) · 崩溃 0**，新增 9 个 value-type 用例，三预设全绿。修崩溃的产物又撞了
仓库自己的 C1 门禁（`ffmpeg_demuxer.cc` 1154 行 > 1140），于是**按既有接缝再拆一次**：
三个"吃 `AVStream`、吐 decoder config、从不碰 `AVFormatContext`"的纯函数搬进新 TU
`ffmpeg_demuxer_configs.cc`，**1154 → 1022**，白名单上限随之下调到 1030（棘轮只许往下）。
> 第四十三轮：**审查 High H1–H7 全部落地修复**，每条配承重测试并经负向测试验证。
> H1——`legacy/clock.cc` 的 `Get()` 把 `speed==0` 当 1.0（暂停时钟照跑，与
> `renderer_impl_controls.cc`"speed 0 保持锚点"的契约相反），改直接用存储速度；
> `SetSpeed()` 重锚改用**旧速率外推的当前读数**（旧代码固定按 1 重放已流逝区间：
> 从 2x 降速时间回跳、从 0.5x 升速时间前跳、暂停后一动就拖到"现在"）。新增
> `clock_unittest.cc` 6 用例（冻结/恢复/变速连续两个方向/1x 基线/invalid）。
> H2——音频致命解码错误只 `LOG(ERROR)` 说"reporting to the pipeline"却不上报，
> 泵停摆、UI 无限缓冲：`AudioRendererImpl` 增 `set_error_cb`，
> `RendererImpl` 初始与轨切换两处接线到 `ReportError`，+1 用例。H3——在飞 seek 期间
> 的新目标被静默丢弃（`pending_seek_superseded_` 只写不读）：改为**排队**，最新意图
> 胜出、被顶替者的完成回调照常触发，在飞 seek 完成后启动排队者（其落点前不再空启
> 一轮渲染）；`MidFlightSeekIsNotDropped` 断言落点锁定第二目标 ±2 帧——第一版断言
> 被负向测试揭穿为假绿（旧代码下自由播放也会爬到第二目标），改为以"seek 批次后第一
> 帧"锚定。H4——`bind.h` 把按值绑定的实参当右值转发，Repeating 回调第二次调用拿到
> 被移空的值（实测复现 exit=1）：`BindState` 增 `kIsOnce` 策略参数，Repeating 的存储
> 值按左值传递（Once 保持可移）；+2 用例（二次 Run 完整性、move 计数为零）。
> H5——`expected` 的 union 构造函数隐式构造 `T` 又被外层 placement-new 覆盖，每个
> 默认构造漏一个 `T` 且强制 T 可默认构造：union 空构造，active member 由外层独管；
> `expected<NonDefaultConstructible, E>` 可编译 + 构造析构平衡计数（旧代码下编译失败
> 与计数失衡双重承重）。H6——`value()`/`error()` 三对 ref 限定重载各加 `CHECK`，
> 文档化的"Terminates on error"成为事实。H7——转码编码器 `time_base = {1, fps_den}`
> （30fps → {1,1}，一刻度一秒）而 pts 送微秒，重编码时间轴被放大 ~10⁶ 倍；改
> `{fps_den, fps_num}` + `av_rescale_q`；`AvFrameToVideoFrame` 硬编码 /90000 改按
> 调用方 `pkt_timebase` 换算（签名 +1 参数）。+4 用例（time_base 值、30fps 连续
> pts、29.97 fps 网格、pkt_timebase 换算），旧代码下 3 个必红。
> 三预设 452 / 565 / 565 全绿，`check_invariants` all rules pass，clang-format 无 diff。
> 第四十二轮：**审查 Critical C1–C4 全部落地修复**。C1——`pipeline_track_select.cc` 那处
> 跨序列 hop 的 `base::Unretained(this)` 换成 `weak_factory_.GetWeakPtr()`（`pipeline_impl.h`
> 成文要求早已写明，兄弟 hop 全是弱引用，就漏了这一处）。C2——`Subscription` 补上析构/
> 移动赋值里的 `Reset()`，退订契约从"靠记得"变成"靠类型"。C3——`EventHub` 观察者表改
> `shared_ptr<ObserverEntry>` + `alive` 标志 + 单调 id，`RemoveObserver` 在另一线程**等待
> 在途 Dispatch 排空**（Dispatch 线程自身跳过等待防死锁）；配 4 个新用例，负向测试还揪出
> 自己用例的假绿——两个观察者共享一个静态计数器，断言恒真，改成每实例计数。C4——
> `pipeline_`/`renderer_factory_` 改 `std::shared_ptr` + `pipeline_lock_` +
> `GetPipeline()/GetRendererFactory()` 快照访问器，任意线程 `Reset()` 与查询并发不再 UAF
> （~25 处使用点）。顺带修掉一个**被新测试逼出的既有潜伏链接缺陷**：`player_impl_events.cc`
> 无条件引用 FFmpeg 专属的 `WriteJpegSnapshot`，此前没有任何测试链接 `PlayerImpl` 才一直
> 没暴露；条件编译后 no-ffmpeg 下 `TakeSnapshot` 按项目惯例返回 `kNotImplemented` 并指路
> 构建开关。**三预设 445 / 558 / 558 全绿**（asan 含全部新用例），`check_invariants` all
> rules pass（365 文件，C23 基线 37 不变），clang-format 无 diff。
> 第四十一轮：**全仓只读代码审查**（报告：[docs/reviews/2026-10-10-code-review.md](reviews/2026-10-10-code-review.md)）。
> 四条 Critical（C1 弱化漏一处、C2/C3/C4 契约写了没实现）与七条 High（bind.h 移空实测复现、
> 暂停时钟不冻结、expected union 双重构造、转码 time_base 单位不一致等）全部记录在案；
> Critical 于第四十二轮修复，High 待后续轮次消化。
> 第四十轮：**同一把尺子量到测试侧——`tests/unit/media_filters/` 按目标拆成三个目录，全仓再
> 没有一行显式源文件列举**。判据不是"测试目录该与源目录同名"，而是**跟目标**：那 37 个 `.cc`
> 里 16 个进 `media_unittests`、21 个进 `media_ffmpeg_unittests`，一个目录混两个二进制，所以它
> 是全仓最后一个必须手写清单的目录（`docs/06 §7.6` 表里那行"显式"的注释就是它）。按目标拆为
> `media_filters/`（核心，16）、`media_ffmpeg/`（FFmpeg 适配层，15）、`media_transcode/`
> （离线转码，6）——最后者与 `avbase_transcode` 同形，配独立目标 `media_transcode_unittests`，
> 于是转码套件不再把编码器与封装写端拖进 FFmpeg 解码测试的链接集。三者全部恢复 glob，
> `tests/CMakeLists.txt` 里那段"`unit/media_filters/` 是例外"的注释随之删除。**判据是目标而非
> 被测主题**：`pipeline_throttle/track/text` 读 `tests/testdata/` 下的真实容器，因此即便测的是
> 核心 media，也必须待在 FFmpeg 二进制里——按主题分组会把它们放进错误的二进制。连带修正
> `media_ffmpeg_unittests` 的 DEPS：它此前靠 `avbase::transcode` 间接取得 `avbase::ffmpeg`，
> 转码套件分出去之后必须自己声明。**同轮修掉一个既有的时序 flake**：
> `UrlDataSourceTest.AsyncReadPostsResultNeverRunsInline` 的 `EXPECT_FALSE(done.IsSignaled())`
> 看着是在测"没有内联执行"，其实是在测**调度**——`-j8` 负载下读线程完成一次 512 字节 file://
> 读并回投，比测试线程走到那一行更快，于是它红得毫无道理。生产实现是对的（worker 干阻塞活、
> 结果 `PostTask` 回投），错的是断言。改为断言**回调所在线程**
> （`== reader_thread_.GetThreadId()`，确定性），并把"没有内联"这半边搬进一个用
> `base::TaskQueue` 的用例——那个 runner **不泵就不跑**，"Read() 返回时回调仍未执行"于是成为
> 事实而非竞态。负向测试：把 `UrlDataSource::Read` 临时改成内联执行，两条用例**都红**，且都是
> 确定性地红。三预设 **554 / 441 / 554** 全绿（+1 即新用例），`check_invariants` all rules
> pass（364 文件，C23 基线 37 不变）。
> 第三十九轮：**离线转码产品线迁出为 `media/transcode/` + 独立 target `avbase_transcode`**
> ——判据不是"目录好看"，而是那十个 `.cc` 在 `media/ffmpeg/` 之外**一个生产消费者都没有**
> （13 条外部 `include` 全来自测试；唯一像调用方的 `player` 截图走的是
> `media/ffmpeg/ffmpeg_image_snapshot.*`，那是"抓一帧"、不属于这一类）。18 个文件 `git mv`
> + 33 条 include 改写 + 9 个头文件保护宏改名。**这一步翻出两件早就该发现的事**：① **C5 需要
> 豁免**——`video_encoder_factory.{h,cc}` 用 `#if defined(__APPLE__) / __linux__` 硬编
> VideoToolbox/VAAPI/NVENC，而解码侧的兄弟早就把 spec 挪到 `platform/hwaccel/*_hw_spec.h`、
> 由 `player/video_decoder_defaults.cc` 组装（player 才是注入点）；豁免给的是**文件级**
> （`ENCODER_SPEC_FILES`）而非目录级，C5 仍守着 `media/transcode/` 其余 16 个文件，并做了
> 负向测试确认去掉豁免必响（响 6 处，恰好全在这两个文件）。② **链接重复**——`avbase::transcode`
> PUBLIC 携带 `avbase::ffmpeg`，测试 DEPS 里再显式写一遍就让 ld 报 "ignoring duplicate
> libraries"；去掉显式那条即止（正是 `avbase_add_unittest` 头部记的那条规矩）。**为什么值得
> 一个新 target**：留在 `avbase_ffmpeg` 里等于每个播放构建都背着编码器与封装写端；独立后它
> **刻意不进 `avbase::avbase` 聚合**，转码调用方显式链接 `avbase::transcode`。三预设
> **553 / 441 / 553** 全绿，`check_invariants` all rules pass（364 文件，C23 基线 37 不变）。
> 第三十八轮：**FFmpeg 从 `platform/` 迁出，全部归入 `media/ffmpeg/`**（用户的判断：FFmpeg 是
> 通用全局框架、不是平台后端）——64 个文件搬家 + 目标改名 `avbase_ffmpeg` + 命名空间统一为
> `avbase::media::ffmpeg`；**`media/filters/` 由此恢复可 glob**（不再是全树唯一要手写清单的
> 目录），`platform/CMakeLists.txt` 里 20 行 `../media/filters/` 跨树路径消失。**搬家当场炸出
> 一个隐藏的重复符号**：`ffmpeg_glue.cc` 的 `GetFFmpegVersionString()` 只是转发给 `log_bridge.cc`
> 的实现，两者原先分处 `avbase::media::ffmpeg` 与 `avbase::platform::ffmpeg` **两个命名空间**
> 所以不冲突，合并后立刻变成重复定义 + 自递归。另补三条门禁 **C16/C26/C27**（并做了负向测试
> 确认它们会响），把新边界用机器锁住；**C4 从"文件名含 ffmpeg 就豁免"改成纯目录规则**，堵掉
> 旧漏洞。**C26 此前不可能加**：`media/filters/` 曾有 19 个文件直接 include `platform/...`
> 而 C5 查的是第三方平台头、不查自家 `platform/` 目录。**撤回原提议的一步**：原想把
> `null_{audio,video}_sink` 移到 `platform/null/`，读了 `null_video_sink.h` 头部注释后撤销——
> 那里有成文理由（headless 属框架非平台 + docs/10 规则 E1），stale 的是文档不是代码。
> 三预设 **553 / 441 / 553** 全绿
> 第三十三轮：cpplint 修树轮 + `check-cpplint` 阻塞门禁（59 违规分五类；48 处真修，
> `build/c++11` 因项目是 C++20 整类关闭并写进新增 `CPPLINT.cfg`）
> 第三十七轮：docs/07 §5 计数三例**全部转绿** —— 第三例（双速）改用 VOD 形态后暴露
> `DecoderStream` **丢弃"被 watermark 掐断的批次尾巴"**（取 8 帧解 1 帧），另修掉
> `ffmpeg_demuxer_unittest` 一处存量 ASan 释放后使用（干净树实测 3/30）
> 第三十六轮：docs/07 §5 计数三例两例转绿 —— 顺线揪出 `SyntheticDemuxer::VideoStream::Read`
> **解锁一个从未持有的 mutex**（paced 一开必死锁），并给夹具补上 S7 设备序列、摊开 pull 节奏、
> 把停止条件从"墙钟预算"改成"媒体目标"；第三例（双速）因**源侧**原因留 DISABLED
> 第三十五轮：4.6 覆盖率链路本机完整闭环复现（门禁/棘轮/基线早已在位）· 4.3b 根因升级为
> "阻塞 pull vs park 死锁" · 2.4 协议验收核实为 pinned FFmpeg 无 rtmp/rtsp/srt（环境项）
> 第二十五轮：macOS 播放器实测与固化（预设 + CI 守护）
> 第二十四轮：全量崩溃清扫（corpus 14/29 关闭竞态根因修复→0/35;TearDown 未 Stop 的
> DISABLED 掩盖崩溃;反向断言）· corpus 矩阵分级（smoke/standard/full,74 产出崩溃 0）·
> 覆盖率门禁接入（棘轮 + 基线入库）· 弱网工具与 CI（暴露 RetryDataSource 不在生产路径）·
> kHardwareOnly 生产接线（SelectVideoDecoder 原本无生产调用者）· SyntheticLiveDemuxer ·
> 视频轨切换闭合（真因=4s 素材交接前读完,非产品缺陷）· throttle 测试三修后转绿
> （40s/12.9KBps 素材 + 变速率节流源）
> 第二十三轮：LiveDataSource + 直播追帧判定核与端到端验收 · HLS/HTTP 协议验收入 CI ·
> format 门禁（C23 286→37）· Windows CI job · RendererImpl 6 处 S1 hop weak 化
> 第二十二轮：纯音/纯视频断言（docs/07 §5）· 合成源单流禁用
> 第十九至二十一轮：LiveDataSource · 追帧判定核 · 追帧端到端验收
> 第十八轮：fuzz 目标（libFuzzer + standalone 双驱动 + 语料入库）
剩余工作以 [12-剩余工作清单](12-剩余工作清单.md) 为准（该项由各轮同步维护,是唯一权威清单）。

## 第三十八轮（本轮）：FFmpeg 不是平台 —— 64 个文件搬家，三条门禁落地

### (1) 起因：一句反对推翻了我给的方向

上一轮评审 `media/` 与 `platform/` 的目录结构，我给出的第 3 步是"把 20 个
`media/filters/ffmpeg_*.cc` 搬进 `platform/ffmpeg/`"，理由是**目录边界应当等于目标边界**
（`media/filters/` 是全树唯一"目录成员 ≠ 目标成员"的目录，因此不能 glob，21 个核心 filter
得手写进 `media/CMakeLists.txt`；而 `platform/CMakeLists.txt` 里挂着 20 行
`../media/filters/ffmpeg_demuxer.cc` 这种跨树路径）。

用户的反对是对的，而且推翻的是方向不是细节：**「ffmpeg 应该是通用全局框架，没必要放在
platform 目录」**。FFmpeg 在 macOS / Windows / Linux 上行为一致，把它和 SDL2、D3D11/VAAPI/
VideoToolbox 并列在 `platform/` 下，是把它错归成"平台"。`docs/02 §1` 自己写着要一一对应
Chromium，而 Chromium 正是把胶水层放 `media/ffmpeg/`。

于是方向反过来：**不是把实现搬进 platform，而是把 `platform/ffmpeg/` 搬出 platform**。
经确认落点为「全部合并进 `media/ffmpeg/`」、目标改名 `avbase_ffmpeg`。

### (2) 做了什么

- `platform/ffmpeg/*`（27，胶水层）+ `media/filters/ffmpeg_*`（37，实现层）→ **`media/ffmpeg/`**
  共 64 个文件。`platform/` 只剩 `sdl2/` 与 `hwaccel/`。
- 命名空间统一为 `avbase::media::ffmpeg`（原先胶水层是 `avbase::platform::ffmpeg`）。有意思的
  旁证：`media/filters/ffmpeg_glue.h` 早在搬家前用的就是 `avbase::media::ffmpeg`——**同一个
  胶水层本来就被拆成了两个命名空间**。
- `media/CMakeLists.txt`：`filters/` 改回 glob；新增 `avbase_ffmpeg` target（glob `ffmpeg/*.cc`）。
- `platform/CMakeLists.txt`：整块删除，注释写明搬去了哪里。
- 三个单文件目录 `platform/{d3d11,vaapi,videotoolbox}/` 合并为 **`platform/hwaccel/`**
  （每个只是一个 30 行的 inline spec 函数，三个目录读起来像三个不存在的后端模块）。

### (3) 搬家炸出来的隐藏重复符号

链接期报 `duplicate symbol avbase::media::ffmpeg::GetFFmpegVersionString()`，来自
`ffmpeg_glue.cc` 与 `log_bridge.cc` 两个目标文件。查下去发现：`ffmpeg_glue.cc` 里那份是个
**纯转发包装器**（`return media::ffmpeg::GetFFmpegVersionString();`，原为
`platform::ffmpeg::...`），真身在 `log_bridge.cc`。两者原先分处两个命名空间，**所以一直不冲突**——
是命名空间分裂掩盖了重复。合并后它同时变成重复定义和**自递归**。且全树**没有任何调用方**
（只有两处声明两处定义），合并成一份放在被导出的 `ffmpeg_glue.h` 那侧即可。

这类东西正是"目录搬家值得做"的理由：**搬家把一套隐式约定变成了编译器能看见的事实**。

### (4) 三条门禁，都做了负向测试

| 门禁 | 守什么 | 为什么现在才能加 |
|---|---|---|
| **C16** | `media/base/` 不得 include `media/filters/` | 接口层不许依赖实现层。原本在"只留能抓 bug 的"那轮被砍，如今边界真了就复辟。当前零违反 |
| **C26** | `media/` 不得 include `platform/` | **此前不可能加**：`media/filters/` 曾有 19 个文件直接 include `platform/...` 而无门禁拦得住——C5 查的是 `SDL2/` `X11/` `GL/` 这类**第三方平台头**，不查自家 `platform/` 目录；C4 又按"文件名含 ffmpeg"豁免。FFmpeg 迁出后 `media/` 对 `platform/` 的依赖归零 |
| **C27** | 核心 media 不得 include `media/ffmpeg/` | 隔离区是**目录**边界。C4 只挡 libav 头，挡不住"核心文件 include `media/ffmpeg/helper.h` 从而间接拖进整个 vendor 层" |

**C4 同时收紧**：从"文件名含 ffmpeg 就放行"改成纯目录规则 `media/ffmpeg/`。旧规则意味着
任何叫 `ffmpeg_*` 的文件都能带 libav 头——是个洞。收紧后零违反。

关键教训：**一条不会失败的门禁等于没有门禁**。三条写好第一次跑负向测试**全部没响**——
因为我把它们放在了字符串字面量剥离之后（`#include "..."` 的路径已被替换成 `""`），而
`C22` 之所以一直是好的，是因为它在剥离**之前**用 `raw_line` 单独处理。改用 `raw_line`
后三条都能正确触发。

### (5) 撤回了一步：null sink 不动

原提议把 `null_{audio,video}_sink` 从 `media/filters/` 移到 `platform/null/`（对齐 docs/02 §6）。
读了 `media/filters/null_video_sink.h:24-31` 后撤销——那里有**成文的、有理由的决定**：
headless 路径属框架而非平台（引 Chromium `media/video/null_video_sink.h` 先例），且"零配置
可播"（docs/10 规则 E1）要求无窗口系统的机器也能端到端跑。**stale 的是文档，不是代码**，
所以改的是 docs/02 §6。

### (6) 同步的文档

`docs/02 §2.1`（门禁表，写清 C5 与 C26 的分工）、`§4.2/4.3/4.4`（新增 `media/ffmpeg/` 一节；
标出 `media/audio/` 尚未建立及其职责实际落在哪）、`§6`（平台目录表按实际重写，加
`platform/hwaccel/`，写明 `platform/null/` 为何不建、规则 1 为何只查 media 不查 player）、
`docs/06 §7.6`（`media/filters/` 由"显式"改回 glob，删掉"跨目录取的那**五个** `ffmpeg_*.cc`"
——实际早已是 20 个，这个数字漂移本身就是转码产品线被塞进 filters 的证据）、
`README.md` D2/D11、`LICENSE` 第 3 节。历史台账（PROGRESS/archive）按惯例保留原路径未改。

### (7) 验证

`ctest build/ffmpeg` **553/553** · `build/no-ffmpeg` **441/441** · `build/asan` **553/553** ·
`check_invariants` all rules pass（364 文件，C23 基线 37 不变）· clang-format 95 个文件 clean。

## 第三十七轮：4.3b 收尾 —— 计数三例全部转绿，代价是揪出解码器丢包

### (1) 上一轮的结论对了一半

上一轮把第三例（双速）留在 DISABLED，理由是"**源侧**"：paced 源固定 1x，喂不动 2x 消费者，
所以需要一个 **VOD 源形态**。这个判断的前半段是对的——1x 源确实喂不动 2x 消费者——后半段错在
**以为要新写一个**。VOD 形态本来就存在：`paced = false`。直播与 VOD 的区别恰恰是"谁决定什么
到期"：前者由**源**（墙钟边缘）决定，后者由**消费者**（时钟）决定；在这个 double 里两者就是
一个 bool。

于是先做实验，不先设计：把 `spec_.paced = false;` 放在 `BuildPipeline()` 之前。结果非常干净——
墙钟 **2532 ms**（对，2.5 s）、媒体 **5 s**（对）、**帧数 35 / 150**（错）。也就是说停止条件和
时间都对了，只有帧数少。**此时最贵的一步是想清楚"是不是真的在丢帧"**，而不是去调容差。

### (2) 真缺陷：被 watermark 掐断的批次，尾巴被丢掉了

打开合成器的丢帧日志（`SetMinVLogLevel(2)` 让 `DVLOG(2)` 生效）→ **零条丢帧**。既然没有丢帧，
那帧就是根本没进合成器的队列。给 `PutCurrentFrame()` 加一行日志，帧时间戳的跳跃立刻现形：

```
... 533333, 566666, 600000, 800000, 1066666, 1333333 ...
```

视频帧是 33333 µs 一帧，而 600000 之后**每隔 7 帧才来一帧**（跳到 800000、1066666…）。
帧是在**进合成器之前**少的，而解复用器与合成器之间只有 `DecoderStream`。看
`DecodeNextBuffer()`：

```cpp
while (pending_buffer_index_ < pending_buffers_.size() &&
       decoded_outputs_.size() < kDecodeWatermark) { ... }
// Exhausted the batch: release it and ask for more.
pending_buffers_.clear();
```

循环有**两个**退出条件，注释却只认一个。watermark 先到时，`pending_buffer_index_` 仍小于
`size()`，而 `clear()` 照样执行——**把已经解复用出来、还没解码的尾巴扔了**，而解复用器的游标
已经越过它们，所以是**永久丢失**。加一个计数器立即定案：**93 个批次，89 个退出时剩 7 个**，
即"取 8 帧、解 1 帧"。

修法只有一行判据：批次没消费完就 `return`，尾巴留给下一次 `DecodeNextBuffer()`。

**它为什么能活到现在**：`kBuffersPerRead = 8` 一直都在，但**只有解复用器真的一次交多个 buffer
时才可达**。paced 合成源按构造一次只给一个（可用量随墙钟逐帧增长），而
`FFmpegDemuxerStream::Read` 是 `PopUpTo(count)`——**真实文件一直是批量的**。所以这不是"为了测试
才要修"：真实播放里只要解码器跑到渲染器前面，就会丢掉解复用器刚交出来的**压缩包**——对
P/B 帧而言，丢包比丢帧更糟。§5 的计数用例只是第一个把它量出来的地方。

### (3) 断言：墙钟换成设备计数

第三例原本有 `EXPECT_NEAR(wall_ms, 2500, 1200)`。它在 ffmpeg 预设下过，在 ASan 下**必挂**：
同一个 run，ASan 把墙钟拉到 4624 ms，而**两个计数断言全对**。原因很清楚——这个夹具的"显示器"
是测试线程自己在拉，一轮的墙钟由**泵自己的开销**决定，不由管线的速率决定。**断言墙钟等于在断言
构建类型。**

速率改到能精确测量的地方断言：**设备侧计数**。2x 时一个 device period 覆盖两倍源帧，所以"5 秒
媒体"离开设备时只有 2.5 秒音频 = **120000 输出帧**；1x 的同一段会发 240000。容差 5%，理由写在
断言旁边：循环的停止条件是**音频时钟**，而它在两次设备回报之间是**外推**的，可能比设备真正拿到
的东西早几十毫秒（ASan 并行负载下实测差 1184 帧 / 1%）；而要区分的另一个答案是 240000，差着
一倍。

### (4) 顺带：解复用器桥测试的存量 ASan 释放后使用

本轮 asan 预设一度变红，挂的是 `FFmpegDemuxerTest.Open*`。抓栈是标准的
`MemoryDataSource::ReadBlocking` **heap-use-after-free**：两个桥测试把文件读进**局部** `vector`，
把 `data()` 交给解复用器——而 `MemoryDataSource` **不复制**这些字节，解复用线程会一直读到它自己
结束。测例体一结束局部 vector 就析构，**和解复用线程的读撞了个正着**。

先判归属再动手：把本轮改动 `git stash` 掉，在干净树上重复 30 次 → **3 次失败、同一条栈**，所以是
**存量竞态**，不是本轮引入的。修法是把字节收进夹具成员，并**声明在 `demuxer_` 之前**（成员按声明
逆序析构，因此它比解复用器活得久）。修后 40 次 0 失败。

### (5) 验证

| 面 | 结果 |
|---|---|
| `ctest build/ffmpeg` | **553 / 553**（较上轮 +1：双速启用） |
| `ctest build/no-ffmpeg` | **441 / 441**（+2：双速 + 解码流回归用例） |
| `ctest build/asan` | **553 / 553**（并行负载下复跑两次均绿） |
| `check_invariants` | all rules pass（364 文件，C23 基线 37 不变） |
| `clang-format` | 四个改动文件全 clean |

两个回归测试：`DecoderStreamTest.EveryBufferSurvivesAWatermarkTruncatedBatch`（**无修复时只有
24/80 输出，有修复时 80/80**——先验证它会失败，才敢说它守住了什么）与
`PipelineCountsTest.DoubleSpeedPresentsTheSameFramesInHalfTheTime`。

### (6) 教训

- **"症状像什么"不是"病因是什么"**。35/150 帧长得像"2x 丢帧"（速率处理错了），实际是解复用器
  交给解码器的东西被丢了。分辨两者**只需要一行日志**（进合成器的帧时间戳），而我先花了几轮去猜
  启发式（`ComputeTargetDelay` 的 diff 该用 `last_timestamp` 还是 `next_timestamp`）——猜错了，
  那几轮没进版本库。
- **一处自相矛盾的注释就是一处未判定的设计**。`clear()` 上面那句 "Exhausted the batch" 与循环的
  两个退出条件是矛盾的；上一轮 `VideoStream::Read` 的缺锁也是同一个模式（注释说"无锁因为单序列"
  而另一处注释说"并发读"）。**两轮连续撞到同一模式**，值得单独记。
- **一个计数器就能定案**。`BATCH-DISCARD undecoded=N` 一行，把"可能"变成"93 批 / 89 批剩 7"。
  与上轮的 `sample <pid>` 抓栈一样：最便宜的测量往往就是决定性的那个。
- **门禁红了先判归属再修**。`git stash` + 30 次重复把"本轮引入"和"存量"分开，既不会修错地方，
  也不会把存量的锅记在本轮账上。

---

## 第三十六轮：docs/07 §5 计数三例两例转绿 —— 顺线揪出一个真死锁

这一轮的起点是上一轮留下的结论："要改的是泵策略（给 `PullPeriod` 非阻塞路径，或让 paced 源
用 test-driven clock）。"方向没错，但**真正坏掉的东西不在泵里**——它在合成源里，而且是一个
自第十四轮就潜伏、只在 `paced` 打开时才可达的**未定义行为**。

### (1) 先修的那一件：`VideoStream::Read` 解锁一个从未持有的 mutex

`SyntheticDemuxer` 的两条腿长得不像一对：`AudioStream::Read` 开头就
`std::unique_lock<std::mutex> guard(owner_->lock_)`，而 `VideoStream::Read` **整个函数体
没有任何入口加锁**，却在 paced park 分支里直接 `owner_->lock_.unlock(); ...; owner_->lock_.lock();`。

这不是"少写一行"，而是一处**与文件自己的注释相矛盾**的写法。`lock_` 的声明处写着两条流会被
**并发**读；`decoder_stream.cc` 的 `ReadFromDemuxer` 也写得很清楚：`DemuxerStream::Read()` 是在
**renderer 自己的序列**上直接调用的（video = S3，audio = S4），只有**回调**才 `PostTask` 回 S3/S4。
两条腿因此**真的并发**进 `Read()`，共享 `owner_->lock_`，也共享 `packets_read_` 这个游标。
`SyntheticDemuxer::VideoStream::Read` 那条"只在 media 序列上调用、所以无锁"的旧注释，是错的。

后果不是"偶发"而是"确定性"：Darwin 的 normal mutex 对**非持有者 unlock** 不会干净失败——它把
mutex 的内部状态改成"有人持锁"，而那个人不存在。于是**音频腿**（它 park 之后会 relock 同一个
mutex）在 `guard.lock()` 上**永久阻塞**。unpaced 时 park 分支不可达，这个 bare unlock 一次都走不到；
`paced` 一开必挂。**修法**：video 腿也持锁整个 read，与 audio 腿一致；并把 `synthetic_demuxer.h`
里那条过时注释改对。

### (2) 三处泵改造（`pipeline_fixture.h` `PumpRoundRealtime` 重写为 `PullOneRealtimeInterval`）

死锁修掉之后**数字还是不对**，而且每一层错都有自己的可观测症状。四层，逐层剥：

1. **设备序列独立（S7）**。render 原本被 marshal 到 **S4**——而 S4 是**解码泵**序列，paced 源正在
   那里 park 等墙钟。排在 park 后面的 render 于是被堵住，整个套件跑成**四分之一速**（每轮实际
   64 ms，而不是 16 ms）。新增 `base::Thread render_thread_{"avbase-pipe-S7"}`——S7 正是
   `audio_renderer_ring.cc` 给**设备回调线程**的名字，此前只在注释里存在，这一轮把它落地。
2. **pull 摊开**。3 个 device period 背靠背拉，会在 1 ms 内抽干 audio ready ring（深度只有
   `kReadyChunks` = 4 个 chunk），而 1x 源**补不上** 16 ms 媒体——于是每个突发都从**空 ring**
   起步，大约**每七个 period 有一个返回静音**。改成一整轮里均匀摊开；显示帧
   （`PullFrames(1)`）并进**最后一段**，让它的开销被那段的余量吸收，而不是加在整轮 deadline 之后。
3. **`PaceUntil` 改纯自旋**。本机 `sleep_until` 实测过冲：请求 15.688 ms，实际睡到
   15.783–19.394 ms（最高 +3.8 ms）。一轮里有三个子 deadline，过冲会吃掉**整轮**——而这几条断言
   数的正是轮。代价是 paced 跑起来占满一个核，换来一个计数可以信的时间轴。
4. **停止条件从"墙钟预算"改成"媒体目标"**。这是决定性的一步：原先 `PlayFor` 按墙钟跑，等于把泵
   自身的速度**折进期望值**——泵慢 2% 就少覆盖 2% 媒体，计数短了，理由却与管线无关。改成
   `while (pipeline_->GetMediaTime() < media)`：某轮静音就不推进媒体，循环**自动多跑一轮**。
   **泵仍然按墙钟自我 pacing**（源和显示必须同步），被挪走的只是**停止条件**。

`PumpRoundRealtime` 另顺手改了一处：`start` 提前到 `RunUntilIdle()` **之前**，让 S1 排空算进这一轮
的媒体预算，而不是额外叠加在预算之上（十秒跑下来那就是白白扔掉的一个轮）。
`pipeline_throttle_unittest.cc` 里显式的 `set_render_runner` 一并删掉：夹具每轮都会 arm render
runner，那一行已是 no-op。

### (3) 结果

- **视频、音频两例转绿并恢复启用**（`TenSecondsOfVideoPresentsThreeHundredFrames` 631 轮、
  `TenSecondsOfAudioEmitsFourHundredAndEightyThousandSamples` 626 轮 → 媒体 **10.01 s**）。
- **第三例（双速）保持 DISABLED，理由在源侧**：本套件的源是 **1x** paced，而 2x 消费者需要一个
  **能喂动它的源**。实测：驱动到 5 s 媒体，2x 下仍然花掉 5 s 墙钟（因为卡在源的速度上），落点
  75 帧——恰好是**源的那一半**，而断言要 150。这需要 `SyntheticDemuxer` 补一个 **VOD 形态**
  （包按消费者请求速度交付，"什么到期"由时钟而非源决定），属 **rate 模型**改动，不是泵的改动。
  按项目一贯原则：**宁可 DISABLED 也不假绿**，并且把原因写成源侧的事实而非"已知问题"。
- 两个转绿的用例，断言消息里都带上 `play_rounds_`（这次 PlayFor 花了几轮）——因为"因为管线原因
  短了"和"因为撞了墙钟护栏短了"在数字上**长得一模一样**，不报轮数就分不出来。

### (4) 验证

| 面 | 结果 |
|---|---|
| `ctest build/ffmpeg` | **551 通过 / 0 失败**（较上轮 +2） |
| `ctest build/no-ffmpeg` | **439 通过 / 0 失败** |
| `ctest build/asan` | **551 通过 / 0 失败** |
| `check_invariants` | all rules pass（364 文件，C23 基线 37 不变） |
| `clang-format` | 五个改动文件全 clean |

ASan 全套件跑出过两次失败、且两次失败的用例还不一样——**是负载偶发**：当时我在**并行构建
ffmpeg**。系统空闲后重跑 **551/551**，单跑与空闲连跑各 3/3。教训写进日志：**asan 全量跑时
不要并行构建**。

### (5) 遗留：双速用例需要的是 VOD 源，不是泵

这是这轮唯一没闭合的一格，且它的"没闭合"是有形状的：`SyntheticSpec::paced` 表达的是
"**直播**边缘按 1x 推进"，而双速要测的是"**消费者**按 2x 消费 VOD"。两者需要源模型不同：
前者由**源**决定 due，后者由**时钟**决定 due。补 VOD 形态是下一件该做的事。

### (6) 教训

- **"先跑再改"再记一笔**：上一轮的文件头注释写着"THE HANG IS FIXED"，而 pacing 一落地，挂起
  就换成了**死锁**——病因换了，注释没换。读注释不等于读事实。
- **`sample <pid>` 抓栈**定位死锁，比往代码里塞日志快，而且不污染版本库（三个等待者互锁时，
  栈上谁等谁一眼可见）。
- **因果链要一层层剥**：死锁（video 腿缺锁）→ 四分之一速（render 在 S4）→ 突发抽干 ring
  （≤1 ms 拉 3 个 period）→ pacing 精度（sleep 过冲）→ 停止条件（改媒体驱动）。**每一层都单独
  可观测、可验证**，所以不是一次"大重构"，而是五次各自站得住的修正。
- **一处自相矛盾的注释就是一处未判定的设计**：`lock_` 的注释说"并发读"，`VideoStream::Read`
  的注释说"无锁因为单序列"。两份注释各自都读过、都像对的；把它们放在一起，缺陷就自己现形了。

---

## 第三十五轮：剩余清单收口 —— 4.6 闭环复现 · 4.3b 根因升级 · 2.4 核实为环境项

### (1) 4.6 覆盖率门禁：清单中段那句"门禁未接"是过期文字

`tools/check_coverage.py` 按 docs/07 §12 逐模块判定。本机把整条链路完整复现了一遍：

- `cmake --preset coverage` + `cmake --build build/coverage -j8`（208 目标）全绿；
- `ctest build/coverage` **549 通过** → 产出 **167 个 `.gcda`**（193 个 `.gcno`）；
- `python3 tools/gcov_to_lcov.py --root build/coverage --source . -o cov.info`
  → 167 gcda → 275 源文件，核心整体行覆盖 **83.17%**；
- `python3 tools/check_coverage.py --info cov.info --root .`（**绝对阈值**）→ **FAIL**：
  核心整体行 **74.3%** < 85%，`media/base` **66.9%** < 95%。gap 与清单 4.6 行的历史记录吻合，
  **稳定、不是新问题**；
- `--baseline coverage_baseline.json --ratchet` → **全绿**（所有模块记为地板）。

**结论**：转换器 + 门禁脚本 + CI 接线 + 基线文件**全部就位且已在跑**（`ci.yml` 的 coverage job
用 `gcov_to_lcov.py --exclude third_party/` + `check_coverage.py --baseline ... --ratchet`）。
本机 core 74.3% vs 基线 75.47%，差异来自 `--exclude third_party/` 与构建环境，量级一致。
**4.6 无需再动手**——它是完成态，只是清单中段的文字没删干净导致误读；真正的待办是
"把覆盖率补到 spec"，那是**补测试**的活，不是门禁的活。

### (2) 4.3b：根因从"泵的线程归属"升级为"阻塞 pull vs park 死锁"

三个 DISABLED 用例的真实现状与旧摘要**不同**：文件头注释已经历两段历史（自建 S3/S4 挂起 →
已走 `StartPipeline` 修好挂起），而 `paced` 落地后数字仍不对（30 帧 vs 300）。去掉 DISABLED、
把 `PlayFor` 换成 `PumpRoundRealtime()`、并跑满 `spec_.duration`（而非硬编码 2500 ms）——方向
正确，但**当场暴露死锁**：renderer ready 之后卡死。用 `sample $PID` 抓 macOS 进程栈，精确定位
到**三方等待闭环**：

- 主线程 `FakeAudioSink::PullPeriod` → `done.Wait()` 同步阻塞，等 S4 完成一次 `Render`；
- S4 在 `DecoderStream::ReadFromDemuxer` → `SyntheticDemuxer::AudioStream::Read` 的 park 循环
  `guard.lock()` 上等墙钟越过边缘；
- 而墙钟只能由**主线程继续泵**推进，主线程却卡在 `done.Wait()`。

三个等待者互锁，零进展。这不是"改三个常量"能解决的，是**夹具泵策略**问题：`PullPeriod` 的
同步阻塞语义与 paced 源的 park 语义互斥。按"宁可 DISABLED 也不假绿"**恢复 DISABLED**（启用但
死锁比禁用但诚实更糟），把死锁机制写进文件头注释，并**保留** `PlayFor` 的正确实现供下一步用
（提交 `d7f6d7b`）。

### (3) 2.4：pinned FFmpeg 没有 rtmp/rtsp/srt

`tools/setup_ffmpeg.sh` 的协议表是 `file,http,tcp,httpproxy,crypto`——**没有** rtmp/rtsp/srt。
这三个需要 `AVBASE_FFMPEG_DEPS="librtmp srt"` 显式开启并链接第三方库（librtmp→openssl，
srt→mbedtls），当前最小集没带。且 `tools/build/` 只有 `.dylib`，没有 ffmpeg/ffprobe 命令行。
**结论**：剩余协议验收在本机**不能闭环**，需重配一套带协议栈的 FFmpeg + 真实 peer（或本地起的
rtmp/rtsp 服务器），属**环境**相关工作项。

### (4) 教训

**"修测试"之前先跑一次。** 文件头注释里的"THE HANG IS FIXED"是**上一阶段的结论**，不是当前
事实；pacing 落地后挂起换成了死锁，病因换了而旧注释没更新。另外，同一轮里三件事的**性质**要
分清：4.6 是"已经好了、只是文档说了反话"，4.3b 是"真坏、但坏在夹具"，2.4 是"本机测不了、
不是仓库的问题"——把这三者混成一句"未完成"，后面的每轮都会踩回来。

---

## 第三十四轮：corpus 加宽到 315 —— 它当场揪出三个真崩溃，修完又撞了 C1

### (1) 加宽：给矩阵补上"组合"维度，full 从 74 条到 315 条

矩阵此前每个块**只动一根轴**：codec、容器、分辨率、帧率场序、多轨、纯音频、损坏。
这证明每一维单独可用，**证明不了它们能组合**——而"只有 1080p60 才出现的解码问题"、
"只在 15fps 才错的 seek" 恰恰需要两根轴一起动。新增的 combination 块是 12 recipe ×
4 scale × 5 rate = **240 条编码**（HEVC/VP9/mpeg4/mjpeg × MP4/MKV/TS/FLV/AVI/WebM，
qvga/vga/hd/fhd，15/24/30/50/60 fps），只挂在 **tier 2**：`smoke`/`standard` 一条不变，
PR 门禁仍然是便宜的 standard 档，240 条编码的成本落在**周常** `corpus-full.yml` 上。

recipe 的**选择标准是"本套 shipped FFmpeg 解得开"**（`tools/setup_ffmpeg.sh` 的解码器表是
**刻意最小**的 `h264,hevc,aac,mp3,vp9,opus,mpeg4,mjpeg`）。写进矩阵的 codec 若本套 build
解不开，那就是一个**冒充失败的 SKIP**，而它污染的是打开率。`--min-samples` 默认值从 35
改到 **300**，于是"未到 300"的那句提示从此只在真的没到时报。

### (2) 加宽立刻变红：`codec_mpeg2.mpg` 撞 `params.is_valid()`

加宽后的第一次 full 跑：**315 样本、打开 308/309、seek 302/309、崩溃 1**。顺这条线挖下去，
发现的是**同一个 CHECK、同一句话的三种进入方式**——玩家把"这个流播不了"变成了**进程 abort**：

```
[FATAL check.cc:20] Check failed: params.is_valid().
  #6 avbase::media::AudioRendererAlgorithm::Initialize
  #7 avbase::media::AudioRendererImpl::Initialize
```

`AudioRendererImpl::Initialize` 的入参由 `RendererImpl::MakeAudioParameters(audio_decoder_config)`
算出，而 `AudioRendererAlgorithm::Initialize` 对它 `CHECK`。三条进入方式：

| # | 触发 | 机制 | 证据 |
|---|---|---|---|
| ① | 容器**没声明采样格式**（裸 `.ac3`、Ogg/Vorbis、MPEG-PS 的 mp2） | libavformat 给 `AV_SAMPLE_FMT_NONE` → `SampleFormat::kUnknown` → `bytes_per_frame()==0` | 用**本套 pinned FFmpeg** 探针：ac3/vorbis/mp2 的 `codecpar.format = -1`；同一文件用 Homebrew 8.0.1 探针则是 `8 (fltp)` |
| ② | 声道数**没有具名布局**（3/4/5/7 声道） | 解复用器只能标 `ChannelLayout::kDiscrete`，而 `ChannelLayoutToChannelCount(kDiscrete)` 答 **0** | `ffmpeg -ac 3/4/5/7` 造四个 m4a，**四个全部**撞同一 CHECK |
| ③ | 容器**压根没量出这条流** | libavformat 警告 "Could not find codec parameters"，交出 0 声道 0 采样率 | `codec_mpeg2.mpg` 里混着一条 `mp3, 0 channels` 的幽灵流 |

①② 的教训是同一句：**`is_valid()` 问错了问题**。采样格式是**源声明的**，而容器**允许不声明**；
`AudioDecoderConfig::IsValidConfig()` 从来不要求它，播放路径也**不读它**（解码器每个包自带真实
格式）——把它算进有效性，就是把"容器没说"变成 abort。声道数则是被一个**有损枚举**吃掉的：
`kDiscrete` 只是个标签，计数只能来自 `AudioDecoderConfig::channels`。

### (3) 修法：两个修点，一个"不修"

* **`AudioParameters` 显式携带声道数**（构造第 5 参，0 = 仍从 layout 推），`channels()`
  优先用它；`is_valid()` 改为 `sample_rate > 0 && frames_per_buffer > 0 && channels() > 0`
  ——**不再要求采样格式已知**。`RendererImpl::MakeAudioParameters` 传 `config.channels`。
  具名布局路径完全不变（既有调用方零改动），`kDiscrete` 不再是 0。
* **解复用器丢弃"量不出来"的音频流**：新增 `AudioDecoderConfig::HasUsableParameters()`
  （= `sample_rate > 0 && channels > 0`，**刻意弱于** `IsValidConfig()`），为假的流在
  `FFmpegDemuxer::AddStream` 里按"播不了"丢弃并记 WARNING——与既有的
  `Data and attachment streams are not playable` 同一条理由。
* **不丢的是"本套 build 没有解码器"的流**。那是**机器的性质**，不是文件的性质：
  `IsValidConfig()` 要求 codec 可解析，用它当丢弃条件会让最小构建**静默隐藏**自己能播的文件。
  于是 corpus 多了一条分类：带 `decoder factories declined` 标记的样本记 **SKIPPED**，
  不计入打开率/seek 率——与"本机没有 libmp3lame 编码器"是同一类事实。

### (4) 加宽的产物撞了 C1：按既有接缝再拆一次解复用器

三个崩溃都修在 `ffmpeg_demuxer.cc`（`AddStream` 的丢弃分支）与 `renderer_impl.cc` 里，而
`ffmpeg_demuxer.cc` 是白名单上**最贴近上限**的那一条：修完 **1154 行**，C1 上限 1140，
**超 14 行**。两个选择——抬上限，或真拆一次。

抬上限在这里是错的，理由就写在这条白名单注解自己身上：它说这个文件**不能按行数切**，因为
里面是"**一段连续的 `AVFormatContext` 生命周期，切它是 use-after-free 的来历**"。但
`MakeVideoConfig` / `MakeAudioConfig` / `MakeTextConfig` 三个函数**不在那条线上**：

| 判据 | 这三个函数 |
|---|---|
| 入参 | 一个 `const AVStream*` |
| 出参 | 一个值类型 config（`Video`/`Audio`/`Text`DecoderConfig） |
| 碰 `AVFormatContext` 吗 | **不碰**——不读它的字段、不碰它的包队列、不碰它的析构 |
| 有状态吗 | **无**——同一个 `AVStream` 永远得到同一个结果 |

所以搬走它们**不可能移动任何生命周期**，这是"安全的切"与"危险的切"之间的可判定分界。

新 TU `media/filters/ffmpeg_demuxer_configs.cc` 收下三个实现（改名加 `Decoder` 后缀，
与头名 `ffmpeg_demuxer_configs.h` 一致，顺手符合 cpplint 的"同名头居首"约定），
`ffmpeg_demuxer.cc` 只留**声明 + 调用**。文件 **1154 → 1022**，`LINE_LIMIT_ALLOWLIST`
里的上限从 1140 **降到 1030**，并把注解改写为记录**两次**拆分（`_track_select.cc` 是第一次、
`_configs.cc` 是第二次）——棘轮只许往下，这条注解现在也得说清"剩下的 1022 行为什么不能再切"。
顺手从 `ffmpeg_demuxer.cc` 摘掉两处**随函数搬走的**依赖：`extern "C" { <libavutil/display.h> }`
（旋转矩阵随视频 config 走）与 `<cmath>`（唯一的 `std::isnan` 也在那边）。

### (5) 加宽顺带把"本机解码器面"量清楚了

`full` 的 315 条里，**7 条**因本套 build 无解码器而 SKIPPED（`codec_mpeg2.ts`/`.mpg`、
`container.ogg`、`audio_vorbis.ogg`/`_44k.ogg`、`audio_ac3.ac3`/`_6ch.ac3`），**6 条**
非零退出（3 条 flac 无解复用器 + 3 条故意损坏样本）——全部**点名**，不再和"打不开"混为一谈。

### (6) 验证

| 面 | 实测 |
|---|---|
| `full`（315 样本，15m50s） | 打开 **308/308 (100%)** · 首帧 **1.026 s** · seek **302/308 (98.1%)** · **崩溃 0** |
| 新增单测 | 9 个 value-type 用例（`AudioParametersTest` 6 + `AudioDecoderConfigTest` 3） |
| `check_invariants` | all rules pass（364 文件扫到，含两个新测试 TU；C23 基线 37 行不变） |
| clang-format | 改动 8 个文件（含拆分后的两个新文件）全部 0 漂移 |
| cpplint | 全 scope **119 文件 0 违规**（含新 TU；scope 只含非测试非 legacy `.cc`） |
| `ctest build/ffmpeg` | **549 通过 / 0 失败** |
| `ctest build/no-ffmpeg` | **437 通过 / 0 失败**（含拆分后仍要编过的两个新测试 TU） |
| `ctest build/asan` | **549 通过 / 0 失败** |

### (7) 遗留

* **CI 首跑未验证**：本机是 pinned FFmpeg 7.1.1（`tools/build`），CI 的 `corpus` job 不带
  `tools/build` 会落到发行版 ffmpeg，两者 `codecpar.format` 的填充情况不同——这正是修在玩家
  而不是修在样本表里的原因。首个完整跑过 `corpus`/`corpus-full` 的 push 才算数。
* **组合维度的回归最多滞后一周**：它只在 `full` 里，PR 档不含。这是刻意换来的运行时间，
  但"组合类缺陷靠周常发现"是这套设计的已知代价。
* flac 三条只有解复用器缺失这一条路径，没有 `decoder factories declined` 标记，
  仍计在 seek 率里（与三条故意损坏样本同类）。

### (8) 教训

* **加宽自己就是一次测试**：240 条新样本进矩阵后，"此前从未在 full 档跑过的那类文件"
  立刻变红。corpus 的数字只有**真跑过**才存在——上一轮记的"崩溃已清零"覆盖的是那一轮
  的 35 条样本，不是这一轮的 315 条。
* **CHECK 会把"输入不对"升级成"进程死"**：边界校验要放在**能拒绝**的那一层。
  `AudioParameters` 的采样格式本就不该参与有效性判断；`kDiscrete` 把计数丢掉则是
  枚举设计替调用方做了它做不到的决定。

## 第三十三轮：cpplint 修树轮——把最后一个关闭的风格门禁打开

### (1) 先量，再决定什么该"修"、什么该"关"

`check-cpplint` 一直没开，理由写的是"未做修树轮"。按 R12（门禁必须首发即绿），修树轮分两步：
先量违规，再判断哪些是**真缺陷**、哪些是**项目约定偏离**。

以 CI 同版 `cpplint==1.6.1`（`--repository=.`，scope 对齐 `check-clang-tidy`：
`base/ media/ player/ platform/ tools/` 下的非测试非 legacy `.cc`，118 文件）量出 **59 处违规**：
`build/include_what_you_use` 29、`build/include_order` 16、`build/c++11` 11、`runtime/int` 2、
`runtime/references` 1。

只有一类是**约定偏离**：`build/c++11` 是一份过时的 Google 白名单，把 `<chrono>/<mutex>/<thread>`
当作"未获批准的 C++11 头"。项目按 C++20 构建（`.clang-format: Standard: c++20`），第 31 轮的
异步转码入口正建在 `<thread>/<mutex>` 上，所以整类关掉，写进新增的 **`CPPLINT.cfg`**
（与 `.clang-format`/`.clang-tidy` 并列为一处真相源：`set noparent` + `linelength=80` +
`filter=-build/c++11`）。其余 **48 处全部当真缺陷修**。

### (2) include_order 不是"有意分块"，是 Preserve 漏掉的漂移

16 处 include 次序违规初看像作者有意分块。但 `.clang-format` 的 `IncludeCategories` 优先级
是 **C 系统 2 → C++ 系统 3 → 项目 4**——**这正是 Google 次序**，只是 `IncludeBlocks: Preserve`
让它**不跨块合并**，于是"项目头块排在系统头块之前"这种漂移既没被格式化抓到、也没被门禁看见。
所以修法是把系统头块移到项目头块之前，而不是关掉规则。

读 cpplint 源码（`_ClassifyInclude`）还定位到一处**针对 C1 拆分的非对称**：它按"文件基名"
判定"本文档实现的那个头"。`pipeline_impl_host.cc` 里的 `pipeline_impl.h` 首段（`pipeline`）
与文件名首段相同 → `_POSSIBLE_MY_HEADER`（可居首、不报错）；而 `track_selection.cc` 里的
`player_impl.h` 首段（`player`）≠ 文件名首段（`track`）→ `_OTHER_HEADER`（必须排在系统头之后）。
所以 `track_selection.cc` 只能以系统头开头——这是"没有同名头"的那类 C1 拆分 TU 的固有形态，
不是笔误。

### (3) 其余三类原地修

- `build/include_what_you_use`（29）：为直接使用、却只靠传递包含拿到的标准库补 include
  （`std::move`→`<utility>`、`make_unique`→`<memory>`、`string`/`vector`→`<string>`/`<vector>`）。
- `runtime/int`（2）：`media_log.cc` / `event_hub.cc` 里给 `vector::erase` 算距离的
  `static_cast<long>` 改成 `static_cast<std::ptrdiff_t>`（补 `<cstddef>`）——`long` 在 Windows
  是 32 位，`ptrdiff_t` 才是迭代器差值的正确类型。
- `runtime/references`（1）：`ffmpeg_transcode_audio.cc` 的 `ScratchFrame(AvFramePtr& slot, ...)`
  出参改指针（`AvFramePtr*`）。这是全树**唯一**一个非 const 引用形参——按 Google 约定（出参用
  指针）修掉，而不是把规则关掉。

### (4) 验收

改动 **34 文件 +65/−31**，全部是 include 增补/重排再加上面 3 处语义改动（两个类型 + 一个形参）。
本机复核：cpplint 全 scope **0 违规**；`ctest build/ffmpeg` **540 通过 / 0 失败**；no-ffmpeg
**277 + 25 通过**；asan **111 通过 / 1 跳过**；`check_invariants` all rules pass（360 文件，
C23 基线 37 行不变）；clang-format 全树 0 违规。`check-cpplint` 以**阻塞**模式接入 CI
（Linux，venv 装 `cpplint==1.6.1`，与 `check-clang-tidy` 同 scope、同文件列表）。

## 第三十二轮：全树 clang-format 扫尾——「树是干净的」这句承诺已经过期

### (1) 门禁开着，树却漂了

`check-format`（第二十三轮开启）是**全树**扫描 + `--Werror`：`git ls-files '*.cc' '*.h'`
去掉 `media/filters/legacy/` 与生成的 `player/option_registry.inc`，再 `clang-format
--dry-run --Werror --style=file`。它开启时立在一个前提上——"树是 clean 的"。用 CI 装的那一个
（Ubuntu 24.04 `apt install clang-format` = **18.1.3**，与本地更晚的 18.1.8 行为一致）量了一遍：
**46 文件漂移**。关键在最后一句：这些文件**从未 push 过**（origin 落后 6+ 提交），
所以 CI 根本没机会报红——门禁的"绿"只是**没跑过**，不是"跑过了且干净"。

### (2) 扫尾 = 纯格式化，且暴露了一个判断错误

逐文件 `clang-format -i` 直到全树 0 违规（354 文件）。改动全是格式化产物：`switch/case`
缩进、宏反斜杠对齐、实参与 include 折行——无 token 变化。大头的旧漂移是
`video_convert.cc`(63) / `gl_loader.h`(55) / `gl_present.cc`(34)；漂移清单里**也包含第三十轮
新提交的转码 TU**（`ffmpeg_audio_encoder_unittest.cc` 18 处等）。这纠正了上一轮的一个说法：
"我新写的文件手工即 canonical"只对我当时手改的那 4 个文件成立，**同批提交的其它文件并非 0 违规**。

### (3) 顺带揪出一个自第二十六轮潜伏的 typo

`player_impl.cc:32` 是 `#include "platform/ffmpeg/url_data_source.h""`——**多了一个引号**，
自 `a3137d2`（第二十六轮 url_data_source 接入生产路径）起就在。编译器把它当作
"extra tokens at end of #include directive" 的 **warning** 放行，所以**从没编译失败**；
但 clang-format 把 `..."h""` **拆成两行**，孤立的 `"` 落到文件作用域，就成了真正的
`error: expected unqualified-id`。**格式化把被编译器宽容的旧错误放大成了硬错误**——
这也是为什么这次是一个 commit 而不是两个：扫尾必须先修掉这个 typo 才成立。

验收：CI 同版 clang-format 18.1.3 全树 **0 违规**（354 文件）；`ctest build/ffmpeg`
**540 通过 / 0 失败**；no-ffmpeg **277 通过**；asan **111 通过 / 1 跳过**；
`check_invariants` all rules pass（360 文件，C23 基线 37 行不变）。

## 第三十一轮：E3b —— Transcode 的异步入口

### (1) 缺口其实不是「线程」，是「取消」

第二十七轮给 E3 登记的范围是「trim / 进度 / **异步**」，第二十八轮落了前两个，异步一直挂着，
理由写的是"没有调用方之前不加调度器（调用方 `std::thread` 即可）"。这句话只说对了一半：
调用方确实能自己起线程，但它自己起的线程**拿不到任何东西去停**——`Transcode()` 是
run-to-completion 的，唯一的"停止方式"是等它跑完。所以"异步"真正缺的不是调度器，是**取消**；
一个没有取消的异步入口，只是把"卡住"从调用线程搬到了工作线程。

### (2) 三层改动

1. **协作式取消 token**。新增 `TranscodeCancelToken`（一个 `std::atomic<bool>`），
   `Transcode()` 增加一个可默认的 `const TranscodeCancelToken* cancel = nullptr`。为 null
   即永不取消——这正是**既有同步调用方与全部既有用例一行不改**的原因。
2. **取消点 = 包边界 + 阻塞读**。主循环条件改为 `!cancelled() && av_read_frame(...)`；
   循环之后、两段 flush 之前各补一次检查。真正让它"立刻"生效的是 **AVIO interrupt
   callback**：现在 `avformat_alloc_context()` 由我们分配、并在 `avformat_open_input`
   **之前**装上中断回调，于是一次卡住的 open 或 read 能被马上打断——否则 token 得等那次读
   自己返回才被看见。**关键的一处正确性**：中断造成的读失败，在循环后被**识别为取消**而不是
   被当成 EOF——否则"用户取消了"会被写成一次"成功"。
3. **异步句柄 `TranscodeJob`**：持有工作线程，`Start`（已有任务在跑则返回 false）/
   `Cancel`（任意线程可调，**包括回调内**）/ `Wait` / `IsRunning`，外加 `TranscodeDoneCB`
   收最终 `Status`。回调在工作线程上跑，且**永不晚于 `Wait()`/析构返回**；析构 =
   `Cancel()` + `Wait()`，工作线程绝不比句柄活得久。`Start` 对"从回调里再 `Start`"（即
   join 自己）直接拒绝，而不是死锁。

**顺手删掉**：`Transcode()` 里一个声明之后从未使用的 `ff::DictPtr open_opts(nullptr)`。

### (3) C1 又一次挡住了"就挤一下"

加完异步句柄后，`ffmpeg_transcode_job.cc` 正好 **499 行**——贴着 500 上限。异步句柄（只管
线程与取消，不碰 FFmpeg）与转码泵（FFmpeg 循环）本就是两个关注点，于是按真 seam 拆出
`ffmpeg_transcode_job_async.cc`（97 行），泵回到 414 行。这条 ratchet 的价值在这一轮最直白：
它逼我把"多出来的一坨"命名为一个独立的东西，而不是塞进一个 499 行的文件。

### (4) 验证结果（macOS 26 arm64 / AppleClang 17 / pinned FFmpeg 61.x）

- `ctest --test-dir build/ffmpeg`：**540 通过 / 0 失败**（较上轮 +2），3 disabled + 1 skipped 未计入；
- `tsan`：受影响四套件（Transcode/Concat/EncodeMuxer/AudioEncoder）**21 通过 / 1 跳过，
  零数据竞争**——新代码是线程 + 原子 + 取消，TSan 正是本轮最该过的门；
- `no-ffmpeg`：`ninja: no work to do`（异步 TU 属 ffmpeg 目标），套件 **126 通过**；
- `check_invariants`：**all rules pass**（360 文件，较上轮 +1）；C23 回到基线 37。

两个新用例都**可判伪**：

- `AsyncJobRunsAndReportsCompletion`：同一句柄连跑两次（覆盖"重收上一个工作线程"的路径），
  断言 100% 进度、最终 Ok，且产物能**解回**成有真实时长的容器；
- `CancelStopsTheJobWithCancelledStatus`：在**第一个进度回调内**调 `Cancel()`，断言最终状态
  是 `kCancelled`——若取消是个空操作，状态会是 Ok，`ASSERT_FALSE(final_status)` 就会红。

### (5) 本轮未做 / 边界

| 项 | 状态与原因 |
|---|---|
| 取消时清理半成品 | 未做：留给宿主决定（`CancelledStatus` 的 suggestion 已明说半成品仍在盘上） |
| 硬编实测（E5） | 仍 ⬜：本套 pinned FFmpeg 无 hw encoder，需另一套构建 + 真机 |

## 第三十轮：音频编码器通用化——从 AAC 专用到任意 libavcodec 编码器

### (1) 缺陷的形态：编码器被写死，而调用方早就以为它不写死

第二十七轮铺 L1 编码器层时，音频侧落成了 `FFmpegAacEncoder`：类名写着 AAC，`Initialize`
里 `avcodec_find_encoder(AV_CODEC_ID_AAC)` 也写死 AAC。视频侧同时落地的是对称的
`FFmpegVideoEncoder`——**天生 codec-name 驱动**，`Params.codec_name`（默认 `"libx264"`），
由工厂或调用方选。两侧一对比，音频侧的硬编码就是历史遗留，不是设计。

更能说明问题的是**调用方那一侧早就假定它是通用的**：`TranscodeJob::AudioCodecSpec` 的
`codec` 字段注释一直写着 `"copy" or encoder name`，默认值 `"aac"`；`PrepareAudioStream`
也确实读了 `params.audio_codec.codec`——**读完却没用**，直接写死 `ms.codec_name = "aac"`。
于是「请求 `libopus`」这种调用会在容器头里被写成 `aac`，描述一条根本不存在的流：这是一个
**说得出、做不出、还不报错**的接口谎言。

### (2) 改法：改名 + codec_name + 解析后名字回填

四项改动，全部对齐视频侧既有约定：

1. **改名**：`ffmpeg_aac_encoder.{h,cc}` → `ffmpeg_audio_encoder.{h,cc}`；类
   `FFmpegAacEncoder` → `FFmpegAudioEncoder`。与 `FFmpegVideoEncoder` 成对称命名，
   名字真实反映能力。
2. **`Params.codec_name`**（默认 `"aac"`，保持向后兼容）：`Initialize` 走
   `avcodec_find_encoder_by_name(name.c_str())`；空串回落 `"aac"`；查不到就
   `return false` 并打印「本套 FFmpeg 无此编码器」。于是 `libmp3lame` / `libopus` /
   `flac` / `ac3` 等**凡构建里有就能直接选**。
3. **解析后名字回填封装写端**（真缺陷修复）：`PrepareAudioStream` 里
   `enc_params.codec_name = params.audio_codec.codec`，`ms.codec_name = enc_params.codec_name`
   ——**写进容器的是解析后的名字**，不再无条件写 `"aac"`。错误路径的 `codec=` 诊断也带上
   真正请求的名字。
4. **采样格式策略**：第一次实现按 `codec->sample_fmts` 协商（FLTP 优先、否则取首个），
   但该字段已被 FFmpeg 弃用并触发编译告警。视频编码器是**硬编码 `YUV420P`** 的，为了与
   对称类一致、且不触碰弃用字段，最终**硬编码 `AV_SAMPLE_FMT_FLTP`**（planar float，正是
   解码路径的输出，也是 aac/libmp3lame/libopus/ac3/flac 全都接受的形式）。格式协商明确
   不在本轮范围——这行注释写进了代码，免得下一轮又有人来"修"它。

### (3) 为什么通用 codec 不会踩坏重采样/FIFO 路径

`ffmpeg_transcode_audio.cc` 里音频成帧（`FramesForEncoder` / `FlushResampler`）本就把
`frame_size <= 0` 当合法输入——**变量帧编码器（如 FLAC）上报 `frame_size == 0` 时直接绕过
FIFO 成帧**，不报错。所以"把 AAC 换成任意编码器"在成帧这一环是天然安全的，这是第二十九轮
拆出 `ffmpeg_transcode_audio.cc` 时顺手留下的正确性红利。

### (4) 测试：两个用例专为"证明它真的按名字走"而写

`ffmpeg_audio_encoder_unittest.cc`（由 `ffmpeg_aac_encoder_unittest.cc` 改名）保留
正弦→AAC 的 round-trip，并新增：

- `UnknownCodecNameFails`：`codec_name = "definitely-not-a-codec"` 必须 `Initialize`
  失败——**这条是判别性的**，写死 AAC 的实现也会"成功"，只有真的按名字查找才会失败；
- `DefaultCodecNameIsAac`：空 `codec_name` 仍初始化成功——**向后兼容的守门**；
- `EveryAvailableEncoderInitializes`：枚举构建里的音频编码器逐个 `Initialize`，本套
  pinned FFmpeg 只有 `aac`，故该用例**干净跳过**（这正是 1 skip 的来源，非失败）。

`ffmpeg_encode_muxer_unittest.cc` / `ffmpeg_concat_job_unittest.cc` /
`ffmpeg_transcode_job_unittest.cc` 三处引用同步改名；`platform/CMakeLists.txt` 与
`tests/CMakeLists.txt` 的源文件名同步。

### (5) 验证结果（macOS 26 arm64 / AppleClang 17 / pinned FFmpeg 61.x）

- `ffmpeg` 配置：**109 通过 / 1 跳过**（跳过项 `EveryAvailableEncoderInitializes`，
  本套构建只有 AAC，设计如此）——零回归；
- `asan` 配置：完整构建通过；受影响套件（AudioEncoder/EncodeMuxer/Transcode/Concat）
  **20 跑 19 过 / 1 跳过，零 sanitizer 报错**；
- `no-ffmpeg` 配置：`ninja: no work to do`（音频编码器属 ffmpeg 目标，本次改动不触及），
  套件 **126 通过**；
- 编译无告警（撤掉弃用的 `sample_fmts` 协商后）。

### (6) 本轮未做 / 边界

| 项 | 状态与原因 |
|---|---|
| 硬件音频编码器 | 未做，也无需做：本套 pinned FFmpeg 无音频硬件编码器，且无业务需求 |
| 采样格式协商 | 明确不做：与视频编码器一致硬编码 FLTP，negotiation 是独立命题 |
| `AudioEncoderFactory` | 未加：无音频硬件编码器候选时，工厂只是过度设计；按名字直连已满足"通用" |

## 第二十八轮（本轮）：转码 E3/E4/E5 落地——两个“测得的样子像对的”时间戳缺陷

本轮接手的是第二十七轮留下的 E3/E4/E5 半成品：源码与单测都已写好并注册进 CMake，但
**从未编译验证过**（E3 单测缺 `#include "base/functional/bind.h"`，编译不过）。编译修好后
9 个用例全绿——而这一轮真正的收获是：**那 9 个绿里有两个是假的**，它们的“通过”恰恰掩盖了
两个正在丢数据的缺陷。两处的共同形状是同一句老话的反面：**不是没有测试，是断言量错了东西。**

| slab | 文件 | 状态 |
|---|---|---|
| E3 离线转码 | `media/filters/ffmpeg_transcode_job.{h,cc}` + `ffmpeg_transcode_streams.{h,cc}` | ✅ trim(-ss/-t)、进度回调、**-ss 起点重基到 0**、写入失败一律返回 Status |
| E4 多输入拼接 | `media/filters/ffmpeg_concat_job.{h,cc}` | ✅ 快路径包拷贝+bsf、时间戳**接续不断裂** |
| E5 硬编工厂 | `platform/ffmpeg/video_encoder_factory.{h,cc}` | ✅ `EncoderNameFor` + NVENC + 平台工厂表，**并已接入 Transcode 生产路径** |

### (1) E4 拼接缝：每一道缝静默丢一帧，而测试是绿的

写完的 Concat 用“上一个片段的 max_pts + 1”当下一个片段的偏移。用一个一次性探针（现跑现弃）
读回我们自己 muxer 写出的 AAC MP4，得到：**16 个包，首包 `pts=dts=-1024`，`max_pts=14336`**
（AAC 编码器的 priming 样本，经 edit list 呈现为负时间戳）。于是第二个片段的首包
`-1024 + 14337 = 13313`，**落后于上一个片段的最后一个 dts 14336**。

这正好报错里的那一行——`non monotonically increasing dts to muxer in stream 0: 14336 >= 13313`，
而这一行原本一直躺在日志里没人看。丢包静默的原因更值得记：**所有 `WritePacket()` 的返回值都被丢掉了。**
于是输出 619ms（两片段该 ~683ms），少一帧 21ms，而原断言只要求“比一个片段长”——**619 > 320，绿。**

修法不是把偏移算准一点，而是改了模型：**把一个片段整体归一化到正在延伸的输出时间轴上**
（首片段落到 0，后续片段落到前一段的末尾），片段内部的相对间隔照旧。顺带也修掉了另一个同源缺陷：
输出时间轴原本从 -1024 起步。

修复后的数字能逐位对上（`n` 个包 × 1024 采样 / 48k）：

| 输入 | 修复前 | 修复后 | 应有值 |
|---|---|---|---|
| 2 × 320ms | 619ms（丢 1 帧） | **682ms** | 32 × 21.3 = 682.7 ✓ |
| 3 × 320ms | — | **1024ms** | 48 × 21.3 = 1024 ✓ |

测试同步从“比一个片段长”改成**“不得短于两个输入时长之和”**，并补了一个三片段用例——原缺陷是
**每道缝丢一帧**，一道缝的写法可能被容忍糊弄过去。同时 concat / transcode 两处的写入结果现在
**一律检查并作为 Status 返回**：一个静默失败比一个响亮的失败糟得多。

### (2) E3 `-ss`：`duration` 是对的，文件是错的

这是本轮最该留下的那条教训。第一版 `-ss` 测试只断言“输出时长 ≤ 输入时长 - 150ms”，**它过了**：
输入 426ms，输出 234ms，看着完美。**但如果当时就收工，我们交付的文件前 192ms 是空的。**

因为 seek 之后直接把包的原时间戳写进输出容器，输出的时间轴**从 192ms 开始**。任何以 duration
为准的检查都看不见这个偏移——包括我自己的第一版断言。真断言是 `MediaInfo::start_time`：
`start_time` = 192000us ≤ 1ms 直接暴露它。修法与 E4 同源地标了同一个 helper：每个流以
**第一个真正写出的包**为原点做平移（不用 seek 目标值——容器未必落在请求的点上）。

### (3) E5：给工厂找一个生产调用方

工厂能编能测，但全仓除了单测没有任何调用者——和第二十三轮 `SelectVideoDecoder` 踩的是同一个坑，
只是这次我们在踩之前就发现了。更直接的漏洞是：`CreateEncoder()` 收的是一个**编码器名字**，
而工厂从不暴露它要为某个 codec 用哪个名字；选出一个 VideoToolbox 工厂之后，调用方依旧无从得知
该去要 `h264_videotoolbox`。**选择了却没法执行**。

补 `EncoderNameFor()`、NVENC 工厂、以及把平台分支收进 `CreateVideoEncoderFactories()`
（`media/` 不该知道这台机器是哪一种），再接进 Transcode 的视频编码器初始化：默认走工厂
（`prefer_hardware` 关 → 只有软件工厂留下 → 行为与从前一致）。本套构建里缺某个编码器不会
拖垮整轮作业：候选按优先级逐个尝试，全失败才报错。

端到端佐证用一个注入的工厂：请求 `"libx264"`，工厂回答 `"mjpeg"`，则输出容器记录的 codec 就是
`mjpeg`——**证明解析出的名字真的到达了编码器**，而不是算完就被丢掉（否则 Output 会是 h264）。

**必须如实登记的边界**：本轮**没有**在任何机器上验证过硬件编码本身。探针显示这套 pinned FFmpeg
里只有 `aac` 与 `mjpeg` 两个编码器可用，`h264_videotoolbox` 甚至在这台 Mac 上也**不存在**。
所以 E5 目前是“已接线 + 已单测”，不是“硬编已实测”。同理 `VideoCodecSpec::codec` 的默认值
`libx264` 在这套构建里**编不出来**——好在它的失败是清晰的报错，而不是悄悄降级。

### (4) 顺手修掉的两个前置缺陷（不修则上面的候选回退不成立）

候选回退要求在**同一个对象上多次 `Initialize()`**，而两个编码器的 `Context` 都没有析构函数：
失败的那次会泄漏半成品 context，重挑的那次还会覆盖掉旧的。加上析构并让 `Initialize()` 先
`ctx_.reset()`。ASan 整线通过（含本轮新增的转码/拼接用例）。

另外记一个**本轮范围内的相邻修复**（不在 E3/E4/E5 内，但它是 ASan 车道红着的原因）：
`video_convert_unittest.cc` 的 `MakePlanarFrame()` 对每个平面都走**整帧高度**，而 420/NV12 的
色度平面只分配了半高——越界写。之前的非 ASan 配置全绿、ASan 下 3 红，**且唯一通过的那一个正好
是 YUV422P（平面全高）**，这条相关性就是定位依据。按 `log2_chroma_h` 取行数后 ASan 3 红 → 0。

### (5) 本轮未做 / 遗留

| 项 | 状态与原因 |
|---|---|
| 硬编（VT/VAAPI/NVENC）实测 | ⬜ 本套 FFmpeg 无这些编码器（探针实测），需另一套构建 + 真机；目前只到“已接线 + 已单测” |
| E4 慢路径（参数不一致 → 重编码再拼） | ⬜→✅ **第二十九轮已收尾**：检测不一致即走重编码路径，320ms+348ms → 710ms 逐位吻合 |
| E3 异步入口 | ⬜→✅ **第三十一轮已落地**：缺的不是调度器而是取消——`TranscodeCancelToken` + AVIO 中断回调 + `TranscodeJob` 工作线程句柄（见第三十一轮） |
| 拷贝模式的时间基假设 | ⬜→✅ **第二十九轮已修**：`RescaleTimestamps` 按 `src_tb → dst_tb` 重基，MKV/ADTS 时间戳差数量级的缺陷关闭 |
| 非浮点解码输出 | ⬜ `AudioState::swr` 字段预留但未接线：本 job 打交道的 codec 都解成 float；遇到 s16 解码器会出垃圾音，需真样本再补 |
| check_invariants C1 | 🔴→✅ **第二十九轮已清**：4 处历史违规 + 本轮引入的 `ffmpeg_transcode_streams.cc`，按真 seam 拆出 5 个 TU，`check_invariants` 现 all rules pass |

### (6) 验证结果（macOS 24.5 arm64 / AppleClang 21 / Homebrew FFmpeg 7.1.1）

- `ffmpeg` 配置：**531/531**（3 个历史 DISABLED 未计）
- `no-ffmpeg` 配置：**428/428**
- `asan`：**531/531**（修复前 3 红，见 (4)）
- `check_invariants`：本轮文件 0 违规；C23 回到基线 37
- `clang-format --dry-run --Werror`：本轮改动文件 0 偏离

## 第二十九轮：清五项 C1 门禁 + 收尾 E3c/E4b

接手第二十八轮留下的两件事：其遗留表里登记的“拷贝模式时间基假设”（⬜）、“E4 慢路径”（⬜）与
“C1 已有 4 项违规”（🔴），以及第二十八轮自己新引入的 `ffmpeg_transcode_streams.cc`（620 行，超 C1）。
本轮把四项全清，且**全部按真 seam 拆分 / 真缺陷修复**，不压行、不糊弄。

### (1) C1 门禁：五项违规按真 seam 拆分

C1 是 500 行（或 allowlist 上限）的“强制命名 seam”门禁——它要的是把一类职责独立成 TU，而不是把
文件压到 500 行以内。`renderer_impl.cc` 在 allowlist 上限 600，但当时 650 仍超限，所以本轮目标只是
把它压到上限以下；其余四项按 500 计。

| 原文件（行数） | 拆出 | 余下 |
|---|---|---|
| `audio_renderer_impl.cc` 503 | `audio_renderer_impl_render.cc`——S4 设备回调 `Render` + `ScaleAndZeroTail` + `OutputFramesToMediaTime` + `OnRenderError` | 410 |
| `pipeline_impl.cc` 528 | `pipeline_impl_properties.cc`——四个运行时 setter（音量/速率/延迟/保音高）+ 读回访问器 | 415 |
| `renderer_impl.cc` 650（上限 600） | `renderer_impl_reporting.cc`——`PushMasterClock` / `PushStatistics` / `GetStatistics` / `TakeSnapshot` | 525（< 600） |
| `player_impl_events.cc` 508 | `player_impl_accurate_seek.cc`——精确 seek 状态机（`player_impl.h` 已注明其归属） | 351 |
| `ffmpeg_transcode_streams.cc` 620（本人第二十八轮引入） | `ffmpeg_transcode_audio.cc`——`AudioState` 析构 + `AvFrameToAudioBuffer` + `ScratchFrame`/`EnsureResampler`/`DrainWholeFrames` + `FramesForEncoder`/`FlushResampler` | 384 |

注册：core 三个新文件进 `media/CMakeLists.txt` 手写的 `filters/` 列表；`ffmpeg_transcode_audio.cc`
进 `platform/CMakeLists.txt` 的 ffmpeg 目标（`player/*.cc` 是 glob，自动纳入）。**所有移出的都是成员函数
定义或头文件已声明的自由函数，声明不动、链接跨 TU 解析，行为零变化。**

### (2) E3c：拷贝路径的时间基假设（真缺陷）

症状：拷贝分支把输入包的 `pts/dts/duration` 直接写进输出流，默认输入轨道时间基 == 输出流时间基。
但容器时间基并不统一——MKV 常为 `1/1000`、ADTS 为 `1/28224000`、MP4 即便同族也可能不同——于是
同一条流在不同容器里被写出差好几个数量级的时间戳。

修复：引入 `RescaleTimestamps(ep, in_num, in_den, out_num, out_den)`，在 transcode / concat 每个
`emit` 处按 `src_tb → dst_tb` 用 `av_rescale_q` 重基；时间基相同则 early-return，不引入舍入。

佐证：`ffmpeg_transcode_job_unittest.cc::CopyModeRescalesAcrossContainerTimebases`（MP4/MKV/ADTS
三类容器）——修复前 mkv 实测 9.3ms、应为 447ms，adts 实测 263s、应为 447ms；修复后逐位吻合。

### (3) E4b：Concat 慢路径（参数不一致 → 重编码再拼）

症状：各段 codec 参数不一致时，旧实现仍走包拷贝（丢掉重编码），输出时长严重偏短、音画错位。

修复：检测不一致即改走重编码路径（复用 transcode 的 decoder→encoder），临时封装文件中转；
时间轴归一化沿用快路径“把每段归一化到延伸的输出时间轴”的算法，避免拼接缝丢帧。

佐证：`ffmpeg_concat_job_unittest.cc::IncompatibleParametersTakeTheSlowPath` 与
`SegmentsFromOtherContainersKeepTheirDuration`——实测 320ms + 348ms → 710ms，逐位吻合。

### (4) 验证结果（macOS 26.2 / AppleClang 17 / pinned FFmpeg 61.x）

- `ffmpeg` 配置：**535/535**（3 个历史 DISABLED 未计）——含 transcode / concat / renderer /
  audio-renderer / pipeline / player 全绿，五项拆分零回归
- `no-ffmpeg` 配置：**428/428**
- `asan`：编译 + 链接全绿；`ctest` 因本沙箱**拒绝 `/bin/ps`**（gtest 测试发现步骤要跑该二进制）
  无法在本环境执行，属环境限制而非代码缺陷（与历史一致）
- `check_invariants`：**all rules pass（359 files）**，C23 与基线 37 持平
- `clang-format --dry-run --Werror`：本轮改动文件 0 偏离

## 第二十七轮：转码基石——编码器层 E1/E2 落地与修复

平替设计分析：对照 ffmpeg.c 七功能域、MediaComponent transcode.cc/combine.cc 能力面与
avbase 现有框架，确认 L1 编码器层和 L2 封装写端是唯一净新增（解封装/解码/滤镜图全部
已有可直接复用）。

| 层 | 文件 | 状态 |
|---|---|---|
| L1 音频编码器 | `media/filters/ffmpeg_aac_encoder.{h,cc}` | ✅ AAC-LC，planar float 输入，extradata 输出 |
| L1 视频编码器 | `media/filters/ffmpeg_video_encoder.{h,cc}` | ✅ mjpeg(base)/libx264(依赖层)，I420 输入，颜色空间透传 |
| L2 封装写端 | `media/filters/ffmpeg_encode_muxer.{h,cc}` | ✅ mp4/matroska/adts，音频 1/rate、视频 1/90000 时间基 |

**关键修复**：`FFmpegEncodeMuxer::Open()` 缺少 `avio_open`——对于 MP4/matroska 等需要文件
IO 的容器，`avformat_write_header` 通过 `fmt->pb` 写数据，`pb` 为 null 时段错误。ADTS 是
裸流（`AVFMT_NOFILE`）不受影响，所以 ADTS round-trip 测试此前能通过而 MP4 测试崩溃。
修复：在 `Open()` 中对非 `AVFMT_NOFILE` 格式调用 `avio_open(&fmt->pb, path, AVIO_FLAG_WRITE)`。

**测试（3 个 round-trip，全部通过）**：
| 测试 | 路径 | 验证 |
|---|---|---|
| `AacEncoderTest.EncodeThenAdtsDecodeRoundTrip` | 正弦→AAC→ADTS→demuxer 解回 | extradata 非空、duration 350-600ms、buffer 数>0、EOS |
| `EncodeMuxerTest.AacIntoMp4RoundTripsThroughTheDemuxer` | 正弦→AAC→MP4→demuxer 解回 | 流数=1、sample_rate=48000、duration 350-600ms |
| `EncodeMuxerTest.MjpegIntoMatroskaRoundTrips` | 渐变帧→mjpeg→mkv→demuxer 解回 | codec=kMjpeg、coded_size 匹配 |

同时清理：`ffmpeg_aac_encoder.cc` 中遗留的 `fprintf(stderr, "DBG ...")` 调试语句；
`ffmpeg_encode_muxer_unittest.cc` 中硬编码的 `/tmp` 路径改为 `testing::TempDir()`，
mjpeg 测试结尾误删 mp4 的 bug 修正为删除正确的 mkv 路径。

验证：ffmpeg **513/513**、asan **513/513**、invariant 302 文件全过。

**后续里程碑（不在本轮范围）**：
- E3：`TranscodeJob`（离线调度：Demuxer→Decoder→FilterStage→Encoder→EncodeMuxer，trim/进度/异步）
- E4：`ConcatJob`（多输入拼接：快路径包拷贝+bsf / 慢路径重编码，时间戳重排）
- E5：硬编工厂（VideoToolbox/VAAPI/NVENC，与解码对称的 `HwCodecFlag` 掩码+工厂优先级）



| 项 | 内容 |
|---|---|
| 2.5 断线重连 GAP 清零 | 新 `platform/ffmpeg/url_data_source.{h,cc}`（avio 承载:open 即装中断回调、ReadBlocking 每 read 寻址、私有 worker 上异步读、回调必投递不内联）；`PlayerImpl::Prepare` 把 http(s) URI 接成 UrlDataSource+RetryDataSource 进入桥（rtmp/rtsp/srt 留协议层）。weaknet 挂断用例 kError 0.2s → **kCompleted 23.7s（5 请求）**。ReconnectNow 如实声明"自动重试已激活",手动强踢需要安全的重开原语(Abort 是永久停止)已记录 |
| 6.2 直播字幕窗口锚点 | `CuesBeyondTheWindowAreDropped` 转绿(3 连跑):改写为**迟到订阅**形状(播 2s 后选轨,积压逐个判过期),断言锚定选轨时刻的媒体时间。两次中间失败均为测试错(选轨时机/钟偏斜边界),策略本身正确 |
| 4.7 clang-tidy | 修树轮实测 15 个跨层文件 **0 发现**,`check-clang-tidy` 以阻塞模式接入 CI(非测试非 legacy 源) |
| corpus CI 分层 | PR 跑 standard,新增 `corpus-full.yml` 每周(周一 03:00 UTC)+手动触发跑 full(见第三十四轮:full 现为 315 样本 ~16 分钟,属周常不属 PR,并另配 `corpus_baseline_full.json`) |
| 新单测 | `url_data_source_unittest.cc` 5 例(file:// 同 avio 路径) |

验证：ffmpeg **487/487**、no-ffmpeg **376/376**、asan **487/487**、weaknet 5 例全过且 **0 GAP**、invariant 302 文件全过。


## 第二十二轮（本轮）：docs/07 §5 纯音/纯视频断言 + 合成源单流禁用

`SyntheticSpec` 增 `enable_video/enable_audio`（GetStream 返回 nullptr、MediaInfo
去流——与"容器没有该流"同形）。两个管线级断言：**AudioOnlySourcePlaysThrough**
（视频侧 sink 装配存在但 start_count 恒 0，播到 EOS 无错误）与
**VideoOnlySourcePlaysThrough**（镜像）。RendererImpl 的单流回退（external/视频
时钟，ffplay 同义）由此获得端到端覆盖。夹具等待上限 10s→30s（并行负载下纯超时型
flaky 的防御；上限不是延迟，只拉长失败路径）。

过程教训重演并强化：多段补丁两次丢 `Play()` 调用——**已按记忆铁律改为单段编辑**，
当场命中。诊断手段记一笔：压测数字在构建失败时无效（旧二进制），必须重跑。

**遗留（park）**：`RendererImplTest.StartPlayingFromOpensTheAudioDevice` 在全量
ctest 下偶发挂起（963s 超时；单跑 15s 通过），挂点在 S3 停止后的析构等待——与本轮
改动的关系未定（时序敏感，两次全量一红一绿）。按 R12 登记专项诊断，候选方向：
DestroyOn 与夹具 TearDown 的线程停止顺序竞争。

## 第二十一轮（本轮）：直播追帧端到端验收（docs/12 §2.1 闭合）

`PipelineSeekTest.LiveSourceChasesToTheEdge`：合成源标记 live（duration 10s 作为
边缘替身）、延迟目标 2s、播放头起步落后 10s——**首个统计节拍触发追帧**（日志：
behind=7942ms, skipping to 9s），播放头落到边缘附近（断言 ≥8.5s），随后帧持续
呈现（无死锁、无错误）。此为 docs/12 §2.1 的端到端验收路径；真"增长边缘"demuxer
（时戳随墙钟推进）留作增强，当前用有限边缘替身先钉住触发→skip→续播全链。
过程修正：共享夹具（dcc876c）的 PumpRound/PlayAndWaitForSinks 已无 bus 参数。

**§2.1 状态：✅ 闭合**（判定核 + 接线 + 端到端验收；协议覆盖验收见 §2.4，另列）。

## 第二十轮（本轮）：直播追帧判定核（docs/12 §2.1 判定侧）

`media/filters/live_edge_policy.{h,cc}`：追帧决策纯函数（DecideNextFrame 模式）——
`ShouldChase(is_live, latency_hint, behind)`：仅直播 + 正向延迟目标 + 严格大于
（规范卷 §3"落后 >8s"）才追；`SuggestedTarget` 落点在边缘减去半个目标（留出
整个目标的余量，防止统计节拍上乒乓）。接线：`PipelineImpl::SetLatencyHint` 现在记录
hint（此前只转发渲染器）；统计节拍（1s，host TU）上检测 直播边缘(duration)−播放头
超过目标即自发起两阶段 skip（renderer flush + demuxer 从最新处续读——**非物理 seek**，
直播容器不能回退），复用既有 seek_in_flight_ 防重入与完成汇合。4 个纯函数单测。

**验收边界**：判定核 + 接线已测；docs/12 §2.1 的端到端验收（合成直播源落后 8s
一个阈值周期内追平、无死锁）需要合成直播 demuxer 假件（定时产流、时戳随墙钟
增长），是下一轮主体。

## 第十九轮（本轮）：LiveDataSource——直播字节源基建（docs/12 §2.2 前半）

`media/filters/live_data_source.{h,cc}`：可增长的直播字节源——生产者 `Append()`
（网络回调/测试 rig），消费者经 DataSource 桥顺序读，**在直播边缘阻塞等待**；
`Close()` 把边缘变成 EOF；`Abort()` 以 10ms 分片及时解锁（Δ15）。三个语义决定：
**部分读立即返回**（直播消费者不能等可能永远不来的整段——只读请求超出已有字节且
未 Close 时若整读阻塞会饿死）；非 seekable + streaming（mpegts 组合，可自动化）；
异步 Read 走私有 worker（契约：回调绝不内联）。6 个单测（模式校验/边缘阻塞直至
生产者追加/部分读不等待/Close 变 EOF/Abort 界定等待/异步投递），无 FFmpeg 依赖走
no-ffmpeg 门禁。

过程坑（更名轮的连锁）：导出宏已是 `AVBASE_MEDIA_EXPORT`、命名空间已是
`avbase::media`、filters 列表是手写的（base/legacy/renderers 才 GLOB）、
ConditionVariable 需显式以锁地址构造——四处全部撞了一遍。

## 第十八轮（本轮）：Phase 4.5 开篇——libFuzzer 目标 · 确定性驱动 · CI smoke

依据 avbase 升级计划 Phase 4.5（"对 platform/ffmpeg 输入做结构模糊，语料入库常跑"）。

### 落地面

| 项 | 说明 |
|---|---|
| `tests/fuzz/fuzz_demuxer.cc` | 共享 harness（无 main）：每份输入 = MemoryDataSource（**堆拷贝**——MemoryDataSource 不复制字节，libFuzzer 输入缓冲返回即失效）→ FFmpegDemuxer::Initialize → 对存在的音/视频流各做 ≤8 轮有界 Read → Stop → 与字节拷贝同批在 media 线程 FIFO 销毁。所有等待限时（init 10s / read 5s / drain 5s），挂死的输入被跳过而不是楔死 fuzzer |
| 双驱动 | `avbase_fuzz_demuxer`（libFuzzer，覆盖引导；运行时按 `check_cxx_source_compiles` 探测，没有则 EXCLUDE_FROM_ALL——AppleClang 不带 libFuzzer runtime）+ `avbase_fuzz_demuxer_standalone`（固定种子 mt19937 四类确定性变异：位翻转/截断/填字节/拼接，65 输入/种子，本机任何 sanitizer 配置可跑，崩溃可从命令行复现） |
| 语料 | `tests/fuzz/seeds/` 入库：3 个完整容器 + 截断头 + 损坏前缀；每次 run 先原样回放全部种子（回归即阻断） |
| CI | `fuzz-smoke` job（ubuntu-24.04，clang + fuzzer+asan）：60 秒覆盖引导 run + 种子回放；macOS 排除（无 runtime） |
| CMake | 根 CMake 的 tests 目录门控放宽为 `AVBASE_BUILD_TESTS OR AVBASE_BUILD_FUZZ`（fuzz-only 构建是合法配置）；测试套件段按 BUILD_TESTS 收进守卫 |

### 过程中抓到并修掉的两颗雷

1. **`base/types/expected.h` 的宏序雷**：命名空间分支用 `#if defined(AVBASE_HAVE_STD_EXPECTED)`，而 BuildConfig.h 的同名 `#cmakedefine01` **恒被定义（无 <expected> 的工具链上恒为 0）**——先含 BuildConfig 再含 expected.h 的 TU 会被推进 std::expected 分支后编译失败。改为带值判断的自包含 `AVBASE_USE_STD_EXPECTED`（include 顺序不再敏感）。
2. **退出期静态析构顺序**：harness 静态 Thread 在 logging 静态互斥量之后销毁，Stop() 里的 LOG 拿已销毁的锁（abort）。fuzzer 基建改为刻意泄漏（`new` 不 delete）——进程级资源的标准处置。

### 验证

- standalone 驱动：默认构建 + asan 配置各一轮，**325 输入全部干净退出**。
- libFuzzer 二进制本机不可链接（无 runtime），由 CI Linux 验证；fuzz-smoke job 已入 workflow。
- 回归：ffmpeg 439/439、no-ffmpeg 376/376、asan 439/439 ×2；invariant 全过（282 文件）。

## 第十六轮（本轮）：Phase 4.2 运行时音轨切换——SelectAudioTrack 全链

### 落地面

| 层 | 改动 |
|---|---|
| MediaResource | 新 `GetStreams(type)` 虚函数（默认单流实现向后兼容），FFmpegDemuxer 覆写为容器序全枚举。流身份 = `DemuxerStream::stream_index()`（容器流号，与 MediaInfo::streams 一致） |
| Demuxer 路由 | 新 `SetActiveStream(type, index)`：**只对被消费的流施加背压**，其余同型流的包直接丢弃。这是切轨的前置——否则旧流的孤儿队列把自己灌到水位线上，demux 循环自锁，新流饿死 |
| RendererImpl | `OnTracksChanged(kAudio, stream, cb)` 从桩变真实现：S1 编排——旧音频渲染器 S4 上停止+排干（`StopAndDrainForTeardown`：经 DecoderStream::Flush 以 kDecodingAborted 完成未决读）→ 重建（新 AudioParameters 取自新流配置，音量/静音/倍速/变速不变调设置从旧实例的原子量捕获重放）→ StartPlayingFrom(当前媒体时间) 续接时钟。kText/kVideo 维持可操作的 kNotImplemented |
| Pipeline | facade 新 `SelectAudioTrack(index, cb)`（默认实现如实回报 kTrackSwitchError）；PipelineImpl 编排流查找 + `SetActiveStream` 先行 + 渲染器交接 |
| 状态码 | `PipelineStatus::kTrackSwitchError` + 全映射（"切轨失败不是播放失败"），ToString/ToMediaError 同步 |
| Player | `SelectTrack` 接线：状态门（kPrepared/kStarted/kPaused）→ MediaInfo 索引校验 → 异步交接；结果以 `kTrackChanged{type,old,new}` 事件上行（实现独立成 `player/track_selection.cc`，两个既有文件都在 C1 天花板上） |

### 排掉的三颗雷（每颗都是测试抓的，不是读代码读出来的）

1. **demux 循环孤儿队列自锁**（切轨必现）：背压等待的三个唤醒者（Flush/Stop/seek）不含
   "消费者消失"。修复即上面的活跃流路由；`PlaysAudioOnlyTwoTrackFileToEosWithoutSwitch`
   是它的无切轨回归锚。
2. **S1 阻塞等待 S4 死锁**（首稿）：切轨的拆除在 S4，而 S4 的未决读回复经 media runner
   投回 S1——S1 等待即死锁。改为全异步交接，S1 零等待。
3. **退役渲染器 UAF**（ASan 抓的）：`delete on quiescence` 之后，更早排队、以
   Unretained 绑定旧对象的控制任务（StartPlayingFrom/SetVolume…）才执行。改为退役表：
   旧对象停止后转入 graveyard（所有入口在 stopping_ 下早退，成为惰性对象），
   ~RendererImpl 在 S4 统一销毁。AudioRendererImpl 析构的生命周期纪律从此与切轨解耦。

### 连带修复的夹具雷（throttle 夹具同样携带，靠 DISABLED 躲着）

- `MemoryDataSource` 不拷贝字节，throttle/track 夹具把局部 vector 传进去即悬垂——
  ASan 在切轨测试上炸出来。字节改挂 fixture 成员。
- 夹具 `PumpRound` 在 pipeline 停止后继续拉取已随 renderer 销毁的 sink——加
  IsRunning 门 + `set_render_runner` 把拉取序列化到渲染线程（seek 夹具早有此步）。
- `FakeAudioSink::Stop` 现在清回调指针，兑现"Stop 后不再 Render"的真 sink 契约。

### 验证

- 新 `two_audio_tracks.m4a`（双 AAC 轨，lavfi 合成）+ 5 个管线级测试 × 重复 4 遍全过：
  枚举、无切轨播放到 EOS、中途切换后到 EOS、往复切换、非法索引失败但管线健康。
- ffmpeg 435/435（+5）；no-ffmpeg 376/376；**asan 435/435（0 泄漏）**；
  invariant 全过（278 文件）；headless 回归正常。
- 未做：字幕轨(kText，基座只出文本+时间的文本腿)、视频轨切换、
  `kHardwareOnly` 生产语义、直播追帧（需直播源基建）。

## 第十七轮（本轮）：Phase 4.2 字幕文本腿——kText 轨选择与 TimedText 事件

### 落地面

| 层 | 改动 |
|---|---|
| media/base | `TimedTextCue{text,ass,pts,duration}`（基座只出文本+时间，渲染归宿主）；`TextDecoderConfig{codec_name,language,extra_data}`；`TextDecoder`/`TextDecoderFactory`——**刻意同步**：字幕包极小、解码器直通，S1 上无节流问题，异步契约是无消费者的仪式 |
| DemuxerStream | `text_decoder_config()` 虚函数（基类默认空配置，单字幕-free 实现零改动）；FFmpegDemuxerStream 从 codecpar 填充（含语言元数据） |
| FFmpegTextDecoder | avcodec_decode_subtitle2；ASS 括号标记剥离出纯文本，原始 markup 进 `cue.ass` 供会排版的宿主；pts 取 **buffer 时间戳**（demuxer 已按容器时基盖戳——MKV 的 packet time_base 不可信，实测差 1000 倍）；时长回退到容器 BlockDuration |
| Demuxer 路由 | 文本流**常驻**有界丢弃最旧队列（`DecoderBufferQueue::TryPushDropOldest`），不参与水位背压：字幕包一次性到达，按 A/V 路由要么楔死循环要么让迟到订阅全失 |
| RendererImpl | S1 文本泵（Read→解码→`client_->OnTimedText`）；世代号使跨切换/flush 的在途读回复失效；kText 选择分支替换"谎报桩" |
| 事件链 | RendererClient/Pipeline::Client 新 `OnTimedText`（默认 no-op）；PlayerImpl 转成 `kTimedText` payload；`SelectTrack(kText)` 接线（`player/track_selection.cc` 与 `media/filters/pipeline_track_select.cc` 各自独立成文件，宿主都在 C1 天花板） |

### 生命周期三连雷（teardown 的历史债，本轮一并封口）

1. **Stop 异步化 vs IsRunning 语义**：FinishStop 在 Flush 完成后才落 kStopped，
   `IsRunning()` 必须包含 kStopping，否则测试在 kStopping 就销毁管线（DCHECK 抓的）。
2. **管线停止需先 Flush 渲染器**：裸 reset 留下 demuxer→S1→S4 回复链在途，落在已释放
   的 AudioRendererImpl 上锁已释放互斥量。DoStop 改为先 Flush（内联完成全部在途读）
   再销毁；~RendererImpl 的音频销毁合并为"排干+销毁"单任务。
3. **泵自续任务弱绑定**：AudioRendererImpl 的 pump_again 以 Unretained 入队，可与销毁
   任意交错（DecoderStream 内部早已全弱绑定，唯独渲染器自己的泵漏了）。改 WeakPtr 后
   该类任务在对象销毁后自动变空操作——这是对这一族竞态的**类级封口**，不依赖调用方顺序。

（另：`FakePipelineClient::OnTimedText` 持锁调 Record 自死锁，样例采样器抓的；已修。）

### 验证

- 新 `audio_two_subs.mkv`（1 音轨 + eng/chi 双 srt）+ 4 个管线级测试 × 重复 4 遍全过：
  未选轨零 cue 且音频正常 EOS、选轨后 cue 文本/顺序/pts/时长逐项断言、往复切换、
  非法索引干净失败。
- ffmpeg 439/439（+4）；no-ffmpeg 376/376；asan 439/439 连续 4 轮全绿；
  invariant 全过（280 文件）；headless 回归正常。
- 未做：WebVTT/ASS 样本覆盖（解码路径同型）、字幕样式信息透传（raw_ass 已带原文）、
  直播字幕的过期 cue 丢弃策略。

## 第十五轮（本轮）：Phase 3 硬解与零拷贝——NativeBuffer · VideoToolbox 全链 · 颜色空间

依据 avbase 升级计划 Phase 3（1–2 人 × 4 周的量，本轮落地的是其全部结构 + macOS 一条
实测链；Linux/Windows 的实测验收依赖对应硬件，留给有设备的环境）。

### 落地内容

| 计划项 | 实现 | 验证 |
|---|---|---|
| VideoFrame NativeBuffer 分支 | `NativeHandleKind{kVaapiSurface,kD3D11Texture,kCVPixelBuffer}` + `NativeHandle{kind,id,subresource}`（§6.2：消费者永不盲转）；`WrapNativeBuffer()` 带 `release_cb`（最后引用释放生产端后备存储）与 `to_i420_cb`；`ToI420()` 显式回读——owned I420 返回自身新引用，否则走生产端回读，无回读路径返回 nullptr | 6 个新单测：句柄类型、释放恰好一次、克隆引用共享句柄、无回读返回 null、回读可重复且不动原帧、owned I420 返回同帧 |
| FFmpeg 硬解路径 | `platform/ffmpeg/ffmpeg_hw_video_decoder.{h,cc}`：`av_hwdevice_ctx_create` + `avcodec_get_hw_config` + get_format 回调（拒答软格式——静默降级是谎报硬解）；hw 帧经 `WrapNativeBuffer` 直出，帧句柄按设备取自 `data[3]`（vt: CVPixelBufferRef / vaapi: VASurfaceID / d3d11: texture+subresource）；中途掉出 hw 路径即报错交给回退链，绝不冒充软解 | 本机实测：`pull_frames` 8/8 帧 `handle=cvpixelbuffer`；headless 全部测试媒体 kCompleted（硬解优先链下） |
| 显式回读 | `platform/ffmpeg/hw_frame_readback.cc`：`av_hwframe_transfer_data` + sws→I420，只在 `ToI420()` 背后被调 | 单测（回调机制）+ SDL2 sink 改造后可在真实播放中走通 |
| 平台工厂 | `platform/{videotoolbox,vaapi,d3d11}/` 各一个 factory target：capability `hardware=true, outputs_opaque_surface=true, priority=10`；codec 级掩码门（`CodecAllowedByMask`），被排除的 codec 是"not me"而非错误 | CMake 选项 `AVBASE_ENABLE_VIDEOTOOLBOX/VAAPI/D3D11`（第一个 Apple 默认 ON）；工厂返回 nullptr → DecoderStream 现成的 "declined" 路径 |
| 选择器接线 | `player_impl` 默认工厂列表：硬解工厂（按偏好）插在软解 FFmpeg 之前，宿主注入仍在最前；掩码过滤在工厂内（DecoderStream 对 null 解码器有现成的 fallback）；`hw_codecs` 默认值从 0 改为 kAll（0 意味着禁用全部硬解，与 kAuto 的"硬解优先"矛盾） | 既有 decoder_stream 回退单测 + 端到端 |
| 颜色空间兜底 | 新 `media/base/video_color_space.{h,cc}`：四轴类型 + `GuessColorSpaceFallback` 数据表（HDR→BT2020/PQ；≥600 行→BT.709；SD→SMPTE170M；每行整组指定）；`platform/ffmpeg/color_space_bridge` 从 AVFrame 映射，无标签时按**容器**信息兜底（风险 §14.4），软硬解路径统一接入 | 8 个新单测含边界行 599/600 与"永不半指定"不变量 |
| examples/pull_frames | 自定义 VideoRendererSink 捕获合成层输出帧并逐帧打印句柄类型——零拷贝取帧的活文档 | 本机实测 8 帧 GPU 直出 |

### 关键设计决定

1. **get_format 拒答软格式**：hw 解码器若在 get_format 接受软格式，FFmpeg 会静默降级软解
   而管线以为自己在硬解——谎报比失败更糟；失败交回退链，事件流里能看到。
2. **VAAPI/D3D11 本轮编译骨架 + 实测顺延**：解码器与工厂代码完整，但零拷贝**显示**链
   （VASurface→EGLImage、D3D11-GL interop）是 sink 侧工作，且验收要求对应硬件；解码侧
   的 GPU 传递对三个平台是同一套代码。
3. **SDL2 sink 显式回读**：SDL2 是 CPU blit，遇到不透明帧走 `ToI420()` 并告警一次——
   硬解播放因此保持正确（每帧一次回读），零拷贝显示路径出现后此处替换。
4. **`pull_frames` 不调 `ToI420()`**：示例的职责是证明零拷贝路径存在，回读反而模糊焦点。

### 与计划的偏差（登记）

- `platform/ffmpeg` 是唯一见 libav 的 target（C8），所以 FFmpeg-hwaccel 介质的硬解解码器
  必须住在那里；三个平台目录各持一个薄工厂 target 链接它。计划设想的
  "platform/vaapi/ 等目录装硬解"在原生（非 FFmpeg）解码出现前受 C8 约束。
- `DecoderPreference::kHardwareOnly` 的"宁败不回退"语义仍只在 Selector 层实现；
  生产接线按 kAuto/kHardwareFirst 排序，DecoderStream 的配置时重排序是 M9 后续项。

### 验证

- ffmpeg preset：430/430（新增 14 个）；no-ffmpeg：376/376（media/base 不依赖 FFmpeg）。
- `check_invariants` 全规则通过（276 文件），C23 基线 286 行未增长（新文件全部 ≤80 列）。
- asan 配置构建+测试（后台）。
- 本机 VideoToolbox 实测：`pull_frames` 输出 `handle=cvpixelbuffer`（零拷贝 GPU 帧到达
  合成层边界），headless e2e 全部通过（corrupt_header 按设计失败于 DecodeFailed）。
- 未跑：4K 硬解 CPU 占用对照、Linux VAAPI-EGL 与 Windows D3D11-GL interop 零拷贝显示链
  （无设备，Phase 3 验收的环境相关部分）。

## 第十四轮（本轮）：Phase 0 仓库改造——更名 avbase · LGPL 清点 · 文档合并

依据 avbase 升级计划（Phase 0，纯工程与法务，要求更名做成独立 commit）。

### 已完成

| 计划项 | 结果 |
|---|---|
| 更名 ijkpp → avbase | ✅ 独立 commit（`4c655fc`）。305 个文件、三种大小写变体（`ijkpp`×1805 / `Ijkpp`×42 / `IJKPP`×1239）。四个 cmake 模块 git mv。**"ijkplayer" 不含 "ijkpp" 子串，267 处上游指称原样保留**——溯源注释与 docs/05 的对照关系不受影响。git 历史全保留（无 squash）。宿主侧的仓库目录改名（GitHub repo rename）需要人工操作，内容层面已全部就位 |
| LICENSE | ✅ 第九轮已落 BSD-3 全文 + 第三方/衍生说明节，本轮仅随更名改抬头。README 声明一致 |
| CI 矩阵 | ✅ 第十轮已加 macOS 档（`macos-14`：no-ffmpeg + ffmpeg 两个 preset），计划项提前完成，无需改动 |
| 文档合并 | ✅ avbase_design.md §5/§7/§8 并入 [11-行为规范卷](11-行为规范卷.md)（线程表 / 背压级联与 seek 序列 / 三级水位表，标注为 Phase 1–2 的验收依据）；docs/05 顶部标注为历史卷（golden 对拍与常量溯源仍引用）；README 标题与定位改为"企业播放器基座"，补项目沿革段 |
| git 历史 | ✅ 更名 commit 与其余改动分离，历史未重写 |

### 更名的两个连带修复（都是"改名暴露既有事实"，不是新 bug）

1. **`ThreadTest.LongNameIsTruncatedNotRejected` 自相矛盾**：原期望截断产物
   `ijkpp-this-name`（15 字符，恰好等于 `kMaxThreadNameLen`）；前缀换成 6 字符的
   `avbase-` 后同样输入截断为 `avbase-this-nam`，与 `EXPECT_LE(15u)` 冲突。
   期望值改为新前缀下的 15 字符产物。
2. **C23 列宽棘轮**：`avbase` 比 `ijkpp` 宽一列，把 45 行顶过 80 列（检查器按字符计，
   中文注释不误伤）。全部手工收窄——按"动过的文件回归干净"的棘轮精神，而不是加基线；
   基线 310→286（-24）。

### LGPL 标记清点（计划项 3，"更名窗口内做完"）

| 文件 | 自述 | LICENSE 声明 | 结论 |
|---|---|---|---|
| `media/filters/legacy/*`（6 个） | 逐行移植 ff_ffplay.c，LGPL-2.1 头 + 隔离目录 + 阈值不可改 | LICENSE §1 逐一列名 | ✅ 一致，合规隔离成立 |
| `media/filters/ffmpeg_demuxer.cc` | "demux 循环的**形状**追随 read_thread()；seek flags 同" | LICENSE §3：全部 ffmpeg_* 为原创 BSD | ✅ 低风险：高层设计思路不受版权保护，seek flags 是 FFmpeg API 常量，无表达式复制 |
| `media/filters/video_renderer_impl.h` | 仅指路注释（"节拍算法住在 legacy/"） | 同上 | ✅ 无移植事实 |
| **`media/filters/ffmpeg_audio_decoder.{h,cc}`** | 头注释自称 **"Ported from ff_ffplay.c audio_decode_frame() (LGPL-2.1-or-later)"**，"restructured behind Chromium's AudioDecoder interface" | LICENSE §3 声明所有 `ffmpeg_*` 为**原创 BSD** | 🔴 **矛盾，待决策** |

🔴 项的两个事实方向冲突：代码结构（avcodec send/receive 解码器）追随的是
Chromium `FFmpegAudioDecoder`（BSD），与 ffplay `audio_decode_frame`（滤镜图取帧 +
swr 重采样的消费端胶水）结构上并不相似，"Ported" 措辞疑似**夸大**；但**头注释白纸黑字
的移植声明不能由写代码的人单方面抹掉**，而按项目自己的规则（legacy/README 准入：
"自认移植的文件应进隔离区"）把它搬进 legacy/ 又会打断"基座纯 BSD"的更名目标。
这是法律判断，本轮**不动代码、不动 LICENSE**，只登记矛盾。两条出路（均需法务/作者确认）：

- **A（推荐，若作者确认是措辞夸大）**：改写头注释为准确的出处描述（结构追随 Chromium，
  原创实现），LICENSE 不变，基座保持纯 BSD-3。
- **B（保守）**：文件搬入 `media/filters/legacy/` 并进 LICENSE §1 例外清单，声明为
  LGPL-2.1；代价是 §1 "隔离目录 = 全部衍生作品" 的清单式边界被稀释。

> **决策：A（同轮拍板）。** 作者确认当初的 "Ported" 是措辞夸大——该文件与 ffplay 的
> 关系是**角色对位**（PROGRESS 第八轮自己就记录了关键差异：输出编解码器原生格式、
> 转换留给消费者，与 `audio_decode_frame` 的回调线程 swr_convert 正相反），不是表达式
> 移植。头注释已改写为准确出处（结构追随 Chromium FFmpegAudioDecoder，原创实现，
> 附一句更名记录防回潮），LICENSE 不变，**基座自此为纯 BSD-3 + legacy/ 一处隔离区**。
> 全仓 `LGPL|Ported from` 扫描：`filters/` 下仅剩两处指向 legacy/ 的指路注释，
> 语义准确，保留。

### 连带登记：`renderer_impl.cc` 的 C1 豁免

更名跑 invariant 时暴露 `media/filters/renderer_impl.cc` 540 行 > 500 限——**分支既有
违规**（`daf6373` 起 512 行，饥饿信号各 commit 推到 540），与更名无关，CI 未及发现。
按 C2 豁免的先例登记进 `LINE_LIMIT_ALLOWLIST` 并写明理由（视频/音频两半互为镜像，
按行数硬拆只会复制生命周期状态机）；M9 BufferController 吸收
`CheckBufferingTransitions()` 时应回头重审。

### 验证

- no-ffmpeg preset 全量构建 + 376/376 测试通过（本机 macOS，GTest 经 Homebrew 补装——
  此前 287 个用例只在 Linux CI 跑过）。
- `check_invariants.py` 全规则通过（258 文件），C23 基线只降不升。
- FFmpeg 配置与 sanitizer 配置本机未跑（依赖 Homebrew ffmpeg/SDL2 环境较重），
  以 Linux CI 为准。

## 第十三轮（本轮）：DataSource 桥（M4 余项收口）+ 饥饿信号（M9 第三块）

| 组件 | 说明 | 备注 |
|---|---|---|
| `platform/ffmpeg/data_source_io.{h,cc}` | **DataSource → AVIOContext 桥**，M4 余项收口：内存缓冲 / 宿主 `DataSource` 经自定义 AVIOContext 进入 FFmpegDemuxer（kUri/kFd 仍走协议层）。桥自持字节位置——`avio_tell()` 是"FFmpeg 消费到哪"，不是"下一字节从哪来"，混用会毁掉每次 seek；读路径只用 `ReadBlocking`（D3 契约），demuxer 的 interrupt flag 短路阻塞读，Stop() 会 `Abort()` 源 | 2 端到端用例（探针/尺寸/时长过桥）；AVFMT_FLAG_CUSTOM_IO 下 `avformat_close_input` 不管 pb，自定义 pb 的正确终结符是 `avio_context_free`（`avio_closep` 会把 opaque 当 URLContext 释放）；`avio_alloc_context` 的缓冲归 FFmpeg 所有，必须 `av_malloc` |
| `renderer_impl` 饥饿信号 | `CheckBufferingTransitions()`（10ms 时钟节拍，S1）：所辖流全部干涸 → kHaveNothing，数据恢复 → kHaveEnough，只有边沿过界。**这是 BufferController 循环的第一个真实触发者**——此前 `OnBufferingStart` 没有任何调用方，三级 HWM 永远停在第一步 | 无新增用例（管线 seek 夹具的手动泵天然制造干涸边沿，409 全绿即回归证据）；防滞回（trickle 振荡）留待限速源用例观察 |
| `player` 侧 | `CreateDemuxer` 接受全部描述符种类；demuxer 内部对不支持的种类报可操作错误 | |

### 本轮补强：饥饿边沿的确定性 + 管线级断言

首版饥饿检测纯靠 10ms 采样，管线 seek 夹具证明它会漏掉短于采样周期的干涸窗口
（关键帧 seek 的边沿捕到了，精确 seek 的没捕到）。修正分两层：

- **Flush 即边沿**：flush 按定义清空所有队列，kHaveNothing 在 Flush 里同步发射，
  不与 demuxer 的回填竞速；恢复边沿仍由采样报告。
- **管线级断言**：`PipelineSeekTest` 两个用例现在都断言 seek 后
  kHaveNothing→kHaveEnough 边沿到达客户端——没有渲染器的饥饿信号，
  `have_nothing` 永远为假，facade 的 HWM 永远不前进（本断言就是防回归的哨兵）。
- 过程教训（同类第三次）：一次多段 python 补丁在中间 assert 失败时**整脚本不落盘**，
  重试脚本只补了报错之后的两段，漏掉了报错之前已"看起来成功"的第一段——
  `rendering_` 标志静默丢失，测试失败信息（have_nothing=false）与根因（标志未置位）
  相距十万八千里。**多段编辑必须每段独立落盘验证。**

### 遗留（M9 内，更新）

1. **饥饿竞态诊断结果（第十四轮闭合一半）**：(a) 精确 seek 的 kHaveEnough 丢失
   **根因已修**——`StartPlayingFrom` 重置 `starved_reported_`，把 Flush 刚发布的
   DRY 边沿状态吞掉；seek 重启后数据若先于首个 10ms tick 到达，RECOVER 永不发布。
   重置本身语义错误（起播时的干涸是合法的 kHaveNothing，ffplay 同义），移除后
   12/12 稳定；(b) 限速源用例 **已转正（第十四轮）**。两级修复：音频干涸判据撞上 WSOLA 残余
   OLA 窗口（960 帧）——地板修到四个周期仍不够；正解是**子渲染器自己发布
   starved 判决**（AudioRendererImpl 在设备回调里发布：短供/空供置位、满供清除，
   暂停不计入；RendererImpl 只做合并，不再猜测队列深度），事件驱动彻底取代采样，
   8/8 压力全稳。过程教训：Render 已被后续轮次重构出 DrainRing，补丁引用了
   不存在的 `requested` 变量——**构建失败时压测结果无效**（跑的是旧二进制），
   修复后必须重跑全部数字。
2. **限速源卡顿-恢复集成用例**：断言层已就位（边沿到达客户端），差的是
   FFmpeg+ThrottledDataSource 的夹具变体（进 media_ffmpeg_unittests）——
   FFmpegDemuxer(ThrottledDataSource(Memory(file))) + 假 sink 的管线级驱动，
   断言 kHaveNothing→kHaveEnough 的边沿与 hwm_step 递进。下一轮第一优先。
2. **RetryDataSource 已落地（第十四轮）**：`media/filters/retry_data_source.{h,cc}`——
   连续失败重试同一请求（成功即重置连败计数，间歇抖动不会累积成放弃），可中止的
   分片睡眠兜住 Δ15，异步 Read 走私有 worker 且绝不内联回调；4 个单测（恢复、
   预算耗尽、异步投递、Abort 及时解锁）走 media_unittests（无 FFmpeg 依赖）。
   剩 LiveDataSource / UrlRewriteInterceptor。
3. docs/07 §5 其余管线级断言。
4. 精确 seek 的 golden 验证等 M10/Q8。

---

## 第十二轮（本轮）：M9 精确 seek——丢弃窗口 · 到达检测 · SeekController

`SeekMode::kAccurate` 的 DoD（"精确 seek ±1 帧"）全链落地。此前它按桩返回警告并降级
关键帧 seek；本轮把它做成真实现。

### (1) 设计与接线

| 层 | 内容 |
|---|---|
| `media/base/renderer.h` | + `BeginAccurateSeek(target, reached_cb)` / `EndAccurateSeek()`（纯追加，默认 no-op，与 SetPaused 同例）。**窗口在 Flush() 中存活**（compositor 的 Flush 刻意不重置 accurate 状态），所以允许在关键帧 seek 之前开窗——解码重启时新世代已经被框住 |
| `renderer_impl_controls.cc` | RendererImpl 实现：S1 开窗（S3 打开 compositor 规则）、S6 呈现钩子 `OnVideoFramePresented` 同时喂视频时钟与目标检测（原子 target 只被观察，判定全在 S1）、**无视频流时立即回报 reached**（否则只能靠超时）；End 的每条路径（到达/超时/取代）都必须关窗，否则窗口会永远吃帧 |
| `pipeline_impl.{h,cc}` | 具体侧转发（不入冻结的 Pipeline 接口——Seek() 的"只做关键帧"契约保持精确，框帧策略属于 facade） |
| `player/seek_controller.{h,cc}` | 策略核（照 BufferController 的纯函数模式）：`Evaluate(reached, deadline, now)` 纯决策——**到达胜过过期**（期限瞬间落地是成功不是竞态）；单槽规则（新 seek 取代在等 seek，旧回调以 kAborted 完成而非悬空）；全部状态在媒体序列 |
| `player_impl.cc/events` | `SeekTo(kAccurate)`：登记目标 → S1 上 BeginAccurateWaitOnMedia（先取代旧的）→ pipeline Seek；`OnMediaSeekDone` 对 accurate 请求**不完成**（显示还在追赶）；到达 hop / 超时任务汇入 `CompleteAccurateSeek`：先关窗（帧恢复流动）再发事件——kAccurateSeekCompleted + kSeekCompleted（result 携带 kTimeout，Δ10 可观测），用户回调以 Ok/kTimeout 收尾 |

### (2) 验证

```
✅ seek_controller_unittest 5 例（到达胜过期、期限边界、Begin/End 身份、取代覆盖）
✅ 管线级 AccurateSeekPresentsNothingBeforeTheTarget：从开窗起**没有任何 < 目标帧的
   帧到达显示**（比关键帧 seek 测试更强——那只能约束上界），reached 回调必须触发
✅ headless --seek 1.5 --accurate（真实 FFmpeg）：落地后首个采样 1.52s（关键帧落在
   1.33s 之前），继续播到 kCompleted；--seek 1.4 → 1.42s
✅ 407/407（FFmpeg）· 372/372（no-ffmpeg）· invariants 252 文件全过
```

### (3) 发现与修正

- `End()` 之后读 `request_id()/target()` 拿到的是重置值——先取值再 End（首版两处顺序
  反了，编译期发现不了，单测的 ReplacingAWaitReplacesItsIdentity 钉住）。
- 取代路径首版漏发用户回调（只清表）——"新 seek 取代旧的"必须以 kAborted 回答，
  与 demuxer 层被取代 seek 的契约同形。

### (4) 遗留（接第十一轮的 M9 清单）

1. ~~SeekController 精确 seek~~ → **本轮完成**。金色验证（与原版对齐）仍等 M10/Q8。
2. `RetryDataSource` / `LiveDataSource` / `UrlRewriteInterceptor`（故障注入假件已就绪）。
3. **管线级 HWM 集成用例的前置缺口（本轮确认）**：ThrottledDataSource 是
   `media::DataSource`，而 `DataSource → demuxer 的 AVIOContext 桥`（M4 余项）未建，
   合成 demuxer 也不收 DataSource——用例要等桥落地；更深一层，`kHaveNothing` 饥饿信号
   还没有从 renderer 端到端接线（BufferController 的 OnBufferingStart 目前没有真实触发者）。
   桥 + 信号两者是下一轮的第一优先。
4. docs/07 §5 其余管线级断言（丢帧数、变速、故障注入、循环、纯音/纯视频）。

---

## 第十一轮（本轮）：M9 开篇——限速假件 · 三级 HWM · 管线级 seek 夹具

M9（缓冲 + Seek 完整版）的前两步，加上上一轮收尾的测试基建：

| 组件 | 说明 | 备注 |
|---|---|---|
| `tests/unit/media_filters/pipeline_seek_unittest.cc` · `tests/support/{fake_sink_factories,fake_pipeline_client}` | 管线级 seek 契约（docs/07 §5 首条）："SeekTo(5s) 后落地帧 ∈ 150–152" + "落地后旧世代不再出现"；`DefaultRendererFactory` 组装 + S1/S3/S4 三条真线程 + 手动泵双 sink | 压力 100/100、`ctest -j8` 4 轮 349/349；连带抓出 #51/#52。确定性规则：等待有界单向 · 渲染只由泵驱动 · 泵轮节奏落后媒体节奏（16ms 睡眠让外推时钟超前，落地帧漂到 155–158）· Play 等 kHaveMetadata |
| `tests/support/throttled_data_source.{h,cc}` | M9 的测量仪器：限速供数（墙钟预算 + 封顶突发）+ `FailFrom/ClearFailure` 故障注入 + `Abort()` 及时解锁 + `bytes_served/stalls` 观测 | 7 用例。设计陷阱：默认突发 = 1s × 速率，小于速率的文件在突发内跑完、节流器形同虚设 → `set_max_burst_bytes()` 旋钮 |
| `player/buffer_controller.{h,cc}` | 三级 HWM 决策核心（照 `DecideNextFrame` 纯函数模式）：`first`(100ms) 起播 · `next`(500ms) 首次恢复 · `last`(4s) 后续 + 上限；每完成一个缓冲周期升一级，**seek 完成回卷到 first**（新位置对旧缓冲一无所知） | 10 用例。挂接 `PlayerImpl`（媒体序列，`SEQUENCE_CHECKER` 免锁）：`kBufferingEnded` 载荷携带本周期实际等待的 mark；缓冲期间统计 tick 发 `kBufferingProgress`；`OnMediaSeekDone` 执行重置 |

**TSan 浸泡与三个真缺陷**（#51/#52/#53，详见 §(3) 表）：30 分钟 232 轮 0 报告（修 #51 前第 68 轮 SEGV）。

### (5) 遗留（M9 内）

1. `SeekController` 精确 seek ±1 帧（`HoldReads` 世代机制为地基，待做）。
2. `RetryDataSource` / `LiveDataSource` / `UrlRewriteInterceptor`（故障注入假件已就绪）。
3. 管线级 HWM 集成用例：限速源下观察 `hwm_step` 递进与起播/恢复判定（DoD"限速 50KB/s 下
   卡顿-恢复循环正常"），假件已就绪、用例未写。
4. docs/07 §5 其余管线级断言（丢帧数、变速、故障注入、循环、纯音/纯视频）。

---

## 第十轮：播放链路打通

本轮把"能构建、能测"推进到"能播放"。新增实现约 2,900 行：`PipelineImpl`（管线编排）、
`DefaultRendererFactory`、media 层 null 双 sink、`FFmpegVideo/AudioDecoderFactory`、
player 层 `PlayerImpl`/`StateMachine`/`EventHub`，以及 platform/sdl2 双后端（M11 的
代码提前落地）与两个示例。**验证边界**：macOS 本机（Xcode 工具链）；Linux 真机
X11/Wayland 未验证；Golden Test 未做（M10/Q8 未动）。

### (1) 新增组件

| 组件 | 说明 |
|---|---|
| `media/filters/pipeline_impl.{h,cc}` · `pipeline_impl_host.cc` | Pipeline 的唯一实现：demuxer 先于 renderer 初始化（并行会让 renderer 拿到零条流）、seek 为"renderer flush ∥ demuxer seek"两阶段完成、Stop 按文档 §10.1 顺序销毁。GetMediaTime 直读共享的 AvSyncController（seqlock），renderer 销毁后仍可安全应答。**C1 拆两个 TU**：`pipeline_impl_host.cc` 承接 stream-id 查询、M9 轨选桩与 Demuxer::Host / RendererClient 回调 |
| `media/renderers/default_renderer_factory.{h,cc}` | 把 Deps 的 sink/decoder 工厂组装成 RendererImpl；sink 工厂缺省时回落 media 层 null sink——零配置 headless 播放的落点 |
| `media/filters/null_{video,audio}_sink.{h,cc}` | 放在 media/filters 而非 platform/null（Chromium 的 media/video/null_video_sink 先例）：headless 是框架能力不是平台能力。null 音频 sink 真实按设备周期消费（音频时钟才能前进），null 视频 sink 以 60Hz 驱动 compositor |
| `media/filters/ffmpeg_decoder_factories.{h,cc}` | 软解工厂（.cc 进 platform_ffmpeg target），排在注入的硬解工厂之后（Δ12 回落链的尾巴） |
| `player/player_impl.{h,cc}` · `player_impl_events.cc` · `state_machine.{h,cc}` · `event_hub.{h,cc}` | 门面实现：S1/S3/S4 三线程 + 事件线程、PlayerState 转移表（非法转移拒绝并给出可操作错误）、seek 按 request_id 匹配回调、loop 循环播放、~Player 有界等待（Δ15）。**C1 拆两个 TU**：`player_impl_events.cc` 承接 seek 完成、运行时控制、观察者回调与状态访问器 |
| `platform/sdl2/`（M11 提前） | Sdl2VideoSink（宿主持窗口/renderer，sink 线程独占绘制 YUV 纹理）+ Sdl2AudioSink（SDL 音频回调只做 planar→interleaved 拷贝，Δ13）+ `cmake/FindSDL2.cmake`。宿主窗口嵌入模式（surface.h 的两条线程规则） |
| `examples/headless.cc` · `play_sdl2.cc` | headless 支持 --seek/--rate/--timeout，是 CI e2e 的雏形；play_sdl2 约 150 行（docs/10 的"main 行数 = API 易用性"度量）——C2 把参数解析、建窗与事件泵移出 `main()`，承载该度量的是文件而不是 main 本身 |

### (2) 对既有文件的修改（全部有理由）

| 文件 | 修改 | 理由 |
|---|---|---|
| `media/base/demuxer.h` | `Demuxer : public MediaResource` | media_resource.h gap 1 预言的改动；RendererImpl::Initialize 需要 MediaResource 视图，而无 RTTI 无法跨无关基类转换 |
| `media/base/renderer.h` | + `SetPaused`/`SetOutputTarget`（默认 no-op，实现移出 .h）；`media/base/pipeline.h` + `Play`/`Pause`/`SetOutputTarget` | Chromium Pipeline 本有 Play/Pause（DRAFT 时被裁掉）；暂停不能走 `SetPlaybackRate(0)`——AudioRendererAlgorithm 明确拒绝 rate 0 |
| `renderer_impl.{h,cc}` | 暂停/换窗转发、外部时钟锚定、视频时钟喂入（Render 回调调 `OnVideoFramePresented`——此前**无人喂视频时钟**，纯视频流永远没有有效主时钟）、EOS 传播（CheckForEnded + 周期复查）、init 回调改为成员持有并 hop 到 S1、Flush 完成回调 hop、dtor 在子渲染器自己的 sequence 上销毁它们 | 逐条见 §(3) 的 bug 列表。**C1 拆两个 TU**：`renderer_impl_controls.cc` 承接启动后的控制面；**C2**：`Initialize()` 的子渲染器装配移到 `CreateSubRenderers()` |
| `audio_renderer_ring.cc` | **C1 第二次触顶后拆 TU**：`PreStretch()`/`DrainRing()`（把算法帧搬进就绪环、设备再从环里取）搬到新 TU，`OutputFramesToMediaTime()` 提为私有静态跨 TU 共用。缝是真的：这里全在 S4，`Render()` 留在原地跑在 S7，两者只共享 `handoff_lock_` 下的环 | 拆后主文件 513 → 422 行 |
| `audio_renderer_impl.{h,cc}` | `SetPaused`（设备路径门控）、`kOk+null`（流排空）标记 EOS、消费方唤醒；取环循环移到 `DrainRing()`（C2） | #36/#41/#42 |
| `video_renderer_impl.{h,cc}` | 自然 EOS（kOk+null）此前被当作"无事发生"无限泵；帧呈现回调；消费方唤醒 | #36 |
| `decoder_stream.cc` | demuxer 读回调整 hop 到自身 sequence | #40 |
| `ffmpeg_demuxer.{h,cc}` | open 时安装 log bridge（否则 FFmpeg 失败原因不可见）；网络专用选项按 URI 协议门控（reconnect*/user_agent/headers 传给本地文件只会变成 Δ2 的"未消费选项"噪音） | |
| `player/player.cc` | 改为纯转发到 PlayerImpl；RunUntilIdle/StepOnce/TakeSnapshot/UpdateConfig/SelectTrack/ReconnectNow 保持 kNotImplemented 并指明里程碑（M9+） | |
| `base/synchronization/lock.h` | `Lock` 补 `CAPABILITY("mutex")`、`AutoLock` 补 `SCOPED_LOCKABLE` + `EXCLUSIVE_LOCK_FUNCTION`；新增 `EXCLUSIVE_LOCKS_REQUIRED` 宏 | 注解此前不成立：`GUARDED_BY` 从未真正生效，且每处 `GUARDED_BY` 自己又产生一条诊断 |
| `cmake/AvbaseCompilerFlags.cmake` | `-Wuseless-cast` 移入 GCC 专有列表 | clang 报 unknown warning option，`-Werror` 下 debug 预设无法编译 |
| 24 个文件的头注释 | 修复第十轮脚本把新段落插进旧句子中间留下的断句、重复的 `(promoted from DRAFT, tenth round)` 与重复空注释行 | 纯重排与合并，除重复片段外未改字面 |
| `.github/workflows/ci.yml` · `CMakePresets.json` | 骨架 job 变真：新增 `ffmpeg`（发行版 FFmpeg，359 用例）、`e2e-headless`（5 个样本播到结束）、`sdl2-build`（Linux 上编译 SDL2 后端）；新增 `ffmpeg` 预设，`linux-sdl2` 补上 FFmpeg | 带 FFmpeg 的配置与端到端此前从未在 CI 跑过（`ffmpeg-matrix` / `e2e-linux` 一直是 `if: false`）；`linux-sdl2` 因缺 FFmpeg 连 `play_sdl2` 都建不出来 |
| `base/synchronization/lock.cc` · `tests/unit/base/synchronization_unittest.cc` | `ObservedOrder()` 补 `thread_local`；新增 `LockTest.ConcurrentOrderRecordingIsRaceFree` | #44 |
| `tests/support/synthetic_demuxer.{h,cc}` | 合成源（M10 的第一片，docs/07 §5）：10s/30fps/48kHz 的可脚本化 `Demuxer`，**编码即契约**——每个包的负载前 4 字节是它自己的序号（`ReadIndexPayload`），时间戳按序号整数算出（不是累加帧时长，所以第 150 帧恰好是 5.000s），音频音高 = 440 + 该包所落的整秒，关键帧每 30 帧一个，`StartPlayingFrom()` 落在"不晚于请求"的关键帧上、回传实际位置并 **bump 流 serial**（新世代，`demuxer_stream.h` 的契约，也是 #50 的判据） | 自测 6 例（序号/节奏/关键帧/seek 落点/音频秒边界/EOS/媒体信息）全过且耗时 **0 ms**；帧号画进像素与故障注入（`fail_read_at_packet` 等）**故意未实现**——没有消费方的钩子只会让替身开始说谎 |
| `tests/support/*` · 三个渲染器套件（`renderer_impl` · `video_renderer_impl` · `audio_renderer_impl`） | 渲染器直接单测：脚本化输入、可编排解码器、手动拉动的双 sink、记录型 renderer client（`renderer_client.h` 里点名"not written yet"的那个）。14 个用例覆盖启动契约（只报一次、绝不内联、视频 sink 在解码器就绪时开、音频设备在 StartPlayingFrom 才开）、结束契约（双流排空 + 尾帧必须发布）、暂停门控（视频不出新帧；音频报静音且不计 underrun）、以及 flush 后的串号隔离（两侧都不许放行旧 serial 的帧） | 第十轮的 12 个渲染器 bug 全靠端到端发现；这套件当场抓出 #47/#48/#49，三处产品修复随它一起进 |
| `tests/unit/media_filters/pipeline_seek_unittest.cc` · `tests/support/{fake_sink_factories,fake_pipeline_client}.{h,cc}` | 管线级 seek 契约（docs/07 §5 首条）："SeekTo(5s) 后落地帧 ∈ 150–152"+"落地后旧世代不再出现"，`DefaultRendererFactory` 组装 + S1/S3/S4 三条真线程 + 手动泵双 sink，注入点就是 `Pipeline::Start()` 的 demuxer 参数（产品代码零改动） | 压力 100/100；连带抓出 #51/#52。等待全部有界单向、渲染只由泵驱动，是它和被撤回首版的全部差别 |
| `tools/check_invariants.py` | 门禁收敛：17 条 → **7 条结构规则**（C1 文件规模 · C4/C5/C22 分层 · C23 列宽棘轮 · C24/C25 构建覆盖），删 10 条风格类（C18 抛异常/C20 命名空间失衡本就是编译器必报；C7/C9/C14/C17/C21 是 taste；C2 函数长度、C8 版本守卫、C11 目录配对按"违规即藏缺陷"标准不再保留）。**规则 ID 不重编**，历史可 grep | 文件 680 → 509 行；探针验证：被砍规则不再报、游离 `.cc` 仍被 C25 拒绝（exit 1）；docs/06 §9 与 docs/07 检查单同步 |
| 10 个 `CMakeLists.txt` · `cmake/AvbaseCheckInvariants.cmake` · `tools/check_invariants.py` | 源文件列举定成一条规则（docs/06 §7.6）：目录成员 == 目标成员处改用 `file(GLOB ... CONFIGURE_DEPENDS)`（`media/base` · `media/renderers` · `media/filters/legacy` · `player` · `platform/{ffmpeg,sdl2}` · `tools/inspect` · `tests/unit/{base,media_base,player}`），其余四处（`media/filters` · `tests/unit/media_filters` · `base` · `examples`）保持显式并在文件里写明理由。**新增门禁 C25**：每个 `.cc` 必须被某个目标覆盖（显式列表，或规则自己展开的 glob），否则非零退出 | 新增文件不必再改 CMake，Ninja 在构建时重跑 glob（`[0/N] Re-checking globbed directories...`）。C24 管"列出的文件存在"，C25 管"存在的文件被编译"——两个方向都不再静默。`aux_source_directory` 明确不用：不递归，且新增文件不触发 CMake 重配（CMake 官方文档警示的正是这一点）。实测 108 个 (target, source) 与改动前逐一相同 |

### (3) 本轮抓到的 bug（接通播放 = 第一条真正跑全链路的路径，收获很大）

| # | 问题 | 性质 |
|---|---|---|
| 35 | `PlayerStateMachine::TransitionTo` 只校验不赋值——状态机形同虚设，每次调用都停在 kIdle | 🔴 实现 bug（本轮新写） |
| 36 | 视频"自然 EOS"（kOk+null）被当作无事发生、无限泵；音频流排空（kOk+null）同样未标记 ended → 播完永不 kCompleted | 🔴 两个子渲染器各一个 |
| 37 | RendererImpl 的 init 回调经视频路径传递，video 先完成时闭包随返回栈帧销毁 → pipeline 永远等不到 ready | 🔴 回调生命周期 |
| 38 | init 回调在最后完成的子渲染器线程（S4）内联执行，PipelineImpl 的 sequence checker 当场击毙——checker 第一次抓住真问题 | 🔴 跨 sequence |
| 39 | RendererImpl dtor 在 S1 销毁子渲染器，DecoderStream 的 checker 拒绝 → 销毁必须发生在各自 sequence（post+Wait，外部由 Δ15 兜底） | 🔴 销毁顺序 |
| 40 | `DemuxerStream::Read` 回调直达 DecoderStream（S1→S3）：不只是 DCHECK 违规，`pending_reads_`/`decoded_outputs_` 是真实数据竞争 | 🔴 数据竞争 |
| 41 | 解码泵内联递归：DecoderStream 缓存同步交付时每帧一层栈，真实文件直接栈溢出（SIGBUS on S4） | 🔴 |
| 42 | 泵在背压处停止后无人唤醒（消费方清空压力没有人通知生产者）——ffplay 用 condvar，这里用 Render 回调里的 post | 🔴 逻辑缺口 |
| 43 | RendererImpl::Flush 给音频路径传空 closure，AudioRendererImpl::Flush 无条件 Run → "null callback" CHECK | 🟠 |
| 44 | 锁序检查器的"已观测顺序"表是**所有线程共享**的 `std::vector`（旁边那张 held 栈写了 `thread_local`，这张漏了），而记录发生在取锁之前 → 并发 `push_back` 直接踩坏堆。表现：`headless` 在 6 路并发下约 1/36 次**静默** `SIGABRT`/`SIGTRAP`（`libmalloc: pointer being freed was not allocated`，栈落 `TaskQueue::PostDelayedTaskImpl` → `AutoLock` → `AssertAcquiredInOrder`），播放本身看不出任何异常 | 🔴 数据竞争（**早于第十轮**，由"把 CI e2e 变成真 job"时的抖动调查发现） |
| 44 | RendererImpl::Flush 完成回调从 S3 直达 PipelineImpl | 🔴 跨 sequence |
| 45 | PipelineImpl 并行初始化 demuxer 和 renderer，后者拿到零条流报 kMissingDemuxerStreams（首跑的"StreamNotFound"假象） | 🔴 编排顺序 |
| 46 | **macOS 移植四件**：根目录 `VERSION` 文件在大小写不敏感 FS 上遮蔽 libc++ 的 `<version>`（改名 VERSION.txt）；`pthread_setname_np` 平台差异（统一截断 15 字符）；FindFFmpeg 的 pkg-config 分支 include 目录经 `PkgConfig::` 中转后丢失（改为从 PC_* 变量直构）；`avbase-inspect` 因 FFmpeg PRIVATE 链接拿不到头（显式链接） | 🟠 平台 |
| 50 | **seek 后新世代一帧都到不了显示**：`RendererImpl::Flush` 用 `av_sync_->master_serial()`（**seek 前**的音频时钟 serial）去 flush 视频解码流，而真实 demuxer 的队列在 seek 时会 bump serial（`DecoderBufferQueue::Flush()`，注释写明"这就是关键"）——于是解码流按上一代过滤，把新世代的包全部丢弃，seek 之后视频永久停摆。headless 端到端看不见：null sink 不数帧，kCompleted 只要求 EOS | 🔴 编排缺陷（合成源 + 管线级夹具发现；修法：子渲染器在 `StartPlayingFrom`（demuxer 已完成 seek）采纳新世代，走 `DecoderStream::AdoptSerial()`；先试过用 `Flush()` 采纳——它会以 kDecodingAborted 收掉在途读、泵不再被重新武装，真实 seek 3/4 挂） |
| 47 | **init 回调内联**：`RendererImpl::Initialize` 的缺流错误路径直接 `std::move(init_cb).Run(...)`，违反 renderer.h 的"绝不内联、调用方可在回调里销毁状态"——调用方会在 `Initialize()` 还在栈上时被重入 | 🔴 契约违背（新单测发现；已改为与成功路径同一条 hop） |
| 48 | **音频 EOS 尾帧永不发布**：`MarkEndOfStream()` 只翻标志不搬帧，而 `PreStretch()` 只在 `OnDecoderOutput` 与"恢复暂停"时调用。解码器一次输出的帧数大于设备周期时，EOS 到达那一刻环是满的 → 泵因背压停摆、此后无人搬运 → `buffered_frames()` 永不归零 → `CheckForEnded()` 永不报 `OnEnded`（"播完了但不结束"）。设备周期与解码粒度相同时不触发，这正是端到端一直没遇到它的原因 | 🔴 逻辑缺口（新单测发现；修法：`PumpDecoder()` 在 `ended_` 后仍搬运一次尾帧） |
| 49 | **初始化上报跨 sequence**：`OnVideoInitialized`/`OnAudioInitialized` 由子渲染器在 S3/S4 上调用，却直接写 S1 的 `video_initialized_`/`audio_initialized_` 并互相读。第十轮只给 "ended" 与 init 完成两条路径加了 hop，这条漏了 | 🔴 数据竞争（TSan 在真线程夹具下报出；已加 hop） |
| 51 | **析构后时钟推送链继续跑**：`RendererImpl::PushMasterClock`/`PushStatistics` 两条 10 ms 自续定时链用 `Unretained(this)` 绑定，对象销毁后队列里残留的任务照常执行，读到已释放的 `Clock` → SEGV。TSan 浸泡第 68 轮抓到（崩在 `~TaskEnvironment` 收尾的 `RunUntilIdle`，普通构建下是无征兆的 UAF） | 🔴 生命周期（修法：两条链与首次定时都改绑 `WeakPtrFactory`，失效任务由绑定层跳过；成员置于末位并注明） |
| 52 | **flush 与重启的窗口烧掉整个新世代**：`VideoRendererImpl::Flush` 写入的是过期 serial（`av_sync_->master_serial()`），此后到 `StartPlayingFrom` 采纳之间，自由运行的消费泵驱动 `DecoderStream` 继续读 demuxer——此时 demuxer 已 bump 到新 serial，于是新世代每个包都按"stale"丢弃，而 EOS 包按契约不过滤序列 → `end_of_stream_` 直接置位：流还没开始就结束了，"ended" 提前、落地帧永不出现。实测约 3% 的管线级 seek 命中（插桩日志：连续 `stale drop buf_serial=1 mine=0` 后紧跟 `EOS buffer accepted, mine=0`） | 🔴 编排缺陷（管线级夹具压力 100 轮发现；修法：`DecoderStream::HoldReads()`——flush 合闸、`StartPlayingFrom` 采纳后开闸，两侧子渲染器同样处理） |
| 53 | **统计路径跨序列读未保护状态**：`RendererImpl::GetStatistics()`（S1）经 `buffered_frames()` 读 `AudioFrameQueue::frames_`，与 S4 的 `SeekFrames`/`Append` 并发。该队列"单序列无锁"是有意设计（头文件注记：音频回调 100 µs 预算不允许加锁），破坏契约的是统计读这一侧 | 🟠 数据竞争（TSan 浸泡第 1 轮报出；修法：`frames_` 改 `std::atomic<int>`——不加锁、保留单序列设计，读方 relaxed） |

### (4) 验证结果（macOS 24.5 arm64 / AppleClang 21 / Homebrew FFmpeg 7.1.1 / SDL2 2.32.6）

```
✅ headless: small_h264_aac_3s.mp4  → completed at 2.99s（对照 ffprobe 3.000s）
✅ headless --seek 1.5             → seek ok → 续播 → completed at 2.11s
✅ headless: audio_only.m4a        → completed at 0.71s（纯音频路径）
✅ headless: video_only.mp4        → completed at 1.00s（纯视频路径，视频时钟经
                                      Render 回调喂入后主时钟正确回退）
✅ headless: truncated_tail.mp4    → completed at 2.88s（截断文件优雅播到 EOF）
✅ corrupt_header.mp4              → prepare 失败，错误含 DecodeFailed/建议（不崩溃）
✅ play_sdl2（真窗口 + 音频）       → 播完 kCompleted，退出码 0
✅ ctest: 373/373（mac 配置）· 338/338（no-ffmpeg）——第十轮末尾各 +1：锁序竞态回归用例（#44）；
   渲染器直接单测再 +14（RendererImpl 6 · VideoRendererImpl 3 · AudioRendererImpl 5）
✅ TSan：三个渲染器套件 14 用例在 `tsan` 预设下 **0 报告**（此前该路径报出 #49 与
   假 sink 自身的 3 处竞争，两者都已修）；**30 分钟浸泡**（渲染器三套件 + 管线级 seek
   连续循环）修 #51 前在第 68 轮 SEGV、修后完整跑完 **0 报告**；TSan 下
   `headless --seek 1.5` 连跑 **20 次全部 exit=0**
✅ 严格告警配置覆盖 FFmpeg 层：`debug`（Debug + 严格告警 + `-Werror` + FFmpeg）实测
   **0 警告**；此前该预设沿用 `AVBASE_ENABLE_FFMPEG=OFF`，FFmpeg 适配层
   （`platform/ffmpeg/*` 与 `media/filters/ffmpeg_*.cc`）从未进过严格门禁。
   合成源是这条门禁扩宽后写下的第一批代码：同样 0 警告
✅ 合成源自测 6 例 + 合成解码器 4 例（348/348 · 383/383 内含），耗时 **0 ms**
✅ 真实 seek 稳定性：修 #50 后 `headless --seek 1.5` 连跑 **6 次全部 exit=0**；而用
   `Flush()` 采纳世代的那一版连跑 4 次里 3 次 exit=1
✅ 管线级 seek 夹具（重建版）：压力 **100/100**，`ctest -j 8` 全量并行 4 轮
   **349/349 ×4**；首轮无闸门版本 100 轮 18 失败（泵速失配：音频每轮只推 5.3 ms 媒体
   时间而显示间隔 16 ms，落地帧永远"未到期"），对齐泵速后仍 3/100——那 3 次就是 #52
✅ 伪证检查：临时撤掉 #48 的修复后，`EndedPublishesTheTailTheRingCouldNotTake` 与
   `EndedIsReportedAfterBothStreamsDrain` 双双报红，恢复后全绿——两条结束用例确实咬住了
   那个缺陷，而不是恰好通过
✅ check_invariants 全过（220 文件）；C23 列宽基线 323 → 310（净减 13 行，棘轮只降不升）；
   新增 C25（源文件必须被某个目标覆盖）后复跑全过，且从零 configure 的构建目录同样 324/324
✅ 零警告（编译器 + 链接器）：RelWithDebInfo 的 no-ffmpeg / FFmpeg+SDL2 与
   Debug + 严格告警 + `-Werror` 三套配置实测。这份成绩单在第十轮末尾一度是虚的：
   `-Wthread-safety` 报的 660 条里有 642 条源于 `base::Lock` 缺 capability 注解，
   `-Wuseless-cast` 又是 GCC 专有选项却无条件传给了 clang（`-Werror` 下 debug 预设
   一行都编不过）。两处根因修好后 660 → 0，中途暴露的 18 条真问题逐条修掉。
```

### (5) 本轮未做 / 遗留

1. ~~**管线级 seek 断言：夹具稳定化**~~ → **已落地（重建版）**。`Pipeline::Start()` 接收一个
   demuxer（冻结签名就是为此），媒体层不改产品代码即可注入合成源。重建版与被撤回的首版差在
   四条确定性规则：**每个等待有界且单向**（轮询必须到达的状态，超时打印完整事件日志）；**渲染
   只由手动泵驱动**（没有自由跑的时钟可竞速）；**泵轮对齐节奏**（每轮 3 个音频周期 ≈ 16 ms
   媒体时间 = 一个显示间隔——首轮 18% 失败正是失配所致）；**Play 等的是 kHaveMetadata**
   （`OnDurationChange` 在 kReady 之前到达，用它触发 Play 会打在 kStarting 上被吞掉）。
   稳定后当场抓出 **#51/#52**（见 §(3)）。`tests/unit/media_filters/pipeline_seek_unittest.cc`
   （1 例）+ `tests/support/{fake_sink_factories,fake_pipeline_client}` 已进库。又补两条夹具
   层的教训，都在 TSan/压力下现形：**音频假 sink 的拉动必须封送回渲染器序列**（S4）——内联在
   gtest 主线程跑渲染回调会让渲染器状态被两个 sequence 同时触碰（TSan 报
   `SyntheticDemuxer::MakeAudioPacket` 竞争；`FakeAudioSink::set_render_runner` 即为此而设，
   RendererImpl 风格的套件单泵无需启用）；**泵轮的真实节奏要落后于媒体节奏**——音频时钟在两次
   消耗回报之间按挂钟外推，轮内睡 16 ms（真实 ≈25 ms）会让外推时钟超前，落地帧漂到 155–158
   被判晚丢弃；改成 4 ms 后时钟滞后（落后只会 hold，安全），100/100；**假件必须像真件一样
   线程安全**——`HoldReads()` 让两条流首次真正并发读取后，TSan 当场抓住 `SyntheticDemuxer`
   的共享游标在 S3/S4 竞争（真 demuxer 内部有锁，替身不能在这条契约上撒谎），已按真件语义
   加锁。
   仍欠：
   docs/07 §5 其余条目（丢帧数、变速、故障注入、循环、纯音/纯视频），以及 S1 泵依赖
   `TaskEnvironment` 的说明——管线任务全走 post，"只睡不泵"的夹具会拿着空事件日志干等。
   夹具形状备忘不变：`RendererImpl` 套件必须用真线程（析构 post 到 S3/S4 后阻塞等待），
   两个子渲染器套件单线程即可。
2. 首播 seek 曾触发一次 NAL 损坏（改为首播不重复寻址后消失）——根因未深究，
   真实 seek 已验证干净，但 seek 后串号隔离的黄金验证要等 M10。
3. `RunUntilIdle`（需要 message_pump_epoll，R2 降级债）、精确 seek（M9
   SeekController）、轨选切换、快照、UpdateConfig 白名单、`option_registry.inc`
   接线（A10 仍 9/66）——全部按桩返回 kNotImplemented 并指明里程碑。
4. SDL2 后端现在**在 CI 的 Linux 上编译**（`sdl2-build` job），但"跑起来"仍只有
   macOS：Linux 真机 X11/Wayland 与 `e2e-linux`（xvfb + llvmpipe + 双后端）留到
   M11/M12。带 FFmpeg 的 359 用例与 headless 端到端已进 CI（`ffmpeg` / `e2e-headless`
   两个 job）；覆盖率门禁仍欠。
5. macOS 首次构建暴露的文档失真已按本轮实测修正 README/BUILDING 的对应行。
6. C1/C2 清理把三个实现文件各拆成两个 TU（`pipeline_impl_host` · `renderer_impl_controls`
   · `player_impl_events`），并把音频取环、渲染器装配与两个示例的 `main` 抽成辅助函数。
   一处代价记在这里：`pipeline_impl_host.cc` 的 `OnDemuxerError`/`OnError`/`OnEnded` 各写
   一次 `state_`/`playing_`，与 M8 那条"具体状态机应与它驱动的 Pipeline 同 TU"的理由
   相反；M9 重排时把这三处收回 `pipeline_impl.cc` 即可闭合。
7. 警告门禁修好之后 `-Wthread-safety` 才开始说真话：它抓出 7 处 `state_lock_` 保护成员
   在锁外读写（`PlayerImpl` 的 `PrepareAsync` 两处、`source_`、`display_` 三处、
   `CreateDemuxer` 一处），以及 `FFmpegDemuxerStream::FulfilPendingReadLocked` 这类
   "约定持锁却没写注解"的助手。两件都已完成（该助手现在写着 `EXCLUSIVE_LOCKS_REQUIRED`）。
   **顺带查出一个门禁空洞并修掉**：严格告警 + `-Werror` 只在 `debug` 预设里，而它沿用
   `AVBASE_ENABLE_FFMPEG` 的默认 OFF——FFmpeg 适配层从未被严格配置编过，所以"零警告"
   只覆盖了不含 FFmpeg 的那半棵树，而那一层恰恰是 `-Wthread-safety` 最常说话的地方之一。
   `debug` 现在打开 FFmpeg（实测 Debug + 严格告警 + `-Werror` + FFmpeg：0 警告、
   373/373 通过），理由写进 BUILDING §3.3。**strict job 已进 CI**（`strict`，clang 编译，
   从零 configure + 构建 + `ctest --preset debug` 384/384 且 DCHECK 开启——这条配置此前
   从未跑过完整测试）：用 clang 而非发行版 GCC 是有意的，严格集会连带打开 GCC 专有的
   `-Wuseless-cast`，该旗标从未对本树实测过，按 R12"不要上线一门必红的门禁"，GCC 半边
   留给能在本地实测的人（job 注释写明了缘由）。
8. glob 改的是"新增文件不用改 CMake"，代价落在 DRAFT 的放法上（docs/06 §7.6）：留在一个
   被 glob 目录里的 DRAFT `.cc` 会被编译，所以它要待在没有任何 glob 能到达的子目录里
   （glob 不递归）直到能编译为止，并在 `DRAFT_FILES` 里登记。显式列表的四个目录不受此限。


---

> **历史轮记录(第 1–9 轮、早期专题)**已移至
> [archive/PROGRESS-rounds-1-9.md](archive/PROGRESS-rounds-1-9.md)——那里的教训
> 仍然有效(34 个编号 bug 的成因都在轮次记录里),只是不再属于"当前状态"。
> 里程碑级的未完成清单见下表。

> **本表为第九轮快照,条目级的最新去向以 [12-剩余工作清单](12-剩余工作清单.md) 为准**;
> 里程碑级的进度对照仍见下表(下次里程碑收口时整体重写)。

## 未完成（按里程碑）

> ⚠️ **本表与下方"工具与门禁现状"表是第九轮快照，仅作里程碑对照的起点**——
> M7/M8/M11 与 M9 大部分已在第十至十七轮完成（见文首状态块与各轮记录），
> 两表待下一次里程碑收口时整体重写。
> **第九轮修订**：本表此前混着几轮的历史行，同一条 M4/M5 既出现在 ✅ 行也出现在
> ⬜ 行（"**M4 ← 下一个关键路径** ⬜"与上面"M4 完成"直接矛盾），读者无法判断真实
> 进度。本轮按当前代码状态重写一遍；各轮的过程记录仍保留在上文对应小节里。

| M | 内容 | 状态 |
|---|---|---|
| ✅ M0 | 工程基建（CMake / Presets / flags / FindFFmpeg / CI） | 完成 |
| ✅ M1 | `base/` 核心件 | 完成，**但已落到 R2 的 L1+L2 降级**（见上文"(4) R2 降级债"） |
| ✅ M2 | `base/task/*` + `base/threading/*` + `TaskEnvironment` | 完成（L2：`TaskQueue` 驱动，无 `message_pump_epoll`） |
| ⬜ M1+ | `base/` 补充件：`feature_list` · `circular_deque` · `trace_event` · `strings` · `files`（含 `ScopedLibrary`，**M12 dlopen 弱依赖的前置**）· `LockOrderChecker`（**R5 应对⑤的前置**） | 非关键路径，但后两项已被 M12/M8 依赖 |
| ✅ M3 | `media/base/` 值类型与两个队列 + `AudioBuffer` + `decoder_config` | 完成 |
| ⬜ M3 余项 | `AudioRendererAlgorithm`（WSOLA） | **单点阻塞 M7 的音频半边** |
| ✅ M4 | `platform/ffmpeg/` + `FFmpegDemuxer` + `ffmpeg_glue` + 5 个测试媒体 | 完成，12 个端到端测试对真实媒体全绿 |
| ⬜ M4 余项 | `DataSource` 后端的 `AVIOContext` 桥（内存 / fd / 自定义源） | ⬜（`avbase-inspect probe` 已在第八轮完成） |
| ✅ M5 | `DecoderStream<Traits>` + `FFmpegVideoDecoder` / `FFmpegAudioDecoder` + `DecoderSelector` | 完成 |
| ✅ M6 | `AvSyncController`（seqlock）· `VideoFrameCompositor` · `Clock` | 完成（第九轮移入 `media/filters/legacy/`） |
| ⬜ M6 余项 | `DisplayGeometry`（letterbox / SAR / DPI，docs/09 §4.4） | ⬜ M12 需要 |
| ⬜ **M7** | `VideoRendererImpl` · `AudioRendererImpl` · `RendererImpl` · `TextRenderer` · `media/audio/*` · WSOLA | ⬜ **下一个关键路径**；接口头第九轮已 DRAFT 冻结 |
| ⬜ M8 | `pipeline_impl` · `player_impl` · `state_machine` · `event_hub` · `seek_controller` · `buffer_controller` · `diagnostics` | ⬜ SDK 的 12 个头已冻结；**`media/base/` 的 7 个管线接口头第九轮已 DRAFT 冻结**；`player.cc` 的 10 个方法仍返回 `kNotImplemented` |
| ⬜ M8 余项 | `tools/gen_options.py` + `option_registry.inc` | ⬜ **`OptionRegistry` 现仅 8 个 key / 规划 60+**，直接卡住验收标准 A10 与 docs/10 Level 3 迁移路径 |
| ⬜ M9 | 三级 HWM `BufferController` · 精确 seek · `RetryDataSource` · `LiveDataSource` · `UrlRewriteInterceptor` | ⬜ |
| ⬜ M10 | `platform/null` · `tools/ijkplayer-recorder` · 15 份 golden JSONL · `golden_record/diff.py` · `tolerance.yaml` · `synthetic_demuxer` | ⬜ **被开放问题 Q8（golden 基线锁哪个 ijkplayer 版本）卡住** |
| ⬜ **M11** | `platform/sdl2`（5 文件）· `FindLinuxMediaDeps.cmake` · `examples/play_sdl2` · `tools/verify_e2e.py` · CI `e2e-linux` | ⬜ **Linux 首次出画**；`platform/CMakeLists.txt` 里该开关目前仍是 `FATAL_ERROR` |
| ⬜ **M12** | `platform/linux/`：`gl/` 8 文件 · `window/` 4 文件（含 Wayland 协议代码生成）· `audio/` ALSA+Pulse · `zero_copy/` dmabuf · dlopen loader（11 个库）· `play_native` + `play_embed` | ⬜ 单项最大（~3800 行 / 3 周） |
| ⬜ M13 | Fuzz ×5（24h）· Stress ×11（含 10 小时 long_play）· Bench ×16 + 趋势守护 · 覆盖率门禁 · 9 份 SDK 文档 · cpack/vcpkg/conan · V1–V10 · 28 项质量门禁 → **发布 0.1.0** | ⬜ 另需补上 R2 降级债（docs/08 说"M13 后补齐"，但 M13 checklist 里没有这一项） |
| ⏳ M14–M18 | VAAPI + 完整 HDR · C ABI · Android · iOS · `ijkio` 缓存 | 后置增量（共 11 周） |
| 🚫 不做 | DRM/CDM · libass 排版 · `ijkavformat` patch 族 · `SDL_VoutOverlay` · SoundTouch · Abseil/spdlog/fmt · Mojo/blink/cc/viz · Java/OC UI · Windows/macOS 出画 | docs/08 §4，净减 ~9000 行 |

**工具与门禁现状**（第九轮实测）：

| 项 | 状态 |
|---|---|
| `tools/check_invariants.py` | ✅ 14 条规则，173 文件全通过；本轮更新 `LINE_LIMIT_ALLOWLIST` 的 legacy 路径 |
| `tools/extract_constants.py` | ✅ **本轮新增**，26 个条目；`--selftest` 两方校验 24 项一致 + 1 项 Δ4 声明偏离 + 1 项待 M9；`--ijkplayer` 三方校验已用合成 ffplay 源码树验证（含 0.04→0.4 手抄错、宏缺失、fork 差异三种失败模式） |
| `tools/inspect/`（`avbase-inspect`） | ✅ probe / decode / sync；⬜ doctor / play / dump / golden |
| `tools/gen_options.py` · `golden_record.py` · `golden_diff.py` · `verify_e2e.py` · `build_linux.sh` · `ijkplayer-recorder/` | ⬜ 全部未建 |
| CI | ✅ quick + full 矩阵（本轮已把 `extract_constants --selftest` 加进两个 job）；⬜ `ffmpeg-matrix` 与 `e2e-linux` 仍是 `if: false`（前者注释写 "enabled at M4"，**M4 早已完成**）；⬜ coverage job 是 `lcov --summary \|\| true`，不会 fail；⬜ `check-format` / `check-cpplint` / `check-no-vendor-leak` |
| 许可证 | ✅ **本轮落地**：根 `LICENSE`（BSD-3 + 4 节第三方说明）· `media/filters/legacy/`（LGPL-2.1 全文 + README 准入规则 + 6 个文件头重写）；⬜ **法务确认（Q1/R8）仍未做** |
| 仓库卫生 | ⬜ `.clang-tidy` · `.editorconfig` 仍缺（STYLE.md 与 README §8 都列了） |

---

## 第二十三轮：清掉两个 park 项，接上一个从未接线的配置字段，开两条门禁

本轮没有新功能里程碑。主题是**把"已完成"和"已接线"这两件事分开看**：四个条目里，
两个是修真实的缺陷，一个是补上一条从未有任何生产调用者的配置字段，两个是开门禁。
其中两项目标没有达成，理由都写在下面而不是绕过去。

### (1) 4.8b RendererImplTest 偶发挂起 —— 根因确定，15004ms → 12ms

此前记为"时序敏感、两次全量一红一绿"。**它可以确定性复现**：单跑
`StartPlayingFromOpensTheAudioDevice` 是 475 ms，排在 `InitCallbackIsNeverRunInline`
之后是 15004 ms——恰好是 `DestroyRenderer` 的 15 s 上限。

根因在测试侧：`DestroyRenderer()` 等 `!env_.HasDelayedTasks()`，而渲染器的 10 ms
时钟推链与 1 s 统计链在 `ended_` 为真之前无限重排。该用例不到 EOS，所以队列永不
静默，只能耗满上限。套件顺序决定前一个用例是否把 sink 排空，于是表现为"时序敏感"。

修法两层：产品侧把 `RendererImpl` 投递到 S1 的 6 处 hop（ended×2 / init×2 /
CheckBufferingTransitions / OnAccurateSeekTargetReached）由 `Unretained(this)` 改为
weak 绑定——`~RendererImpl` 不再依赖"所有在飞 hop 都已跑完"这个调用方无法建立的性质；
测试侧改为有界 drain。**套件整体 7131 ms → 78 ms。**

**伪证检验的结论值得留着**：只回退测试侧的 15 s 等待（产品修复保留）→ 15 s 复现；
只回退产品侧 weak 绑定（保留短 teardown）→ 套件仍全绿。即 weak 绑定是**结构性封口**，
这套件抓不到它，不要把它当回归锚点。

### (2) 4.8 throttle 测试 —— 修掉两个真缺陷，并定位到真正的阻塞

重新启用后暴露两个共享夹具里的真实缺陷：

- **use-after-free**：`PipelineTestFixture::StartPipeline()` 无条件重载
  `media_bytes_`，而调用方已经基于 `media_bytes_.data()` 造好了
  `MemoryDataSource`。只有 throttle 套件会包 source，所以只有它炸——FFmpeg 报 46 次
  连续 "Invalid data found when processing input"，被误读成节拍问题，实际是解码器
  拿到了**已释放内存**。两处改成"仅在为空时加载"。
- **数据竞争**：`FakePipelineClient::have_nothing()` / `have_enough()` 无锁读标志，
  而其他访问器都持锁。

**测试仍保持 DISABLED**，但记录的理由换成了真的那个：节拍恢复循环需要"比消费慢到
会饿死、又比消费快到能恢复"的源，而 testdata 里最长的素材只有 3 s / 327 KB
（≈109 KB/s 实时）表达不了两者。>110 KB/s 永不饿死（实测 `bytes_served=371692`、
3 s 素材上 `media_time=60s`）；=60 KB/s 永久饿死。解封前置：需 ≥30 s 素材。
实时节拍泵 `PumpRoundRealtime` 与字节/停顿诊断已落地并保留。

### (3) 2.3 kHardwareOnly 生产语义 —— 清单记的还轻了

docs/12 §2.3 记的是"宁败不回退只在 Selector 层实现，DecoderStream 需要配置时重排"。
实测更严重：**`DecoderSelector::SelectVideoDecoder` 没有任何生产调用者**（全仓只有
它自己和单测）。也就是说 `config.video.decoder_preference` 在播放路径上无人读——
文档告诉用户它能用，实际 `kHardwareOnly` 会静默回退到软解。比"未实现"更糟。

排序点选在 `VideoRendererImpl::Initialize`（首次拿到流 config 的地方），而不是工厂
列表的拥有者：排序是"流 config × 各工厂能力"的函数，提前排就等于猜 config，那正是
要替换的 ijkplayer 病根。链路 PlayerImpl → DefaultRendererFactory::Deps →
RendererImpl::Deps → `set_decoder_preference`。空候选表改为带可操作日志的初始化失败，
而不是一个永不产帧的解码器。宿主自带工厂列表且未设 preference 时原样保留。

6 个新测试。伪证：禁用排序 → 4 个偏好专属用例红，2 个"未配置行为"（kAuto 回退、
宿主顺序保留）仍绿——说明它们咬的是排序本身而非周边管线。

### (4) 2.2 合成直播 demuxer —— 边缘真的在动

`LiveDataSource`（第十九轮）供给的是**增长的字节**，但没有任何东西供给**增长的
时戳**。`SyntheticDemuxer` 能报 `IsLive()` 并把有限 duration 当边缘，但它的时戳是
包索引的纯函数——一个跑在真实时间前面的消费方会永远跑在前面，而这正是直播管线
绝不能进入、也正是追帧逻辑要纠正的状态。

`SyntheticLiveDemuxer` 补上三件有限源做不到的事：边缘=墙钟流逝（`media_info().duration`
每次查询重算，且 `duration_is_estimate` 明说）；产包受时钟约束（读不会越过边缘）；
边缘处的读**挂起**（`DemuxerStream` 契约是 1..count，"还没有"无法用空回复表达）。

三个非显然决定都写进了注释：包从 index 0 起（不从 `start_offset` 起，否则滞后被完全
掩盖）；seek 被**拒绝**并给可操作错误（追帧正因为没有物理 seek 而存在，默默接受 seek
的假件会正好掩盖追帧要防的 bug）；挂起上限用**真实时钟**度量（防挂起不能依赖测试可控
的时钟——首版正是如此，结果"从不推进 mock 时钟"的用例会永久挂起，而那恰恰是上限要防
的失败本身）。

9 个用例。伪证：冻结边缘（退化为有限源）→ 4 个依赖边缘的用例红，5 个无关契约的仍绿。

### (5) 4.7 format 门禁 —— 先修树，再开门

按 R12 路径：一次性 `clang-format -i` 全量重排（排除 `media/filters/legacy/` LGPL
隔离区与生成的 `option_registry.inc`）→ 开门禁。

STYLE.md 旧记录里"不做全量重排"的理由是"那 323 行里有一部分是模板声明，手工重排
有可能改变含义，而收益只有行更短"。**机器重排把前半句直接推翻**（不改语义，只重新
折行），而后半句严重低估了收益：**C23 基线 286 → 37 行**（少 249 行），4K diff 一次性
付清，之后每个 PR 不再为格式付费。

两个副作用如实处理：`ffmpeg_demuxer.cc` 与 `audio_renderer_algorithm.cc` 因折行涨行
触及 C1，上限随之棘轮化。`check-format` job 检查**全树**而非 diff——diff-only 更便宜
也更容易绕开，而本项的意义正是".clang-format 是唯一真相源"。

### (6) 4.5 Windows CI —— job 已加，范围按"能守住的说法"划定

R12 风险 3 提到的 MSVC 分支**早已存在**（`/GR- /EHs-c-` vs `-fno-exceptions
-fno-rtti`），缺的只是 job。而 job 必须诚实地划范围，因为**没人用 MSVC 编译过这棵树**：
job 断言的是"能编译、测试通过"（no-ffmpeg 配置），**刻意不开 /WX**——给新平台一把
最严的警告换来的是一片红，埋掉唯一有用的信号"这套工具链到底能不能用"。

CI 的命令形式（`cmake --preset no-ffmpeg -B <dir>`）已本地验证；**能否编译通过只有
首次运行才能回答**，而那需要 push。

### (7) 4.6 覆盖率门禁 —— 脚本就绪，**门禁未接**

`tools/check_coverage.py` 已写好：按 docs/07 §12 逐模块逐指标判定，`video_frame_compositor`
与 `av_sync_controller` 要求 100% 行覆盖，core 整体 85/80，且**空 tracefile 直接判失败**
（读了空文件就通过，比没有门禁更糟）。

**没有接成门禁**，因为阈值从未在本代码库实测过（本机无 lcov，coverage 预设也没产出
可执行文件）。抄规格的阈值和实测的阈值看起来一样，但只有后者能让人在接门禁那一刻
就知道结果——否则要么首次运行就红（R12 说的"上线即红"，会教人忽略门禁），要么红了
以后当场没法解释为什么。**待一次能产出 tracefile 的环境实测后再接。**

### (8) 6.2 直播字幕过期 —— 实现通了，测试夹具没通

策略端到端接通：`SubtitleConfig::live_cue_max_age`（默认 10 s，0 = 关闭）→
`PipelineImpl::SetLiveCueMaxAge` → `Renderer::SetSourceLiveness` → 字幕泵的
`IsCueStale`。过期 cue **丢弃**而非前移对齐——前移会把属于过去的台词贴到现在播。

liveness 只能"下达"不能"发现"：`MediaResource` 刻意只发流、不暴露容器（否则每个假件
都要改），渲染器问不到。管线是唯一知道的地方（它已经在算
`seekable_ = info.seekable && !info.is_live`），于是在那一点声明一次。

**5 个测试全部 DISABLED，这是本轮最该看的部分**：实现通了，夹具没通。字幕腿在当前
夹具里送不出 cue，于是 4 个"应当送达 cue"的用例失败，而"应当丢弃 cue"的那个
**空洞地绿了**——因为 0 个 cue 也满足"结果为空"。交这种绿等于给一个从未真正执行过的
策略背书，比不交测试更糟，所以全部标 DISABLED 并在文件头写清状态与已查明的事实
（`text_stream_` 在 `OnTracksChanged(kText)` 里设、**不是** `Initialize()`；字幕泵由
`StartPlayingFrom` 启动——漏掉任一个就什么都观察不到，空洞绿正是这样被发现的）。

另有一个真实交互：端到端版本（走 `PipelineImpl` + 直播源）会**挂死**——demux 循环挂在
直播源里，而那条序列正是唯一能推进边缘的东西。已记入 `live_cue_expiry_unittest.cc`。

### (8b) 第二十三轮后半：视频轨切换闭合，以及三条错误假设

后半场最值得记的不是做了什么，而是**排除了什么**。

**kVideo 轨切换（4.1）** 从"未接线"做到"可切换且切换后画面恢复"，但连续三个提交都在
追同一个症状（"切换后一帧不出"），前两个假设都是错的：

1. "替换渲染器没拿到 `SetMasterClock`" —— 读代码排除；
2. "demux 循环卡在背压等待" —— **实测排除**，而排除它才是关键：探针显示替换渲染器的
   解码泵**只启动一次**、Read 从未完成；demux 循环若卡死**正是这个现象**，所以它没卡；
3. **真因**：一行日志 `DemuxLoop EXIT ret=-541478725` = `AVERROR_EOF`——**4 秒素材在
   交接完成前就读完了**。新流其实已被解复用 100 个包，但都在切换改向前抵达、作为非活动
   流被丢弃，**切换无流可切**。换 60 秒素材即通过。

**交接本身一直是好的**，前三个提交都在修一个没坏的产品。教训很窄：症状说"新东西什么
都不产出"，与"源读完了"**同样吻合**，而区分它们只需要 demux 循环退出处**一行日志**——
那是最便宜的测量，我不是第一个做的。

顺带修掉两个**确实成立**的缺陷：`SetActiveStream` 现在会 `resume_event_.Signal()`
（此前背压等待只被 Flush/Stop/seek 唤醒，轨切换不在其中），且等待中重检
`IsActiveRoutingTarget`——消费者已走的队列永远排空不了，等在它上面本身就是缺陷。

**合成源节拍（4.3b 的前置）**：`SyntheticSpec::paced` + `SyntheticDemuxer::set_tick_clock()`
把产包绑定到墙钟、边缘处挂起。此前不限速时**解码器多快给多快**，媒体时钟跑到墙钟前面
（实测 2.5s 墙钟走 3.56s 媒体），合成器**正确地**只呈现到期帧，于是测试数到 30 帧
而非 300，误判成"管线丢帧"。这与 throttle 套件是同一条教训的两端。

**本轮反复出现的一个模式，值得单独记**：**"新东西不产出"和"源头已经结束"症状完全
一致**，而我至少三次是靠猜、而不是靠源头的一行状态去区分。4.1 花三个提交，4.3b 花
两个，都是同一个错。

### (9) 验证结果（macOS 24.5 arm64 / AppleClang 21 / Homebrew FFmpeg 7.1.1）

- `ffmpeg` 配置：**476/476**（较本轮开始的 406 增 70）
- `no-ffmpeg` 配置：**372/372**
- `asan`：干净（`--preset asan` 完整重建后验证）
- `asan`：**476/476** · `no-ffmpeg`：**372/372**
- `check_invariants`：**all rules pass (302 files)**，C23 基线由 286 降至 37
- `clang-format --dry-run --Werror`：全树 0 偏离

### (10) 本轮未做 / 遗留

| 项 | 状态与原因 |
|---|---|
| 4.6 覆盖率门禁 | 脚本就绪，门禁未接——阈值需实测（本机无 lcov） |
| 6.2 字幕过期测试 | 夹具送不出 cue，5 个用例 DISABLED；策略本身已接线 |
| 4.8 throttle 测试 | 仍 DISABLED，阻塞是**素材长度**（需 ≥30 s）而非夹具 |
| 3.1/3.2 零拷贝显示 | 需 VAAPI / D3D11 硬件，本机无法验证 |
| 4.1 kVideo 轨切换 | ✅ 本轮闭合（见 (8b)） |
| 4.3 corpus 扩充 / 4.4 soak 48h | 本轮未开始 |
| `check-cpplint` · `check-clang-tidy` | 仍关闭：各自没做修树轮，一次开两个门禁就是制造红板 |
| Windows MSVC 是否真能编译 | job 已加但**未运行**，需 push 后由 CI 回答 |


