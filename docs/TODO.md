# TODO · 未完成工作（按建议排序）

> 本文件是**待办清单**，与 [12-剩余工作清单](12-剩余工作清单.md)（执行快照，含历史
> 决策与已闭合项）和 [PROGRESS.md](PROGRESS.md)（轮次活文档）互补。
> 排序依据是**性价比 × 用户可见性**：先修正确性 bug，再补结构性缺口，
> 最后做「等需求出现才启动」的能力。每条给出现状证据与验收判据。

---

## 0. 背景：差距的位置已经变了

L1–L5（可观测性 / FBO 合成地基 / 多源场景层 / 硬件零拷贝 / 多源编排）全部完成后，
**渲染层不再是短板**。剩余差距集中在下两层：

| 层 | 状态 |
|---|---|
| `render/gl/`（零依赖 GL） | ✅ 已闭合 |
| `media/gl/`（渲染编排） | ✅ 基本闭合，残留图层级属性 |
| `media/filters/`（播放器管线） | 🔶 单源硬限制 |
| `player/public/`（SDK 门面） | ❌ 多图层/多源无出口 |

---

## 1. 【P1】✅ 已完成（第六十六轮）渲染侧消费 rotation 元数据

**原状**：rotation 被一路解析、上报，但**渲染完全不看它**。

```
media/base/decoder_config.h:69      int rotation{0}
media/base/video_frame.h:128        int rotation() const
media/ffmpeg/ffmpeg_demuxer_configs.cc:60-67   从 display matrix 解析
player/player_impl.cc:362           video_rotation_ = v->rotation
player/public/player.h:117          int video_rotation() const
player_event.h:50                   事件 kRotationChanged
```

而 `media/gl/`、`render/gl/`、`platform/` 三个目录里 grep `rotation` **零命中**。

**后果**：手机竖拍素材（编码为横向像素 + rotation=90 标记）会**横着显示**。
这是用户可见的正确性 bug，不是缺失功能。

**验收**（已完成）：
- ✅ `VideoFrame::NaturalSizeForRotation()`：90/270 时返回 `(h, w)`，0/180 时原样。
- ✅ 图层可显式设 rotation（含「恢复跟随帧」）。
- ✅ 像素测试：同一帧分别在 0 / 90 / 180 / 270 下出画，白色半边每次落在不同边。

**落地要点**

`VideoFrame::rotation()` **此前全仓没有任何写入者**，永远读回 0 —— 所以光让渲染
看它是没用的，得先把值送上帧。三处补齐：

| 层 | 改动 |
|---|---|
| `media/base/video_frame.h` | `set_rotation()`（与既有 `set_color_space()` 同风格）+ `NaturalSizeForRotation()` |
| `media/ffmpeg/ffmpeg_video_decoder.cc` / `ffmpeg_hw_video_decoder.cc` | 出帧时 `out->set_rotation(config_.rotation)` —— 软硬两条路都要，否则硬解素材丢了旋转 |
| `render/gl/gl_shaders.cc` | `kQuadVertexShaderWithRectBody` 加 `#ifdef ROTATE`，`uRotation` = (cos, sin) |
| `media/gl/gl_video_layer.cc` | `CreateLayerProgram` 拼 `kVertexRotationDefine`，并把新 program 留在「不旋转」的默认状态 |
| `media/gl/gl_scene.{h,cc}` | `SetLayerRotation()` / `LayerRotation()`；`Upload()` 时捕获帧的旋转 |
| `media/gl/gl_present.cc` | 单帧路径按帧的旋转设 uniform |

两个不显然的决定：

1. **旋转纹理坐标，不是旋转 quad。** 两者只在 rect 为正方形时等价；动 quad 会把
   图层自己的矩形从画面底下转走，于是「图层相对自己的 rect 转了」而不是
   「画面转了」。
2. **`uRotation` 必须在建 program 时设成 (1, 0)。** 没人设过的 `vec2` 读回
   (0, 0)，而 (0, 0) 不是单位元 —— 它把四个顶点全送去中心，结果是一帧
   全黑，没有错误也没有日志。这跟「翻转写反」是同一类静默失败。

**已知限制（诚实写明）**：旋转后的画面仍按 stretch-to-fill 铺进 rect，所以
竖拍素材转正后会被横向拉伸。要真正正确显示，需要 **aspect-fit**
（见 §4）—— 渲染层现在完全不读 `natural_size`，这是独立的第四项。

---

## 2. 【P2】SDK 层暴露场景 API —— 库里有，调用方拿不到

**现状**：`player/public/player.h`（47 个公开方法）与 `player/public/deps.h` 里
grep `scene|layer|pip` **零命中**。`GlScene` / `GlSceneEngine` / `GlLayerSource`
全是 `media/gl/` 内部类型，SDK 用户摸不到。

**后果**：多图层合成与多源编排**只有宿主自己绕过播放器才能用** ——
`examples/scene_demo.h:178` 的 `--second`（第二个 `Player` + `PublisherSink`）
正是这么绕的。库有这个能力，SDK 用户没有。

**验收**：`Player` 上能拿到场景句柄、加源、设图层 rect/opacity/z；
示例不用绕过 `Player` 也能出双层画面。

---

## 3. 【P3】CPU 滤镜与硬件零拷贝互斥，且是静默退化 —— 内部矛盾

**现状**：滤镜挂在 `media/filters/video_renderer_impl.cc:370-403`，位置是
**解码之后、`compositor_.PutCurrentFrame()` 之前**。

- `ffmpeg_video_filter.cc:151` 要求帧格式与 ctx 匹配；
- `VideoFrame::visible_data()` 对不可映射的 GPU 原生帧返回**空 span**
  （`media/base/video_frame.cc:150-155`）。
- 而 `GlVideoLayer::Upload()` 的零拷贝路线只认**不可映射**的原生帧
  （`gl_video_layer.cc:117-120, 182-222`）。

**后果**：两条路**不能同时走**。开了 `filter_graph`，硬解帧就得先读回或退回软解，
`upload_path` 从 `hw_zero_copy` 落到 `cpu_sync` —— **没有任何日志说明这不是配置错误，
也没有测试钉住这条退化**。这是「两个功能各自都对、合起来互相拆台」的形状。

**验收**：至少改成**明确拒绝并说明**（日志/统计里可见），而不是静默退回 CPU；
更好的做法是让统计里能看出「本帧因为滤镜而失去了零拷贝」。

---

## 4. 【P4】图层级属性与多转场 —— 等需求出现再启动

**现状**：`GlScene::Layer` 只有 5 个属性（`gl_scene.h:143-151`）：
`rect` / `z` / `opacity` / `visible` / `id`。

确认缺失（全目录 grep 零命中）：

| 缺失项 | 备注 |
|---|---|
| 旋转 rotation | 见 §1，单独处理 |
| 背景色 | 硬编码 `ClearColor(0,0,0,1)`（`gl_scene.cc:186`） |
| 混合模式 blend mode | 固定 `GL_SRC_ALPHA`（`gl_scene.cc:194`） |
| 源矩形裁剪 crop | shader 只有 `vTexCoord` 直通，无 uv 变换 uniform |
| aspect-fit 模式 | 渲染层不读 `natural_size`，当前是 stretch-to-fill |
| 镜像 / 色彩调整 / 图层名 | 均无 |

**转场**：只有 cross-fade 一种（`render/gl/gl_transition.h:19` 原文
"the only transition the module ships"）。36 个转场 GLSL 属**刻意不移植**
（[12 §4 5.5](12-剩余工作清单.md) 决策：avbase 无此产品形态），不算欠账。

---

## 5. 【P4】确认没有的产品化能力

| 能力 | 现状 |
|---|---|
| 录制（播放侧落盘） | ❌ 无。只有 `media/transcode/` 的离线转码（输入是文件，不是实时输出） |
| 推流 / RTMP publish | ❌ 无。`rtmp` 仅作**输入**协议（`ffmpeg_demuxer.cc:300-301`） |
| 缩略图 / seek preview | ❌ 无。`get_frame_mode` 是**死选项**：`player_config.h:218` 有配置、
  `option_registry.inc:212` 有注册，`media/` 里**零命中** |
| HDR / tone mapping | ❌ 无实现。`player_config.h:51` 有枚举、`:117` 有字段，
  但 `hdr_tone_mapping` 在 `media/`/`render/` 里**零命中**；
  事件 `kHdrUnsupported` **从未被 emit** |
| PBO 上传 | ❌ 未实现。`render_perf_stats.h:28` 的 `kPbo` 注明 "not built" |
| 真色彩管理 | ❌ 无。只有 YUV→RGB 矩阵（`gl_video_layer.cc:69-113`），
  无 gamut 转换 / EOTF / ICC / HDR→SDR |

---

## 6. 环境相关（非代码，需特定机器/权限）

沿用 [12 §1 2.4 / 2.5 / §2](12-剩余工作清单.md) 的登记：

| 项 | 阻塞原因 |
|---|---|
| RTMP / RTSP / SRT 播放验收 | 无法从文件伺服，需真实 peer |
| 真 `tc netem` 弱网 | 需 root；macOS 无 |
| Linux 显示侧零拷贝（VAAPI→EGL） | 需 Linux + VAAPI 设备 |
| Windows 显示侧零拷贝（D3D11-GL） | 需 Windows + D3D11 环境 |
| 硬解性能对照（4K CPU < 15%） | 需 4K 样本入库 |
| Qt 门面 / soak 48h | Phase 4 剩余 |

---

## 7. 进度记录

| 项 | 状态 | 落地 |
|---|---|---|
| §1 旋转（渲染侧消费 rotation） | ✅ 完成 | 第六十六轮 |
