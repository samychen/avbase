# TOOLS · `tools/` 逐文件说明

> 更新：**2026-10-23**。`tools/` 下 44 个受版本控制的文件，**没有一个是产品代码的
> 一部分**（唯一例外是 `tools/inspect/`，它本身就是交付给宿主的诊断 CLI）。
> 这个目录装的是四类东西：**门禁**、**行为验收**、**度量**、**生成器**。
>
> 与 [BUILDING §5](BUILDING.md)（"不需要编译器就能跑的四个工具"）互补：那一节讲
> 怎么用，本文讲**都有什么、该挑哪个、红了该信几分**。
> 姊妹篇：[EXAMPLES.md](EXAMPLES.md)（`examples/` 逐文件说明）。

---

## 0. 三十秒速查：我要……该跑哪个

| 我要 | 跑这个 |
|---|---|
| 提交前把 CI 那套在本地过一遍 | `tools/gate_local.sh --all` |
| 只查架构/风格不变量 | `tools/check_invariants.py` |
| 确认我测的二进制是新的 | `tools/check_binary_freshness.py --root build/<preset>` |
| 看一帧渲染各环节花多少 | `tools/bench_render.sh build/<preset>` |
| 看整片播放的 CPU 代价 | `tools/measure_e2e_cpu.py`（经 `gate_local --jobs e2ecpu`） |
| 看微基准有没有退化 | `tools/check_bench_regression.py` |
| 验证协议能不能播 | `check_protocols.py`（HLS/HTTP）· `check_live_protocols.py`（RTMP/RTSP/SRT） |
| 验证弱网不挂死 | `tools/check_weaknet.py` |
| 长跑看内存曲线 | `tools/run_soak.py` |
| 改了公共头，重生成 API 文档 | `tools/gen_api_docs.sh` |
| 装依赖 | `setup_ffmpeg.sh` / `setup_glfw.sh` |

---

## 1. 门禁（gate）

### 1.1 `gate_local.sh` — 本地复刻 CI

`tools/gate_local.sh --jobs <name...>` 或 `--all`。它存在的原因是**这个沙箱看不见
GitHub Actions**（代理掐掉了 api.github.com），而 CI 是"这棵树是否还守着自己的门"的
唯一权威；看不见就等于每次 push 后盲飞。所以它跑的是 **CI 跑的同一条命令序列**。

19 个 job（`ALL_JOBS`）：

```
inv fmt lint tidy apidocs commentcov apicompat bench e2ecpu macos capi qt \
core ffmpeg e2e asan tsan ubsan coverage
```

映射关系（脚本头注释有完整表）：`inv`→`check_invariants.py`、`fmt`→check-format、
`lint`→check-cpplint、`tidy`→check-clang-tidy、`apidocs`/`commentcov`/`apicompat`→
同名 CI job、`macos`/`capi`/`qt`/`core`/`ffmpeg`/`e2e`→对应 preset 的 build+ctest。

### 1.2 `check_invariants.py` — 结构不变量

执行 [02 §2.1](02-总体架构与模块划分.md) 与 [06 §9](06-CMake工程与构建体系.md) 的
规则，**只留编译器和构建系统自己抓不到的那几条**：

| ID | 规则 |
|---|---|
| C1 | 单文件 500 行（逼真的接缝必须被命名） |
| C4 | FFmpeg 头留在隔离区（no-ffmpeg 构建要活） |
| C5 | platform 头/宏不得进核心层 |
| C16 | `media/base` 不依赖 `media/filters`（接口 vs 实现） |
| C22 | `media/`、`base/` 不依赖 `player/` |
| C23 | 80 列棘轮（见 `column_baseline.txt`） |
| C24 | 目标列出的每个源文件都存在 |
| C25 | 树里每个 `.cc` 都被某个目标编译 |
| C26 | `media/` 不 include `platform/` |
| C27 | 核心 media 不 include `media/ffmpeg/` |
| C28 | `render/` 不 include `media/`/`platform/`/`base/`（`avbase_gl` 零依赖） |
| C29 | `player/public/` 不出现别的模块的导出宏（V6） |
| C30 | `examples/` 不许起裸 `std::thread`（V7：例子是宿主抄的模板） |

风格类规则（C2、C7–C9、C11、C14、C17、C18、C20、C21）已在"只留能抓 bug 的"那次
清理中删掉——C18（throw）在 `-fno-exceptions` 下本来就是编译错误。**ID 不复用**，
历史才 grep 得到。

`--update-baseline` 重生成 `column_baseline.txt`；该文件**只能变小**：手工加一行
等于发一张没写理由的豁免。

### 1.3 `check_comment_coverage.py` — V9 注释覆盖率

[10 §5](10-SDK易用性设计.md) 要求每个公共方法的注释回答四个问题：做什么、哪个
线程/是否阻塞、何时失败返回什么、副作用/发什么事件。§12 把它变成可查项 V9：
**存在率 ≥95%**（四维度齐全率默认 0，只报告）。

用行状态机解析 `player/public/**/*.h`（不依赖 libclang：CI runner 不该为门禁装东西）。
**类级注释的信号继承给它的成员**——`Player` 在类作用域声明一次线程契约，逼每个
方法重复只会训练人写废话。特殊成员（构造/析构/operator）不计数。

信号是**关键词级，不是语义级**：它能抓"这条注释没说阻塞与否"，不能证明注释是真的。
这是注释门禁诚实的边界。

### 1.4 `check_api_compat.py` — V10 API 兼容性

次要版本升级不能让已有用户代码编不过（[10 §4](10-SDK易用性设计.md)）。**不调用上游
的 abi-compliance-checker**（要 Perl + abi-dumper + 完整编译产物），而是用 V9 同一套
状态机抽出**声明出来的**公共 API 表面，与 `api_baseline.json` 比对。

判为破坏（rc=1）：公共函数/方法/构造/析构/operator 被删除；签名变化（返回类型、
参数类型、参数个数、成员函数 const 限定）；C ABI 函数签名变化；枚举项被删或值变；
类的虚函数集合变化（vtable 位移会打断外部子类）。

判为兼容：新增符号；给已有类加非虚方法。

`--snapshot` 重写基线，`--allow-breaking` 只在切 MAJOR 时用。

### 1.5 `check_link_deps.sh` — V3 最小集成依赖

"最小集成只需要 FFmpeg"。本树发的是**静态库**，`ldd` 无话可说，所以这个性质在它
真正所在的地方查，分两半：

1. **LINK**：核心归档不得引用任何后端的符号（SDL2 / GLX / EGL / PulseAudio / ALSA /
   X11 / Wayland）——后端是运行时 dlopen 的，硬引用会把可选后端变成必需依赖。
2. **HEADERS**：只包含 `player/public/*.h` 的 TU，必须**只要 FFmpeg 的 include 路径**
   就能编过。

脚本头的教训值得记：LINK 那半的第一版是个带 `-i` 的单行 grep，于是 `avbase::gl::
GlApi::Load(NativeGLContext*)` 里的 "eGL" 被当成 EGL 依赖——**一个对着自己代码库的
名字开火的门禁，会逼人把正则放宽到它通过为止**。所以：先 demangle，再大小写敏感
匹配，并逐个点名"别人的"前缀。

### 1.6 `check_coverage.py` — 覆盖率门禁

[07 §12](07-测试策略与可观测性.md) 的阈值是**分模块、分指标（行 + 分支）**的，还有
两个模块被要求 100% 行覆盖。这些状态塞不进 shell 一行式；而"只有 CI 能跑的 YAML 块"
正是门禁腐烂的形状。前一版是 `lcov --summary ... || true`——**构造上就不可能失败**。

两种模式：默认（绝对值）是规约的可执行形式；`--ratchet` 把今天测到的数当地板，
让覆盖率只能涨。**阈值不可从命令行调**：能让一次 CI 调用悄悄放过一个本该红的构建，
正是 `|| true` 的那个失效模式。

### 1.7 `gen_api_docs.sh` — `docs/API.md` 生成 + 文档门禁

验收判据是 **doxygen 零 warning**，这里用两种方式断言：`WARN_AS_ERROR=YES` 让
doxygen 自己非零退出，另外直接检查 WARN_LOGFILE——warning 既不能悄悄过，也不能藏在
零退出码后面。缺 doxygen 是**硬失败（rc 2）不是跳过**。

第三道门：INPUT_FILTER 必须真的跑过。doxygen 在 filter 输出为空时**是静默的**——空流
解析成空文件，两道 warning 门全绿而每个 brief 都没了。这个假绿在本地真出现过
（doxygen 用 stdin 传文件且**不带 argv**，filter 于是打印了它的用法行）。

管线：`docs/Doxyfile`（XML 后端 + INPUT_FILTER）→ `doxy_xml_to_md.py` → `docs/API.md`。
**`docs/API.md` 是生成物，禁止手改**；门禁判据是重新生成零 diff。

---

## 2. 度量（性能）

### 2.1 `bench_render.sh` — GL 渲染端到端实测

用 `play_glfw` 播合成片，打印 sink 测到的四段耗时（upload / compose / blit /
present）、输出帧率与 upload_path，外加画布尺寸与场景（`SCENE=pip|fade`）。

```
tools/bench_render.sh [build-dir] [seconds-per-clip] [canvas WxH]
```

**测量前先查新鲜度**：Ninja 树陈旧就拒绝测（只问它真正要跑的 `play_glfw`，用
`check_binary_freshness.py --binary`），非 Ninja 树 warn 不阻断。这道门是踩出来的：
一个早于修复三天的树让 `play_glfw` 播到约 40 帧就"饿死"，读起来正像一次渲染回归。

需要 display 与 ffmpeg CLI。数字的解释见 [PERFORMANCE.md](PERFORMANCE.md) §6。

### 2.2 `measure_e2e_cpu.py` — 整片播放的 CPU 代价

回答一个问题：**1080p30 播一秒要多少 CPU**。量的是子进程 CPU 时间
（`getrusage(RUSAGE_CHILDREN)`）÷ 片源媒体时长 = `cpu_ms_per_media_s`。

- 不用墙钟：播放是按实时节流的一—片源快 20% 墙钟也还是 ~10 s，那是常数加噪声。
- 不用 RSS：本沙箱禁读他进程 RSS，且 [04 §8](04-线程模型与数据流.md) 预算的是 CPU。
- 片源**现造不入库**（1080p30 H.264 几十 MB 不该进 git），文件名带参数，改参数即重造。

噪声地板是阈值不是 5% 的原因：同一二进制、不改代码，单次跑在 45.7–53.2 ms/媒体秒
之间（8–16% 抖动）。经 `gate_local --jobs e2ecpu`，5 次取中位数，对
`e2e-cpu-baseline.jsonl` 比，容差见 `e2e-cpu-tolerance.json`。

### 2.3 `check_bench_regression.py` — 基准趋势守卫

比较当前基准与 git 跟踪的基线。**这是趋势守卫，不是硬性 PR 门**：CI 机器共享且吵，
>10% 抖动是常态，所以默认只报告，要 `--fail-on-regression` 才失败（本机安静，结果
才可行动）。

比的是 **p75 而不是均值**；基线是**已记录各次运行的中位数**，所以追加多次运行再
`--update-baseline` 才是预期用法——一次运行决定太多。逐项噪声地板在
`bench-tolerance.json`（10% 阈值伺候不了 6 ns 的 seqlock 读和跨线程往返两种量级）。

同一个比较器也服务于单位不是 ns/op 的生产者（`measure_e2e_cpu.py`）。

### 2.4 `check_binary_freshness.py` — 被测物新鲜度

**任何测量之前先跑它**。两种陈旧形态：

1. **部分构建**（`--target libX.a` 或 preset 的 `targets:` 收窄）——库比重链新；
2. **改完源码没构建**——整树一致地旧，第一种比较全绿。

所以查两遍：二进制 vs 它链接的库（读 `.ninja_log`），以及二进制 vs 喂进那些库的
源码（沿 `build.ninja` 文本边"库→对象→源"，比 stat）。源码 mtime 只有编辑器和 git
会写，构建系统从不碰，所以 stat 是权威的"何时改动"。

`--binary NAME` 把范围收窄到单个可执行文件（`bench_render.sh` 就是这么用的）。

**已知局限写在 docstring 里而不是绕过去**：只改头文件不可见（头依赖在二进制
`.ninja_deps` 中，刻意不解析二进制格式），同一次编辑里的 `.cc` 会被抓到；`ninja -n`
因 glob 重检在刚建完的树上持续误报，已被否决为判据。

退出码 0 新 / 1 陈旧 / 2 无从判断（一个"什么都没比"的运行必须是 2，不能是绿）。

---

## 3. 行为验收

| 文件 | 验收什么 | 判据 |
|---|---|---|
| `check_protocols.py` | HLS VOD 与 HTTP progressive 走**真 socket**（本地 `http.server` + 现造媒体）——"要网络但不需要网络" | 每个协议到 `kCompleted` |
| `check_live_protocols.py` | RTMP / RTSP / SRT 对**真流媒体对端**（`mediamtx` 或 ffmpeg 当 listener） | `headless` 到 `kCompleted` **或媒体时钟走过** `--media-threshold`（默认 2.0 s）——时钟真的在走才说明帧真的过了协议，不只是连上了 |
| `check_weaknet.py` | 源不回答 / 慢 / 中途挂断时播放器的行为 | 不是"播放成功"（全断时没什么可成功），而是**进程自己走到定义好的终态，且在限定时间内** |
| `check_weaknet.sh` | 直播三协议在 `tc netem` 真实损伤下的降级 | 需 Linux + root；结束时**无论如何都拆掉 qdisc** |
| `check_corpus.py` | 维度语料（codec × container × 损坏 × HDR × 多轨）的四个指标 | 语料**现造不入库**（300+ 真实媒体是几百 MB，而且入库的语料就是不再生长的语料） |
| `run_soak.py` | 长跑 | 交付物**不是"跑了 N 小时"**（那是秒表），而是**内存曲线平** + 零 sanitizer 报告 |

`check_weaknet.py` 的损伤**做在服务器里而不是 `tc` 里**：netem 要 root、macOS 没有、
于是只能在 CI 跑——而"只有 CI 能执行的门禁"正是 `check_protocols.py` 坚持把 httpd
放本地的同一个理由。

`check_corpus.py` 不把"本构建没这个解码器"算失败而是跳过：`setup_ffmpeg.sh` 是
最小解码集，CI 上发行版 ffmpeg 能解的多得多，计失败会让同一个仓库在两地一个红一个绿。

`run_soak.py` 的驱动是 `tests/fuzz` 的 standalone 构建：它推的是**真实管线正门**
（FFmpegDemuxer Initialize → 有界 Read → 完整 Stop），所以卡死的输入是一次漏掉而非
一次挂住。它不是 libFuzzer 的替代品——这里的变异是四个固定类别，找的是**生命周期**
问题（泄漏、teardown 竞争、增长），不是覆盖率引导的那些。

---

## 4. 生成与三方核对

| 文件 | 作用 |
|---|---|
| `gen_options.py` | 生成 `OptionRegistry` 表，并把 **docs/05 表 4（规约）⟷ `player_config.h`（代码）⟷ `option_map.py`（映射）** 三方对账。`--check` CI 安全，`--emit` 写 `.inc` |
| `option_map.py` | ijkplayer 选项键 → `PlayerConfig` 字段的映射表（数据）。kind 解释了迁移不是恒等的：三个键取反、 `volume` 换量程（Δ6，0..100→0.0..1.0）、四个键把毫秒送进 `base::TimeDelta` |
| `extract_constants.py` | 风险 R1（A/V 同步阈值抄错）的缓解措施 4："阈值用本工具提取，**禁止手抄**"。三方交叉：`ffplay #define` ⟷ avbase 代码 ⟷ docs/05 表 7 |
| `ported_constants.py` | 移植常量表（数据）。**单位在这里声明，绝不推断**：ffplay 自己就不一致（`AV_SYNC_THRESHOLD_MIN` 是 0.04 秒，`MAX_SLEEP` 是 1000000 微秒，`AV_NOSYNC_THRESHOLD` 是 10.0——一个整数秒，任何"是不是浮点"的启发式都分不出它和"10 个别的东西"） |
| `doxy_xml_to_md.py` | doxygen XML → `docs/API.md`。由 `gen_api_docs.sh` 调用，不单独跑 |
| `doxygen_comment_filter.py` | doxygen 的 INPUT_FILTER：把仓库用的**前置 `//` 块**与**尾注**改写成 doxygen 认得的 `///` / `///<`。不重写纯分隔线块（挂到下一个成员会变成它的 brief，比丢掉更糟）；跟踪引号状态，所以 `"http://x"` 不会被当注释 |
| `gcov_to_lcov.py` | macOS 缺 `lcov`、Apple clang 的 gcov 也不出那个格式，于是把 `.gcov` 文本转成 tracefile，让门禁只有**一种**输入格式。**行数是精确的，分支是近似的**（gcov 只说"71% taken"，不说跑了多少次：是否执行过是精确的，执行多少次是编的） |
| `golden_diff.py` | golden 对拍的 **diff 件**（[07 §7](07-测试策略与可观测性.md)）。按容差分级判 pass/fail，有意差异走 `allowlist.yaml`。**诚实边界写在文件头**：0.1.0 没有可对比的基线（录制要 ijkplayer 原版，被 Q8 卡住），所以 CI 不跑对拍；`audio.checksum` 按**严格相等**判（它是 CRC32，32 bit 摘要表达不了"差 1 LSB"） |

---

## 5. 环境搭建

| 文件 | 说明 |
|---|---|
| `setup_ffmpeg.sh [version] [prefix] [jobs]` | 从源码编一个**钉住版本**的 FFmpeg 到仓库内 `tools/build`，让所有人和所有 CI job 对着同一版本编译（Debian 12 发 5.1、Ubuntu 24.04 发 6.1，版本不定则兼容层无意义）。组件列表**刻意最小**（2 核近 3 分钟）；要什么往这里加，不要 `--enable-everything`。`AVBASE_FFMPEG_DEPS="openssl x264 srt ..."` 可加第三方，装进同一 prefix |
| `setup_glfw.sh [version] [prefix]` | 从源码编 GLFW 到仓库内，纯粹为了让 GLFW 后端在包管理器够不到 github 时**能编能跑**。avbase 本身不依赖它（后端可选，缺 glfw3 时 warning 而非 error）。macOS 上静态库按 `AVBASE_OSX_DEPLOYMENT_TARGET`（默认 11.0）编，免得链接时警告部署版本比宿主新 |
| `mediamtx/mediamtx.yml` | MediaMTX 的最小配置，只给 `check_live_protocols.py` 用：仅 loopback、无鉴权、无录制、只开被测的三个协议 |

---

## 6. 数据文件（基线 / 容差 / 棘轮）

| 文件 | 是什么 |
|---|---|
| `api_baseline.json` | V10 的对照面——上一次发布时的公共 API 表面，由 `check_api_compat.py --snapshot` 写 |
| `bench-baseline.jsonl` | 微基准基线（每次一行）。**只追加新条目**，不要整体 `--update-baseline`——一次运行决定太多 |
| `bench-tolerance.json` | 逐项噪声地板。文件里写了为什么 10% 不行：一个 6 ns 的读和一个跨线程往返用不了同一个阈值 |
| `e2e-cpu-baseline.jsonl` | 端到端 CPU 基线，同上 |
| `e2e-cpu-tolerance.json` | 端到端 CPU 容差，文件里写了实测噪声（单机 8–16%） |
| `column_baseline.txt` | C23 的 80 列棘轮基线。**工具拒绝把条目调大**，所以只能变小；禁止手改 |

---

## 7. 辅助模块（不是可执行文件）

| 文件 | 说明 |
|---|---|
| `_jsonl.py` | `check_bench_regression.py` 与 `golden_diff.py` 共用的 JSONL 解析（三行循环收在一处，免得格式长了个褶子就两边漂移） |
| `sim_wsola.py` | WSOLA 路径的**数值模型**。`audio_renderer_algorithm.cc` 的单测断言的是信号处理性质（"2x 时长减半"、"2x 音调 ±2%"），这些容差**读代码推不出来**，靠猜制造过两次"看着像实现 bug 其实不是"的失败。它是验证夹具，**不属于任何构建目标，CI 也不跑**，且 `check_invariants.py` 只扫 `.h`/`.cc`，所以它也在棘轮之外 |
| `option_map.py` / `ported_constants.py` | 见 §4（数据模块，与解析器分开是为了"加一个里程碑的配置面时，只长这一张表"） |

---

## 8. `tools/inspect/` — `avbase-inspect` 诊断 CLI

**这个子目录是产品，不是工具**：它是交付给宿主的诊断命令行，只是住在 `tools/` 下。

它存在的理由（写在 `inspect_main.cc` 头）是：ijkplayer 唯一能看到自己行为的地方是
`ff_ffplay.c` 的一墙 LOGD，而"时间基错了"、"同步修正反了号"、"解码器静默产出零帧"
这几类 bug 在那份输出里**全都看不见**。每个子命令打印的是开发者原本要挂调试器才能
拿到的中间值，走的是播放器走的那条代码路径，**没有测试替身**。

| 子命令 | 文件 | 打印什么 |
|---|---|---|
| `probe` | `inspect_probe.cc` | 容器与逐流信息 |
| `decode` | `inspect_decode.cc` | 解一遍，报帧数/时间戳范围/**单调性**。有意思的不是帧数——时间戳不单调是缺时间基的指纹（bug #26），而它在帧数里是隐形的：帧数对，全打零 |
| `sync` | `inspect_sync.cc` | 用真实时间戳驱动 `AvSyncController`。**这是抓到 bug #32 的那个子命令**：同步控制器有 28 条单测全绿（都从零开始的假时钟），而对真文件真宿主，主时钟读数是 -33185 s |
| `transcode` | `inspect_transcode.cc` | `media/transcode` 的第一个生产消费者，跑完**重新打开产出**证明它可读 |
| — | `inspect_common.{h,cc}` | 三个子命令共用的管道：参数解析、demuxer Host、任务队列 Pump |
| — | `inspect_main.cc` | 入口与用法 |

按子命令分文件是为了不顶到 C1 的 500 行（[06 §7.6](06-CMake工程与构建体系.md)：一个
目录一个目标，目录整体在顶层 CMakeLists 里开关，所以源清单是 glob，加子命令不用改
CMake）。它需要 FFmpeg——**建立在假件上的诊断 CLI 什么都证明不了**。

---

## 9. 退出码约定（全目录通用）

| 码 | 含义 |
|---|---|
| 0 | 通过 / 达到验收判据 |
| 1 | **被测物有问题**：不变量违规、基准退化、协议 FAIL、挂死或崩溃、覆盖率不达标 |
| 2 | **无从判断**：前置缺失（二进制、ffmpeg CLI、基线、解码器）、用法错、解析失败 |

区分 1 和 2 是这个目录最重要的一致性：**环境缺口不是产品回归**，报成 1 会逼人去修
一个不存在的 bug。`check_live_protocols.py` 把它做成了三档判词（PASS / MISSING ·
SERVER_MISSING / FAIL）——`MISSING` 表示这个协议没编进这个 FFmpeg 构建，`SERVER_MISSING`
表示后端提供不了服务器，只有 `FAIL` 才是"服务器起来了但播放器没往前走"。

跑门禁时**别把退出码接在管道后面**——`pipeline | tail` 后 `$?` 是 `tail` 的。要退出码
就用 `PIPESTATUS`（bash）/`pipestatus`（zsh，小写且 1 起始），或者干脆看工具自己打的
PASS/FAIL 汇总。

---

## 10. 两个生成目录（不在版本控制里）

`tools/build/`（FFmpeg / GLFW 的构建前缀，约 1.1 GB）与 `tools/deps/`（下载的依赖）
都在 `.gitignore` 里。它们**不是工具**，是工具的产物；误把它们当源文件读会得出
"tools/ 有上 GB 的代码"这种结论。
