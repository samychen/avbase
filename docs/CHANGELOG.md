# CHANGELOG

> 格式沿用 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。
> 语义化版本。**每条变更关联代表性 commit**；完整过程记录（第九十五轮前为
> 英文、之后统一中文）见 `git log` 与 docs/archive/PROGRESS-rounds-1-9.md。
>
> 2026-10-18 起提交信息统一中文（`656dbac` 之前的英文历史未改写——历史
> 即历史，改它会让「当时决定了什么」失真）。

## [0.1.0] - 2026-10-18

首个发布版：M0–M13 质量收口完成（docs/08 路线图的本段范围）。
「可发版」原定义含 Golden 对齐 + Sanitizer 全绿 + 基准达标 + SDK 文档四件事，
0.1.0 实测只达成 Sanitizer（CI full matrix asan/tsan/ubsan）与 SDK 文档两件；
Golden 对拍与微基准未落成，如实登记在文末 Known limitations（docs/12）。

### Added（首发即有的能力面）

- **播放器门面**：`player/public/` 冻结面——`Player` / `PlayerConfig` /
  `Deps` / `PlayerBuilder` / `PlayerEvent` / `PlaybackStats` / `MediaInfo`，
  事件驱动（`SetEventHandler`）与阻塞（`PrepareSync`/`StopSync`）双形态。
- **管线**：demux → decode → render 三腿 + 文本腿；背压、serial  seek、
  三级缓冲水位（`first/next/last_high_water_mark`）。
- **原生多源**（docs/14）：`CompositeDemuxer` 聚合 N 源，每源一对渲染腿，
  共用一个 pipeline 与一个时钟；`AddSource` 即画中画/多图层入口。
- **协议**：HLS / HTTP / RTMP / RTSP / SRT 播放验收全绿
  （`tools/check_live_protocols.py`，RTMP ≈3.9s / RTSP ≈3.0s / SRT ≈3.6s 起播）。
- **硬解**：VideoToolbox（macOS）/ VAAPI（Linux）/ D3D11（Windows）spec 与
  选择链；`DecoderPreference` 四档 + `HwCodecMask` 放行掩码 + 回落事件。
- **零拷贝取帧**：`VideoFrame::NativeHandle` + `examples/pull_frames`
  （GPU 帧直通，不读回）。
- **选项面**：注册表 9 个 key 全部有真实读取者，未知 key 报错并带
  "did you mean"（Δ2）——不接受「能读能写但不做事」的选项。
- **C ABI**（M15）：`player/public/c/avbase_c.h`，60 个导出函数，C99 可编译，
  已接门禁（`f485d53`、`3cc34fb`）。
- **转码**：`media/transcode/` 离线转码 E1–E5 + E3b 异步 + E5b 平台表外置。
- **测试与门禁**：unit / corpus（315 样本，`5f3eff0`，CI 跑 full，
  `4b2d2c8` 起阻塞）/ fuzz（libFuzzer + standalone 双驱动）/
  `tools/gate_local.sh` 本地复刻 CI 序列（`aee6c2f`）/
  二进制新鲜度门禁（旧二进制曾造成误诊，第八十五轮起）/
  apidocs 门禁（doxygen 零 warning + filter 生效断言 + API.md 新鲜度，
  CI 与 gate_local 同构，`bde3e8c`）。
- V9 注释覆盖率门禁（`e77c476`）：tools/check_comment_coverage.py 行状态机解析
  （无 libclang 依赖），player/public 四问注释存在率 ≥95%；gate_local
  --jobs commentcov 与 ci.yml commentcov job 同构（--min-existence-rate 95
  --min-full-rate 0，python3 无外部依赖）。解析器修两处漏计 bug 后实测 170 个
  public 声明、存在率 100%。
- V10 API 兼容性门禁（`a945db9`）：tools/check_api_compat.py 抽取
  player/public 声明 API 面（函数/方法/运算符/构造析构 + 枚举 + 每类虚函数表
  布局）并与入库基线 tools/api_baseline.json 做 diff（纯 Python，无 libclang、
  无构建）；删符号/改签名/枚举值变/虚表变判 breaking（rc=1），纯新增判
  compatible。gate_local --jobs apicompat 与 ci.yml apicompat job 同构。基线由
  --snapshot 生成、仅切 MAJOR 时带 --allow-breaking 刷新。v1 不跟踪公开数据成员。
- 微基准自包含 harness（`64c13e8`）：`bench/` 落地零依赖 micro-bench——
  不用 Google Benchmark（沙箱取不到，CI 也不该装），并接上 `AVBASE_BUILD_BENCH`
  （此前该 option 定义了但**全树无人消费**，没有任何 benchmark target）。只链
  `avbase::base`，所以 `AVBASE_ENABLE_FFMPEG=OFF` 也能编能跑。首批两项：
  `BM_PostTask_RoundTrip` ≈ 8.6 µs、`BM_BindOnce_Invoke` ≈ 118 ns（mac 实测）。
  趋势守护 `tools/check_bench_regression.py` 对比入库基线
  `tools/bench-baseline.jsonl`（回退 >10% 告警）；`gate_local --jobs bench`
  本地硬判定，`ci.yml` 的 bench job 只记录并上传 artifact——共享 runner 噪声
  大，且入库基线测于 mac、CI 跑在 linux，跨平台绝对值不可比，硬 fail 会假红。
- 微基准扩到 media 层（`129c6f3`）：新增 `BM_DecideNextFrame` ≈ 15.5 ns、
  `BM_GetMasterClock` ≈ 6.3 ns（mac 实测 p75），即 docs/04 §8 预算里最紧的两条
  （< 1 µs / < 200 ns），实测在预算内两个数量级；`bench/` 因此改为链
  `avbase::media`，FFmpeg 仍不需要。
- 微基准扩到 player 层（`0952d06`）：新增 `BM_EventHub_PostVisit` ≈ 1.94 µs
  （mac 实测 p75），即 docs/04 §8 预算里的 `EventHub::Post()` < 3 µs；`bench/`
  因此改为链 `avbase::player`（传递 media），FFmpeg 仍不需要。形态是「发一批
  + 末尾一个 FIFO 哨兵等排空」，不是「发一个等一个」——后者测的是两次线程
  唤醒（≈10 µs 量级，见 `BM_PostTask_RoundTrip`），不是预算写的那条路径；
  故数字是 **max(入队, 派发)**，两侧任一变慢都推高它。噪声源是「每事件一次
  唤醒」而不是「跨线程」本身：安静时三次 p75 落在 1892–1947 ns（±2%），有
  负载时最高 +23%，故给单独容差 35%（低于 PostTask 的 40%、高于 default 的
  25%）。突变反证（每次迭代发两个事件）→ +100.3% → 判 REGRESSION，其余 4 项
  无误伤。
- 微基准避坑（同一 commit）：`EventHub` 以**静态存储期**对象析构会让进程在
  退出时崩溃（实测 6 次中 5 次 SIGABRT、1 次 SIGILL，且都发生在数字全部打印
  完之后）——`~EventHub` 会 join 派发线程，而静态析构期 base 的进程级状态已在
  拆解。常规作用域内析构安全（同形态 3/3 rc=0）。bench 改为进程内共享一个
  有意泄漏的 hub；每轮栈上重建虽也能跑，但首轮冷线程+冷调度会高出 32%
  （2644 vs 1978/1997 ns），共享版更稳。
- 端到端 CPU 回退的机器判据（`6e11e7b`）：`tools/measure_e2e_cpu.py` 用
  `headless` 完整播一遍 1080p30 H.264（片源由 ffmpeg 生成并按参数缓存），取
  子进程 CPU 时间（user+sys，getrusage）÷ 媒体时长 = `cpu_ms_per_media_s`，
  5 次取中位数，与入库基线 `tools/e2e-cpu-baseline.jsonl` 比较——复用
  `check_bench_regression.py`（为此新增 `--value-key`，判定/基线/容差逻辑
  不重写第二套）。mac 实测 ≈ 45.6 ms/媒体秒（约一个核的 4.6%）。判据
  **15% 而非 5%**：零改动时单次 43.1–53.2（8–16%），两组 5 次的中位数差
  8.1%，且噪声是**比例性**的（4K 片源也没让它变小），摊薄启动开销无效。
  鉴别力：合成负载烧 1 CPU 秒 → +175% → 判 REGRESSION。**敏感度局限如实记**：
  null sink 不做像素处理，换 4K 片源仅 +12%（低于门槛），故它守的是解码与
  管线的 CPU，渲染 CPU 仍靠 `tools/bench_render.sh` 人工。CI 在已构建的
  headless 上顺手测一遍并上传（基线测于 mac 硬解、CI 跑 linux，绝对值不可
  比），本机 `gate_local --jobs e2ecpu` 硬判定（约 1 分钟）。
- 微基准测量方法修正（同一 commit）：① 每轮迭代数自适应放大到跑够 20 ms——
  此前 5 ns 的 op 只跑 250 µs 一轮，三次连跑出过 5.3/13.6/6.4 ns；② 判定量从
  mean 换成 p75；③ 基线取 3 次运行中位数。另新增 `tools/bench-tolerance.json`：
  docs/04 §8 的 10% 是设计门槛，低于本机单次运行噪声（实测无改动波动 13–21%，
  已发生零改动误报），故判定阈值设 25%（PostTask 40%）。降噪而非放宽：突变
  反证（给 DecideNextFrame 加倍工作量）→ +120% → 判 REGRESSION，其余无误伤。
- golden 对拍 diff 件（`64c13e8`）：`tools/golden_diff.py` 按 docs/07 §7.4
  容差分级对比两份 JSONL 录制（帧数 / pts ±0、视频 hash 汉明距离 ≤5、事件序列
  类型与顺序一致、arg ±5%、`late_us` ±5000 µs、`stat` 跳过）+ `allowlist.yaml`
  豁免；自带 selftest（7 类判定）与突变反证。如实收窄一处：`audio.checksum`
  按全等判定——格式里它是 CRC32，表达不了「逐样本差 ≤1 LSB」，需豁免走
  allowlist。**录制基线仍缺**（需 ijkplayer 原版录制，Q8 未决 + 沙箱无原版），
  故 CI 不跑对拍。
- **文档**：COOKBOOK 22 条（每条按真实签名核对，代码块由
  `examples/doc_snippets` 编译验证）/ TROUBLESHOOTING 13 症状 /
  MIGRATION（docs/05 全部 Δ 项）/ EXTENDING / PERFORMANCE / API（Doxygen，
  `bde3e8c`）。

### Changed

- 提交信息统一中文（`656dbac`；2026-10-18 起）。
- `max-buffer-size` 口径修正为「全播放器预算、按流拆分」
  （`SplitQueueBudget`，`c573195`）：此前按每流全额传递会让 A/V 文件默认
  内存包络从 15 MiB 翻倍到 30 MiB，且无测试读回上限——静默回归。
- `examples/play_sdl2.cc` 486 → 161 行：主机样板拆入
  `examples/sdl2_demo.h`（docs/10 §12 的 ≤300 度量，`8c7a2bc`）。
- CMake 预设移除 `targets` 收窄（`2f027b8`）：`--preset macos-sdl2` 曾只建
  `play_sdl2`，漏掉 `player_unittests`/`headless`，CI 的 ctest 步长期隐形红。

### Fixed

- **EOF 之后到达的 seek 永不回调**（`0cb2aaf`，第九十三轮）：demux 循环在
  `AVERROR_EOF` 处结束后无人消费已发布的 seek 请求，调用方等到自身超时。
  症状易误判为死锁（实测两线程都空闲在 `TaskQueue::Run()`——不是互等，
  是没人做这件事）。修法：循环仍在 EOF 结束（驻留会饿死排队的 Flush），
  由 `StartPlayingFrom` 重挂；所有权规则「谁把 `LoopState` 置 kRunning 谁
  PostTask」。确定性红测 5/5 红 → 修后绿 → 禁用重挂 3/3 红。
- **nodisp 运行期虚报 presented**（`c573195`）：宿主晚到的
  `SetVideoSurface(有效 display)` 会把 sink 切出 discard 模式。
  新增 `always_discard` 构造位，配置层决定压得住晚到目标。
- **关闭竞态**（第二十三/二十四轮）：`~PipelineImpl` CHECK 崩溃 14/29 →
  0/35，改等 `!IsRunning()`。
- **VOD 完成链死锁**（第八十轮）：fps-cap 丢帧时解码泵饿死；re-arm 条件
  改为 present 或 drop 都触发。
- **`buildPresets.targets` 截断陷阱**（第九十轮，`2f027b8`）：见 Changed。
- **审查发现的三处假门禁**（第八十四轮）：check-clang-tidy 三处失效
  （header-filter 负向前瞻不支持 / 可选后端不在编译库 / HeaderFilterRegex
  未锚定 vendored glad），exit=127 让门禁看起来绿。

### Known limitations（0.1.0 未含，见 docs/12）

- Golden 对拍：diff 件已建（`tools/golden_diff.py`，按 docs/07 §7.4 容差分级
  判定 + `allowlist.yaml` 豁免，自带 selftest 与突变反证），但**录制基线不在
  仓库**——需 ijkplayer 原版录制（docs/07 §7.3），被开放问题 Q8（锁哪个
  ijkplayer 版本）卡住、沙箱无原版，故 CI 不跑对拍（无基线可对比）。
  `tools/bench_render.sh` 仍是人工触发的单机基准。
- 微基准：已落地 base 层两项（`bench/` 自包含 harness，无 Google Benchmark；
  `BM_PostTask_RoundTrip` / `BM_BindOnce_Invoke`），有入库基线
  `tools/bench-baseline.jsonl` 与趋势守护 `tools/check_bench_regression.py`；
  `gate_local --jobs bench` 硬判定，CI 只记录不硬 fail（跨平台噪声）。
  **仍缺** media/player 层 `BM_*` 与端到端 CPU 门槛的机器判据。
- 显示侧零拷贝未实测（需 Linux VAAPI-EGL / Windows D3D11-GL 硬件环境）。
- E5 硬件编码器本体未实测（VideoToolbox / VAAPI / NVENC）。
- soak 48h 未跑（沙箱禁读他进程 RSS，需 CI 长跑机）。
- Qt 门面（`player/qt/`）未建；C ABI 已作跨语言挂点。
- 产品化能力（录制 / 推流 / 缩略图 / HDR tone mapping / PBO / 色彩管理）
  全部未做，见 docs/TODO §1。
- V1（新人冷启动实测）与 V10（abi-compliance-checker）待人工/CI 环境。

[0.1.0]: https://github.com/samychen/avbase/releases/tag/v0.1.0
