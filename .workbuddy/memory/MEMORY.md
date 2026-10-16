# avbase 长期约定

Chromium 风格分层 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。轮次记录见
`docs/PROGRESS.md`，GL 契约见 `docs/13`，门禁命令见 `operations.md`。

## 分层（docs/02 §6）
`platform/` 只装「被宿主选中」的代码；可移植代码不进（FFmpeg→`media/ffmpeg/`，
GL→`render/gl/`）。**C28：`render/` 不得 include `media`/`platform`/`base`/`player/`。**

## 四条「写下来会静默失效」的约定（都真踩过）
- **朝向**：从渲染目标采样回来的 pass 都要 `flip_vertical=true`。搞反 = 上下
  颠倒、无 GL 错误 → 有朝向的代码必须配像素断言。
- **stride**：`GL_UNPACK_ROW_LENGTH` 单位是 **texel 不是字节**；1080p 恰好相等
  把 bug 藏住，320x240/854x480 必现（周期 8 行绿色横条）。
- **GL 常量**：一个常量别在两个头世界各写一遍 → 后端用 `target()` 说出来。
- **变换顺序**：顶点 shader 是 screen→source **反向映射**，观众最后看到的一步
  计算上最先做 → CROP 写在 ROTATE **之后**。别用算术顺序描述视觉顺序。

## 其它踩过的坑
- `VideoFrame::rotation()` 曾全仓无写入者：**修消费端前先确认真有人在写**。
- `uRotation`/`uSrcRect` 建 program 时设单位元；未设 vec2 读回 (0,0) = 全黑。
- SDK 合成能力由 sink 自答（`scene_engine()` 虚接口）——**不能 static_cast**
  （全仓 `-fno-rtti`，默认 Deps 给 NullVideoSink，在那里是 UB）。
- 本机 clang-tidy 对 `media/ffmpeg` **静默零发现**（PCH 格式读不了 → exit 0），
  必须配无 PCH scratch 库（命令在 `operations.md`）。**本机的干净 ≠ CI 的。**
- virtual/override 方法**不挂默认参**（违反 google-default-arguments，tidy 实跑
  抓到，零测试能发现的编译期规则）。

## docs/14 管线多源（第七十四～七十八轮落地）
`CompositeDemuxer` 聚合 N 源；聚合索引 `index = source_id*stride + child_index`
（stride = max 子索引+1，源 0 偏移 0 → 编号不变）；`FindStream` 与
`SourceIdForIndex` 对称共用 stride；`SetActiveStream` 先校验再按源翻译；
`media_info()` 聚合偏移 + 标 `metadata["avbase:source"]`；`GetStreams` 仍只答
源 0（守卫就此成立）。RendererImpl 每源一对 video、audio 仅源 0、文本腿
`vector<TextLeg>` 按 source_id 选腿、`cue.source_id` 上行。L5「第二路 Player +
邮箱」绕行已退役；`LatestFrameQueue` 决定保留（GlLayerSource 唯一生产用户）。

## 第七十九轮：阶段 4 审查 + 清理（未推送）
对 `origin/main..HEAD` 跑 code-review + simplify-code 六路，修：
- **真 bug**：`DoSelectTrack` 先路由后拒绝、无回滚 → 次源 video/audio 选中后
  demuxer 楔死而回调报 kOk。修为路由前按 `SourceIdForIndex` 拒绝（M-C5 钉住）。
- **`media_info_` 一身二役**：多源 `media_info()` 覆写该成员冲掉 SetDuration
  时长高水位 → 高水位拆到 `reported_duration_`（M8 钉住）。
- **冗余收敛**：`DecodeIndex`（替三处开放编码）/ `text_leg()`（替三处边界
  检查）/ `MakeAnySource`（解重载陷阱）/ 双次 `media_info()` 合一。
- **测试 7 例**：kText 越界守卫（M9 删守卫→崩）/ flush 逐腿重武装（M10）/
  次源 video、audio 拒判 / 次源文本端到端（M-C6）。
- **开放项**（docs/14 §7）：stream-id 轴不对称；stride 走 GetStreams 而 merge
  走 media_info().streams 的信息源不对称。

## 路线图
L1–L5 全部完成；docs/14 阶段 0–4 全部落地；第七十九轮审查清理待提交。
