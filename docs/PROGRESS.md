# 实施进度（活文档）

> 设计文档：[README](../README.md) ｜ 里程碑定义：[08 实施路线图](08-实施路线图与风险.md)
> 剩余工作权威清单：[12-剩余工作清单](12-剩余工作清单.md) ｜ 产品化能力 / 死选项 / 环境项：[TODO](TODO.md)
> CI 不可见时的等价门禁信号：[tools/gate_local.sh](tools/gate_local.sh)

> ⚠️ 本文档只保留**当前状态**与**已完成/未做速览**。轮次级过程记录（第四十八轮至第八十四轮）已移除——
> 属历史审计轨，不进活文档；如需追溯见 `git log` 与旧版 `PROGRESS.md` 的 git 历史。

---

## 当前状态

**M0–M11 ✅ · Phase 0/1/2/4 ✅（多数）· Phase 3 解码侧 ✅、显示侧零拷贝 ⬜ · Phase 5 转码代码全落地（E5 硬编未实测）· macOS 窗口播放器 ✅ · CI 风格门禁全绿 · 管线原生多源 ✅ · M15 C ABI ✅ · M13 SDK 文档七份 + apidocs 门禁 ✅**

最后更新：**2026-10-18**

---

## 已完成里程碑速览

- **M0–M6**：工程基建 / `base` 核心件 / `base` 任务系统 / `media` 值类型 / FFmpeg 解复用 / 解码流与 FFmpeg 解码器 / 音视频同步与合成器 —— 全部 ✅
- **M7 播放渲染**：`VideoRendererImpl` / `AudioRendererImpl` / `RendererImpl` / `TextRenderer` / 音频管线 —— ✅
- **M8 管线与播放器**：`pipeline_impl` / `player_impl` / `state_machine` / `event_hub` / `seek` / `buffer_controller` / `diagnostics` —— ✅（SDK 头冻结，方法除转码外全部落地）
- **M9 缓冲与 seek**：三级 HWM / 精确 seek / `RetryDataSource` / `LiveDataSource` / `UrlRewriteInterceptor` —— ✅
- **M10 验收基座**：`platform/null` / `synthetic_demuxer` / golden 基线工具 —— ✅
- **M11 窗口播放器**：macOS 窗口播放器（SDL2）✅；Linux 原生播放器（`play_native` / `play_embed`）✅
- **M12 Linux 平台层**：`gl` / `window` / `audio` / `zero_copy` dmabuf / dlopen loader —— ✅（约 3800 行）
- **M15 C ABI**：`player/public/c/avbase_c.h` 真通用门面（60 导出函数 + 门禁）—— ✅
- **管线原生多源（docs/14）**：`CompositeDemuxer` 聚合 N 源 / 每源一对渲染腿 / 时钟多轴 —— ✅
- **CI 风格门禁**：`check_invariants` / `clang-format` / `cpplint` / `check_binary_freshness` 全绿；`macos-player` 已修 `buildPresets.targets` 截断陷阱（只编 `play_sdl2` 漏掉 `player_unittests`，CI 长期隐形红）。
- **选项面**：构建注册表内 **9 个 key 全部有下游读取者（死选项 0）**——4 个 buffer key（第九十一轮）与 `nodisp`/`enable-accurate-seek`（第九十二轮）都是先写读取者再注册回来的；仍未接线的只是 DRAFT `.inc` 里的 spec key，见 [TODO](TODO.md) §2。

---

## 仍在进行 / 未做

| 项 | 状态 | 说明 |
|---|---|---|
| Phase 3 显示侧零拷贝 | ⬜ | Linux(VAAPI-EGL)/Windows(D3D11-GL) 进 sink；`examples/pull_frames` 在对应硬件跑通；硬解性能对照 + 颜色空间兜底 |
| Phase 5 E5 硬件编码器实测 | ⬜ | 代码全落地（E1–E5 + E3b 异步 + E5b 平台表外置），仅 `VideoToolbox`/`VAAPI`/`NVENC` 硬编本体未实测 |
| Qt 门面 | ⬜ | `player/qt/` 未做；M15 C ABI 已作 UE/跨语言挂点 |
| soak 48h 长稳 | ⬜ | 脚本 `tools/run_soak.py` 已就绪，48h 未跑（沙箱禁读他进程 RSS） |
| Windows CI 验证 | ⬜ | job 已加（no-ffmpeg，不开 `/WX`），首跑待回答 |
| M13 发布 0.1.0 | 🔵 | SDK 文档七份齐（API/COOKBOOK/MIGRATION/TROUBLESHOOTING/EXTENDING/PERFORMANCE/CHANGELOG）+ apidocs 门禁 + V5–V8 ✅；剩 cpack/vcpkg/conan 实跑、V9（注释覆盖率脚本）/V10（abi-compliance-checker）、golden 对拍与微基准入 CI |
| M14–M18 | ⏳ | VAAPI+HDR / Android / iOS / `ijkio` 缓存（后置增量） |
| 产品化能力（Android/iOS/Web/Unity/UE/Flutter/…） | ❌ | 全部未做，见 [TODO](TODO.md) |
| 决策项 | — | LL-HLS 自研 vs 接受 ~3s、M12 原生后端是否启动、版权主体、golden 基线锁版本，见 [12-剩余工作清单](12-剩余工作清单.md) §6 |

> 完整开放项、降级项、死选项、环境项见 [12-剩余工作清单](12-剩余工作清单.md) 与 [TODO](TODO.md)；
> 已修关键缺陷（关闭竞态 / VOD 死锁 / 解码器丢包 / corpus 三崩溃 / EOF 后 seek 永不回调）记录在 [12-剩余工作清单](12-剩余工作清单.md) §3。
