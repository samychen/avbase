# 实施进度（活文档，每个里程碑结束时更新）

> 设计文档：[README](../README.md) ｜ 里程碑定义：[08 实施路线图](08-实施路线图与风险.md)

## 当前状态：**M0 ✅ · M1 ✅ · M2 ✅ · M3 ✅ · M4 ✅ · M5 音视频 ✅ · M6 ✅ · DecoderStream ✅ · inspect CLI ✅ · M8 契约 ✅**

最后更新：2026-09-29（第九轮）—— **工程治理轮：LICENSE 落地 + LGPL 隔离目录 · `tools/extract_constants.py`（R1 应对④「禁止手抄」）· `media/base/` 七个管线接口头冻结为 DRAFT**
> 第八轮：音频解码链打通 · `DecoderStream<Traits>` 泛型化 · `ijkpp-inspect` CLI 落地；CLI 首跑即抓到 bug #32（主时钟被 uptime 偏移）与 bug #33（水位线未生效）

## 第九轮（本轮）：许可证落地 · 阈值提取工具 · 管线接口冻结

本轮不增加播放能力，专门收三类**非功能性缺口**：合规、可验证性、以及 M7/M8 的并行前置。

### (1) LICENSE 与 LGPL 隔离目录 — ✅ 完成（D10 / R8 落地）

此前 **166 个源文件头都写着 "governed by a BSD-style license that can be found
in the LICENSE file"，而仓库里没有 LICENSE 文件**；同时 `video_frame_compositor` /
`av_sync_controller` / `clock` 是 `ff_ffplay.c` 的逐行移植（LGPL-2.1 衍生作品），
却挂着 BSD 头。R8 的应对①要求"移植算法的文件保留 LGPL-2.1 头 + ijkplayer 版权
声明，归入 `media/filters/legacy/` 子目录"，本轮执行：

| 动作 | 结果 |
|---|---|
| 新增根 `LICENSE` | BSD-3-Clause 全文 + 4 节第三方/衍生说明（legacy LGPL · base/ 镜像 Chromium BSD-3 · platform/ffmpeg 适配层与系统 FFmpeg 的许可证边界 · testdata 为 lavfi 合成）+ 维护者注记（Q1 未决） |
| 新建 `media/filters/legacy/` | `git mv` 三对文件（compositor / av_sync / clock，共 6 个），**保留 git 历史** |
| 重写 6 个文件头 | LGPL-2.1-or-later 通知 + Zhang Rui / Bilibili / Fabrice Bellard / ijkpp Authors 四方版权 + **"为什么这个文件是 LGPL 而不是 BSD-3"** + 指回 `legacy/README.md`；原有 `ALGORITHM PROVENANCE` 段完整保留 |
| 新增 `legacy/LICENSE.LGPL-2.1` | LGPL-2.1 全文 501 行（取自 gnu.org，§0–§16 齐全） |
| 新增 `legacy/README.md` | 目录内清单 + **准入/禁入规则**（按"算法出处"而非"新旧/难度"判定）+ 动态链接与静态链接两种情形下集成方的义务 + 若必须纯 BSD 的应急路径（R8：重新独立推导，+2 周） |
| 更新 `media/CMakeLists.txt` | 三个 .cc 改为 `filters/legacy/` 路径，并注明隔离原因 |
| 更新 7 处 include | `legacy/*.cc`、`legacy/av_sync_controller.h`、2 个单测、`tools/inspect/inspect_sync.cc` |
| 更新 3 个头文件卫士 | `IJKPP_MEDIA_FILTERS_*_H_` → `IJKPP_MEDIA_FILTERS_LEGACY_*_H_`（路径与卫士一致是既有约定） |
| 更新 `tools/check_invariants.py` | `LINE_LIMIT_ALLOWLIST` 的 compositor 键改为 legacy 路径，理由里补注隔离说明 |

**验证**：`python3 tools/check_invariants.py --root .` → **all rules pass (166 files scanned)**。
文件数与第八轮一致（6 个文件是移动不是新增；新增的是 3 个非源码文件）。

**已知遗留**：`docs/01`–`docs/08` 与 `STYLE.md` 里共 22 处仍写
`media/filters/video_frame_compositor.cc` 等旧路径。这些是 **v2.0 已评审的设计文档**，
本轮刻意**不做批量改写**——设计文档是历史决策记录，事后改路径会让"当时决定了什么"
失真。**以 PROGRESS 为准**；下次设计文档整体改版（v2.1）时一并同步，并在
`docs/02` 的目录树里补 `legacy/`。

### (2) `tools/extract_constants.py` — ✅ 完成（R1 应对④）

R1（音视频同步移植出错，P4×I5=**20**，风险登记册里唯一"可能让项目失败"的风险）
的应对④原文是：**"阈值用 `extract_constants.py` 提取，禁止手抄"**。而实际情况是：
`av_sync_controller.h` / `video_frame_compositor.h` / `media_constants.h` 里的阈值
**全部是手抄的**，且 `media_constants.h` 的头注释自己就写着 "must be regenerated
by tools/extract_constants.py, never retyped by hand" —— 工具此前并不存在。

本轮补齐，19 个常量的**三方交叉校验**：

```
ffplay 源码 (#define)  ⟷  ijkpp 代码 (constexpr / 默认成员初始化器 / 尾注释)  ⟷  docs/05 表 7
```

- `--ijkplayer <path>`：从 `ff_ffplay_def.h` / `ff_ffplay_options.h` / `ff_ffplay.c` /
  `ff_ffplay.h` 递归解析 `#define`（支持 `15*1024*1024` 这类乘积与宏引用宏），
  再解析 ijkpp 侧三种写法，逐项比对，不一致即 **exit 1**
- `--selftest`：不需要 ijkplayer 源码树即可跑——用内嵌的合成 `#define` 片段验证
  解析器本身（递归展开、乘积、`_MS`/`_US` 单位换算、十六进制），并跑
  "ijkpp ⟷ docs/05 表 7" 两方校验。**这是 CI 可以无条件启用的那一半**
- `--json`：机器可读输出，供后续 `tools/golden_diff.py` 与 CI 消费
- `--emit`：生成 `media_constants.h` 的常量块（M13 收口时用生成物替换手写值）

覆盖：`MAX_QUEUE_SIZE` · `MIN/DEFAULT/MAX_MIN_FRAMES` ·
`VIDEO_PICTURE_QUEUE_SIZE_DEFAULT/MIN/MAX` · `AUDIO/SUBPICTURE_QUEUE_SIZE` ·
`DEFAULT_{FIRST,NEXT,LAST}_HIGH_WATER_MARK_IN_MS` ·
`BUFFERING_CHECK_PER_MILLISECONDS` · `BUFFERING_UPDATE_PER_MILLISECONDS`(Δ4) ·
`MAX_ACCURATE_SEEK_TIMEOUT` · `AV_SYNC_THRESHOLD_MIN/MAX` · `AV_NOSYNC_THRESHOLD` ·
`AV_SYNC_FRAMEDUP_THRESHOLD` · `AV_DIFF_AVG_NB/COEF` · `AV_DIFF_THRESHOLD` ·
`SAMPLE_CORRECTION_PERCENT_MAX` · `MAX_SLEEP`。

> ⚠️ **待真机验证**：本轮在**没有 ijkplayer 源码树**的环境里完成，因此
> `--ijkplayer` 路径尚未对真实 `ff_ffplay_def.h` 跑过；`--selftest` 已跑通。
> 下一步第一件事就是在有源码的机器上跑一次 `--ijkplayer`，
> docs/05 表 7 的注记本身就警告过"个别项（`DEFAULT_MIN_FRAMES`、`BUFFERING_*`）
> 在不同 fork 中有差异"——那正是要抓的东西。

#### 落地形态与验证（含负向测试）

拆成两个文件，与 `check_invariants.py`（374 行）保持同一量级：

| 文件 | 行数 | 职责 |
|---|---|---|
| `tools/ported_constants.py` | 222 | **数据**：`Spec` 类 · 26 条 `SPEC` 表 · 单位常量与 `convert()`/`canonicalise()` · 自测用的合成 `#define` 与期望值 |
| `tools/extract_constants.py` | 547 | **逻辑**：C `#define` 解析器 · ijkpp 三种写法的扫描器 · docs/05 表 7 解析 · 三方比对 · 报告 / `--json` / `--emit` · `--selftest` |

拆分的理由写在 `ported_constants.py` 的头注释里：这张表是**每个里程碑都会长**的
部分（M9 加缓冲常量、M12 加显示常量），而"审计 A/V 同步阈值是否仍是 ffplay 的原值"
这件事应该能一个文件从上读完，不必趟过解析器。

**开发过程中修掉的 4 个自己的 bug**（都不是设计问题，是实现问题，但都会导致
"工具静默给出错误的通过"，比工具不存在更糟）：

| # | bug | 后果（若未发现） |
|---|---|---|
| a | 单位换算比写反（`/ UNITS_PER_SECOND[to] * [from]`） | `base::Seconds(5)` 被算成 0.005 ms，所有时间常量全部 MISMATCH |
| b | 用"是不是非整数浮点"猜源单位 | **`AV_NOSYNC_THRESHOLD 10.0` 是整数值的秒**，被当成 10 ms → 静默通过一个 1000× 的错误。这是本轮最重要的一次自我纠错：猜单位的启发式在 ffplay 上根本不成立，改为 `Spec.source_unit` 显式声明 |
| c | 取字段初始化器时只匹配到第一个数字 | `size_t max_bytes{15 * 1024 * 1024}` 被读成 **15 字节**，而 `media_constants.h` 那一份是 15728640 → 两份不一致却被判为一致 |
| d | 结构体作用域名把导出宏当成名字 | 全部记成 `IJKPP_PLAYER_EXPORT::first_high_water_mark`，`BufferConfig::*` 一个都查不到，6 项假性 NOT_IN_IJKPP |

**验证**（正向 + 三种失败模式）：

```
$ tools/extract_constants.py --selftest
  → exit 0 · 24 项一致 · 1 项 Δ4 声明偏离 · 1 项待 M9（BUFFERING_CHECK_*）
$ tools/extract_constants.py --ijkplayer <合成的 ffplay 源码树>
  → exit 0 · 三方全部一致（ffplay ⟷ ijkpp ⟷ docs/05 表 7）
# 负向 1：把 AV_SYNC_THRESHOLD_MIN 从 0.04 改成 0.4（最典型的手抄错）
  → AV_SYNC_THRESHOLD_MIN  ms  ffplay=400  ijkpp=40  docs/05=40  MISMATCH · exit 1
# 负向 2：删掉 MAX_SLEEP 宏
  → MAX_SLEEP  us  ffplay=-  ijkpp=1e+06  NOT_IN_FFP · exit 1
# 负向 3：把 DEFAULT_MIN_FRAMES 改成 2（模拟 fork 差异）
  → DEFAULT_MIN_FRAMES  n  ffplay=2  ijkpp=5  docs/05=5  MISMATCH · exit 1
$ --json  → 26 行结构化输出（供 golden_diff.py / CI 注解消费）
$ --emit  → 28 行带出处的注释块（M13 用它替换 media_constants.h 的手写值）
```

**已知局限（写在这里，免得被当成保证）**：
- `--selftest` 里的合成 `#define` 是**上游 ffplay / ijkplayer 的常见值**，不是从
  某个具体 commit 提取的。真正权威的校验必须跑 `--ijkplayer`，而**本轮环境里没有
  ijkplayer 源码树，该路径只用合成树验证过**。
- docs/05 表 7 的解析是**故意宽松**的：一行里写 `VIDEO_PICTURE_QUEUE_SIZE_DEFAULT/MIN/MAX`
  时只有 `..._DEFAULT` 在反引号里，`MIN`/`MAX` 提取不到，会记为
  "macro absent from docs/05 table 7" 的 note 而不是失败。多宏共用一行（`3 / 2 / 16`）
  时判为"歧义行"，只记 note。宁可漏报也不误报，与 `check_invariants.py` 的
  保守取向一致。
- 只认 3 种 ijkpp 写法（`inline constexpr` / 默认成员初始化器 / 尾注释标注宏名）。
  若有人把阈值改成局部 `const double` 或函数参数默认值，工具会报 NOT_IN_IJKPP
  而不是静默通过——这是想要的行为。

#### 顺带修的两处仓库卫生

- `.gitignore` 缺 `__pycache__/` 与 `*.pyc`（项目有两个 Python 工具，却没有忽略
  它们的字节码）→ 已补。
- CI 的 quick 与 full 两个 job 都加了
  `python3 tools/extract_constants.py --root . --selftest`——它不需要 ijkplayer
  源码树，因此可以无条件进门禁。

### (3) `media/base/` 七个管线接口头 — ✅ 冻结为 DRAFT（M7/M8 解锁）

M7（Renderer 三件套）与 M8（Pipeline + Player 接线）此前**无法并行启动**，因为
`Renderer` / `Pipeline` 这些接口头一个都不存在，而 docs/03 §6 已经把签名写到了方法级。
本轮按第五轮 `ffmpeg_demuxer` 用过并被证明有效的 DRAFT 流程把它们落地：

| 新增（`media/base/`） | 内容 |
|---|---|
| `pipeline_status.h` | `PipelineStatus` 枚举 + `PipelineStatusCallback` |
| `media_resource.h` | `MediaResource`（Demuxer 与 byte-range 源的统一入口，D8 的 CDM 留空位在此挂钩） |
| `renderer_client.h` | `RendererClient`（`OnError`/`OnEnded`/`OnBufferingStateChange`/`OnWaiting`/`OnDurationChange`/`OnStatisticsUpdate`/`OnVideoConfigChange`）+ `BufferingState` + `PipelineStatistics` |
| `renderer.h` | `Renderer` + `RendererType`（对齐 Chromium，去掉浏览器专属部分） |
| `renderer_factory.h` | `RendererFactory`（M8 由 `player/` 注入，`platform/` 后端据此选 sink） |
| `pipeline.h` | `Pipeline` + `Pipeline::Client` |
| `pipeline_controller.h` | `PipelineController` 状态机包装（`kCreated`→`kStarting`→`kReady`→`kStopping`→`kStopped`→`kDestroying`） |

**刻意按项目自己的 DRAFT 规矩办**（第五轮的流程记录）：每个文件头显著标注
`STATUS: DRAFT — NOT YET IN THE BUILD` 并逐条列出缺口；**不加入任何 CMake target**
（不能编译的东西绝不可从构建可达）；`check_invariants.py` 对 DRAFT 豁免风格/长度
规则但**仍强制 C20（命名空间闭合）与 C22（分层方向）**，并在每次运行末尾打印
DRAFT 清单防止被遗忘。本轮 `check_invariants.py` 输出已包含这 7 个文件。

**与 docs/03 §6 的两处刻意偏离**（都写进了文件头注释）：
1. `base::SingleThreadTaskRunner` → 用 `base/task/sequenced_task_runner.h` 里既有的
   别名（`using SingleThreadTaskRunner = SequencedTaskRunner;`），不新增类型；
2. `RendererClient::OnAudioOutputDeviceChanged(...)` 在 docs/03 里是省略号，
   本轮按 Chromium 补成 `(const std::string& device_id, bool is_default, OutputDeviceStatus)`，
   并新增 `OutputDeviceStatus` 枚举——`media/audio/`（M7）落地时若要改，走接口评审。

### (4) R2 降级债的显式登记 — ✅ 本轮补记

`docs/08` §5.1 给 R2（自研 `base/` 工期超支，P4×I4=**16**，高危）准备了 L0–L3
四级降级预案。核对代码后确认：**项目已经落到 L1 + L2，但此前 PROGRESS 未把它记为
降级**，只在 `bind.h` 的头注释里提过一句。补记如下，免得后来者以为 `base/` 已按 L0 完成：

| 级别 | 证据 | 尚未偿还的部分 |
|---|---|---|
| **L1** `BindOnce` 子集 | `base/functional/bind.h` 头注释原文："Supported subset (**the L1 tier of docs/08 §5.1 R2's fallback plan**)" | 不支持 `Owned()`（注释理由：无处 own/delete，硬发会静默泄漏）、`Passed()`、`IgnoreResult()` binder、泛型（auto 形参）lambda |
| **L2** 无 message pump | `base/threading/thread.h` 原文："A dedicated thread running a **TaskQueue**"；规划中的 `base/threading/message_pump_epoll.cc`（~550 行）**不存在**；`Player::RunUntilIdle()` 是空函数体 | 无 fd / 定时器事件驱动能力（D3 二期"本地文件改 `base::File` 异步 IO"没有基础）· 无嵌套 `RunLoop` · 延迟任务精度降低 · `TaskEnvironment` 是"同步执行 + 手动推进虚拟时钟"形态 |
| 连带缺失 | `base/files/`（含 `ScopedLibrary`）· `base/containers/circular_deque.h` · `base/feature_list.h` · `base/trace_event/` · `base/strings/` · `base/synchronization/lock_order_checker` | `ScopedLibrary` 缺失 → **dlopen 弱依赖（G-L3 / docs/09 §6，11 个平台库）没有实现基础**，M12 会被它卡住；`LockOrderChecker` 缺失 → **R5（stop/析构死锁，R=15）的应对⑤"debug 构建全程开启"落空**，而"stop 卡死"正是 docs/01 十二条病灶的第 11 条 |

docs/08 §5.1 明确说 L1/L2 **保持接口形态不变**（所以 `media/`、`player/` 调用方零改动，
这是"参照 Chromium API 而非 Chromium 实现"的直接收益），并且"**M13 后补齐**"。
但 **M13 的 checklist 里并没有列这笔债** —— 本轮把它登记在此，并要求在 M13 规格中补两项：
① `message_pump_epoll` + `base/files/ScopedLibrary`（M12 的前置，不能等到 M13 之后）；
② `LockOrderChecker`（R5 应对⑤，应在 M8 接线之前就有，否则死锁只能在压力测试里偶发）。

> 顺带核对到的另一处不对齐：`player/option_registry.cc` 只覆盖 **8 个 key**
> （`buffer.enabled` / `buffer.max_bytes` / `buffer.min_frames` / `buffer.unlimited` /
> `render.disable_video_output` / `seek.accurate` / `video.max_fps` / `video.max_frame_drop`），
> 而 `PlayerConfig` 已有 ~80 个字段、docs/05 表 4 要求 60+ 个 key。
> `option_registry.h:42` 与 `option_registry.cc:197` 的注释都声称这张表由
> `tools/gen_options.py` 生成"so the two can never drift"——**该工具不存在，表是手写的，
> 且已经漂移**。直接影响验收标准 **A10（所有原版 option 都有等价入口）= 8/60+**，
> 以及 Δ2（未知选项返回 `kInvalidArgument`）会让存量 ijkplayer 配置大面积报错。
> 归入 M8 的 DoD 前置，不在本轮范围内。

---

#### 七个文件的内容与刻意偏离

| 文件 | 行数 | 关键内容 |
|---|---|---|
| `pipeline_status.h` | 60 | `PipelineStatus` 枚举（13 个状态，按产生阶段分组）· `PipelineStatusCallback` · **`PipelineStatusToMediaError()` 要求映射是全射**——任何 status 都不得落到泛化的 "playback failed"，这正是 docs/10 §4 要防的失败模式 |
| `media_resource.h` | 63 | `MediaResource::GetStream(type)`。注释写清了**为什么不直接用 `Demuxer*`**：① `tests/support/synthetic_demuxer` 要能替身；② `SelectTrack()` 改流不应重开源。返回 nullptr 不是错误——无音轨时必须退回 external 时钟，与 ffplay 一致 |
| `renderer_client.h` | 155 | `RendererClient` 9 个回调 · **`BufferingState`（4 值枚举）** · `OutputDeviceStatus` · **`PipelineStatistics`（21 个计数器，含 `avg_av_diff_ms` / `max_av_diff_ms`——R1 的触发信号"av_diff 稳态 > 40ms"从此可观测）**。注释标明它是 `FFP_PROP_INT64_*` × 20 的替代：一次返回结构体，消费者不会看到半更新的画面 |
| `renderer.h` | 132 | `Renderer` 11 个方法 · **`RendererType`**。注释含"替代了什么"（`ff_ffplay.c` 整体 + ~200 字段的 `VideoState`）、线程归属（仅 `GetMediaTime()` 线程安全，走 seqlock）、**顺序契约**（Initialize 先于一切；违反在 debug 是 DCHECK、release 是 `kInvalidState`，**绝不是 UB**） |
| `renderer_factory.h` | 87 | `RendererFactory` 6 个方法。注释写明**这就是平台接缝**：`platform/` 可依赖 `media/`，反向不行（C22），所以 media 层不能提 `Sdl2VideoSink` 的名字，只能向工厂要"给这个 display 的 video sink"。`CreateRenderer` 返回 nullptr 表示本工厂不服务该 type，从而支持链式回退（与 `DecoderSelector` 同形，Δ12） |
| `pipeline.h` | 148 | `Pipeline` 24 个方法 + 嵌套 `Client`（7 回调）+ `Statistics`。注释含：`Stop()` 非阻塞（Δ1）与 `shutdown_timeout` 兜底 detach（Δ15）· **四个 const getter 为何必须线程安全**（否则 SDK 门面的 UI 线程会被 media sequence 挂住，正是本类要消灭的失败模式）· `Seek()` 故意只有关键帧语义，精确 seek 归 `player/seek_controller`（M9），**media 层不应知道"丢帧直到目标"这种概念** |
| `pipeline_controller.h` | 90 | `PipelineController` + 6 态枚举 + 转移图。注释含 kDestroying **为什么必须存在**（析构期到达的回调要能被识别并丢弃，而不是投给半销毁的 owner——第二轮 sanitizer 抓到的 lost wakeup 就是这一类）· `Stop()` 重复调用是 no-op 而非 error（`~Player()` 与 `Player::Stop()` 都会调） |

**与 docs/03 §6 的三处刻意偏离**（都写进了文件头，M8 若要改走接口评审）：

1. **`PipelineController` 声明为抽象类**，而 docs/03 与 Chromium 都是持有 `unique_ptr<Pipeline>` 的具体类。理由：具体状态机应与它驱动的 Pipeline 住在同一个 TU（`pipeline_impl.cc`），这样转移表与销毁顺序（docs/03 §10.1）不会跨文件——R5（stop/析构死锁）的防线之一。
2. **`base::SingleThreadTaskRunner` 用既有别名**（`base/task/sequenced_task_runner.h` 里已有 `using SingleThreadTaskRunner = SequencedTaskRunner;`），不新增类型。
3. **`RendererClient::OnAudioOutputDeviceChanged`** 在 docs/03 里是省略号，本轮补成 `(device_id, is_default, OutputDeviceStatus)` 并新增 `OutputDeviceStatus` 枚举；同时**在文件头写明**：若 M7 不做设备切换就应**删掉这个方法而不是留一个没人触发的回调**——"接口里有死钩子"正是 SDK 用户开始不信任其余接口的起点。

**自检结果**（无编译器环境，故用可机械验证的部分兜底）：

```
7 个文件：include guard ✅ · STATUS: DRAFT 标记 ✅ · namespace 开闭配平 ✅
         全部 #include 的目标文件存在 ✅ · 无 >80 列的行（按字符计）✅
         C20（命名空间）+ C22（media/ 不得 include player/）✅
tools/check_invariants.py --root .  →  all rules pass (173 files scanned)
                                    →  note: 7 DRAFT file(s) … （逐一列出，防遗忘）
```

**未做/不能做的验证**：`-fno-exceptions -fno-rtti -Werror` 下的真实编译、以及
`video_decoder_factory.h` / `audio_decoder_factory.h` 被 `renderer_factory.h`
包含后是否有循环包含。这两项必须在有编译器的机器上跑一次
`cmake --preset debug && cmake --build build/debug`（把它们临时加进
`ijkpp_media` 的 HEADERS 或直接 `g++ -fsyntax-only -std=c++20 -I.`）。
**转正前必须做**，否则就是第五轮 `ffmpeg_demuxer` DRAFT 期犯过的同类错误的反面。

### 本轮未做 / 下一步

- **`--ijkplayer` 真机跑一次**（需要 ijkplayer 源码树），把 docs/05 表 7 的
  "个别 fork 有差异"变成确定结论；有差异就登记 Δ 而不是悄悄改阈值
- CI 打开 `ffmpeg-matrix`（注释写 "enabled at M4"，**M4 早已完成**）；
  给 `linux-ffmpeg711` 加 job，否则 322/322 与 43/43 永远只是本机结果；
  coverage job 的 `lcov --summary || true` 改成真门禁
- 把 `extract_constants.py --selftest` 加进 CI 的 quick gate
- README 的测试数（238/293/266）与本轮 PROGRESS（287/322）仍不一致，待同步
- M7 的第一个真实现：`AudioRendererAlgorithm`（WSOLA）——它单点阻塞音频半边
- R2 的降级债（`bind.h` 的 L1、`thread.h` 的 L2、缺失的 `message_pump_epoll` /
  `base/files/ScopedLibrary` / `LockOrderChecker`）此前未在 PROGRESS 显式登记，
  **本轮登记**：docs/08 说"M13 后补齐"，但 M13 的 checklist 里没有这一项，
  需要在 M13 规格中补上，否则 `LockOrderChecker` 缺失会让 R5（stop/析构死锁，R=15）
  失去应对⑤

---

### 可执行的验证命令

```bash
cmake --preset no-ffmpeg && cmake --build --preset no-ffmpeg && ctest --preset no-ffmpeg
cmake --preset debug       && cmake --build build/debug       && (cd build/debug && ctest)
cmake --preset asan        && cmake --build --preset asan  -j2 && (cd build/asan  && ctest)
cmake --preset tsan        && cmake --build --preset tsan  -j2 && (cd build/tsan  && ctest)
python3 tools/check_invariants.py --root .
python3 tools/extract_constants.py --root . --selftest   # 第九轮新增，无需 ijkplayer 源码
python3 tools/extract_constants.py --root . --ijkplayer /path/to/ijkplayer  # 三方校验
```

**实测结果（Debian 12 / GCC 12.2 / CMake 3.25 / Ninja）**：

| 配置 | 结果 |
|---|---|
| `no-ffmpeg`（RelWithDebInfo，无 FFmpeg/SDL2/X11） | ✅ **287/287** |
| `linux-ffmpeg711`（FFmpeg **7.1.1** + 真实媒体测试） | ✅ **322/322** |
| `debug`（Debug + `IJKPP_ENABLE_DCHECK` + `-Werror`） | ✅ **287/287** |
| `asan`（AddressSanitizer + UBSan + **LeakSanitizer**） | ✅ **287/287，0 泄漏** |
| `tsan`（ThreadSanitizer） | ✅ **287/287，0 data race** |
| `check_invariants.py` | ✅ 14 条规则全通过（166 文件；含 **C22**：`media/`、`base/` 不得依赖 `player/`） |
| **抗抖动**：并发/时钟类测试 `--repeat until-fail:25`（asan）/`:10`（tsan） | ✅ 0 失败 |
| **`media_ffmpeg_unittests`（FFmpeg 7.1.1 + 真实媒体）** | ✅ **43/43**（M5 视频 + 本轮 M5 音频 8 个） |
| **`ijkpp-inspect` CLI（probe / decode / sync）** | ✅ 3 个子命令在 5 个真实容器上跑通，含错误路径 |
| `platform/ffmpeg/` 兼容层双版本编译 | ✅ **7.1.1 与 5.1.9 均通过**（4 个 .cc） |

> 注 1：preset 名为 `debug`（非早期文档写的 `dev`）。`debug` 此前从未被完整构建过，本轮首次全量编译暴露出 8 处 `-Werror` 违规（均为既有代码，见"第八轮"）。
> 注 2：`no-ffmpeg`/`debug`/`asan`/`tsan` 为 287（不含需要 FFmpeg 的 35 个），`linux-ffmpeg711` 为 322 = 287 + 35。

编译选项：`-std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow
-Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wnull-dereference
-Wdouble-promotion -Wimplicit-fallthrough -Wformat=2 -Werror=return-type
-Werror=uninitialized -Werror=narrowing -Werror=delete-non-virtual-dtor -Werror=reorder`
—— **零警告**。

---

## 第八轮（本轮）：音频链 · `DecoderStream` · `ijkpp-inspect` · 2 个真 bug

用户要求的 4 项按序推进，前 3 项完成并验证，第 4 项（SDL2 出画）因依赖 M7/M8 未完成而顺延。

### (1) 音频解码链 — ✅ 完成

| 新增 | 说明 |
|---|---|
| `media/base/audio_buffer.{h,cc}` | 解码后音频值类型。替代 ijkplayer 里到处传递的裸 `AVFrame`——ffplay 在每个使用点重新推导 planar/interleaved、采样格式、声道数；这里构造时描述一次 |
| `media/base/audio_decoder.{h,cc}` | 异步音频解码器契约。**刻意与 `video_decoder.h` 同形**（`Initialize(config, …, InitCB, OutputCB, WaitingCB)` + `Decode(buffer, DecodeCB)` + `Reset(closure)`），因为 `DecoderStream<Traits>` 要泛型化两者 |
| `media/base/waiting.{h,cc}` | 从 `video_decoder.h` 抽出 `WaitingReason`/`WaitingCB`。音频也要报告等待原因，但不应为此依赖视频头文件 |
| `media/filters/ffmpeg_audio_decoder.{h,cc}` | 移植自 `ff_ffplay.c` 的 `audio_decode_frame()` |
| `DecodeSample()`（`audio_parameters.cc`） | u8/s16/s32/f32 + 三种 planar → float 的单一转换点 |

**与 ijkplayer 的关键差异**：ffplay 在 `audio_decode_frame()` 里对每帧做 `swr_convert`，且发生在**音频回调线程**上。ijkpp 的解码器输出编解码器原生格式，把转换留给消费者（`AudioBuffer::ReadFrames`）。这样解码器不依赖任何音频设备，输出设备切换时也不必销毁重建 resampler。

**8 个端到端测试**（真实 AAC）：解码全流 / 采样率与声道正确 / pts 单调且覆盖 3 s 时长 / 转 float 不溢出且无 NaN / EOS 标记恰好一个 / 未初始化即 Decode 干净失败 / 未知编解码器给出**含编解码器名**的错误 / `kUnsupportedConfig` 与 `kUnsupportedCodec` 必须可区分（bug #28 的教训）。

### (2) `DecoderStream<Traits>` — ✅ 完成

替代 ffplay 的 `video_thread()` + `audio_thread()`：两个近乎相同的 ~200 行函数，唯一真实差异是包队列、帧队列和编解码器调用——正是 Traits 能承载的东西。

```
VideoDecoderStreamTraits / AudioDecoderStreamTraits
  ├ CreateDecoder(factory, config)      // CreateVideoDecoder vs CreateAudioDecoder
  ├ InitializeDecoder(…)                // (low_delay, cdm) vs (has_pending_clear, serial)
  ├ ConfigFromStream(stream)
  ├ IsEndOfStreamOutput(output)         // VideoFrame 无 EOS 标记；AudioBuffer 有
  └ DecoderName(decoder)                // name() vs GetDisplayName()
```

职责：按 `DecoderSelector` 的排序逐个工厂尝试 → 初始化失败则回退并发 `kDecoderFallbackOnInit` 事件（Δ12）→ demuxer/decoder 两侧队列 + 水位线背压 → `Flush(serial)` 丢弃上一代 seek 的在途数据 → 连续解码失败超阈值则发 `kDecoderFallbackOnDecodeError`。

**9 个测试**全部用假件（无 FFmpeg、无线程），因此选择/回退/水位线/flush 都可确定性断言——这正是从 ffplay 两个手写线程迁移过来的收益：那些行为在原结构下无法测试。

### (3) `ijkpp-inspect` CLI — ✅ 完成

三个子命令，全部走**真实** demuxer + 解码器，无测试替身：

```
$ ijkpp-inspect decode tests/testdata/small_h264_aac_3s.mp4
video stream [0] codec=h264
  initialize  : ok (FFmpegVideoDecoder)
  frames      : 90                      ← 3 s × 30 fps，一帧不差
  size        : 320x240 format=I420
  first pts   : 0.000000 s
  last pts    : 2.966667 s
  non-monotonic pts: 0                  ← bug #26 的特征就在这里
audio stream [1] codec=aac
  buffers     : 141      samples: 144384      decoded: 3.008 s
  non-monotonic pts: 0
```

`probe` 打印容器/流信息；`sync` 用真实时间戳驱动 `AvSyncController`，逐步打印 `media_time / master / av_diff / adjust`。错误路径输出完整三段式错误（summary + context + detail + hint），例如文件不存在时直接给出 `native = -2 (No such file or directory)` 和 `ffprobe` 建议。

### ★ bug #32：主时钟被 uptime 偏移 —— CLI 首跑即抓到

`sync` 第一次运行在真实文件上输出：

```
step  kind   media_time  master        av_diff
1     audio  0.000000    -33185.173673 0.000000     ← 负的系统 uptime
```

`Clock::Get()` 原来写的是 `drift + elapsed * rate`，而 `drift` 的定义已经是 `pts - updated_at`。于是 `updated_at` 被**减了两次**，返回值偏移 `-updated_at`——真实机器上就是负 uptime。

ffplay 的原式是：

```c
pts_drift + time - (time - last_updated) * (1.0 - speed)
```

代数展开恰好等于 `pts + (time - last_updated) * speed`。修复后采用展开式，**在所有 speed 下都与 ffplay 可证等价**，而不只是在 1.0 时凑巧相等。

**为什么 28 个同步测试全都没发现**：它们都驱动 `SimpleTestTickClock`，起点是 0，于是 `drift == pts`，两种写法完全一致。这是典型的"测试时钟从零开始掩盖 bug"。后果若上线则是灾难性的：主时钟 ≈ -33185 s 意味着**每一帧视频都被判定为无限迟到并丢弃**。

回归测试 `MasterClockIsUnaffectedByWallClockOrigin` 先 `clock_.Advance(Seconds(33185))` 再断言。**已验证其有效性**：临时还原 bug 后该测试失败，而既有的 `MasterClockAdvancesWithWallTime` 依然通过——精确复现了"为什么之前没被发现"。

### ★ bug #33：水位线只在函数入口生效

`DecoderStream` 的水位线检查原先只放在 `DecodeNextBuffer()` 入口。但 FFmpeg 解码器**同步完成**：`Decode()` 返回时输出已入队，于是入口检查之后整批 8 个 buffer 会被一路解码到底。实测 `buffered_outputs() == 32`，水位线是 16。

修复：把水位线写进循环条件。这正是 ffplay 用 `frame_queue_signal()` 阻塞解码线程来避免的内存增长——背压必须在**产生输出的那一层**生效，而不是只在取数据的那一层。

### bug #34：并发时钟测试的启动竞态（既有 flaky）

`ConcurrentReadersNeverSeeATornClock` 在 `ctest --repeat until-fail` 下以 33309 个"违规"失败。原因不是撕裂读：4 个读线程一创建就开始自旋，**在写线程第一次 `Set()` 之前**的读会拿到无效时钟并返回 `kNoTimestamp`——这是正确行为，但测试把它计为违规。

修复：读线程启动前先自旋等待 `master_clock_valid()`。修复后 asan 下连续 25 次、tsan 下连续 10 次全绿。

### `debug` preset 首次全量编译暴露的 8 处 `-Werror` 违规

`debug` 此前从未被完整构建过（早期文档误记为 `dev`）。全部是既有代码，逐一修正而非放宽编译选项：

| 文件 | 违规 | 处理 |
|---|---|---|
| `base/time/time.cc` | `-Wconversion` int64→double | 显式 `static_cast` + 注明 2^53 边界不可达 |
| `media/base/audio_bus.cc` | 2× `-Wsign-conversion` | `channels` 转 size_t；下标运算显式化 |
| `media/base/video_frame.cc` | 3× `-Wsign-conversion` | 平面下标 `int`→`size_t`；`stride * rows` 显式化 |
| `media/base/native_display.{h,cc}` | `-Wuseless-cast` | `X11WindowHandle::window` 由 `uint64_t` 改 `uintptr_t`——X11 的 `Window` 本就是 `XID`（`unsigned long`），原类型声明才是错的 |
| `media/filters/clock.cc` | `-Wconversion` | 外推的 int64→double 显式化 |
| `media/filters/video_frame_compositor.cc` | `-Wconversion` | 同上（播放速率缩放） |
| `media/filters/decoder_stream.cc` | `-Werror=attributes` | 显式实例化**定义**上不能再带 `IJKPP_MEDIA_EXPORT`（属性属于头文件的 `extern template` 声明） |
| `base/observer_list.h` | `-Wsign-conversion` | `std::count_if` 返回 `difference_type`，显式转 `size_t` |
| 3 个测试文件 | `-Wunused-parameter` | 按 Google Style 省略未用形参名 |

### CLI 自身的 C1/C2 违规

`inspect_main.cc` 初版 743 行、`RunDecode` 250 行、`RunSync` 181 行——违反 C1（≤500 行/文件）和 C2（≤80 行/函数）。**没有走豁免清单**，而是按子命令拆分：

```
tools/inspect/
  inspect_common.{h,cc}   参数解析 / InspectHost / Pump / OpenDemuxer
  inspect_probe.cc        RunProbe（+ PrintContainer/PrintVideoStream/PrintAudioStream）
  inspect_decode.cc       RunDecode（+ PumpDecoder<模板> / DecodeVideo / DecodeAudio / CountNonMonotonic）
  inspect_sync.cc         RunSync（+ SyncFromAudio / SyncFromVideo / ReadBatch / PrintSummary）
  inspect_main.cc         仅 main()
```

拆分还消掉了重复：视频与音频的 demux→decode 循环本来近乎相同（因为两个解码器契约已同形），现在共用一个 `PumpDecoder<Decoder, Output>` 模板。

### (4) SDL2 出画 — ⏸ 顺延（依赖未满足）

**未开始**，原因不是工作量而是依赖：SDL2 sink 需要一个能喂给它的渲染器，而 `VideoRendererImpl` / `AudioRendererImpl` / `RendererImpl`（M7）与 `Pipeline` / `Player` 装配（M8）尚未实现——`Player` 的方法目前仍返回 `kNotImplemented`。

到画面之间的**确定路径**（每步都可独立验证）：

1. **M7**：`VideoRendererImpl` = `DecoderStream<Video>` + `VideoFrameCompositor`（已有）+ `AvSyncController`（已有）；`AudioRendererImpl` = `DecoderStream<Audio>` + `AudioRendererAlgorithm`（WSOLA，未实现）+ `AudioRendererSink`
2. **M8**：`RendererImpl` 组合两者 → `Pipeline` → `Player` 装配
3. **M11**：`platform/sdl2/` 的 `VideoRendererSink`（SDL_Renderer 贴 I420）+ `AudioRendererSink`（SDL_AudioDevice）

**注意**：`platform/CMakeLists.txt` 里 `IJKPP_ENABLE_SDL2` 目前仍是有意的 `FATAL_ERROR "scheduled for milestone M11"`，需要一并解除。

若目标是**尽快看到画面**而非按里程碑推进，最短路径是先做 M7 的视频半边 + 一个只渲染视频、音频走 null sink 的 `RendererImpl`，即可用 SDL2 出画（无声）。这条路绕开 WSOLA，约需 `VideoRendererImpl` + `RendererImpl`(视频only) + `Pipeline`(视频only) + SDL2 sink 四件。

### 仍未完成

- **`AudioRendererAlgorithm`（WSOLA）** —— 本轮 (1) 的剩余部分。`AvSyncController::ComputeAudioSampleAdjustment()` 已有真实调用方（`ijkpp-inspect sync` 会打印它），但还没有消费者去实际拉伸/压缩音频
- **M7 / M8 / M11** —— 见上
- **`ijkio` 缓存（M18）、C ABI（M15）、Android/iOS 后端（M16/M17）** —— 按原计划顺延
- 既有待办：`thread_unittest.cc` 里 `make_shared<atomic<int>>` 触发的 2 处 GCC `-Wnull-dereference` 误报（在 `shared_ptr_base.h` 内），发布构建启用 `IJKPP_WERROR` 前需处理

---

## 已完成


### (a) M0 工程基建 — ✅ 完成

| 项 | 状态 |
|---|---|
| `CMakeLists.txt` + 4 个子目录 CMakeLists | ✅ |
| `CMakePresets.json`（12 个 configure preset + 7 build + 4 test） | ✅ |
| `cmake/IjkppOptions.cmake`（全部开关 + 互斥校验） | ✅ |
| `cmake/IjkppCompilerFlags.cmake`（`-fno-exceptions -fno-rtti`、Google 警告集、`-Wthread-safety`、sanitizer、coverage、LTO） | ✅ |
| `cmake/IjkppThirdParty.cmake`（GTest 探测，兼容 Debian 无 CMake package 的情况） | ✅ |
| `cmake/IjkppCheckInvariants.cmake` → `check-invariants` target | ✅ |
| `cmake/BuildConfig.h.in` / `Version.h.in` | ✅ |
| `.clang-format`（Google，80 列 2 空格）、`STYLE.md` | ✅ |
| `.github/workflows/ci.yml`（quick + full 矩阵 + M4/M11 占位 job） | ✅ |
| `tools/check_invariants.py`（C1/C2/C4/C5/C7/C8/C9/C11/C14/C17/C18/C20/C21） | ✅ |

**`check_invariants.py` 已抓到并修掉的真实问题**（这就是把架构约束变成 CI 的价值）：

| 规则 | 抓到的问题 | 处理 |
|---|---|---|
| C20 | `player.h` 内层 `namespace media {` 缺闭合注释 | 补上 |
| C20 | `weak_ptr.h` / `ref_counted.h` / `bind.h` 等 3 个文件命名空间开合不匹配（真编译错误） | 修正 |
| C2 | `DecideNextFrame` 139 行、`Render` 90 行，超 80 行上限 | 拆出 `ClassifyCandidate` / `DeriveFrameDuration` / `ShouldDropForLateness` / `BuildFrameSyncInputLocked` / `AccountDropLocked`，两者分别降到 78 / 40 行 |
| C18 | 误报：行尾注释里的 "catch up" 被当成 `catch` | 检查前剥离注释 |
| C1 | 测试文件 760 行 | 测试文件豁免（对齐 Chromium） |
| C7 | `base/types/expected.h` 回退实现用 placement new | 加入 allowlist（它就是分配器本体） |

### (c) M1 `base/` 核心件 — ✅ 完成

| 组件 | 状态 | 单测 |
|---|---|---|
| `base/check.h` + `check.cc`（CHECK/DCHECK/NOTREACHED/CHECK_EQ...） | ✅ | 间接覆盖 |
| `base/logging.h` + `.cc`（LOG/LOG_IF/DLOG/VLOG/DVLOG + `LoggingDelegate`） | ✅ | 间接覆盖 |
| `base/types/expected.h`（C++23 时 alias `std::expected`，否则自研回退） | ✅ | 6 |
| `base/expected_macros.h`（`RETURN_IF_ERROR` / `ASSIGN_OR_RETURN`） | ✅ | 含在上 |
| `base/time/time.h` + `.cc`（`TimeDelta` / `TimeTicks` / `Time`，饱和算术） | ✅ | 17 |
| `base/time/{tick_clock,default_tick_clock,simple_test_tick_clock}.h` | ✅ | 3 |
| `base/memory/{scoped_refptr,ref_counted,raw_ptr,ptr_util}.h` | ✅ | 8 |
| `base/memory/weak_ptr.h`（flag 持有 sequence 亲和性，WeakPtr 本身可平凡拷贝） | ✅ | 8 |
| `base/functional/{callback_forward,callback}.h`（`OnceCallback`/`RepeatingCallback`） | ✅ | 5 |
| `base/functional/bind.h`（`BindOnce`/`BindRepeating`/`Unretained`/`Owned`） | ✅ | 10 |
| `base/functional/callback_helpers.h`（`DoNothing`/`ScopedClosureRunner`） | ✅ | 4 |
| `base/synchronization/{lock,condition_variable,waitable_event,atomic_flag,atomic_sequence_number}.h` | ✅ | 12 |
| `base/sequence_checker.h`（`SEQUENCE_CHECKER`/`DCHECK_CALLED_ON_VALID_SEQUENCE`） | ✅ | 3 |
| `base/observer_list.h`（Chromium `kAll` 语义 + 迭代中增删） | ✅ | 8 |
| **未做** `base/containers/circular_deque.h`、`base/feature_list.h`、`base/trace_event/`、`base/files/` | ⬜ M3+ | — |

### M2 `base/task` + `base/threading` + `TaskEnvironment` — ✅ 完成（提前）

| 组件 | 状态 | 单测 |
|---|---|---|
| `base/location.h`（`FROM_HERE`，任务投递点可追溯） | ✅ | — |
| `base/task/task_runner.{h,cc}` | ✅ | — |
| `base/task/sequenced_task_runner.{h,cc}`（thread_local 当前默认 runner） | ✅ | 3 |
| `base/task/task_queue.{h,cc}` ★**ijkpp 的消息循环** | ✅ | 11 |
| `base/task/task_runner_util.h`（`PostTaskAndReplyWithResult`） | ✅ | 1 |
| `base/threading/thread.{h,cc}`（`Start`/`Stop`/`task_runner`/`GetThreadId`） | ✅ | 8 |
| `base/threading/platform_thread_posix.{h,cc}`（线程命名 + 优先级） | ✅ | 含在上 |
| `base/test/task_environment.{h,cc}`（`MOCK_TIME`/`RunUntilIdle`/`FastForwardBy`/`AdvanceClock`） | ✅ | 11 |

**一处刻意的简化**：Chromium 把这块拆成 `MessageLoop` + `MessagePump` + `MessagePumpEpoll` + `TaskQueue` 四层。ijkpp 合并成单个 `TaskQueue`，因为核心层既不需要 fd 监听也不需要嵌套 `RunLoop` —— 平台层（Wayland/epoll）跑自己的 poll 循环（见 docs/04 §2.1 D3）。`task_queue.h` 的类注释里写明了将来需要 fd 监听时如何在**不改调用方**的前提下把 `MessagePump` 拆回去。

**R2 风险（自研 `base/` 工期）已基本消除**：原评估里三个"大"项——`bind.h`、消息泵、`TaskEnvironment`——全部实现完毕并有测试。

### (b) M8 `player/public/` — ✅ 12 个 SDK 头文件已定稿（1130 行）

`player.h` · `player_config.h` · `player_event.h` · `media_info.h` · `playback_stats.h` · `error.h` · `native_display.h` · `deps.h` · `global.h` · `version.h` · `player_export.h` · `option_registry.h`

全部**编译通过**。已实现：
- `error.cc`：`MediaError` 三段式（summary/detail/hint）+ `ToString()` + `ToJson()` + `GetErrorCodeName()` 覆盖全部 30 个 `ErrorCode`
- `player_config.cc`：`ValidateConfig()` 一次性返回全部问题（13 项校验）
- `player_event.cc`：`GetPlayerStateName` / `GetEventTypeName` / 16 个类型化访问器 / `ToLegacyEvent()` 把强类型事件降维成 ijkplayer 的 `(what,arg1,arg2,obj)` 四元组（Java 层零改动迁移路径）
- `option_registry.cc`：9 个代表性选项 + `SuggestNearestKey()`（Levenshtein，"did you mean" 提示）。**完整 60+ 项表格由 `tools/gen_options.py` 在 M1 生成**（规则 C13）
- `deps.cc`：move-only + `std::shared_ptr`，使 `player/public/deps.h` 不必包含 `media/base` 接口头
- `player.cc`：M8 骨架。已接线的：`DataSourceDescriptor` 4 个工厂、`Player` 构造（含 `ValidateConfig` 日志）、`state()`、`config()`、`DumpDiagnostics()`、`PlayerBuilder` 全链路（含"一次性报出全部配置问题"）。未接线的：一律返回 `kNotImplemented` + 指明所需里程碑，**绝不静默 no-op**

### (d) M6 `media/filters/video_frame_compositor` — ✅ 核心完成（提前于路线图）

| 项 | 状态 |
|---|---|
| `DecideNextFrame()` 纯静态函数（无副作用/无分配/无日志/无时钟） | ✅ |
| `ComputeTargetDelay()` — `compute_target_delay()` 逐行移植，符号约定与 ffplay 的 `diff` 相反已在注释中说明 | ✅ |
| `ApplyFpsCap()` — ijkplayer `max-fps` | ✅ |
| `ClassifyCandidate()` — 拒绝规则（stale serial / accurate seek / fps cap），优先级顺序有注释 | ✅ |
| `DeriveFrameDuration()` — 含 `lastvp->serial == vp->serial` 守卫 | ✅ |
| `ShouldDropForLateness()` — ffplay framedrop 块 | ✅ |
| deadline 窗口语义（Chromium `Render(deadline_min, deadline_max)`，Δ18） | ✅ |
| 有状态包装器（`PutCurrentFrame`/`Render`/`Flush`/`GetStats` + 6 个 atomic 计数器） | ✅ |
| **61 个单测全绿** | ✅ |

测试覆盖：基础呈现(6) · 丢帧(7) · compute_target_delay(9) · fps cap(6) · serial/seek(3) · 精确 seek(4) · 变速(3) · 暂停/单步(4) · 主时钟(2) · 数值边界(6) · deadline(3) · 纯度与确定性(3) · 有状态(6)。

**移植过程中发现并修正的两个真实缺陷**（都写在测试注释里作为回归保护）：
1. `frame_timer` 语义在"当前帧到期时刻"与"下一帧到期时刻"之间摇摆 → 首帧被多等一个间隔（33ms 首帧延迟回退）。修正为"下一帧到期时刻"，null 表示从未呈现 → 首帧立即出画，与 ffplay 一致。
2. `MakeHold` 重构时返回新对象，丢掉已算出的 `target_delay`/`av_diff` → `PlaybackRateScalesTargetDelayNotTheHoldGate` 立刻失败并定位。

### M3 核心 · `media/base/` 值类型与两个队列 — ✅ 完成

| 文件 | 内容 | 单测 |
|---|---|---|
| `media/base/media_constants.h` | `kNoTimestamp` 哨兵 + 全部移植阈值/上限（标注来源，禁止手抄） | 1 |
| `media/base/decoder_buffer.{h,cc}` | 压缩样本；`Storage` 类型擦除 + `storage_as<T>()`（无 RTTI，错类型返回 nullptr）；`CopyFrom`/`FromStorage`/`CreateEOSBuffer` | 11 |
| `media/base/decoder_buffer_queue.{h,cc}` | ★**serial 世代号** + 双维度限流（条数/字节）+ `Flush`/`Abort`/`MarkEndOfStream` + 阻塞可中断 Push/Pop | 15 |
| `media/base/video_frame_queue.{h,cc}` | ★**`SlotGuard` 两段式写入** + serial + `Peek` 前瞻 | 15 |
| `media/base/media_log.{h,cc}` | 结构化事件（Level×Type×properties）+ 环形缓冲 + `ToJson()` + `MEDIA_LOG` 宏 + null-log 零分支 | 9 |

**两个队列各自解决 ijkplayer 的一类无法靠纪律修复的 bug**：

1. **`DecoderBufferQueue` 的 serial**：`Flush()` 递增世代号并给后续 buffer 打标；消费者见到 `buffer->serial() < queue.serial()` 就丢弃不解码。这是 seek 正确性的基石（docs/04 §4 R1–R3），原版把它散落在 9 个 C 函数里且零注释。测试 `FlushBumpsSerialAndClears` 明确断言"**已经交出去的 buffer 仍报旧 serial**"——这正是消费者识别过期数据的依据。

2. **`VideoFrameQueue` 的 `SlotGuard`**：原版 `frame_queue_peek_writable()` 返回裸 `Frame*`，调用方必须记得随后调 `frame_queue_push()`；中间任何 early return（解码错误、abort 检查、分辨率变化）都会**永久泄漏一个槽位**，槽位耗尽后解码器永久阻塞，且无错误无日志。`SlotGuard` 用 RAII 让"忘记提交"不可能发生。测试 `UncommittedGuardReturnsItsSlot` 连续 50 次 reserve-then-drop，断言槽位一个没少、队列仍完全可用。

   另一个细节：`Flush()` **不动 reserved 槽位**（只清 filled），因为 reserved 槽位仍属于它的 guard；若同时释放，两个生产者会拿到同一个槽位并把两帧写进同一块存储。这条由 `FlushLeavesReservedSlotsToTheirGuards` 守护。

### 本轮由测试/sanitizer 抓到的问题

| # | 问题 | 性质 | 处理 |
|---|---|---|---|
| 5 | `Pop()` 在 `empty && eos` 时返回 `kEmpty` 而非 `kEndOfStream` | 🔴 **实现 bug**：消费者无法区分"流已结束"和"暂时没数据"，会对已完成的流无限轮询 | 把 `eos_` 检查提到 `closed_` 之前，并写明理由 |
| 6 | `MEDIA_LOG` 宏依赖 `LogMessageVoidify::operator&(std::ostream&)`，而 `MediaLogRecord` 不是 ostream | 编译错误 | 把 `operator&` 模板化；`base/logging.h` 注明原因 |
| 7 | 三个类第三次撞同一个坑：`RefCountedThreadSafe` 派生类写了 `~T() override` | 设计文档缺失 | 在 `ref_counted.h` **写死规则**（无虚成员→`~T();`；有虚成员→自己声明 `virtual ~T()`），根治 |
| 8 | 两个并发测试用"阻塞 Pop + 计数达标就退出"的循环 | 🔴 **测试自锁**：N 次 push 对 N 次 pop 时，看到 count==N-1 的消费者进 Pop 后永久阻塞 | 改为"drain 直到 Abort"，并把原因写成注释（这也是生产环境 shutdown 的正确形态） |
| 9 | 并发测试在 join 生产者后立刻 `Abort()`，丢了队列里剩的 2 个 | 测试 bug（TSan 构建下暴露，因为时序变了） | 改为**先排空再 Abort**；同时在测试里写明"Abort 按设计会丢弃 pending" |

第 9 条值得单独说：`consumed == 1998` 而不是 2000，看起来像队列丢数据，实际是测试没理解 `Abort()` 的语义。**把语义写进测试注释**比修数字更重要，否则下一个人会以为是队列的 bug 去"修"队列。

---

### ✅ M4 · `FFmpegDemuxer` 端到端验证通过

对 5 个真实容器（`small_h264_aac_3s.mp4` / `audio_only.m4a` / `video_only.mp4` / `corrupt_header.mp4` / `truncated_tail.mp4`）跑 12 个测试，全绿：

| 测试 | 验证的事实 |
|---|---|
| `OpensAndProbesAnMp4` | 2 条流、时长 3s、`seekable`、非直播、`Host::SetDuration` 被调用 |
| `VideoStreamConfigMatchesTheFile` | h264 / 320x240 / avg_frame_rate > 25 |
| `AudioStreamConfigMatchesTheFile` | aac / 48000 / 2ch / stereo |
| `AudioOnlyFileHasNoVideoStream` `VideoOnlyFileHasNoAudioStream` | 单流容器选流正确 |
| `CorruptHeaderProducesActionableError` | **三段式错误齐全**：summary + native_code + `hint:` 段，且 `Host::OnDemuxerError` 收到 |
| `EmptyUriIsRejectedWithoutTouchingFFmpeg` | 空 URI 在进 FFmpeg 前就拒，hint 指向 `SetDataSource()` |
| `TruncatedFileReachesEndOfStream` | moov 完整但 mdat 截断 → 正常读到 EOS，不挂死 |
| `ReadsWholeFileWithMonotonicTimestamps` | **读完整文件：80–100 帧、video pts 不回退、每个 buffer 的 serial 与队列一致、`data_size > 0`** |
| `SeekBumpsSerialAndReportsCompletion` | ★seek 回调按 `request_id` 送达、`seek_count == 1`、**serial 递增**（docs/04 §4 R1） |
| `StopIsPromptEvenWhileBlockedInRead` | ★队列塞满、demux 线程阻塞时 `Stop()` < 2s（Δ15 的可执行证明） |
| `StageEventsAreLogged` | `kOpenInput` 与 `kFindStreamInfo` 两个阶段事件进了 `MediaLog` |

**关键设计已落地并被测试钉住**：
- `DemuxerStream::Read(count, ReadCB)` **永不 inline 执行回调**——数据已在队列里也走 `PostTask`，这是 Chromium 契约，也是消费者能安全重入的前提
- 待决 `Read` 由 **demux 线程回填**（`EnqueueFromDemuxThread` → `FulfilPendingReadLocked`），media sequence 上没有任何阻塞调用（D3）
- `interrupt_flag_` 全项目只有 **3 个写入点**（`StartPlayingFrom`、`Stop`、`HandleSeekRequest` 清理），对照 ijkplayer 的 5 处 `continue_read_thread` signal
- 被新 seek 取代的旧请求**回 `kAborted` 而不是静默丢弃**，调用方可用 `request_id` 精确匹配（原版做不到）
- 打开失败时报告**未被 FFmpeg 消费的 option 名**，拼错的 `extra_format_options` 键不再静默失效（Δ2）

### ✅ M5 核心 · 解码层端到端跑通

**这是项目第一个"从真实文件到真实像素"的完整链路。** `DecodesEveryFrameOfARealFile` 用真实 h264 文件跑完整条链：`FFmpegDemuxer` → `DecoderBufferQueue` → `FFmpegVideoDecoder`（`avcodec_send_packet`/`receive_frame` + `sws_scale`）→ `VideoFrame`，解出 85–95 帧（testsrc2 30fps×3s）。

| 新增 | 内容 |
|---|---|
| `media/base/video_decoder.{h,cc}` | ★**异步回调式接口**，签名逐行对齐 Chromium：`Initialize(config, low_delay, cdm, InitCB, OutputCB, WaitingCB)` / `Decode(buffer, DecodeCB)` / `Reset(closure)` / `GetMaxDecodeRequests()`。四条契约（decode_cb 绝不 inline、即使不再 Decode 也必回调、output_cb 可先于 decode_cb、EOS buffer 触发 flush）全部写进头注释并有测试 |
| `media/filters/ffmpeg_video_decoder.{h,cc}` | 软解实现；`sws_scale` 按需惰性创建（源格式已是 CPU 可读时走零转换的平面拷贝快路径）；分辨率/格式变化时自动重建 sws |
| `media/filters/decoder_selector.{h,cc}` | ★纯函数排序 + `reasons` 输出（"为什么没用上硬解"从需要调试器变成读一行日志）；`DecoderPreference` / `HwCodecMask` 定义在此（C22） |
| `media/base/video_decoder_factory.h` | `VideoDecoderCapability`（hardware / outputs_opaque_surface / max_width/height / priority） |
| `media/base/video_frame.h` | 新增 **`mutable_data(Plane)`** 生产端可写访问器，消费者仍只见 `visible_data()` |

**7 个解码测试**：帧数、几何（320×240 I420、Y 平面非全零）、pts 单调不回退、serial 从 buffer 传播到 frame、未知 codec 的可操作状态、未 Initialize 就 Decode 报 `kNotInitialized`、**decode_cb 绝不 inline**。
**8 个 DecoderSelector 测试**：Auto 硬解优先、Software 排除硬解、HardwareOnly 不回退、hw_codecs 掩码过滤（且拒绝理由可解释）、分辨率上限、priority 排序、空列表自我解释、掩码辅助函数。

### ✅ M6 核心 · 音视频同步层（R1 风险的收口）

| 新增 | 内容 |
|---|---|
| `media/filters/clock.{h,cc}` | ★**seqlock 媒体时钟**：pts + drift + serial + speed，读侧无锁、写侧单 sequence。替代 ffplay 的非原子 `double pts/drift`（**Δ14**：TSan 无条件报 data race，64 位下通常不撕裂所以多年隐形） |
| `media/filters/av_sync_controller.{h,cc}` | `ResolveMasterType`（= `get_master_sync_type`）、`GetMasterClock`（= `get_master_clock`，含 external 回退）、`ComputeAudioSampleAdjustment`（= `synchronize_audio`）、`AlignAudioDurationToVideo`（= `synchronize_audio_to_video`）、`GetWallClockTimes`（`media::TimeSource`） |
| `media/base/time_source.h` | `WallClockTime` + `TimeSource` 接口，让 sink 能问"结束于 T 的刷新窗口里该显示哪一帧" |

**28 个新单测**，逐条对应 ffplay 的阈值常量（`AV_DIFF_AVG_COEF` 0.9 / `AV_DIFF_AVG_NB` 10 / `AV_DIFF_THRESHOLD` 0.1 / `SAMPLE_CORRECTION_PERCENT_MAX` 10 / `AV_NOSYNC_THRESHOLD` 10s），并注明"上游改了就必须重跑 `extract_constants.py`，不要凭记忆改测试"。

覆盖：三种主时钟 + 两种回退（audio↔video、→external）、时钟外推与变速、av_diff、Flush 失效、**diff 滑动窗口填满前不校正**、校正量被 10% 夹住、超过 `AV_NOSYNC_THRESHOLD` 放弃校正、退化输入、`GetWallClockTimes` 单调性与 2x 压缩、快照完整性、以及 **4 读者 × 1 写者的 seqlock 撕裂检测**。

至此 **R1（音视频同步移植）的两个高危部分都已用测试钉住**：`VideoFrameCompositor::DecideNextFrame`（第六轮，61 例）与 `AvSyncController`（本轮，28 例）。剩下的是 M7 把两者接起来后的 Golden Test 验证。

### 第七轮抓到的 2 个真 bug —— R1 风险的实体化

| # | 问题 | 性质 |
|---|---|---|
| 29 | 🔴 **`AlignAudioDurationToVideo` 的符号反了**：我写的 `adjustment = -diff`，导致 audio 落后时反而**拉长**时长、超前时**缩短**——校正方向与需求完全相反，会把两路流越推越远而不是拉近 | **这正是 R1 描述的那类 bug**：不崩溃、不报错、播放"看起来能跑"，只在长时间播放或弱网后表现为音画逐渐失同步。**在任何媒体被播放之前就被单测抓住了** |
| 30 | 🟠 `ComputeAudioSampleAdjustment` 的边界断言：`1024 * 90 / 100 == 921`（整数截断），最大收缩是 **103** 个样本而非浮点算出的 102.4 | 不是实现 bug，是**我的测试预期用了浮点直觉**。ffplay 用整数运算，移植必须一致——测试注释已写明"断言 -102 等于断言一个与原版不同的移植" |
| 31 | 🟠 seqlock 并发测试里写线程刻意把 pts 回绕到 1s，读线程把回绕当成撕裂 → 14 次误报 | 测试 bug。改为只检测"值落在写线程范围外"这一真正的撕裂特征，单调性由其他用例覆盖（注释说明） |

第 29 条是本次最有价值的发现。它验证了一个判断：**同步算法的移植错误无法靠肉眼 review 发现**（代码读起来完全合理），只能靠"每个分支都有断言"的单测。这也是我在 01 §4 P6 坚持时钟可注入、在 07 §3.3 要求 `DecideNextFrame` 100% 行覆盖的原因。

### 第六轮抓到的 3 个 bug

| # | 问题 | 性质 |
|---|---|---|
| 26 | **`codec_ctx->pkt_timebase` 从未设置** → `av_rescale_q` 对每一帧都返回 0 → **所有解码帧 pts 都是 0** | 🔴 **真 bug，直接摧毁 A/V 同步**。`TimestampsAdvanceAcrossDecodedFrames` 抓住：断言 `previous > 2s`，实际 `0us`。修法：`VideoDecoderConfig` 增加 `time_base` 字段，demuxer 从 `AVStream::time_base` 填充，`OpenCodec` 设 `pkt_timebase` |
| 27 | `CodecAllowedByMask` 先判 `mask == kAll` 就返回 true，**导致 kUnknown/kVp8/kTheora 这些根本没有硬件位的 codec 被判为"允许"** | 🔴 会让解码器被选中去解它不支持的流。修法：先解析 codec 的位，`bit == 0` 直接 false |
| 28 | `Initialize` 对无效配置一律返回 `kUnsupportedConfig: "config is not valid"` | 🟠 违反可操作错误原则（docs/10 §4）。修法：区分 `kUnsupportedCodec`（带 codec_name）与 `kUnsupportedResolution` |

第 26 条值得强调：这个 bug **不会崩溃、不会报错、编译和 250 个既有测试全绿**，只会让播放出来音画完全不同步。只有"解码真实文件并断言 pts 递增"的端到端测试能发现它。这是 M5 坚持做真实媒体测试的直接回报。

### 其他已完成

- `media/base/video_frame.{h,cc}`：`StorageType`（`kStorageOwned`/`kStorageDmaBufs`/`kStorageGpuMemoryBuffer`/`kStorageOpaque`）、平面布局表、64 字节对齐、`CreateBlackFrame` — **11 个单测**
- `media/base/native_display.{h,cc}`：X11 / Wayland / SDL2 / DRM 四种句柄 + `Wrap()` 逃生舱
- `media/base/audio_renderer_sink.h`：签名逐行对齐 Chromium（`Render(delay, delay_timestamp, glitch_info, AudioBus*)`）
- `media/base/video_renderer_sink.h`：`Render(deadline_min, deadline_max)`
- `media/base/audio_bus.{h,cc}` · `audio_parameters.{h,cc}` · `decoder_status.{h,cc}` · `media_types.{h,cc}`
- `media/base/video_decoder_factory.h`（含 `VideoDecoderCapability`，替代 `mediacodec-*` 布尔组合）· `audio_decoder_factory.h`

---

## 未完成（按里程碑）

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
| ⬜ M4 余项 | `DataSource` 后端的 `AVIOContext` 桥（内存 / fd / 自定义源） | ⬜（`ijkpp-inspect probe` 已在第八轮完成） |
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
| `tools/inspect/`（`ijkpp-inspect`） | ✅ probe / decode / sync；⬜ doctor / play / dump / golden |
| `tools/gen_options.py` · `golden_record.py` · `golden_diff.py` · `verify_e2e.py` · `build_linux.sh` · `ijkplayer-recorder/` | ⬜ 全部未建 |
| CI | ✅ quick + full 矩阵（本轮已把 `extract_constants --selftest` 加进两个 job）；⬜ `ffmpeg-matrix` 与 `e2e-linux` 仍是 `if: false`（前者注释写 "enabled at M4"，**M4 早已完成**）；⬜ coverage job 是 `lcov --summary \|\| true`，不会 fail；⬜ `check-format` / `check-cpplint` / `check-no-vendor-leak` |
| 许可证 | ✅ **本轮落地**：根 `LICENSE`（BSD-3 + 4 节第三方说明）· `media/filters/legacy/`（LGPL-2.1 全文 + README 准入规则 + 6 个文件头重写）；⬜ **法务确认（Q1/R8）仍未做** |
| 仓库卫生 | ⬜ `.clang-tidy` · `.editorconfig` 仍缺（STYLE.md 与 README §8 都列了） |

---

## 第四轮新增：分层违规与一个 flaky 测试

| # | 问题 | 性质 | 处理 |
|---|---|---|---|
| 15 | `media/` 反向依赖 `player/public/`：`MediaError`、`MediaInfo`、`PlayerConfig`、`StageReachedPayload` 都住在 SDK 层，但 `media::Demuxer` 要用 | 🔴 **架构违规**（与上轮 `DataSourceDescriptor` 同类，说明这不是偶发而是系统性倾向） | 把 `MediaError`/`MediaInfo` 下沉到 `media/base/`，`player/public/` 改为转发头；`Demuxer::Initialize` 改用新的 `media::DemuxerOptions`；阶段埋点改走 `MediaLog` |
| 16 | 上述违规**没有任何规则能自动发现** | 🟠 治理缺口 | **新增 invariant C22**：`base/`、`media/` 出现 `#include "player/` 即 fail。上线当天又抓到 5 处 |
| 17 | `VideoFrameQueueTest.ConcurrentProducersAndConsumersLoseNoFrames` **偶发失败** | 🔴 **flaky 测试**，drain 循环 `Peek()` 后用**阻塞** `Pop()`；消费者抢走那一帧后主线程永久阻塞（`Abort()` 永远不会来）。低竞争侥幸通过，CPU 饥饿即挂 | 改为轮询 `size()`/`reserved_count()`，无竞争窗口。**连续 12 轮 0 失败**验证 |
| 18 | `av_dict_iterate` 是 FFmpeg 6.0 才有 | 🔴 跨版本 | `IJKPP_FFMPEG_HAS_DICT_ITERATE` 分支，5.x 走 `av_dict_get(..., IGNORE_SUFFIX)`。**7.1.1 与 5.1.9 双版本编译验证** |
| 19 | `FindFFmpeg` 版本正则用小写组件名（实际宏是 `LIBAVCODEC_VERSION_MAJOR`）→ 版本解析为空 → **最低版本门禁形同虚设** | 🔴 静默失效 | `string(TOUPPER)`；现在正确输出 `libavcodec major 61` |
| 20 | `RefCountedThreadSafe` 派生类析构规则，我写进 `ref_counted.h` 后自己又违反一次（`DataSource`） | 🟠 | 按规则改为 `virtual ~` + friend |
| 21 | **测试数据本身是错的**：`sine` lavfi 源默认**单声道**，而测试断言 stereo；`truncated_tail.mp4` 截断了 MP4 尾部的 moov，导致连 `open` 都失败（而我想测的是"能打开但提前 EOF"） | 🟠 测试前提错误 | 用 `aformat=channel_layouts=stereo` + `-ac 2` 重造立体声文件；用 `-movflags +faststart` 把 moov 移到头部再截 70%，才是"能打开但 mdat 截断" |
| 22 | `media/filters/ffmpeg_demuxer.h` 的私有方法签名里出现了 `AVFormatContext*` / `AVPacket*` / `AVStream*` | 🟠 违反 C4（media 层不得见 libav 类型） | 改为 `void*` + `.cc` 内部 `Ctx()` 转换，头文件零 libav 类型 |
| 23 | `AtomicFlag` 只有 `Set()`（Chromium 是单向的），但 demuxer 的 interrupt flag 每次 seek 后必须能清 | 🟠 API 缺口 | 加 `Reset()`，并在 `atomic_flag.h` 写明理由 |
| 24 | **C18 误报**：错误提示文案里的英文单词 "try"（`"...reachable (try \`ffprobe ...\`)"`）被当成 `try` 关键字 | 🟠 检查器缺陷 | 检查前**先剥离字符串字面量再剥离注释**，规则只匹配真实代码 |
| 25 | 为满足 C2（函数 ≤80 行）把 `avformat_open_input` 抽成 helper，helper 按值收 `void* ctx` → 失败路径只置空**局部副本**，`format_ctx_raw_` 悬空 → **析构 double-free，10 个端到端测试全 SEGFAULT** | 🔴 **我为了满足风格规则而引入的严重 bug** | 回滚抽取，改为**带理由的 C2 豁免**（豁免文本记录了这次事故） |

### ★第 25 条值得单独讲

为了把 `OpenOnDemuxThread` 压到 80 行以内，我把 `avformat_open_input` 抽成 `RunAvformatOpenInput(void* ctx, ...)`。**按值传指针**意味着 `avformat_open_input(&ctx, ...)` 在失败时释放并置空的是**局部副本**，调用方的 `format_ctx_raw_` 仍指向已释放内存，`~FFmpegDemuxer()` 随即 double-free —— 10 个端到端测试全部 SEGFAULT。

三点教训，都已固化：

1. **测试是唯一抓住它的东西。** 编译通过、invariants 通过、静态检查看不出悬空指针。是"对真实媒体跑一遍"的端到端测试立刻炸了。这印证了 M4 把真实容器测试当收口条件的决定。
2. **风格规则不该以引入 bug 为代价满足。** 正确做法是登记豁免并写明理由，而不是硬拆。豁免文本里现在直接记录了这次事故，下一个人看到就知道为什么这个函数是线性的。
3. **"AVFormatContext 的生命周期必须线性可见"** 现在写进了 C1 豁免理由：把这个生命周期拆到多个文件，正是 use-after-free 的温床。

同一轮里还修了一个我自己写的 double-free 隐患：`avformat_open_input` 失败时 FFmpeg 会 free 并置空它的参数，原代码没有同步清 `format_ctx_raw_`（回滚版本已加 `format_ctx_raw_ = nullptr;` 并注明原因）。

---

### 关于 DRAFT 文件

~~`media/filters/ffmpeg_demuxer.{h,cc}`（924 行）本轮写了但没写完，且不能编译。~~
**（第五轮已转正：DRAFT 标记移除，进入 `ijkpp_platform_ffmpeg` target，12 个端到端测试全绿。）**
当时的处理方式，作为流程记录保留：
- 文件头显著标注 `STATUS: DRAFT — NOT YET IN THE BUILD`，并逐条列出 4 个缺口
- **从所有 CMake target 排除** —— 不能编译的文件绝不可从构建可达
- `check_invariants.py` 对 DRAFT 文件豁免风格/长度规则（C1/C2/C18），但**仍强制 C20（命名空间）与 C22（分层）**，并在每次运行末尾打印 DRAFT 清单，防止被遗忘

这是刻意的取舍：与其把 900 行半成品塞进构建让 CI 变红，或删掉丢失设计成果，不如显式标注 + 排除 + 可追踪。

---

## Sanitizer 抓到的 4 个真 bug（第二轮产出）

这四条都不是测试写错，是实现或 API 设计的真问题，且**全部只在 sanitizer 下暴露**——普通构建 187/187 全绿：

| # | 缺陷 | 发现者 | 影响 | 修复 |
|---|---|---|---|---|
| 1 | `TaskQueue::Quit()` 不持锁就 `Broadcast()`，而 `Run()` 在持锁前检查 `quit_` → **lost wakeup，`Stop()` 永久挂起** | ASan（时序变慢才触发） | 🔴 这就是 ijkplayer "release 卡死" 那一类 bug（病灶 11 / R5） | `Quit()` 在锁内置标志；`Run()` 在锁内检查 |
| 2 | `WaitableEvent::Signal()` 在锁外 `notify_all()` → 等待者可能在 notify 返回前销毁对象 → **use-after-free** | TSan | 🔴 | notify 移到锁内；并在头文件写明析构契约 |
| 3 | 任务捕获自己所属 `TaskQueue` 的 `scoped_refptr` → **引用环，队列永不析构** | LSan | 🟠 真泄漏 | 在 `task_queue.h` 写死 RULE：绝不强引用自己所在的 sequence，用 `WeakPtr` 或 `Clear()` |
| 4 | `BindOnce` 里右值 `WeakPtr<T>&&` 匹配到泛型转发重载而非 `WeakPtr` 专用重载 → **未解引用成 `T*`**；且 `WeakPtr::get()` 传入会**静默关闭失效保护**（实测死对象被调用，`touches == -1`） | 普通构建 + TSan | 🔴 直接击穿 `WeakPtr` 这个核心安全机制 | 补右值重载；陷阱写进 `weak_ptr.h` 类注释 + 回归测试 |

第 4 条尤其说明问题：`BindOnce(lambda, weak_ptr.get(), ...)` 能编译、看起来完全正确，但 `WeakPtr` 的全部价值消失了。这类"沉默失效"只能靠 sanitizer + 针对性测试发现。

### 另外由 `check_invariants.py` 抓到的

| 规则 | 问题 | 处理 |
|---|---|---|
| C5 | `base/threading/platform_thread.cc` 含 `__linux__`（核心层出现平台宏） | 按 Chromium 做法拆成 `platform_thread_posix.cc`，并给 `base/` 的平台后缀源文件开一个**有理由的窄豁免**（`PLATFORM_SUFFIX_RE`） |
| C20 | `location.h` / `thread.h` / `platform_thread.h` 命名空间开合不匹配 | 修正 |
| C2 | `DecideNextFrame` 139 行、`Render` 90 行 | 拆成 5 个 helper，分别降到 78 / 40 行 |

---

## 本轮 (f) 未执行

`(f) platform/linux/gl 最小出画 demo` 未开始。原因：它依赖 M2（task runner）或至少要一个能跑起来的 sink 骨架，而当前 `player` 层还是 M8 骨架（`SetDataSource` 返回 `kNotImplemented`），没有可播放的数据通路。

**建议调整顺序**：`(f)` 改为在 M11 执行，此前先用 `examples/headless` + `SyntheticDemuxer`（M10）验证端到端。若你希望**先看到画面**，最短路径是：
1. 写一个 200 行的独立 `tools/gl_probe.cc`（不进 SDK，直接用 FFmpeg C API 解码 + OpenGL 贴一帧），验证 GL/EGL/X11 工具链与着色器 —— 约 0.5 天，可与 M2 并行；
2. 正式后端仍在 M11/M12。

---

## 代码规模现状（第八轮实测）

```
base/            33 头 + 11 源    3682 行   (M1+M2 完成)
media/           37 头 + 29 源    8958 行   (M3~M6 + DecoderStream 完成)
player/          12 头 +  7 源    1906 行   (M8 契约完成，实现待 M7)
platform/         5 头 +  4 源     730 行   (ffmpeg 适配层；sdl2/linux 待 M11/M12)
tools/inspect/    1 头 +  5 源     894 行   (本轮新增：probe/decode/sync CLI)
tests/           22 个测试文件    5772 行
cmake/            6 个模块        ~340 行
docs/            13 篇           ~9100 行
──────────────────────────────────────────────────
C++ 代码合计约 21900 行，其中测试 5772 行（26%）
166 个 .h/.cc 文件
```

测试用例数：`no-ffmpeg`/`debug`/`asan`/`tsan` 各 **287**，`linux-ffmpeg711` **322**
（差值 35 = 需要 FFmpeg 与真实媒体的用例）。

**五个配置全部通过，零警告，`check_invariants.py` 14 条规则全通过（166 文件）。**

本轮净增：音频解码链（4 个新文件）、`DecoderStream<Traits>`（泛型化解码流）、
`ijkpp-inspect` CLI（5 个文件）、29 个新测试用例，以及 3 个真 bug 的修复
（#32 主时钟 uptime 偏移、#33 水位线未生效、#34 并发测试启动竞态）。
