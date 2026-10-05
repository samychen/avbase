# 构建与运行

> 本文档描述**当前分支的真实状态**，不是目标状态。规划中的完整构建矩阵见
> [docs/06](06-CMake工程与构建体系.md)；进度与缺口见 [PROGRESS.md](PROGRESS.md)。
> 两者的差别很重要：docs/06 描述的很多东西还不存在。

---

## 1. 依赖

| 用途 | 需要什么 | 检查命令 |
|---|---|---|
| **一切** | CMake ≥ 3.20 · Ninja · 支持 C++20 的编译器（GCC ≥ 10 / Clang ≥ 12）· Python 3 | `cmake --version; ninja --version; g++ --version; python3 --version` |
| 单元测试 | GoogleTest（`libgtest-dev`，CMake 会 `find_package`，找不到会退化到系统路径探测） | `dpkg -l \| grep gtest` |
| **只跑 Python 工具** | 仅 Python 3 —— **不需要编译器、不需要 FFmpeg** | 见 §5 |
| 真实媒体测试 | 系统 FFmpeg 开发包（4.4 ~ 7.x 均可，**不需要打 patch**） | `pkg-config --modversion libavformat` |
| FFmpeg 版本锁定 | `tools/setup_ffmpeg.sh`（从源码编一个固定版本到 prefix） | — |
| SDL2 后端 | ✅ 已实现（M11 的代码提前落地）：`-DAVBASE_ENABLE_SDL2=ON`，另需 SDL2 开发包 | 见 §4 |
| 原生 Linux 后端（X11/Wayland） | ❌ **尚未实现**（M12）。打开 `AVBASE_ENABLE_LINUX_NATIVE` 会直接 `FATAL_ERROR` | 见 §4 |
| macOS 构建 | ✅ **已落地（第二十五轮）**：`cmake --preset macos-sdl2`，需 Homebrew 的 SDL2 + FFmpeg | 见 §3.6 |

Debian/Ubuntu 一次装齐（不含 FFmpeg）：

```bash
sudo apt-get install -y build-essential cmake ninja-build python3 libgtest-dev
# 需要真实媒体测试时再加：
sudo apt-get install -y libavformat-dev libavcodec-dev libavutil-dev \
                        libswscale-dev libswresample-dev
```

---

## 2. 取代码

```bash
git clone https://github.com/samychen/avbase.git && cd avbase
git checkout fill-gaps        # ← 本轮全部工作在这个分支，main 还是首次提交
```

---

## 3. 构建与测试

所有 preset 都定义在 `CMakePresets.json`，**不需要手写 `-D` 参数**。

### 3.1 最快的一条路：核心，不需要 FFmpeg

```bash
cmake --preset no-ffmpeg
cmake --build --preset no-ffmpeg
ctest --preset no-ffmpeg --output-on-failure
```

这条验证设计目标 **G2**：核心只用一个 C++20 编译器就能构建并通过测试。
预期 **324 个用例全绿**（不含需要 FFmpeg 的 35 个）。

### 3.2 带真实媒体的完整测试

```bash
tools/setup_ffmpeg.sh 7.1.1                # 装到仓库内 tools/build（免 sudo）
cmake --preset linux-ffmpeg711             # FindFFmpeg 自动发现，无需任何 env
cmake --build --preset linux-ffmpeg711
ctest --test-dir build/linux-ffmpeg711 --output-on-failure
```

预期 **359 = 324 + 35** 全绿。默认装进仓库内的 `tools/build/`（gitignored），
`FindFFmpeg` 在 `AVBASE_FFMPEG_ROOT` 未设置时自动优先使用它——MediaComponent 的
「头文件在仓库里、clone 即用」的体验，等价但不破坏 C4 头隔离约束；要装到别处
（如 `/opt/ffmpeg-7.1.1`）就传第二个参数并 `-DAVBASE_FFMPEG_ROOT=<prefix>`（或
同名环境变量）。`tools/build` 存在时系统 FFmpeg（brew/apt）会被忽略，删除该目录
即回退到系统包。

脚本的行为（都是从 MediaComponent 的 `build_ffmpeg.sh` 教训里同步过来的）：

- **tarball sha256 校验**：7.1.1 的校验值内置于脚本；对不上（下载截断/镜像被篡改）
  在解压前就报错。新版本要么加进脚本的 `PINNED_SHA256`，要么
  `AVBASE_FFMPEG_SHA256=<hash>` 临时传入。
- **持久缓存 + 增量构建**：tarball、解压源码、各构建树都在
  `tools/deps/`（可用 `AVBASE_FFMPEG_SRC_DIR` 改），已加入 `.gitignore`。
  第二次运行命中缓存直接进 make；改了 prefix 或开关后按 configure 指纹自动重配。
- **链接模式**：默认只编动态库（与旧行为一致）。`AVBASE_FFMPEG_LINK=static`
  编静态（自动 `--enable-pic`，可安全链入共享对象）；`=both` 同 prefix 两个
  构建树各出一套，给 Android prebuilt / 自包含分发用。
- **可选依赖链**（对齐 MediaComponent 的 `build_ffmpeg.sh` 体验，默认不开）：
  `AVBASE_FFMPEG_DEPS="openssl x264 fdk-aac opus librtmp mbedtls srt"`，全部
  从源码编进与 FFmpeg 同一个 prefix，FFmpeg 的对应 configure 开关（TLS/编码器/
  协议/muxer）自动追加。依赖来源一律 git clone 并钉死 commit（tag→commit 校验，
  比文件哈希更强）；隐式依赖自动补齐（`srt`→mbedtls、`librtmp`→openssl；
  TLS 后端冲突时 openssl 优先）。选了 `x264`/`fdk-aac` 会连带打开
  mp4/matroska/adts muxer 和原生 aac 编码器。注意 `fdk-aac` 使产物变成
  `--enable-nonfree`，不能以 Apache/BSD 身份再分发。`libyuv` 启用
  `platform/ffmpeg/video_convert.cc` 的 SIMD 像素转换快速路径（NV12/422/10bit→I420、
  I420→RGB 带色彩矩阵），是**可选的**：没编 libyuv 时该模块自动退化为纯 sws_scale，
  行为不变只是慢一些。
- **头/库同源探测**：配置 avbase 时，`cmake/FindFFmpeg.cmake` 会编译并运行一个
  调 `avcodec_version()` 的探针程序，把**链接到的库**的真实版本与**头文件**解析出的
  版本比对，不一致直接 `FATAL_ERROR`——这正是"编译期无提示、运行期 SIGBUS"的那类
  故障（MediaComponent 的最大教训）。交叉编译会自动跳过；强行绕过用
  `-DAVBASE_SKIP_FFMPEG_PROBE=ON`（后果自负）。

也可以直接用发行版 FFmpeg（这正是设计目标之一，验收标准 A14）：

```bash
cmake --preset ffmpeg                 # 系统 FFmpeg，不开窗口后端
cmake --build --preset ffmpeg
ctest --preset ffmpeg

cmake --preset linux-sdl2             # FFmpeg + SDL2 双后端（需要 SDL2 开发包）
cmake --build build/linux-sdl2        # 唯一能建出 play_sdl2 的 preset
```

`ffmpeg` 与 `linux-sdl2` 只需要发行版/Homebrew 的 FFmpeg，不需要
`AVBASE_FFMPEG_ROOT`（例外：仓库内存在 `tools/build/` 时会优先用它，见 §3.2）；
`linux-sdl2` 也是 CI 里用来验证 SDL2 后端能在 Linux 上编译的那份配置。

### 3.3 Debug + `-Werror`（第一次构建应该跑这个）

```bash
cmake --preset debug && cmake --build build/debug
(cd build/debug && ctest --output-on-failure)
```

`debug` 打开 `AVBASE_STRICT_WARNINGS` + `AVBASE_WERROR` + `AVBASE_ENABLE_DCHECK`。
警告集包含 `-Wshadow` `-Wnon-virtual-dtor` `-Woverloaded-virtual` `-Wcast-align`
`-Wnull-dereference` `-Wdouble-promotion` `-Wimplicit-fallthrough` `-Wformat=2`
以及 `-Werror=return-type/uninitialized/narrowing/delete-non-virtual-dtor/reorder`。
**新代码第一次编译就应该在这里过，而不是在 `no-ffmpeg` 里过。**

`debug` 同时打开 `AVBASE_ENABLE_FFMPEG`（严格告警必须覆盖全部目标）。在此之前它
沿用默认值 OFF，于是 `platform/ffmpeg/*` 与 `media/filters/ffmpeg_*.cc` 只被非严格的
`ffmpeg` / `linux-sdl2` 预设编过——"零警告"实际只覆盖了不含 FFmpeg 的那半棵树，
而 FFmpeg 适配层正好是 `-Wthread-safety` 最容易说话的地方之一。

### 3.4 Sanitizer

```bash
cmake --preset asan && cmake --build --preset asan -j2 && (cd build/asan && ctest)
cmake --preset tsan && cmake --build --preset tsan -j2 && (cd build/tsan && ctest)
cmake --preset ubsan && cmake --build build/ubsan && (cd build/ubsan && ctest)
cmake --preset coverage && cmake --build build/coverage
```

`asan` 含 UBSan 与 **LSan**（阈值：单实例泄漏 0 字节）。并发/时钟类测试建议加抗抖动重复：

```bash
(cd build/asan && ctest --repeat until-fail:25 -R "Clock|Concurrent|Queue")
(cd build/tsan && ctest --repeat until-fail:10 -R "Clock|Concurrent|Queue")
```

### 3.5 诊断 CLI

```bash
cmake --build --preset linux-ffmpeg711          # 或任何 AVBASE_ENABLE_FFMPEG=ON 的配置
./build/linux-ffmpeg711/tools/inspect/avbase-inspect probe  tests/testdata/small_h264_aac_3s.mp4
./build/linux-ffmpeg711/tools/inspect/avbase-inspect decode tests/testdata/small_h264_aac_3s.mp4
./build/linux-ffmpeg711/tools/inspect/avbase-inspect sync   tests/testdata/small_h264_aac_3s.mp4
```

`decode` 会打印帧数、pts 单调性、音频样本数；`sync` 用真实时间戳逐步驱动
`AvSyncController` 并打印 `media_time / master / av_diff`。
**`sync` 是历史上抓到 bug #32（主时钟被 uptime 偏移）的那个命令**——它比单测更能发现
时钟类问题，因为单测用的 `SimpleTestTickClock` 起点是 0，会掩盖一整类偏移错误。

### 3.6 macOS（第二十五轮起）

macOS 上的 SDL2 真窗口后端与完整测试套件已验证通过（`macos-sdl2` 预设 + `macos-player`
CI job 守护）。需要 Homebrew 安装依赖：

```bash
brew install sdl2 ffmpeg cmake ninja
cmake --preset macos-sdl2
cmake --build --preset macos-sdl2
ctest --preset macos-sdl2 --output-on-failure     # 预期 481 用例全绿
```

`macos-sdl2` 产出 `play_sdl2` 真窗口二进制（`otool -L` 确认链接 `libSDL2-2.0.0` +
`libavformat.61`）。VideoToolbox 硬解走 `platform/ffmpeg/ffmpeg_hw_video_decoder.cc`
的 libavcodec hwaccel，**需实机验证**（CI 沙箱无 VideoToolbox 设备）。

也支持不带 SDL2 的无 FFmpeg 配置（`cmake --preset no-ffmpeg`，同 Linux）。

---

## 4. 播放：现在能跑了（第十轮起）

```bash
# headless：不需要窗口系统，整个 decode→sync→render 链路对 null sink 播完
./build/<cfg>/bin/headless tests/testdata/small_h264_aac_3s.mp4
#   → "completed at media time 2.99s"，退出码 0
#   可选 --seek 1.5 / --rate 2.0 / --timeout n

# SDL2 真窗口（需 AVBASE_ENABLE_SDL2=ON 配置；宿主创建窗口，嵌入模式见
# platform/sdl2/surface.h）：
./build/<cfg>/bin/play_sdl2 --url tests/testdata/small_h264_aac_3s.mp4
```

`Player` 门面已接线：SetDataSource / PrepareAsync / PrepareSync / Start / Pause /
Stop / SeekTo（按 request_id 回调）/ 音量 / 静音 / 倍速 / 循环 / SetVideoSurface /
事件流全部可用。**仍返回 kNotImplemented 的**：StepOnce、SelectTrack、TakeSnapshot、
UpdateConfig、RunUntilIdle（各自注明所需里程碑）。

## 4.1 还跑不起来的东西

| 想跑的 | 状态 | 原因 |
|---|---|---|
| `cmake --preset linux-native` / `linux-all` | ❌ 配置期 `FATAL_ERROR` | 原生 GL 后端仍是 M12 |
| `cmake --preset android-arm64` | ❌ | `platform/android/` 不存在（M16） |
| Golden Test | ❌ | `tools/ijkplayer-recorder/`、`golden_*.py`、`tests/golden/` 不存在（M10），且被开放问题 Q8 卡住 |
| 精确 seek / 轨选切换 / 快照 | ❌ kNotImplemented | M9（SeekController、子渲染器重建） |

**一句话：这个仓库现在能构建、能测、能分析媒体文件，并且能播放视频（headless +
SDL2 双通道验证过）；精确 seek、golden 对齐和原生 GL 后端仍在 M9–M12。**

---

## 5. 不需要编译器就能跑的三个工具

这三个是**当前唯一有 CI 门禁的自动化检查**，只要有 Python 3 就能跑：

```bash
# 架构与风格不变量（14 条规则），并列出所有 DRAFT 文件防止被遗忘
python3 tools/check_invariants.py --root .

# 移植常量三方交叉校验：ffplay #define ⟷ avbase 代码 ⟷ docs/05 表 7
python3 tools/extract_constants.py --root . --selftest            # 不需要 ijkplayer 源码
python3 tools/extract_constants.py --root . --ijkplayer /path/to/ijkplayer   # 三方
python3 tools/extract_constants.py --root . --selftest --json     # 机器可读

# 选项表：A10 覆盖率 + docs/05 ⟷ player_config.h ⟷ option_map.py 三方核对
python3 tools/gen_options.py --root . --check
python3 tools/gen_options.py --root . --coverage
python3 tools/gen_options.py --root . --selftest
python3 tools/gen_options.py --root . --emit     # 生成 player/option_registry.inc
```

> ⚠️ **改完 `tools/option_map.py` 或 `tools/ported_constants.py` 后先
> `rm -rf tools/__pycache__` 再跑。** 这两个是数据模块；`cp` 还原时若文件大小相同
> 且 mtime 落在同一秒，Python 的 pyc 校验（mtime + size，秒级精度）会判定源码未变而
> 继续用旧字节码，于是测试结果与实际源码不符。这个坑在第九轮真实发生过一次。

CI（`.github/workflows/ci.yml`）跑五组：`quick`（no-ffmpeg + `check_invariants`）、
`ffmpeg`（发行版 FFmpeg，359 个用例，验证 A14）、`e2e-headless`（`examples/headless`
把 testdata 的 4 个正常样本 + 1 次 seek 播到 `kCompleted`，并断言损坏文件以可操作错误
退出）、`sdl2-build`（Linux 上编译 SDL2 后端与 `play_sdl2`）、`full` 矩阵
（GCC 11/13 · asan · tsan · ubsan · coverage · macOS no-ffmpeg · macOS + Homebrew FFmpeg）。

**仍未开的是 `e2e-linux`（`if: false`）**：它要的是 xvfb + llvmpipe 把两个 Linux 后端
真跑起来（G12/A16，M11/M12），还需要尚未存在的 `verify_e2e.py`。`check-format`、
`check-cpplint`、`check-clang-tidy` 三个门禁也刻意没开——见下文 §7 的说明。

---

## 6. [历史] 19 个 DRAFT 文件是怎么转正的

> **状态：第十轮已完成。** 现在仓库里**没有任何 DRAFT 文件**——下文那 19 个文件全部
> 进了构建，`STATUS: DRAFT` 的横幅按项目决定留在文件头作为历史。本节保留，因为它是
> 下一次"从 DRAFT 到构建"的现成流程，下面所有数字与命令都是当时的实测记录。

当时的规矩是：**19 个 DRAFT 文件不在任何 CMake target 里**，所以 §3 的构建与测试
不受影响、始终是绿的——不能编译的文件绝不可从构建可达。

要转正，**按这个顺序**（依赖关系决定的，不是随意排的）：

```bash
# 第 0 步：确认基线是绿的（不碰 DRAFT 文件）
cmake --preset debug && cmake --build build/debug && (cd build/debug && ctest)
python3 tools/check_invariants.py --root .

# 第 1 步：只做语法检查，不进构建。一次一个文件，错误面最小
for f in media/base/pipeline_status.h media/base/media_resource.h \
         media/base/renderer_client.h media/base/renderer.h \
         media/base/renderer_factory.h media/base/pipeline.h \
         media/base/pipeline_controller.h; do
  echo "--- $f"; g++ -fsyntax-only -std=c++20 -fno-exceptions -fno-rtti -I. "$f"
done
```

第 1 步要重点看四件事，都是我在无编译器环境下**无法验证、且最可能出错**的：

| 检查 | 为什么它最可能出错 |
|---|---|
| `renderer_factory.h` 的前置声明够不够 | 我为了缩小传递闭包把 5 个类型改成了前置声明（33 → 27 个头）。只以指针出现的类型够用，但任何 include 它的 TU 若要**调用** `Create*()` 就必须自己 include 完整类型 |
| `base::BindOnce` 能不能搬 `unique_ptr` | `renderer_impl.cc` 的 `Initialize()` 把 `unique_ptr<VideoRendererSink>` 绑进了 `BindOnce`。`bind.h` 自称是 R2 降级的 **L1 层**，明确列出不支持 `Passed()`/`Owned()`/变参包，**但没说 move-only 绑定参数支不支持**。若不支持，改成"任务体内读成员字段" |
| `pipeline_controller.h` 的抽象声明 | 我把它声明为抽象类，而 docs/03 §6 与 Chromium 都是持有 `unique_ptr<Pipeline>` 的具体类。这是刻意偏离，编译期不会有意见，但 M8 接线时要认账 |
| 80 列 / 命名 / `-Wshadow` | 列宽已由 C23 棘轮守着（基线 310 行），`-Wshadow` 与其余编译告警由 `debug` preset 的 `-Werror` 全量守（该预设现已含 FFmpeg 层，见 §3.3）；但**命名规则没有任何门禁**——`.clang-tidy` 已落盘却未接线，`check-format` / `check-cpplint` 两个 job 也刻意没开（见 §7 末） |

```bash
# 第 2 步：语法过了再进构建。media/base 的 7 个头加入 avbase_media
#         （它们只有声明，除 pipeline_status/renderer_client/renderer/
#           pipeline/pipeline_controller 各自欠一个 .cc，见各文件头的
#           ".cc owed by this header" 清单，共 10 个符号）

# 第 3 步：WSOLA 三件套 + AudioRendererImpl + VideoRendererImpl + RendererImpl
#         audio_renderer_algorithm.cc 是 555 行，超 C1 的 500 行上限，
#         进构建时会报 C1 —— 两个选择都写在该文件头里（登记豁免，或按
#         "队列容量策略应归 M9 BufferController" 再拆一次）

# 第 4 步：两个 DRAFT 测试文件加进 tests/CMakeLists.txt
#         base_unittests += unit/base/refcount_ownership_unittest.cc
#         新建 media_filters_unittests += unit/media_filters/audio_renderer_algorithm_unittest.cc
#         unit/player/deps_ownership_unittest.cc 需要一个还不存在的 player_unittests target
#         ★加进去之后 287/322 这两个数字就变了，PROGRESS 与 README 要同步

# 第 5 步：摘掉 DRAFT 横幅（19 处），再跑一次 check_invariants
grep -rln "STATUS: DRAFT" --include='*.h' --include='*.cc' .
```

**已知的、我预判会在第一次编译时炸的点**（按可能性排序）：

1. `BindOnce` 搬 `unique_ptr`（上面第 2 条）
2. `renderer_factory.h` 的前置声明不足
3. `audio_renderer_algorithm.cc` 里的 DSP 循环触发 `-Wsign-conversion` /
   `-Wconversion`（`int` 与 `size_t` 混用，`static_cast` 我已经尽量加了，但盲写难免漏）
4. ~~`wsola_internals.cc` 缺 `<algorithm>`~~ —— **已修**。做过一次系统性审计：
   对 22 个 DRAFT 文件扫描"用了 `std::X` 但没有对应头（含经项目内头文件的传递引入）"，
   只发现这一处；`renderer_impl.h` 的 `std::optional` 经 `renderer.h` 传递已满足。
   `AudioBuffer::ReadFrames()` 也确认是 const，所以 `AudioFrameQueue::PeekFrames()`
   的 const 版本调用它没问题

---

## 7. 常见故障

| 症状 | 原因与处置 |
|---|---|
| `AVBASE_ENABLE_LINUX_NATIVE is scheduled for milestone M12` | 不是 bug，是有意的 `FATAL_ERROR`：原生 GL 后端还没写。SDL2 后端已经可用，见 §4 |
| `FindFFmpeg` 报版本为空 / 门禁形同虚设 | 第四轮修过一个：版本正则用小写组件名而实际宏是 `LIBAVCODEC_VERSION_MAJOR`。若再现，检查 `cmake/FindFFmpeg.cmake` 的 `string(TOUPPER)` |
| `header/library consistency probe FAILED TO RUN` / `VERSION MISMATCH` | 头文件与实际链接的 libav\* 不是同一次构建——编译期无提示、运行期 SIGBUS 的那类问题，现在是配置期报错。把 `AVBASE_FFMPEG_ROOT` 指向 `tools/setup_ffmpeg.sh` 的产物，或清理 pkg-config 路径里的混装。确认要强行绕过才用 `-DAVBASE_SKIP_FFMPEG_PROBE=ON` |
| `av_dict_iterate` 未声明 | 那是 FFmpeg 6.0 才有的；5.x 走 `AVBASE_FFMPEG_HAS_DICT_ITERATE` 分支。兼容层已在 7.1.1 与 5.1.9 双版本验证过 |
| 工具跑出来的结果与源码不符 | `__pycache__` 陈旧字节码，见 §5 的警告 |
| `check_invariants` 报 C1 超长 | DRAFT 文件豁免；非 DRAFT 文件要么拆，要么在 `LINE_LIMIT_ALLOWLIST` 登记**带理由**的豁免（每条豁免要关联 issue，见 R12） |
| 构建产物里出现 `libSDL2` | 应然：`AVBASE_ENABLE_SDL2=ON` 时 `platform_sdl2` 是唯一链接 SDL2 的 target。`libGL` 则仍不可能（原生后端不存在）；M12 之后应由 dlopen 弱依赖保证 `ldd libavbase.so` 只有 libc/libstdc++/libm/libdl/libpthread |

---

## 8. 提交前

```bash
python3 tools/check_invariants.py --root .      # 必须 all rules pass
python3 tools/extract_constants.py --selftest --root .
python3 tools/gen_options.py --check --root .
clang-format --dry-run -Werror $(git diff --name-only origin/main -- '*.h' '*.cc')
```

提交信息格式见 [STYLE.md §9](../STYLE.md)：`<layer>: <祈使句 ≤72 字符>`，
正文写**为什么**而不是改了什么，末尾带 `Test:` 行。
`<layer>` 取 `base` / `media` / `player` / `platform/linux` / `platform/sdl2` /
`cmake` / `docs` / `tools` / `tests`。
