# avbase 项目长期约定

Chromium 风格分层的 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。

## 分层判据（docs/02 §6，决策 D11）

`platform/` 装「**被宿主选中**」的代码。可移植、各宿主行为一致、不看宿主 SDK 的东西不进
platform/（FFmpeg 在 `media/ffmpeg/`，GL 渲染层在 `render/gl/`）。
两条正交轴不要压平：**图形 API 轴**（GL / 未来 Metal、D3D11）与**窗口系统轴**（SDL2/GLFW/EGL），
否则加第二图形 API 就是 N×M 个目录。参考 OBS：`libobs-opengl/` 是根级模块，窗口系统只是它
内部的文件。

## `avbase_gl`：可独立分发的零依赖模块（`render/gl/`）

context 契约 + 装载器 + program/texture/quad + GLSL 方言。**不依赖 media/base/系统 GL 头/
外部加载器**；被 `VideoFrame` 塑形的东西在 `media/gl/`。对外契约见 `docs/13`。

- 用法：`find_package(avbase_gl REQUIRED)` + `avbase::gl`（**不是** `avbase::avbase_gl`，
  靠 `EXPORT_NAME gl`；删掉那行不报错、只让消费者把模块名写两遍）。
- `GlApi` **一 context 一实例**（无进程全局表），成员是 glad 的 `GladGLContext`，经
  `context()` 暴露（`gl.context().Clear(...)`）。
- 隐患：`GLAPIENTRY` 定义为空，**32 位 Windows 入口点 `__stdcall` 会崩**；x64 无差异。

### 装载器 = glad 生成物（vendored，不是依赖）

`render/gl/third_party/glad/` 是 glad 2.0.8 的**输出**，随源码入库：消费者不装 glad、不联网、
不跑生成。重新生成命令与各选项理由见该目录 README 与 `docs/13 §4.2`：**`--mx` 必开**（否则
指针进全局）、**`--loader` 不能开**、**`--alias` 不要开**（7 个扩展放大成 75 个）。

- 唯一覆盖不到的入口点：`glEGLImageTargetTexture2DOES`（ES 扩展，桌面规格生成器拒绝），
  在 `GlApi` 里手工解。
- 为什么不要手写表：手抄 = 造一个**写错也不报错**的失败模式（曾把 `GL_VERTEX_SHADER` 写成
  `0x8B33`，`glCreateShader` 返 0 且 info log 空，看着像驱动坏了）。那个失败模式是**被删掉**，
  不是被守住。
- **glad 枚举是 `#define` 的 `int` 字面量**：三元表达式结果是 `int`，braced init 窄化进
  `GLenum` 触发 `-Wc++11-narrowing`，要显式 `static_cast`。
- 换装载器会泄漏到调用方（枚举改名、入口点改形）。只要模块把 GL 名字暴露出去就必然；
  选的是「名字会变、形状不变」。

## 合成面：canvas FBO + blit 上屏（两段式，L2）

`Compose(frame, timing*)` 画进离屏 canvas FBO → `PresentComposed(timing*)` blit 上屏；
`Present()` = 两者 + `SwapBuffers`（签名不变，宿主不改）。

- **每一帧都走 canvas，包括单视频** —— 因为 L3 要建在其上的路径必须是被跑过的路径；
  绕过 canvas 的快路径才是没被测的那条。
- **朝向**：`GlQuad(flip_vertical=false)` 把 `v=0` 放在 NDC 顶部，对自上而下的 CPU 位图对，
  但往 framebuffer 画时「图像第 0 行 = framebuffer 最后一行」，于是**该纹理第 0 行装的是图像
  底** → **凡从渲染目标采样回来的 pass 都要 `flip_vertical=true`**（管着 `GlBlitter` 和
  `GlTransition`）。搞反 = 上下颠倒，**无返回值无 GL 错误，只能读回像素抓**。
  这条推导被写反过两次（第五十九、六十轮）。**有朝向的代码必须配像素断言。**
- **`GL_UNPACK_ROW_LENGTH` 单位是 texel 不是字节**：上传前设 `stride(plane)`，用完还原 0；
  NV12 交错色度用 `GL_RG`（1 texel = 2 字节）所以传 `stride/2`。1080p 恰好色度 stride = 可见
  宽度把 bug 藏住；320x240、854x480 必现，症状是**周期 8 行的绿色横条**。
- canvas 尺寸语义：非正数 = 跟随 drawable（每帧重读，保住 resize；同尺寸 `Ensure` 是 no-op）。
  放在 `GlSurfaceBinding` 里（它是 surface 的属性）。已分配尺寸只活在 `canvas_`。
- `GlFramebuffer::Ensure()` 幂等且同尺寸保留内容；`Destroy()` 先删 fbo 再放纹理。
  纹理必须设采样参数（缺了 blit 当 mipmap 不完整而采到黑）。

### GL 像素测试（`tests/unit/gl/`，`AVBASE_ENABLE_GLFW` 门控）

首个对**读回像素**断言的套件。**隐藏窗口**（`GLFW_VISIBLE=FALSE`）→ 不需要显示器、CI 可跑；
`glfwSwapInterval(0)` 覆盖 `MakeCurrent()` 里那个 1。

- 断言用**内容**不用 GL 名字（GL 复用刚 delete 的名字，比名字两边都证明不了）。
- 读像素要用 `Compose` + `PresentComposed`，**不要用 `Present`**（swap 后 back buffer 未定义，
  会读到全黑）。
- **新原语必须做反证且「只红该红的」**：反证区分「测试通过」与「测试真的在测东西」。

## 多源场景层（L3，`media/gl/`）

| 文件 | 职责 |
| --- | --- |
| `gl_video_layer.{h,cc}` | 一层的平面纹理 + 色彩矩阵 + YUV 格式差异 + stride；上传与「画这一层」 |
| `gl_scene.{h,cc}` | N 层：句柄（**永不复用**）、z 序、归一化 rect、opacity、visible；`Compose()` 画进**当前绑定**的目标 |
| `gl_scene_driver.{h,cc}` | sink 侧钩子：帧→主图层，交宿主补层后 present。无钩子时**转发 `Present()`**（单帧不是另一条分支） |
| `render/gl/gl_transition.{h,cc}` | 两纹理按 progress 交叉淡化（在零依赖模块：两张画好的图不是视频概念） |

- `GlPresenter` 侧：`EnsureScene/UploadLayer/ComposeScene/PresentScene/ComposeTransition/
  PresentTransition`。
- **场景每帧作参数传，不存成 presenter 状态**（「哪个场景在屏上」是宿主每帧重做的决定）。
- **上传与合成分开**：上传在帧到达时（N 路不同时刻），合成每显示帧一次；所以
  `UploadLayer()` 耗时**累加**进 `upload_ms`（赋值会把最后一层报成整帧）。
- **`EnsureScene()` 归属由生命周期决定**：场景 GL 对象属 presenter 的 context，
  `GlPresenter::Shutdown()` 是唯一重新 make current 再删对象处。`GlSceneDriver` 只持借用、
  `Shutdown()` 不碰 GL；销毁顺序固定「先 driver、后 presenter」。
- **NV12 只有 2 个平面**：显式 `plane_count = nv12 ? 2 : 3`，该在的平面缺失就失败。

**成本模型（真机 1080p60）**：单源 upload 0.77 / compose 0.07 / blit 0.03 ms，59.98 fps；
两层 upload **1.55** / compose 0.085 / blit 0.019，**59.98 fps** —— 第二层成本是它的上传
（线性），合成几乎免费。4K30 两层掉到 26.96（第二层 4K 上传 4.07 ms）。

**边界**：播放器管线**仍单源**（`VideoRendererSink` 一次一帧），N 路独立解码 → 多图层需
N 个解码器，属播放器层。`--scene pip|fade` 用**同一流的延迟副本**（12 帧环形缓冲）当第二路
源。场景↔场景转场只有测试覆盖。

## 硬件零拷贝导入（L4）

形状在 `media/gl/gl_hw_texture_importer.h`（`target()/CanImport()/ImportPlane()/name()`，
只认 `VideoFrame`、不认 SDK —— C5 把 CoreVideo 头挡在 media 外），实现在 `platform/hwimport/`
（macOS CGL/IOSurface；**没有互操作 API 的主机返回空指针，不是桩**）。

- **岔路口是 `GlVideoLayer::Upload(frame, importer)`，判据是帧不是调用方**：`IsMappable()` →
  拷贝路线；否则导入路线，**没有 importer 就是 false**（绝不拿旧纹理凑一帧）。
- **macOS 只接受 `GL_TEXTURE_RECTANGLE`**：`CGLTexImageIOSurface2D` 对 2D 每种组合都回
  `kCGLBadValue`。→ 必须有 `sampler2DRect` 版 body（多带 `uTexSize`/`uChromaSize` 把
  `vTexCoord` 乘成 texel）；两套 program 都在 `Init()` 编译。
- **`GL_TEXTURE_RECTANGLE` 在 Apple `gl.h` 里不存在**（只有 `glext.h` 的 `_ARB`/`_EXT`）。
  调用方用 glad、后端用系统头 → 两边各写一遍就是「新版手写 GL 常量」（不编译失败，只画不出）。
  修法：**后端用 `target()` 说出来**，全仓仅一处字面量。
- `GlTexture` 多了 **target** 成员：纹理名字只属于创建它的 target，切路线就得切纹理集。
  `GlScene::Compose()` **按层**选 program → 一个场景可混合导入层与拷贝层。
- 导入纹理引用帧的 surface，**实现不持有**（帧的 release 回调才释放）；靠「每次上传都重新
  导入、紧接着画」满足。失败**粘滞**（拒绝一次后不再重试）。
- **只有 8-bit NV12**，P010 被拒绝而不是当 8-bit 采样。
  **SDL_Renderer 路径永远回读**（`SDL_UpdateYUVTexture` 要 CPU 字节）并报 `cpu_sync` —— 正确。
- 真机 A/B（同机同 clip）：1080p60 upload 0.70~0.82 → **0.12~0.17**；2160p30 1.875 → **0.150**
  （6.4× / 12.5×），且几乎与分辨率无关。
- pinned FFmpeg 原先 `CONFIG_VIDEOTOOLBOX 0` → 产不出 GPU 帧，做完也验证不了。
  `setup_ffmpeg.sh` 现在按 `uname` 加 `--enable-videotoolbox --enable-hwaccel=...`
  （`--disable-everything` 也关 hwaccel，两个都要）。**相对 prefix 会被 FFmpeg 按构建目录解析**
  （装到 `tools/deps/ffmpeg-*-build-shared/tools/build` 却照样打印 Installed 退出 0）→ 脚本内
  已转绝对路径。

## 渲染性能数据的唯一入口

- 三个宿主示例都支持 **`--stats`**，每秒打印 `stats {…}`。`tools/bench_render.sh` 批量取数，
  `SCENE=pip|fade` 测多源路径。**判断回归先看四段耗时，不要只看 fps。**
- **`present ≈ 刷新周期`（16.7ms）是设计意图**（`SwapBuffers` 是唯一在 vsync 阻塞的一段）。
  真正的 bug 长相是 `present ≈ 1ms` 却只有 30 fps。
- `blit_ms` 独立成字段：折进 `compose_ms` 会让「绘制变贵」与「多一次往返」分不开。
  SDLRenderer 后端它是 0 且应为 0；但 upload/compose/present 在那条路上必须是真值 ——
  **恒为 0 的字段读起来是「全都免费」而不是「没测」，比字段缺失更危险**。
- `UploadPath` 报路径而不只是耗时。L4 后 `kHwZeroCopy` 是真值；**`kPbo` 仍是空槽位**。
- **「present ≈ 1ms 却 53 fps」是既有现象**（`PostDelayedTask` 每次唤醒超 ~1.8ms，请求 16.67
  实际 ~18.5），软解 A/B 同。**改路线前后比 fps 前先看 `present_ms`**。修法：按上次的**计划**
  起始时间累加，而不是按「现在」。
- 帧计数**刻意不放进 `RenderPerfStats`**（`VideoSinkStats` 已有 presented/dropped）。
- **`max_present_ms` 是自进程启动累积的**，新进程里总是启动尖峰，不是稳态最差帧。

## 门禁命令

```sh
python3 tools/check_invariants.py --root .          # C1/C4/C5/C22-C27；C23 列宽棘轮 → 425 文件
/usr/local/opt/llvm/bin/clang-format --dry-run --Werror --style=file    # 全树 → 419 文件
cmake -S . -B <dir> -G Ninja -DAVBASE_WERROR=ON -DAVBASE_ENABLE_FFMPEG={ON,OFF} \
      -DAVBASE_ENABLE_SDL2=ON -DAVBASE_BUILD_EXAMPLES=ON -DAVBASE_BUILD_TESTS=ON
ctest --output-on-failure                            # no-ffmpeg 479 / macos-sdl2 630
```

- `avbase` 是 INTERFACE 库，ninja 不能 `--target avbase`。
- 两个预设都要跑：默认 `macos-sdl2` 预设**不开** `-Werror`，告警只在手动加时暴露。
- **`clang-format` 不在 PATH**（裸调 exit=127），必须 `/usr/local/opt/llvm/bin/clang-format`；
  同目录 clang-tidy 也是本机唯一可用那份。
- **两套文件清单数不同别套用**：不变量扫 425（全部自有文件），format 只扫 h/cc/cpp 且排除
  `render/gl/third_party`、`media/filters/legacy`、`player/option_registry.inc` → **419**
  （实测，第六十一轮提交后）。这条就是被自己违反过一次：这里原先写的是 425，那是把不变量
  的数字套到了 format 上。
- `check_invariants.py` 的 `cmake_source_paths()` 会解析 `${CMAKE_CURRENT_SOURCE_DIR}`（按
  CMakeLists 所在目录）。之前跳过任何含 `$` 的路径 → 这种写法虽被 CMake 认、文件也真编译，
  却被 C25 报「没有 target 编译它」；**喊狼来了的规则会被关掉**，所以修了。其它变量仍跳过。
- `tools/deps` / `tools/build` 在 `.gitignore`（git 视图中不存在），基于 `git ls-files` 的清单
  天然不含它们。`tools/inspect` 是自有代码（7 个文件）。
- 拼清单要连未跟踪新文件：`git ls-files --cached --others --exclude-standard`。

## CI 两股风格门禁在 main 上是**红的**（存量）

别信「门禁通过」，先看它有没有真按 job 的文件清单跑过：

- `check-clang-tidy`：`player/player_impl.cc` 20+ 条、`platform/sdl2/sdl2_audio_sink.cc` 13 条、
  头文件中 `base/logging.h` 上百条（`enum LogSeverity { LOG_VERBOSE }` 与 `.clang-tidy` 的
  `EnumConstantPrefix: k` 直接冲突）。
- `check-cpplint`：4 条 error（3 条缺 `<utility>`：`media/ffmpeg/audio_convert.cc:136`、
  `platform/{egl,glfw}/*_video_sink.cc`；1 条 `tools/inspect/inspect_common.cc:87`）。
- 根因：文档里「15 个代表性文件零发现」是**抽样**，不是跑 job 清单。

### `.clang-tidy` 的坑

- `ConstantPrefix: k` 要求**所有** const 局部量（含 `const auto x = f();`）都 k 前缀。
- `HeaderFilterRegex` 是**子串匹配**，仓库目录叫 `avbase/` → 所有头文件都命中，别被骗。
- lint 抑制唯一可用形态：**类别名写进上一行散文注释 + `// NOLINTNEXTLINE` 裸行**（带括号
  cpplint 不认，带尾随散文 clang-tidy 不认）。
- 本机四个 llvm formula 的 clang-tidy 都是 20.1.7，复现不了 CI 的 18。

## 操作教训（都真踩过）

- **声称「全树通过」前必须真扫全树。** 曾连续几轮报「0 偏离」而实际只扫了当轮触达目录。
  更根本的版本：**门禁的「绿」常常是「没跑」。**
- **`exit=127` / `command not found` 会让门禁看起来绿**（裸 `clang-format`、toybox `grep`）。
  跑完看 exit code 与输出行数；2 秒跑完的「全树格式检查」一定没跑。
- **PATH 上的 `grep` 是 toybox shim**，不支持 `\b` 与 BRE `\|` 且**静默返回 0**。用
  `/usr/bin/grep` 或 Grep 工具。**凡「用 grep 查证某符号不存在」的结论都要换工具复核。**
- clang-format 与构建**不能并发**跑同一批文件（会读到半格式化文件）。
- 后台命令末尾别用 `grep -c` 收尾（计数 0 → exit 1 → 整条任务被误报 failed）。
- 批量改名要用词边界（`platform_gl` 会误伤 `platform_glfw`）。
- **清大目录不要 `rm -rf`**（撞批量删除守卫，任务中断）→ `mv` 到 `/tmp` 再 `mkdir`。
- 改 include 路径必然影响排序，要跟着跑一次 clang-format。
- macOS sed 的 `-i ''` 空参数会被吃掉 → 用 `-i.bak` 再删。
- **macOS bash 3.2 没有 `mapfile`**：拼数组静默得空数组，`clang-format "${FILES[@]}"` 对着
  0 个文件 **exit=0**（又一次「绿是因为没跑」）。改用
  `git ls-files … > /tmp/list && xargs clang-format … < /tmp/list`（BSD xargs 无 `-a`），
  并用故意畸形的探针文件做**阳性对照**。
- **bash 3.2 + `set -u` 下空数组 `"${arr[@]}"` 会中止脚本**。`tools/bench_render.sh` 因此从来
  没在「不带 `--canvas`」下跑通过。修法 `"${arr[@]+"${arr[@]}"}"`。**可选参数分支的组合数
  要真都跑一遍。**
- 本机无 glfw3 / EGL 开发库（brew TLS 被代理拦）：GLFW 走仓库内 `tools/build/glfw`
  （`tools/setup_glfw.sh`）；EGL 后端只能结构验证。
- GitHub raw/API 被代理拦截（CONNECT 502），核实外部仓库布局用 WebSearch；
  `registry.khronos.org` 与 github.com/blob 可直连。
- **基准工具本身要先被基准过。** `bench_render.sh` 曾把 60fps 报成 53（取 median 但素材只有
  3s，启动爬坡的 `fps=0`/`fps=51` 被算进去）；又曾从**最后一个采样**读四段耗时（单点落在被
  抢占的一秒上）。现约定：**素材 ≥ 12s、丢启动段、fps 与四段耗时都取尾部 5 个采样的中位数**，
  并要求播放超出片尾。**短素材让测量工具说谎；单点采样让它随机说谎。**
- **别急着把「掉帧」归因给代码。** 两条完全独立的 present 通路（SDL_Renderer / GLFW）报出
  **同一个**可疑帧率 → 是环境（本机 clangd 后台索引把 4 核 load 压到 12）在说话。
  两条通路给出同一个错数是环境；只有一个变才是代码。

## 提交切片怎么切（L1–L4 落成了 8 片）

- **按依赖层次切，不按「轮」、也不按功能主题。** 同一批文件内部是交织的
  （`gl_present.cc` 同时含 L2/L3/L4，`sdl2_video_sink.cc` 横跨四层），所以「每片都能独立
  编译」做不到；能让每片不自相矛盾的是**先提交被引用的，后提交引用者**。
  实际顺序：tools → 数据面(L1) → render/gl 原语(L2) → media/gl 场景层(L3) →
  platform 后端(L4) → 宿主装配 + 示例 → 测试 → 文档。
- **接口扩展必须和它的全部实现者一起，或者放到最后** —— 这是硬约束。反过来说，只加**数据
  字段**的接口扩展（`RenderPerfStats render;`）不破坏既有实现者，可以单独成片。
- **新 `.cc` 是 GLOB 收进来的**（`render/`、`media/` 都用 `file(GLOB ... CONFIGURE_DEPENDS)`），
  新增源文件不用改 CMakeLists。代价是：新增文件与引用它的改动必须同一片，否则中间那片会
  引用未声明的符号。
- 每片提交后跑 `check_invariants`（几秒）；**完整构建只在最后跑一次** —— 中间片的自洽靠上面的
  依赖顺序保证，不靠逐片编译验证。

## 其它

- 可见性：`CMAKE_CXX_VISIBILITY_PRESET hidden`，导出类需 `AVBASE_MEDIA_EXPORT`；从隐藏基类
  派生导出类触发 `-Wvisibility` → 共享基类也要导出。
- 文档：`docs/09-*` 已归档到 `docs/archive/`，注释用短形式 `docs/archive/09 §N`，**不要写完整
  中文文件名**（撑破 80 列）。`docs/13` = `avbase_gl` 对外契约；`render/gl/third_party/glad/
  README.md` = glad 生成命令。

## 渲染四层路线图（对照 MediaComponent + libobs-gl）

目标：把 `render/gl/` + `media/gl/` 提到 MediaComponent `framework/render`（29,228 行）与
libobs-opengl 的水平。avbase 原渲染层 1,936 行，差 15 倍 → 拆四层**按序**，每层跑全门禁 + 真机验证。

| 层 | 内容 | 状态 |
| --- | --- | --- |
| L1 | 渲染性能可观测性（upload/compose/present 耗时、上传路径、帧率） | **完成** |
| L2 | FBO 合成地基（离屏 FBO → blit 上屏两段式） | **完成** |
| L3 | 多源场景层（图层 + z 序 + rect/opacity + 交叉淡化） | **完成** |
| L4 | 硬件零拷贝导入（IOSurface/EGLImage → GL 纹理） | **完成** |

参考框架对应物：L2 ↔ `render/gl/render_fbo.h` + `gl_compositor`；L3 ↔ `render/scene/`
（5,464 行）；L4 ↔ `render/hw/hw_texture_importer*`（2,209 行）。

**四层已全绿收口，剩余差距在播放器层**（管线仍单源）与既有的 `PostDelayedTask` 节流。
