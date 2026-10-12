# avbase 门禁与操作手册（MEMORY.md 的长尾）

## 门禁命令
```sh
python3 tools/check_invariants.py --root .     # C1/C4/C5/C16/C22-C28
/usr/local/opt/llvm/bin/clang-format --dry-run --Werror --style=file
cmake --preset macos-sdl2 -DAVBASE_WERROR=ON -B <dir> && cmake --build <dir> && ctest
tools/bench_render.sh
```
- **文件数一直在变，别记死数字**（曾把不变量的 425 套到 format 上）。看 exit code 与行数。
- **C28：`render/` 不得 include `media`/`platform`/`base`/`player/`。**
- `avbase` 是 INTERFACE 库，ninja 不能 `--target avbase`。
- 默认 `macos-sdl2` 预设**不开** `-Werror` → 两套预设都要跑。
- **`clang-format` 不在 PATH**（裸调 exit=127），必须给全路径。
- 两套清单数不同别套用。拼清单连未跟踪新文件：
  `git ls-files --cached --others --exclude-standard`。
- `tools/deps`/`tools/build` 已 gitignore；`tools/inspect` 是自有代码（7 个文件）。
- 新 `.cc` 是 GLOB 收的（CONFIGURE_DEPENDS），新增源文件不用改 CMakeLists。

## 提交切片
**按依赖层次切**（不按「轮」/功能主题）：同一批文件内部交织，「每片独立编译」做不到；
能让每片不自相矛盾的是**先提交被引用的、后提交引用者**。顺序：tools → 数据面 →
render/gl 原语 → media/gl 场景层 → platform 后端 → 宿主+示例 → 测试 → 文档。
**接口扩展必须和它的全部实现者一起，或放到最后**（只加数据字段的可单独成片）。
每片提交后跑 check_invariants（几秒）；**完整构建只在最后跑一次**。

## CI 两股风格门禁在 main 上是红的（存量）
别信「门禁通过」；「15 个代表性文件零发现」是**抽样**不是跑 job 清单。
- clang-tidy：`player/player_impl.cc` 20+、`platform/sdl2/sdl2_audio_sink.cc` 13、
  `base/logging.h` 上百（`enum LogSeverity { LOG_VERBOSE }` 与 `EnumConstantPrefix: k` 冲突）。
- cpplint：4 条 error（3 条缺 `<utility>`，1 条 `unsigned long long`）。
- `.clang-tidy` 坑：`ConstantPrefix: k` 要求**所有** const 局部量（含 `const auto`）；
  `HeaderFilterRegex` 子串匹配 `avbase/` → 所有头文件命中；抑制 = 上一行散文注释 +
  裸 `// NOLINTNEXTLINE`。本机 clang-tidy 20.1.7 复现不了 CI 的 18。

## 操作教训（都真踩过）
- **声称「全树通过」前必须真扫全树。** 门禁的「绿」常常是「没跑」；2 秒跑完的「全树格式
  检查」一定没跑；**exit=127（command not found）会让门禁看起来绿**。
- **构建失败那一轮，基准跑的是上一个二进制** → 把 build 与测量串起来前先确认 exit code。
- **PATH 上的 `grep` 是 toybox shim**：不支持 `\b`，且**静默返回 0**。用 `/usr/bin/grep`
  或 Grep 工具。**凡「用 grep 查证某符号不存在」的结论都要换工具复核。**
- **macOS bash 3.2 没有 `mapfile`**：拼数组静默得空数组，对着 0 个文件 **exit=0**（又一次
  「绿是因为没跑」）。改用 `xargs` + 故意畸形的探针文件做**阳性对照**。`set -u` 下空数组
  写 `"${arr[@]+"${arr[@]}"}"`。
- 后台命令末尾别用 `grep -c` 收尾（计数 0 → exit 1 → 整条任务被误报 failed）。
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
- **撤临时探针时不要 `git checkout -- <file>`**：它会把该文件**尚未提交的改动一起回退**。
  第六十五轮手滑用了一次，把 `examples/scene_demo.h` 的整轮 L5 改动清掉了（靠 `/tmp`
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
