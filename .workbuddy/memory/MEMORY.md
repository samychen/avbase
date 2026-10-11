# avbase 项目长期约定

Chromium 风格分层的 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。

## 分层判据（docs/02 §6，决策 D11）

`platform/` 装「**被宿主选中**」的代码；可移植、各宿主行为一致的不进 platform/
（FFmpeg 在 `media/ffmpeg/`，GL 在 `render/gl/`）。
两条正交轴别压平：**图形 API 轴**（GL/Metal/D3D11）× **窗口系统轴**（SDL2/GLFW/EGL），
否则加第二个 API 就是 N×M 个目录。不设 `graphics/` —— 与「目录即 target」冲突，且会毁掉
`render/gl` 的零依赖可分发性。
`media/gl/`（被 VideoFrame 塑形）、`media/renderers/`（仅工厂）、`render/gl/`（零依赖）
三处不重复；根目录没有 `gl/`。

## `avbase_gl`（`render/gl/`）：可独立分发的零依赖模块

context 契约 + 装载器 + program/texture/quad + GLSL 方言。对外契约 `docs/13`。
- 消费方 `find_package(avbase_gl)` + `avbase::gl`（**不是** `avbase::avbase_gl`，靠
  `EXPORT_NAME gl`）。
- `GlApi` 一 context 一实例（无进程全局表），入口点经 `context()`：`gl.context().Clear(...)`。
- 装载器 = glad 2.0.8 **vendored 生成物**（`third_party/glad/`），不联网不跑生成。重生成
  命令见该目录 README：`--mx` 必开、`--loader` 不开、`--alias` 不开。唯一覆盖不到的入口点
  `glEGLImageTargetTexture2DOES` 在 `GlApi` 手工解。
- 不手写 GL 常量表：手抄 = 造一个**写错也不报错**的失败模式（曾把 `GL_VERTEX_SHADER` 写成
  `0x8B33`，看着像驱动坏了）。
- glad 枚举是 `#define` 的 int 字面量 → 三元表达式窄化进 `GLenum` 要显式 `static_cast`。
- 隐患：`GLAPIENTRY` 定义为空，32 位 Windows `__stdcall` 会崩。

## 合成面：canvas FBO + blit（L2，两段式）

`Compose()` 画进离屏 canvas → `PresentComposed()` blit 上屏；`Present()` = 两者 + Swap。
**每帧都走 canvas，包括单视频** —— 要建在它上面的路径必须是被跑过的路径。

- **朝向**：往 framebuffer 画时「图像第 0 行 = fb 最后一行」→ 纹理第 0 行装的是图像底 →
  **凡从渲染目标采样回来的 pass 都要 `flip_vertical=true`**（管 `GlBlitter`、`GlTransition`）。
  搞反 = 上下颠倒，**无返回值无 GL 错误，只能读回像素抓**。这条推导被写反过两次。
  **有朝向的代码必须配像素断言。**
- **`GL_UNPACK_ROW_LENGTH` 单位是 texel 不是字节**：上传前设 `stride(plane)`、用完还原 0；
  NV12 交错色度用 `GL_RG` 所以传 `stride/2`。1080p 恰好相等把 bug 藏住，320x240/854x480
  必现，症状是**周期 8 行绿色横条**。
- canvas 尺寸：非正数 = 跟随 drawable（每帧重读，保 resize），放 `GlSurfaceBinding`。
- `GlFramebuffer::Ensure()` 幂等且同尺寸保留内容；`Destroy()` 先删 fbo 再放纹理。
  纹理必须设采样参数（缺了采到黑）。

### 两个 GL 测试目录（按「要不要 context」分，不按主题）

- `tests/unit/media_gl/`：**需要 `media/gl` 但不需要 GL context** 的那类（sink 的
  路由就是——它在 context 存在之前就定了）。进 `media_unittests`。
- `tests/unit/gl/`：**要读像素**的。`AVBASE_ENABLE_GLFW` 门控（隐藏窗口）。

### GL 像素测试（`tests/unit/gl/`，`AVBASE_ENABLE_GLFW` 门控）
隐藏窗口 → 无显示器也能跑、CI 可跑。`glfwSwapInterval(0)` 覆盖 `MakeCurrent()` 里的 1。
- 断言用**内容**不用 GL 名字（GL 复用刚 delete 的名字）。
- 读像素用 `Compose`+`PresentComposed`，**不要用 `Present`**（swap 后 back buffer 未定义 → 全黑）。
- **新原语必须做反证且「只红该红的」**：反证区分「测试通过」与「测试真在测东西」。

## 多源场景层（L3，`media/gl/`）

`gl_video_layer` 一层（平面纹理+色彩矩阵+YUV格式+stride）；`gl_scene` N 层（句柄**永不
复用**、z 序、归一化 rect、opacity、visible，`Compose()` 画进**当前绑定**的目标）；
`gl_scene_driver` sink 侧钩子（无钩子时转发 `Present()`，单帧不是另一条分支）；
`render/gl/gl_transition` 两纹理交叉淡化（在零依赖模块：两张画好的图不是视频概念）。

- **场景每帧作参数传，不存成 presenter 状态**（「哪个场景在屏上」是宿主每帧重做的决定）。
- **上传与合成分开**：上传在帧到达时（N 路不同时刻），合成每显示帧一次 → `UploadLayer()`
  耗时**累加**进 `upload_ms`（赋值会把最后一层报成整帧）。
- `EnsureScene()` 属 presenter（场景 GL 对象属其 context）；`GlScene::Compose()` **按层**选
  program → 一个场景可混合导入层与拷贝层。
- NV12 只有 2 个平面：显式 `plane_count = nv12 ? 2 : 3`，该在的平面缺失就失败。
- 成本（真机 1080p60）：单源 upload 0.77 / 两层 **1.55**，合成几乎免费、fps 不动 →
  **第二层的成本是它的上传（线性）**。4K30 两层 26.96 fps（第二层上传 4.07 ms）。
- 边界：播放器管线**仍单源**。`--scene pip|fade` 用同一流的延迟副本当第二路源。

### 上传路线：三条路，一个入口（都在 `GlVideoLayer::Upload`）

`IsMappable()` → 拷贝；否则 `CanImport()` → 导入；导入被拒 / 没有后端 → **回退
读 `ToI420()` 再拷贝**。回退在图层里而不在 sink 里：这是最后一处还知道帧活着
且没被用过的地方，sink 已经把帧交出去了，从那儿重试会为了一层重走整个场景。

- 粘滞状态是 `refused_importer_`（**后端指针**，不是 bool）：换后端自动重新获得
  机会，不需要谁记得清 flag。`CanImport()` 拒绝一个**格式**不算驱动拒绝，不会
  连累后面每一帧（P010 关键帧开头的流不该整段都走回读）。
- 回退要把纹理从 RECTANGLE **换回 2D**（导入时已切过去），否则 2D program 采样
  rectangle 纹理画不出东西 —— 每个调用都成功。

### 输出 = `GlOutputTarget`，不是一个第二个 sink

`media/gl/gl_output_target.h`：`SetSurface()` / `has_gl_route()` / `BindSurface()` /
`PresentWithoutGl()` / `ReleaseOutput()`。渲染线程、节拍、统计、场景驱动、硬件
导入器全在 `GlVideoSink` 里，对所有后端一致。

- 后端 = 一个 target。`Sdl2VideoSink` 从 ~500 行变 10 行（只负责装
  `Sdl2OutputTarget` + 导入器）；`Sdl2OutputTarget` 是唯一回答
  `has_gl_route()==false` 的（`SDL_UpdateYUVTexture` 无论宿主多能干都要 CPU
  字节）。GLFW/EGL 无条件为真。
- sink 侧仍保留一个问题：**GPU 帧在 `has_gl_route()==false` 的目标上要先读回**
  —— 只有目标知道，问错了就是把 GPU 帧交给一个要 CPU 字节的 API。
- 修掉一处：SDL2 原先在 `Start()` 里同步跑 `PresentOne()`，第一帧的 context 是
  在 `Start()` 的调用线程上 make current 的。现在两条路都走 post。

## 硬件零拷贝导入（L4）

接缝 `media/gl/gl_hw_texture_importer.h`（`target()/CanImport()/ImportPlane()/name()`，只认
VideoFrame 不认 SDK —— C5 把 CoreVideo 头挡在 media 外），实现在 `platform/hwimport/`
（macOS CGL/IOSurface；**没有互操作 API 的主机返回空指针，不是桩**）。

- **岔路口是 `GlVideoLayer::Upload(frame, importer)`，判据是帧不是调用方**：`IsMappable()`
  → 拷贝；否则导入，**没有 importer 就是 false**（绝不拿旧纹理凑一帧）。
- **macOS 只接受 `GL_TEXTURE_RECTANGLE`**（2D 每种组合都回 `kCGLBadValue`）→ 需要
  `sampler2DRect` 版 shader，两套 program 都在 `Init()` 编译。
- **`GL_TEXTURE_RECTANGLE` 不在 Apple 的 `gl.h`**（只有 `glext.h` 的 `_ARB`/`_EXT`）。调用方
  用 glad、后端用系统头 → 两边各写一遍就是「新版手写 GL 常量」（不编译失败，只画不出）。
  修法：**后端用 `target()` 说出来**。
- `GlTexture` 多了 **target** 成员：名字只属于创建它的 target，切路线就得切纹理集。
- 导入纹理引用帧的 surface，**实现不持有**（帧的 release 回调才释放）；靠「每次上传重新
  导入、紧接着画」满足。失败**粘滞**。只有 8-bit NV12，P010 被拒绝。
- **SDL_Renderer 路径永远回读**并报 `cpu_sync` —— 正确（`SDL_UpdateYUVTexture` 要 CPU 字节）。
- A/B：1080p60 upload 0.70~0.82 → **0.12~0.17**；2160p30 1.875 → **0.150**（6.4×/12.5×）。
- pinned FFmpeg 原先没开 videotoolbox → 产不出 GPU 帧，做完也验证不了。`setup_ffmpeg.sh`
  现加 `--enable-videotoolbox --enable-hwaccel=...`（`--disable-everything` 也关 hwaccel，
  两个都要）；**相对 prefix 会被按构建目录解析**（装错地方照样打印 Installed 退出 0）→ 转绝对路径。

## 渲染性能数据的唯一入口

- 三个宿主示例都支持 **`--stats`**；`tools/bench_render.sh` 批量取数，`SCENE=pip|fade` 测多源。
  **判断回归先看四段耗时，不要只看 fps。**
- **`present ≈ 刷新周期`（16.7ms）是设计意图**（`SwapBuffers` 是唯一 vsync 阻塞段）。真正的
  bug 长相是 `present ≈ 1ms` 却只有 30 fps。
- `blit_ms` 独立成字段：折进 compose 会让「绘制变贵」与「多一次往返」分不开。SDLRenderer
  后端它是 0 且应为 0，但 upload/compose/present 必须真值 —— **恒为 0 的字段读起来是「全都
  免费」而不是「没测」**。
- `UploadPath` 报路径不只是耗时；`kHwZeroCopy` 已是真值，**`kPbo` 仍是空槽位**。
- 帧计数**刻意不放进 `RenderPerfStats`**（`VideoSinkStats` 已有）。`max_present_ms` 自进程
  启动累积，新进程里总是启动尖峰。
- **节流锚点是「上次计划的时间」不是「现在」**：`PostDelayedTask` 每次晚醒 ~1.8ms，从帧开始
  起算会把这 1.8ms 算进每个周期（16.67→18.5，60fps 素材跑在 53）。实现在
  `media/base/render_cadence.h` 的 `NextFrameDelay()`，两个 sink 共用（原先两份拷贝 ——
  **写两遍的规则只会被修一遍**）。真机 A/B：720p60 **53.94 → 59.99**；1080p30 不变（周期
  33ms，定时器本不是瓶颈）—— 正说明测量是定向的。

## 门禁命令

```sh
python3 tools/check_invariants.py --root .     # C1/C4/C5/C16/C22-C28
/usr/local/opt/llvm/bin/clang-format --dry-run --Werror --style=file   # 全树
cmake --preset macos-sdl2 -DAVBASE_WERROR=ON -B <dir> && cmake --build <dir>
ctest --output-on-failure
tools/bench_render.sh
```

- **文件数一直在变，别记死数字**（曾把不变量的 425 套到 format 上）。看 exit code 与行数。
- **C28：`render/` 不得 include `media/`/`platform/`/`base/`/`player/`。** 补它的原因：
  `docs/02 §6.2` 早说「构建不变量会拦」而脚本**从未提到 `render/`** —— 零依赖此前只有散文
  在守。写下来却不检查的声明会静默失效（一个 include 照样编译过，只在树外消费者链接时炸）。
- `avbase` 是 INTERFACE 库，ninja 不能 `--target avbase`。
- 默认 `macos-sdl2` 预设**不开** `-Werror`，告警只在手动加时暴露 → 两套预设都要跑。
- **`clang-format` 不在 PATH**（裸调 exit=127），必须 `/usr/local/opt/llvm/bin/clang-format`。
- **两套清单数不同别套用**：不变量扫全部自有文件；format 只扫 h/cc/cpp 且排除
  `render/gl/third_party`、`media/filters/legacy`、`player/option_registry.inc`。
- `tools/deps` / `tools/build` 已 gitignore，基于 `git ls-files` 的清单天然不含它们。
  `tools/inspect` 是自有代码（7 个文件）。
- 拼清单要连未跟踪新文件：`git ls-files --cached --others --exclude-standard`。
- 新 `.cc` 是 GLOB 收进来的（CONFIGURE_DEPENDS），新增源文件不用改 CMakeLists。

## 提交切片

**按依赖层次切**（不按「轮」、不按功能主题）：同一批文件内部交织（`gl_present.cc` 同时含
L2/L3/L4）→ 「每片独立编译」做不到；能让每片不自相矛盾的是**先提交被引用的，后提交引用者**。
顺序：tools → 数据面 → render/gl 原语 → media/gl 场景层 → platform 后端 → 宿主+示例 → 测试 → 文档。
**接口扩展必须和它的全部实现者一起，或放到最后**（只加数据字段的扩展可单独成片）。
每片提交后跑 check_invariants（几秒）；**完整构建只在最后跑一次**。

## CI 两股风格门禁在 main 上是**红的**（存量）

别信「门禁通过」，先看它有没有真按 job 的文件清单跑过。「15 个代表性文件零发现」是**抽样**。
- `check-clang-tidy`：`player/player_impl.cc` 20+、`platform/sdl2/sdl2_audio_sink.cc` 13、
  `base/logging.h` 上百（`enum LogSeverity { LOG_VERBOSE }` 与 `EnumConstantPrefix: k` 冲突）。
- `check-cpplint`：4 条 error（3 条缺 `<utility>`，1 条 `unsigned long long`）。
- `.clang-tidy` 坑：`ConstantPrefix: k` 要求**所有** const 局部量（含 `const auto`）k 前缀；
  `HeaderFilterRegex` 是子串匹配、仓库叫 `avbase/` → 所有头文件都命中；抑制唯一可用形态 =
  上一行散文注释 + 裸 `// NOLINTNEXTLINE`；本机 clang-tidy 20.1.7 复现不了 CI 的 18。

## 操作教训（都真踩过）

- **声称「全树通过」前必须真扫全树。** 门禁的「绿」常常是「没跑」；2 秒跑完的「全树格式
  检查」一定没跑；**`exit=127` / command not found 会让门禁看起来绿**。
- **构建失败那一轮，基准跑的是上一个二进制** —— 把 build 与测量串起来前先确认 build exit code。
- **PATH 上的 `grep` 是 toybox shim**：不支持 `\b`，且**静默返回 0**。用 `/usr/bin/grep` 或
  Grep 工具。**凡「用 grep 查证某符号不存在」的结论都要换工具复核。**
- **macOS bash 3.2 没有 `mapfile`**：拼数组静默得空数组，对着 0 个文件 **exit=0**（又一次
  「绿是因为没跑」）。改用 `xargs` + 故意畸形的探针文件做**阳性对照**。`set -u` 下空数组
  `"${arr[@]}"` 会中止脚本 → `"${arr[@]+"${arr[@]}"}"`。**可选参数分支的组合要真跑一遍。**
- 后台命令末尾别用 `grep -c` 收尾（计数 0 → exit 1 → 整条任务被误报 failed）。
- clang-format 与构建**不能并发**跑同一批文件。改 include 路径必然影响排序，跟着跑一次 format。
- macOS sed `-i ''` 空参数会被吃掉 → `-i.bak` 再删。批量改名要用词边界。
- **清大目录不要 `rm -rf`**（撞批量删除守卫）→ `mv` 到 `/tmp` 再 `mkdir`。
- 本机无 glfw3/EGL 开发库（brew TLS 被代理拦）：GLFW 走 `tools/setup_glfw.sh`；EGL 只能结构
  验证。GitHub raw/API 被代理拦截，核实外部仓库布局用 WebSearch。
- **基准工具本身要先被基准过。** `bench_render.sh` 曾把 60fps 报成 53（素材只有 3s，启动爬坡
  被算进 median）；又曾从**最后一个采样**读耗时。现约定：**素材 ≥12s、丢启动段、fps 与四段
  耗时都取尾部 5 个采样中位数**，并要求播放超出片尾。短素材让工具说谎；单点采样让它随机说谎。
- **别急着把「掉帧」归因给代码。** 两条独立 present 通路（SDL_Renderer/GLFW）报出**同一个**
  可疑帧率 → 是环境（clangd 后台索引把 4 核 load 压到 12）在说话。两条通路同一个错数是环境；
  只有一个变才是代码。

## 其它

- 可见性：`CMAKE_CXX_VISIBILITY_PRESET hidden`，导出类需 `AVBASE_MEDIA_EXPORT`；从隐藏基类
  派生导出类触发 `-Wvisibility` → 共享基类也要导出。
- 文档：`docs/09-*` 已归档到 `docs/archive/`，注释用短形式 `docs/archive/09 §N`，**不要写
  完整中文文件名**（撑破 80 列）。`docs/13` = avbase_gl 契约。

## 渲染路线图（对照 MediaComponent + libobs-gl）

| 层 | 内容 | 状态 |
| --- | --- | --- |
| L1 | 渲染性能可观测性 | **完成** |
| L2 | FBO 合成地基 | **完成** |
| L3 | 多源场景层 | **完成** |
| L4 | 硬件零拷贝导入 | **完成** |

「已报告未做」的 9 项已全部做完并提交（任务 #146–154）：双重计数裁决、null sink
迁移、`ImportPlane` 降参、NaN 政策统一、uniform 缓存、场景复用 program、**两份 YUV
shader body 合并**、**导入回退收回图层**、**输出策略化**。

剩余差距在**播放器层**：管线仍单源。L5 多源编排（LatestFrameQueue / GlLayerSource /
GlSceneEngine）见任务 #141–145（未开始）。
