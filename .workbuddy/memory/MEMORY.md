# avbase 长期约定

Chromium 风格分层 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。
详细轮次记录见 `docs/PROGRESS.md`，GL 模块契约见 `docs/13`。
门禁命令与操作教训见 `operations.md`。

## 分层（docs/02 §6，D11）
`platform/` 装「被宿主选中」的代码；可移植、各宿主一致的不进（FFmpeg→`media/ffmpeg/`，
GL→`render/gl/`）。**图形 API 轴 × 窗口系统轴**别压平，否则加第二个 API 就 N×M 个目录。
不设 `graphics/`。`media/gl/`（被 VideoFrame 塑形）/ `media/renderers/`（仅工厂）/
`render/gl/`（零依赖）不重复。
**C28：`render/` 不得 include `media`/`platform`/`base`/`player/`。**

## avbase_gl（render/gl/）零依赖可分发模块
`find_package(avbase_gl)` + `avbase::gl`（靠 `EXPORT_NAME gl`）。`GlApi` 一 context
一实例，入口点 `gl.context().Clear(...)`。装载器 = glad 2.0.8 vendored 生成物
（`--mx` 开、`--loader`/`--alias` 不开）；唯一漏网 `glEGLImageTargetTexture2DOES`
手工解。**不手写 GL 常量表**（手抄 = 造一个写错也不报错的失败模式）。glad 枚举是
`#define` int → 三元窄化进 GLenum 要 `static_cast`。隐患：`GLAPIENTRY` 为空，
32 位 Win 会崩。

## 三条「写下来会静默失效」的约定（都真踩过）

**朝向。** 图像第 0 行落在视口顶 = framebuffer 最后一行 → 纹理第 0 行装的是图像底 →
**凡从渲染目标采样回来的 pass 都要 `flip_vertical=true`**（管 `GlBlitter` 与
`GlTransition`）。搞反 = 上下颠倒，**无返回值、无 GL 错误**。这条推导被独立写反过
两次 → **有朝向的代码必须配像素断言**。

**stride。** `GL_UNPACK_ROW_LENGTH` 单位是 **texel 不是字节**：上传前设
`stride(plane)`、用完还原 0；NV12 交错色度用 `GL_RG` 所以传 `stride/2`。1080p 恰好
相等把 bug 藏住，320x240/854x480 必现（**周期 8 行绿色横条**）。

**GL 常量。** 一个常量别在两个头世界里各写一遍（`GL_TEXTURE_RECTANGLE` 不在 Apple 的
`gl.h`）→ 后端用 `target()` 说出来。

## 多源编排（L5）
`media/base/latest_frame_queue` → `media/gl/gl_layer_source` →
`media/gl/gl_scene_engine`；`gl_scene_driver.{h,cc}` 已删（职责被引擎完整吸收）。
- **单槽邮箱，不是队列**：生产者超前 → **取代**槽里那帧。`Publish()` 交回被取代的帧，
  因为生产者是**唯一**看见它的一方，否则「解码了没显示」根本无从知晓。
  恒等式 `published == consumed + superseded + cleared + (pending?1:0)`，四项分开是因为
  「没被显示」的三种原因在排查时是不同的诊断。
- **两个方向都保留**：播放器的流被 `Render()` 拉（A/V 同步基准）；其它源推到各自邮箱
  （有自己的时钟）。压成一个形状会让显示等外来解码器，或给播放器的帧套一个仅为满足
  抽象而存在的邮箱。
- **图层持有它上传的那一帧**：拷贝路线看不出差别，但**导入纹理指向解码器的 surface，
  释放它的正是帧的 release 回调** → 不专门测则本机全绿、有导入后端的机器崩。

### 已知未钉住
引擎的早退（无源、无钩子时走单帧路径）**没有反证**：去掉它 5 条测试全绿。原因是从公开
API 观测不到 —— 两条路径逐像素输出相同，差别只在「有没有分配一个场景」。钉住它要加
`scene()` 查询 = 扩大导出类接口，第六十五轮没擅自做。

## 性能数据唯一入口
三宿主示例均支持 `--stats`；`tools/bench_render.sh`，`SCENE=pip|fade` 测双层，
`SECOND=<url>` 测真第二路解码。
- **判断回归先看四段耗时，不要只看 fps。**
- `present ≈ 16.7ms` 是设计意图；真 bug 是 `present ≈ 1ms` 却 30fps。
- `blit_ms` 独立成字段；恒 0 读作「全免费」而不是「没测」。
- **「第二层让 upload 翻倍」是 L4 之前的旧结论**（当时 CPU 拷贝）。硬件导入只是重指
  指针，1080p60 单帧约 0.03~0.09 ms，**低于本机逐轮噪声** ——
  数字没变不等于没生效（第六十五轮实测确认）。
- 节流锚点是「上次计划的时间」不是「现在」（`media/base/render_cadence.h`）。

## 路线图
L1 可观测性 / L2 FBO 合成 / L3 多源场景层 / L4 硬件零拷贝 / L5 多源编排 —— **全部完成**。
剩余差距在播放器层：管线仍单源，L5 用「第二路 Player + 邮箱」绕过去。
