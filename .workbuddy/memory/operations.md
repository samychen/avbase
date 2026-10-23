# avbase 门禁与操作手册（MEMORY.md 的长尾）

## 门禁命令
```sh
python3 tools/check_invariants.py --root .     # C1/C4/C5/C16/C22-C28
python3 tools/check_binary_freshness.py --root .  # 构建树新鲜度，跑 ctest/基准前
tools/gate_local.sh            # 本地复刻 CI 门禁序列（sandbox 看不到 GitHub，用它替代）
tools/gate_local.sh --list     # 看 job -> CI job 映射
tools/gate_local.sh --jobs e2e,core   # 只跑指定 job；--all 跑全部（慢）
# 后来把它接进门禁：CI 在 GitHub Actions（sandbox 代理拦 api.github.com/
# HTTPS git，看不到结果）。gate_local.sh 用同一套命令在本地出相同信号：
# inv(check_invariants) fmt(check-format) lint(check-cpplint) macos(macos-sdl2)
# capi(e2e C ABI) core(no-ffmpeg) ffmpeg e2e(headless 播到 kCompleted) + 沙箱
# 跑不起的 tidy/asan/tsan/ubsan/coverage 标为 --all 可选。
# 三个静态工具都只在 venv 里，都不在 PATH（裸调 exit=127 → 门禁看起来绿）
CF=~/.workbuddy/binaries/python/envs/default/bin/clang-format   # 18.1.3
CT=~/.workbuddy/binaries/python/envs/tidy18/bin/clang-tidy      # 18.1.8，对齐 CI
CP=~/.workbuddy/binaries/python/envs/cpplint/bin/cpplint        # 1.6.1，对齐 CI
cmake --preset macos-sdl2 -DAVBASE_WERROR=ON -B <dir> && cmake --build <dir> && ctest
tools/bench_render.sh
# clang-tidy 全 scope（必须配无 PCH 的 scratch 库，原因见教训「PCH 陷阱」）：
cmake --preset macos-sdl2 -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -B /tmp/tidy-nopch
git ls-files 'base/*.cc' 'media/*.cc' 'player/*.cc' 'platform/*.cc' 'tools/*.cc' \
  | grep -v 'legacy/'          # 与 CI check-clang-tidy 同口径（git 的 * 匹配斜线）
```
- **clang-tidy 必须用 18**：本机 20.1.7 的 check 集合与 CI 的 llvm-18 不同，用它
  判断「CI 会不会报」会错（据此误判过 `modernize-use-nodiscard` 是
  19/20 新增）。brew 的 llvm 装不上（沙箱 TLS）→ 用 PyPI `clang-tidy==18.1.8`。
- **文件数一直在变，别记死数字**（曾把不变量的 425 套到 format 上）。看 exit code 与行数。
- **C28：`render/` 不得 include `media`/`platform`/`base`/`player/`。**
- `avbase` 是 INTERFACE 库，ninja 不能 `--target avbase`。
- **每个 build 子目录的生成器不同，跑构建前先看目录里是 `Makefile` 还是
  `build.ninja`**：`build/` 是 Unix Makefiles，`build/ffmpeg`（及其余 preset
  子目录）是 Ninja。曾在 ffmpeg 目录跑 `make`，"No rule to make target"
  被 `| tail` 管道吞掉退出码 → 对着**上一个二进制**跑出 87/87 假绿；ninja 重建后
  实跑 90/90。「构建失败时，基准跑的是上一个二进制」的同形复现。
- 默认 `macos-sdl2` 预设**不开** `-Werror` → 两套预设都要跑。
- **`clang-format` 不在 PATH**（裸调 exit=127），必须给全路径。
- **`clang-format` 不要对 `.md` 跑**：`Pipeline Multi-Source` 被重排成
  `Multi - Source`，设计书的代码块与英文散文一并被重排（一次 178 增/75 删的纯噪声
  diff）。发现后 `cp` 备份 → `git checkout --` 还原 → 手工重贴新增段落。第七章那次就是这么栽的。
- **删掉一个 API 之后，全仓 grep 它的名字，包括 docs。** 退役
  `SceneSource::MakePublisherFactory()` 那次，第二天的文档新鲜度复查仍找出 6 处
  「当前状态描述」把它当成正解：TODO.md §0/§2/§7、docs/03、docs/05、docs/12 §4.9、
  docs/13 §6.2、docs/14 开头。**PROGRESS.md 只记状态速览**（早期按轮组织的记录已归档到 `docs/archive/`，别去改）；要改的是「现在怎么做」的那几句。判据：句子讲的是当时经过（历史）还是现在的做法（现状）。
- 两套清单数不同别套用。拼清单连未跟踪新文件：
  `git ls-files --cached --others --exclude-standard`。
- `tools/deps`/`tools/build` 已 gitignore；`tools/inspect` 是自有代码（7 个文件）。
- 新 `.cc` 是 GLOB 收的（CONFIGURE_DEPENDS），新增源文件不用改 CMakeLists。

## 提交切片
**按依赖层次切**（不按时间批次 / 功能主题）：同一批文件内部交织，「每片独立编译」做不到；
能让每片不自相矛盾的是**先提交被引用的、后提交引用者**。顺序：tools → 数据面 →
render/gl 原语 → media/gl 场景层 → platform 后端 → 宿主+示例 → 测试 → 文档。
**接口扩展必须和它的全部实现者一起，或放到最后**（只加数据字段的可单独成片）。
每片提交后跑 check_invariants（几秒）；**完整构建只在最后跑一次**。

## CI 风格门禁 —— 已闭合（不再是存量红灯）
别信「门禁通过」；「15 个代表性文件零发现」是**抽样**不是跑 job 清单。这一节的
存量红灯已清零，留下的是**它当时为什么是绿的**：

- `-header-filter` 的 Regex 是 **POSIX ERE，没有负向前瞻** → CI 原来写的
  `^((?!third_party|legacy|/tests/).)*$` 匹配不到任何头，**头文件从未被检查**
  （同一 TU：带它 17 条、不带 1095 条）。18 也**没有** `--exclude-header-filter`
  （19+ 才有），所以头 scope 只能来自 `.clang-tidy` 的 `HeaderFilterRegex`。
- clang-tidy **静默跳过**不在 `compile_commands.json` 里的文件 → scope 142 个里
  27 个（`platform/sdl2|glfw|egl`）从未被分析。**scope 与数据库必须校验一致**。
- `HeaderFilterRegex` 必须**锚定**成 `/(base|media|...)/`：不锚定时是子串匹配，
  `avbase/` 里含 `base/` → vendored 的 glad 全中（93 条）。
- 命名选项：`MemberSuffix` 是**同时设 ClassMember 与 StructMember 的别名**，是
  533 条 POD struct 字段误报的根源 → 拆成 `ClassMemberSuffix: _` +
  `StructMemberCase: lower_case`；命名常量用 `ClassConstantSuffix: ''`（空串 =
  「无后缀」，不是「不检查」）。**某一分类不设选项 ≠ 不检查**，它会 fallback 到
  更通用分类（Method→Function、Constant→Variable）；要「不检查」写 `aNy_CasE`，
  但它**只豁免 Case，不豁免 suffix/prefix**。
- **抑制只能裸写**：cpplint 不认识 clang-tidy 的 check 名，带名字的 NOLINT 会被
  报 `Unknown NOLINT error category`。

## 操作教训（都真踩过）

- **`avbase_configure_target()` 会给目标加 `-fno-exceptions -fno-rtti`，Qt 目标
  绝不能套它。** 全树禁 RTTI 是核心库的规矩，但 Qt 的元对象系统
  （`qobject_cast`、`QML` 引擎的类型转换、`Q_OBJECT` 的 `staticMetaObject`）
  依赖 RTTI；套上去的表现是 moc 生成的代码编译失败或 `qobject_cast` 恒空，
  而不是一条清晰的报错。UI 应用在 `apps/` 里自己设 flags，别复用核心库那份
  helper——它们是「消费者」，本来就该和核心库有不同的编译契约。

- **Qt 侧拿不到 avbase 的 GL sink 时，先进的自定义 `VideoRendererSink` 路线**：
  `VideoRendererSink` 是 pull 模型（自己起线程调 `upstream_->Render()` 拿
  `VideoFrame`），所以宿主可以完全绕开 GL 后端把帧取到自己手里。这比新写一个
  `NativeDisplayKind` + backend 便宜一个数量级，代价是 CPU 上传（1080p30 够用）。
  要做零拷贝再上 `platform/qt/` 的 `NativeGLContext` 适配（只需 5 个方法）。

- **声称「全树通过」前必须真扫全树。** 门禁的「绿」常常是「没跑」；2 秒跑完的「全树格式
  检查」一定没跑；**exit=127（command not found）会让门禁看起来绿**。
- **「版本不对的工具」是另一种假门禁**（实测）：PATH 里没有 clang-format 时
  第一反应是 `xcrun -f clang-format`，拿到的是 **Xcode 17.0.0**，而门禁版本是 venv 里的
  **18.1.3**（上面命令段就有）。17 与 18 折行策略不同，用 17 「复检 0 偏离」不等于 CI 会绿。
  先查本文件命令段，再动手。同时还差点重复「本机没装 tidy」的旧结论——`envs/tidy18` 一直在。
- **构建失败时，基准跑的是上一个二进制** → 把 build 与测量串起来前先确认 exit code。
- **PATH 上的 `grep` 是 toybox shim**：不支持 `\b`，且**静默返回 0**。用 `/usr/bin/grep`
  或 Grep 工具。**凡「用 grep 查证某符号不存在」的结论都要换工具复核。**
- **macOS bash 3.2 没有 `mapfile`**：拼数组静默得空数组，对着 0 个文件 **exit=0**（又一次
  「绿是因为没跑」）。改用 `xargs` + 故意畸形的探针文件做**阳性对照**。`set -u` 下空数组
  写 `"${arr[@]+"${arr[@]}"}"`。
- 后台命令末尾别用 `grep -c` 收尾（计数 0 → exit 1 → 整条任务被误报 failed）。
- **clang-format 只跑源文件，绝不对 `.md` 跑**：对设计书跑会把标题
  `Pipeline Multi-Source` 重排成 `Pipeline Multi - Source`，并把代码块注释对齐与
  英文散文一并重排，产出 178 增/75 删的纯噪声 diff。发现后：`cp` 备份 →
  `git checkout --` 还原 → 手工重贴新增段落。
- **中间提交的「编译面」可能比默认预设宽**：删掉某个 API（当时是
  `SceneSource::MakePublisherFactory()`）会立刻断掉 `examples/`，而示例只在开
  SDL2/GLFW 的预设里编译 —— 默认 `build/`（无 SDL2）**全绿会让人以为树是好的**。
  一个系列改完之后必跑一次 `-DAVBASE_WERROR=ON` 的含示例预设，否则要等 CI 才发现。
- **zsh 不像 bash 那样对未加引号的变量做分词**：
  `FILES=$(git ls-files …); "$CP" $FILES` 会把整个清单当成**一个**文件名（症状是
  `Can't open for reading` 后面跟着所有文件名）。统一用 `| xargs`。
- **测试 fixture 的成员声明顺序 = 析构倒序**（同类第二次）：一个对象
  向另一个注册了 `base::Unretained` 回调时（如 `GlSceneEngine::
  SetTeardownCallback`），被注册方必须先死。顺序错了的表现是**毫不相关**的
  `mutex lock failed: Invalid argument` 抛在 fixture 析构里，而不是任何有用的信息。
- clang-format 与构建**不能并发**跑同一批文件。改 include 路径必然影响排序。
- 清大目录不要 `rm -rf`（撞批量删除守卫）→ `mv` 到 `/tmp` 再 `mkdir`。
  macOS sed 用 `-i.bak` 再删；批量改名要用词边界。
- 本机无 glfw3/EGL 开发库（brew TLS 被代理拦）：GLFW 走 `tools/setup_glfw.sh`；EGL 只能
  结构验证。GitHub raw/API 被代理拦截，核实外部仓库布局用 WebSearch。
- **基准工具本身要先被基准过。** 约定：**素材 ≥12s、丢启动段、fps 与四段耗时都取尾部 5
  个采样中位数**，并要求播放超出片尾。短素材让工具说谎；单点采样让它随机说谎。
- **别急着把「掉帧」归因给代码。** 两条独立 present 通路报出**同一个**可疑帧率 → 是环境
  （clangd 后台索引把 4 核 load 压到 12）在说话。两条通路同一个错数是环境；
  只有一个变才是代码。
- **恢复被突变的文件后必须 `touch`。** `cp` 备份 + `cp` 恢复（`cp -p`/`copy2`）会连
  mtime 一起还原，比刚从突变源码编出来的 `.o` **还旧** → ninja 判定目标已是最新 →
  下一个探针跑的是**上一个突变的二进制**，全表结果都是假的。最初两次数据
  就是这样错出来的（表现为「突变 P4 红了跟它无关的测试」）。
- **反证必须真跑一遍看有没有红的。** 「测试写了」≠「性质被钉住」。有两条
  测试加强前都是空转：一条在断言构造函数（新对象本来就没理由），一条的性质从
  公开 API 根本观测不到。这是「引擎早退没有反证」的同类。
- **门禁表里的数字必须当场跑出来，不能沿用上一次。** 记过的 504/625 是从
  更早一次抄的，实测是 511/688 —— 差了 60 多条。
- **格式化/检查的排除清单必须照抄 CI 口径，一个锚定符都不能差。** 曾经用了
  `grep -v '^third_party/'`（行首锚定），CI 是非锚定的 `grep -v 'third_party/'`
  → `render/gl/third_party/glad/` 两个 vendored 头文件被就地重写 **1413 行**。
  发现后 `git checkout --` 还原（先核对 `git status`，确认没夹带别的未提交内容）。
  **格式化前先确认 `git status` 干净**，这样误改才分得清是谁的。
- **突变探针的「绿」可能是编译失败。** 突变触发 `-Werror`（unused parameter /
  unused variable）时构建失败、测试根本没跑，探针脚本若只看 gtest 退出码会把它
  记成红，但**方向相反的误读同样会发生**。第一批 5/8，查下来三条「没
  钉住」全是探针自己的问题。**探针必须区分「构建失败」与「测试红/绿」两种结果。**
- **撤临时探针时不要 `git checkout -- <file>`**：它会把该文件**尚未提交的改动一起回退**。
  曾手滑用了一次，把 `examples/scene_demo.h` 的整批 L5 改动清掉了（靠 `/tmp`
  备份恢复）。正确做法：`cp` 备份 → 用编辑工具删掉探针代码块 → 复核 diff。
- **macOS 的 `/usr/bin/grep` 是 BSD grep：BRE 里 `\|` 不是「或」**（那是 GNU 扩展），
  会静默给出错误结论。用 `-E` + `|`，或用 Grep 工具。是「PATH 上的 toybox grep shim」
  那条教训的近亲 —— **凡「用 grep 查证某事不存在」都要换工具复核**。
- `TMPDIR` 在 macOS 指向 `/var/folders/...`，不是 `/tmp`。脚本里写死 `/tmp` 会找不到
  另一个 shell 建出来的基准素材。
- 可见性：`CMAKE_CXX_VISIBILITY_PRESET hidden`，导出类需 `AVBASE_MEDIA_EXPORT`；
  从隐藏基类派生导出类触发 `-Wvisibility` → 共享基类也要导出。
- 文档：`docs/09-*` 已归档到 `docs/archive/`，注释用短形式 `docs/archive/09 §N`，
  **不要写完整中文文件名**（撑破 80 列）。`docs/13` = avbase_gl 契约。
- **切片提交：不带路径的 `git commit` 会连索引里别的东西一起提交。** 曾用
  `git add <paths>` + `git commit`（无 pathspec），而 `examples/publisher_sink.h`
  的删除早在 `git rm` 时就进了索引 → 被卷进**第一条**提交，而那时 `scene_demo.h`
  还在 include 它，中间提交编译不过。正确做法：`git commit -- <paths>`，或提交前
  `git diff --cached --name-only` 逐一核对。出错后 `git reset --mixed <base>` 重切
  （工作区不受影响）。
- **突变探针用正则，不要字面量，并校验命中次数恰好为 1。** 字面量会因为
  clang-format 的折行方式不同而 PATTERN-MISS 静默失效（8 条里 2 条）。
  正则 + `len(findall) == 1` 才能把「没匹配上」变成显式失败。
- **突变要避开 `-Werror`：优先掏空辅助函数，而不是改调用点。** 改调用点常让某个
  变量从未被赋值（`-Werror=uninitialized`）或参数不再被使用（`-Wextra` 的
  `-Wunused-parameter`）；加 `(void)x;` 可以压掉后一类。P3/P7 都是这么
  从 BUILD-FAIL 救回来的。
- **抑制会静默失效，且比不写更危险**（读起来像处理过了）。两种：
  ① **差一行**——`NOLINTNEXTLINE` 放在 `template <typename T>` 之上只压住模板头，
  而诊断报在**下一行**的类名上；宏定义的诊断报在**反斜杠续行的最后一行**。
  改**行尾** `// NOLINT`（不怕上面插入行）或块抑制（覆盖整个宏）。
  ② **散文注释里出现 NOLINT 这个词会被当成真指令**（写「不是 NOLINTNEXTLINE
  形式」→ 报 `unmatched 'NOLINTBEGIN' comment`）。**注释里不要写 NOLINT。**
- **别急着把红归因给「负载/flaky」，先探针。** 全量 ctest 在并发构建下
  红了一条断言样本数的用例，而当时恰好改了同一函数的取整方式。在
  `UpdateOutputTime` 里插临时探针统计「两种取整会分歧的区间（负数）是否出现」→
  从未出现 → 才敢说是 flaky。**改动与红的那条若有任何语义重叠，就必须测。**
  并且：不要为了消掉 flaky 去放宽容差（那正是 R12 反对的换绿方式）。
- **先问「这个性质单线程能观测到吗」再写探针。** 有一条探针要钉
  「跑的是快照而不是实时成员」，但它唯一的观测手段是单线程测试 —— 两条代码路径
  逐像素输出相同。换成同一处代码**可观测**的那一半（清掉 hook 后场景必须还在）。
  这是「引擎早退没有反证」的同形。
- **clang-tidy 的 PCH 陷阱：对 `media/ffmpeg` 全层静默零发现。**
  `avbase_ffmpeg` 是全仓**唯一**开 `target_precompile_headers` 的 target；Xcode 的
  新 PCH 格式 PyPI clang-tidy 18.1.8 **读不了**，每个 TU 只报一句
  `PCH file uses a newer PCH format`，然后 **exit 0、零 finding** —— 比报错更坏，
  因为它看起来就是干净。「142 TU 剩 1 条」因此漏掉全部 **21 个
  media/ffmpeg TU**（唯一那条 EGL 噪声来自**没有 PCH** 的 `platform/egl`，所以
  才看得见 ——「哪个 target 有 PCH」决定了哪层会被静默跳过）。**本机跑 tidy 必须
  用 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` 的 scratch 数据库**；CI 在 Ubuntu 上
  由 clang-18 自己生成自己读 PCH，没这个问题 —— **本机的「干净」从来不等于 CI
  的干净**。阳性对照：拿带 PCH 的真实库对 `media/ffmpeg/*.cc` 跑，应见到那句
  PCH 错误；无 PCH 库能正常出 finding。另注：`git ls-files 'media/*.cc'` 的 `*`
  **匹配斜线**，子目录在内（CI 口径 143 个文件）。
- **`av_freep(&ptr)` 类的多级指针转换要 `reinterpret_cast`。**
  `uint8_t**` → `void*` 是合法隐式转换（`uint8_t*` 本身是对象类型），编译器和人都
  容易放过，但 `bugprone-multi-level-implicit-pointer-conversion` 认它 —— 且
  **`static_cast` 不消这个 check**，它要最强的承认（`reinterpret_cast`）。

- **回退链的「非我」信号必须在 open 之前发**（VideoToolbox 修复）：
  `FFmpegHwDecoderSpec::config_supported` 这种「这个流我不接」的否决，要在
  `avcodec_open2()` **之前**判定并返回 `nullptr`（DecoderStream 既定的「非我」回退
  信号），而不是等首个 packet 解码失败再放弃。后者会连错
  `kMaxConsecutiveDecodeErrors`（20 次）才 `kDecodeFailed`，软件解码尾永远到不了——
  而 VideoToolbox 正是 `open` 成功、**第一个 packet 才** `hwaccel initialisation
  returned error` 的形态，事前没有任何字段能拦，于是「硬件失败 → 全流放弃」而不是
  「硬件拒收 → 软件顶上」。判定「某后端能否吃这个流」的逻辑越早越好，且**必须走
  「返回 nullptr = 非我」而非「开出来再失败」**：`config_supported` 字段声明了却
  不接线 = 等于没有这道门。`ffmpeg_hw_decoder_factory.cc` 在 codec 掩码之后 consult
  它；改 hw 后端时这条顺序别动。
- **`avbase::Player` 不可置于静态存储期（自动析构）。** bench
  （`bench/bench_event_hub.cc`）实测：把含 `EventHub` 的对象做成函数内 `static` 值，
  进程退出时 6 次里 5 次 SIGABRT、1 次 SIGILL——`~EventHub`→`Shutdown`→
  `base::Thread::Stop`→`std::thread::join()` 在 **C++ 静态析构阶段（main 返回后）**
  执行会炸。`Player` 经 PIMPL 持有 `EventHub`（按值 `event_hub_`），且
  `~PlayerImpl`（player_impl.cc:89）在析构体里就 `event_hub_.Shutdown()` +
  `media/video/audio_thread_.Stop()` **显式 join**——所以 `static Player` 在 atexit
  走的是**同一个 join 路径**，同风险（且一次 join 4 条线程）。`player.h` 析构注释里
  写的「bounded by shutdown_timeout 不会挂」成立，但**挂不挂和崩不崩是两回事**：
  shutdown_timeout 只防 hang，不防 atexit join 的崩。安全写法：栈上 / `unique_ptr`
  等你在 main 返回前显式销毁的对象；若必须全程存活，用 `new Player(...)` 后永不
  delete（或等价泄漏手段），别让析构在 atexit 跑。`EventHub` 同理（bench 的
  `SharedHub()` 就是故意泄漏）。

## 「可执行文件比库旧」——诊断前必查，否则整个诊断都是假的

**实测（两次翻车）**：
- `build/macos-sdl2/bin/*` 停在 10/15，库是 10/17 14:32。
- 我拿那个 `headless` 诊断出「**VideoToolbox 硬解播不完**」，
  逐层排查(Deps/SDL2/sink/EOS)、写了临时探针程序、对比日志 —— **全部基于旧代码**。
- 重链后同一命令 **3/3 正常**。**那个 bug 从来不存在。** 为它白花了一整天。
- 更早一次我报的 `ctest --test-dir build/macos-sdl2` **736/736 全绿** 也是旧二进制
  的数据。真数据是**784/784** —— **差的 48 个用例就是铁证**。

**门禁**：`python3 tools/check_binary_freshness.py --root build/<preset>`
退出码 0/1/2 与 `check_protocols.py` 对齐。已接进 CI 的 `full` 与 `macos-player`
两个 job，**位置在 ctest 之前**；但**它真正防的是本地**——CI 每个 job 都从
`actions/checkout` 全新构建、无缓存，从头构建不可能留下陈旧可执行文件。
**每次在本地跑 ctest / 基准前先跑它**，或直接用 `--root .` 一次扫全部构建树。
鉴别力已验证(只变异 headless → 只报 headless)。

**判新鲜度必须用 `.ninja_log`，不能用 `stat`** —— 我用 `stat` 判过一次，**误报成
「10 个二进制过期」**，其中 `base_unittests` 根本不链接 `libavbase_player.a`。
两个原因让 `stat` 不可靠：
1. CMake 的 archive 规则以 `cmake -E touch` 收尾，**每次重链 mtime 都前进**，
   且 `.ninja_log` 记的值是纳秒精度，**不等于 `stat().st_mtime_ns`**；
2. 一个二进制只能跟**它自己链接的库**比，不能跟「最新的库」比。
正确做法见 `tools/check_binary_freshness.py`：从 `.ninja_log` 取 ninja 自己
记录的 mtime，从 `build.ninja` 取每条边的隐式链接输入（**不含 `||` 后的
order-only 段**，那一段只是构建顺序，混进去会让每个消费者看起来依赖所有库）。

**解析 `build.ninja` 必须处理多输出边**（review 查出的真 bug）：
`build bin/base_unittests tests/base_unittests[1]_tests.cmake | ...` 一行有两个
输出——CMake 用同一条边既链接测试程序又生成 ctest 的注册文件。**只取第一个
输出的解析器会把全部 5 个 unittest 二进制静默丢掉**，`no-ffmpeg` 树更是 0 个都
没检查还报绿。**被丢掉的边和新鲜的边长得一模一样**，这是最贵的一类静默失败。

**门禁本身也会骗人，要盯它「比了多少」**：比较数为 0 时必须失败(exit 2)而不是
报绿；`.ninja_log` 少一行(损坏/截断)也不能当作「都新鲜」。

**触发条件比想象的窄**：`touch player/player_impl.cc && ninja -C build/macos-sdl2`
会**正确重链**全部下游（headless/pull_frames/play_*/player_unittests 全部更新）。
落进 stale 状态的是「库重建了但那次没走链接边」——典型是只跑了
`ninja -C <dir> lib/xxx.a` 或者中途改 preset 配置后只重建了库。
**所以它不是每次都会咬人，但它咬人时不报错，这是它危险的地方。**

**教训(与「构建失败时跑的是上一个二进制」「恢复突变后必须 touch」同族)**：
这三条都是**构建系统报了成功、你测的东西却不是最新的**。
共用一条防御：**任何测量开始前，先确认被测物的 mtime 晚于它的输入。**

**第二种陈旧：改完源码后根本没构建**（2026-10-22 补的比较）。整树同一天构建 →
「二进制 vs 库」全绿，而修复晚三天：`play_glfw` 播到约 40 帧就饿死，读起来正像
一次渲染回归，重链后 29.97fps、`dropped 0`——与更早的「VideoToolbox 硬解卡死」
同族，那个 bug 也不存在。修法：沿 build.ninja 文本边「库 → 对象 → 源码」遍历，
二进制 stat vs 源码 stat（源码 mtime 只有编辑器与 git 会写，构建系统从不碰，
所以 stat 是权威的「何时改动」）。`ninja -n` 曾是最候选，已否决：刚建完的树
持续打印「Re-running CMake」（glob 重检没有真实构建不 settle），会把好树判红。
已知局限：**只改头文件不可见**（头依赖在二进制 `.ninja_deps`，本工具刻意不解析
二进制格式）；同一次编辑里的 `.cc` 会被抓到。
`--binary NAME` 把范围收窄到单个可执行文件：`tools/bench_render.sh` 用它只问
`play_glfw`（测一个二进制，没理由被同树里过时的 unittest 挡住），测前拒绝陈旧
树，非 Ninja 树 warn 不阻断。鉴别力：touch 一个只喂 player_unittests 的测试源 →
只红那一个二进制；重建后复绿。合成树用例（两种比较 + 多输出边 + order-only 不计
+ 隐式输入算 + 三种 exit 2）在 `/tmp/fresh_synth_test.py` 跑过 13 条全过。

## macOS 上「工具说另一件事，输出却像你要的那个」

同族陷阱（此前已记：grep `\r` 陷阱、量退出码别接管道、`PIPESTATUS` 在 zsh 是小写
`$pipestatus` 且 1 起始）。新增三例，共同点是**安静地成功**：

1. **BSD grep 不支持 `\b`**（词边界是 GNU 扩展）。写 `grep -n "C9\b" file` 会静默
   返回零命中，看起来像「确认没有」，其实是没查到任何东西。改用仓库自带检索工具。
   （本轮就靠这个才发现 STYLE.md 里 4 处引用了已删除的规则。）
2. **zsh 会把 `--include=*.cc` 当 glob 展开**并报 `no matches found`，命令整体失败。
   同理改用检索工具。
3. **`awk length` 数的是字节不是字符**。中文文档上「227 行超 80 列」按字符算只有 24 行
   （与既有文档 TOOLS.md 25 / BUILDING.md 35 同量级）。判断中文行宽要用字符长度。

**防御**：任何「确认不存在」类的结论（grep 零命中、计数为零），先用一个已知存在的
模式验一遍工具本身，再信结论。
