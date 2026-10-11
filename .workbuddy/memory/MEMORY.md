# avbase 项目长期约定

Chromium 风格分层的 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。

## 分层判据（docs/02 §6，决策 D11）

`platform/` 装的是「**被宿主选中**」的代码。可移植、各宿主行为一致、不看宿主 SDK 的东西
不进 platform/：FFmpeg 在 `media/ffmpeg/`，GL 渲染层在 `render/gl/`（第五十二轮从
`platform/gl/` 迁出）。

两条正交轴不要压平到同一层：**图形 API 轴**（GL / 未来 Metal、D3D11）与**窗口系统轴**
（SDL2 / GLFW / EGL）。否则加第二个图形 API 就是 N×M 个目录。
参考 OBS：`libobs-opengl/` 是根级模块，窗口系统只是它内部的文件（gl-x11-egl.c / gl-cocoa.m）。

## GL 层：`avbase_gl` 是可独立分发的零依赖模块（第五十六轮）

`render/gl/` 同时是一个能单独拿走的库（对标 libobs-opengl）：context 契约 + 装载器 +
program/texture/quad + GLSL 方言。**不依赖 media/base/系统 GL 头/外部加载器**；任何被
`VideoFrame` 塑形的东西在 `media/gl/`。

- 对外用法：`find_package(avbase_gl REQUIRED)` + `target_link_libraries(app PRIVATE avbase::gl)`。
  注意是 `avbase::gl` 不是 `avbase::avbase_gl` —— 靠 `set_target_properties(... EXPORT_NAME gl)`
  实现，删掉那行不报错、只会让消费者把模块名写两遍。完整契约见 `docs/13`。
- `GlApi` 是**一 context 一实例**（无进程全局表），成员是一份 glad 的 `GladGLContext`，
  经 `context()` 访问器暴露（`gl.context().Clear(...)`）。
- 隐患：`GLAPIENTRY` 定义为空，**32 位 Windows 入口点是 `__stdcall`，会崩**。x64 无差异。

## GL 的表是 glad 生成物，不是手写的（第五十七轮，取代原手写表 + 机核脚本）

`render/gl/third_party/glad/` 是 glad 2.0.8 的**输出**（`include/glad/gl.h` +
`include/KHR/khrplatform.h` + `src/gl.c`），随源码入库 —— 「vendored」≠「依赖」：
消费者不装 glad、不联网、不跑生成、不继承额外链接项。

- 重新生成的命令与每个选项的理由在 `render/gl/third_party/glad/README.md` 与 `docs/13 §4.2`。
  **`--mx` 必须开**（否则指针进全局）、**`--loader` 不能开**、**`--alias` 不要开**
  （7 个扩展会被静默放大成 75 个，且 `CopyImageSubData(NV)` 会合并）。
- `glEGLImageTargetTexture2DOES` 是 glad 覆盖不到的**唯一**一例（属 ES 扩展，桌面 `gl`
  规格下生成器直接拒绝），在 `GlApi` 里手工解。
- 为什么删掉手写表：手抄一遍 = 给自己造一个**写错也不报错**的失败模式（`GL_VERTEX_SHADER`
  曾写 `0x8B33`，`glCreateShader` 返回 0 且 info log 为空，看起来像驱动坏了）。
  那个失败模式现在是**被删掉**，不是被守住 —— 所以 `gl_constants.h`、`tools/check_gl_constants.py`
  与 CI 的 `check-gl-constants` job 三者一起退役。
- **glad 的枚举是 `#define` 的 `int` 字面量**（不是 `constexpr GLenum`）：三元表达式结果是
  `int`，braced init 窄化进 `GLenum` 会触发 `-Wc++11-narrowing`。要显式 `static_cast`。
- **换装载器会泄漏到调用方**：接缝确实在 `GlApi` 内部，但枚举改名（`kGLTexture2D` →
  `GL_TEXTURE_2D`）与入口点改形（`gl.Clear` → `gl.context().Clear`）会打到每个调用方。
  只要模块把 GL 的名字直接暴露出去，这就是必然；选的是「名字会变、形状不变」。
- `third_party/` 已被 `check_invariants.py` 与 CI 的 check-format 排除（格式扫描还要排除
  `media/filters/legacy/` 与 `player/option_registry.inc`）。

## 提交前必跑的门禁

```sh
python3 tools/check_invariants.py --root .          # C1/C4/C5/C22-C27；C23 列宽棘轮
clang-format --dry-run --Werror --style=file  # 必须扫全树（不变量 395 个文件）
cmake -S . -B <dir> -G Ninja -DAVBASE_WERROR=ON -DAVBASE_ENABLE_FFMPEG={ON,OFF} \
      -DAVBASE_ENABLE_SDL2=ON -DAVBASE_BUILD_EXAMPLES=ON -DAVBASE_BUILD_TESTS=ON
ctest --output-on-failure                            # no-ffmpeg 468 / ffmpeg 592
```

- `avbase` 是 INTERFACE 库，ninja 不能 `cmake --build --target avbase`。
- 两个预设都要跑：默认 `macos-sdl2` 预设**不开** `-Werror`，告警只在手动加 `-Werror` 时暴露。
- `tools/deps`(2471) / `tools/build`(160) 属第三方与产物，排除出格式与不变量检查；
  `tools/inspect`(6) 是自有代码，要查。
- 拼文件清单要连**未跟踪的新文件**一起数：`git ls-files --cached --others --exclude-standard`
  （只 `git ls-files` 会漏掉刚新增、还没 `git add` 的文件）。

## CI 的两股风格门禁在 main 上是**红的**（存量，第五十六轮实测）

别相信「门禁通过」这句话，先看它有没有真的按 job 的文件清单跑过：

- `check-clang-tidy`：`player/player_impl.cc` 20+ 条、`platform/sdl2/sdl2_audio_sink.cc` 13 条、
  `base/logging.h` 等头文件上百条。`base/logging.h` 的 `enum LogSeverity { LOG_VERBOSE }` 与
  `.clang-tidy` 的 `EnumConstantPrefix: k` 直接冲突，而这个选项 clang-tidy 18 同样会报。
  `[[nodiscard]]` 全仓库只有 3 处，但 `modernize-use-nodiscard` 是开着的。
- `check-cpplint`：CI 同一文件集实测 4 条 error（3 条缺 `#include <utility>`：`media/ffmpeg/
  audio_convert.cc:136`、`platform/{egl,glfw}/*_video_sink.cc`；1 条 `tools/inspect/
  inspect_common.cc:87` 的 `unsigned long long`）。
- 根因同上：文档里的「15 个代表性文件零发现」是**抽样**，不是跑 job 清单。

### `.clang-tidy` 的三个坑

- `ConstantPrefix: k` 会把**所有** const 局部量都要求 k 前缀 —— `const auto x = f();` 也一样
  （实测）。生产代码里零例非 k 的 `const <具体类型>` 局部量，这是被机检强制的真约定。
- `HeaderFilterRegex: '(base|...)/.*\.h$'` 是**子串匹配**，而仓库目录叫 **`avbase/`**，
  于是所有头文件都被当成命中。在头文件过滤上做文章时别被这个骗到。
- lint 抑制要同时伺候两个工具，唯一可用形态：**类别名写进上一行散文注释，
  `// NOLINTNEXTLINE` 裸着单独一行**。带括号的类别名 cpplint 不认（`readability/nolint`），
  带尾随散文 clang-tidy 不认。
- 本机 `llvm`/`llvm@16/@17/@20` 四个 formula 的 clang-tidy **都是 20.1.7**，复现不了 CI 的 18；
  二进制在 `/usr/local/opt/llvm/bin/`，`run-clang-tidy` 需要把它加进 `PATH`。

## 操作教训（都是真踩过的）

- **声称「全树通过」前必须真的扫全树。** 曾连续几轮报告「clang-format 0 偏离」，
  实际只扫了当轮触达目录，全树有 12 个文件偏离。第五十六轮第三次撞上：本轮开始时
  `media/base/media_log.h` 与 `player/public/player_event.h`（上轮提交信息声称已「归零」）
  仍在偏离。**更根本的版本：门禁的「绿」常常是「没跑」。**
- **clang-format 与构建不能并发**跑同一批文件：会读到半格式化文件，产生假的超长行报错。
- **后台命令末尾别用 `grep -c` 收尾**：计数为 0 时退出码 1，整条任务被误报 failed。
- 批量改名要用词边界：`platform_gl` 前缀替换会误伤 `platform_glfw`。
- 改 include 路径后必然影响 include 排序，要跟着跑一次 clang-format。
- macOS sed 的 `-i ''` 空参数会被吃掉；用 `-i.bak` 再删。
- **macOS 的 bash 是 3.2，没有 `mapfile`**：用它拼数组会静默得到空数组，然后
  `clang-format "${FILES[@]}"` 对着 0 个文件 **exit=0** —— 又一次「绿是因为没跑」。
  改用 `git ls-files ... > /tmp/list && xargs clang-format ... < /tmp/list`（BSD xargs 无 `-a`），
  并拿一个故意畸形的探针文件做**阳性对照**确认真会 exit=1。
- 本机无 glfw3 / EGL 开发库（brew TLS 被代理拦），GLFW 依赖走仓库内
  `tools/build/glfw`（`tools/setup_glfw.sh`）；EGL 后端只能做结构验证。
- GitHub raw/API 被代理拦截（CONNECT 502），核实外部仓库布局用 WebSearch；
  但 `registry.khronos.org` 与 github.com/blob 页面可直连。

## 可见性

`CMAKE_CXX_VISIBILITY_PRESET hidden`，导出类需 `AVBASE_MEDIA_EXPORT`。从隐藏可见性的基类
派生导出类会触发 `-Wvisibility` —— 共享基类本身也要导出。

## 文档引用

`docs/09-Linux平台实现方案.md` 已归档到 `docs/archive/`。代码注释用短形式
`docs/archive/09 §N`（沿用仓库按编号引用的约定）；**不要写完整中文文件名**，会撑破 80 列。

新增文档（第五十六轮）：`docs/13-GL模块独立使用指南.md` —— `avbase_gl` 的对外契约
（find_package 用法、入口点表的来源与重新生成命令、glad 覆盖不到的例外、GLSL 方言、
零依赖边界与换装载器的接缝）。索引在 README §1 文档表；`docs/BUILDING.md §3.8` 有简版。
