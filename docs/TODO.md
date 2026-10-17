# TODO · 未完成工作

> 与 [12-剩余工作清单](12-剩余工作清单.md)（剩余工作权威清单）和
> [PROGRESS.md](PROGRESS.md)（当前状态速览）互补。
> 本文件只收**未完成**工作；已完成项已移出，详见 12 / PROGRESS。

---

## 0. 现状

渲染 / 管线 / SDK 门面三层均已闭合（第六十六～七十九轮）。`AddVideoStream(int)` 等
6 个「第 id 路」接口经证实是死接口——多源轴是 `GetStream(source_id, type)`，不是
「同一资源多同类型流」——已点名拒绝，不再占用篇幅。

---

## 1. 产品化能力（全部未做）

| 能力 | 现状 |
|---|---|
| 录制（播放侧落盘） | ❌ 无。仅 `media/transcode/` 离线转码（输入是文件） |
| 推流 / RTMP publish | ❌ 无。`rtmp` 仅作**输入**协议 |
| 缩略图 / seek preview | ❌ 无。`get_frame_mode` 是死选项（config 有、registry 有，media 零命中） |
| HDR / tone mapping | ❌ 无。`hdr_tone_mapping` 在 media/render 零命中；`kHdrUnsupported` 从未 emit |
| PBO 上传 | ❌ 未实现（`render_perf_stats.h:28` 标 "not built"） |
| 真色彩管理 | ❌ 无。仅 YUV→RGB 矩阵，无 gamut/EOTF/ICC/HDR→SDR |

---

## 2. 已配置但未生效的选项（14 个死选项，需清理或接通）

`docs/05-迁移对照表.md` 曾标 22 个 ✅，逐字段追「谁读了它」后 16 个不成立（配置有
字段、registry 有条目、`SetOption` 不报错、`GetOption` 能原样读回，但无一处变成行为）。
`max-fps` / `framedrop` 已于第七十一轮接通，剩 **14 个无读取者**：

`fast` · `rdftspeed` · `min-frames` · `packet-buffering` · `sync-av-start` · `infbuf` ·
`video-pictq-size`（仅被区间校验读到）· `no-time-adjust` · `preset-5-1-center-mix-level` ·
`get-frame-mode` · `video-mime-type` · `ijkmeta-delay-init` · `nodisp` · `render-wait-start`

> 处置：要么接通（写读取者），要么从 config/registry 移除并更正迁移表。属「看起来支持、
> 实际无效」的高危类别——不会报错，但误导配置者。

---

## 3. 环境相关（非代码，需特定机器/权限）

| 项 | 阻塞原因 |
|---|---|
| RTMP / RTSP / SRT 播放验收 | 需真实 peer（本机 MediaMTX 已可跑，见 operations.md） |
| 真 `tc netem` 弱网 | 需 root；macOS 无 |
| Linux 显示侧零拷贝（VAAPI→EGL） | 需 Linux + VAAPI 设备 |
| Windows 显示侧零拷贝（D3D11-GL） | 需 Windows + D3D11 环境 |
| 硬解性能对照（4K CPU < 15%） | 需 4K 样本入库 |
| Qt 门面 / soak 48h | Phase 4 剩余（见 12 §2） |

---

## 4. 记录更正（evergreen 教训）

- **门禁「绿」可能是「没跑」**：check-clang-tidy 三处失效（header-filter 负向前瞻不支持 /
  platform 可选后端不在编译库 / HeaderFilterRegex 未锚定 vendored glad）；exit=127 让门禁
  看起来绿；2 秒跑完的全树检查一定没跑；CMake **多输出边**会让 ctest 二进制被静默丢弃
  （`tools/check_binary_freshness.py` 已修）；`add_test()` 写在 `enable_testing()` 前被
  静默丢弃。数字必须当场跑出来。
- **测试写了 ≠ 性质被钉住**：反证必须真跑一遍看有没有红的。
- **抑制（NOLINT）会静默失效**，比不写更危险：诊断行与 `NOLINTNEXTLINE` 差一行、散文注释里
  出现 NOLINT 被当真指令。
- **门禁「干净 ≠ CI 干净」**：本机 clang-tidy 读不了 Xcode PCH 会静默零发现；strict 在发布前
  编不过会一直红而无人见（sandbox 看不到 CI）；预设 `targets:` 收窄会让 `--preset` 只编部分
  目标，CI 的 ctest 步找不到可执行文件（CMakePresets.json 已修）。
