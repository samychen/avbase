# avbase 长期约定

Chromium 风格分层 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。轮次史见
`docs/PROGRESS.md`，GL 契约见 `docs/13`，门禁命令见 `operations.md`。本文件只留
跨轮仍有效的硬规则；长尾教训在 `operations.md`，别在此复述。

## 分层（docs/02 §6）
`platform/` 只装「被宿主选中」的代码；可移植代码不进（FFmpeg→`media/ffmpeg/`、
GL→`render/gl/`）。**C28：`render/` 不得 include `media`/`platform`/`base`/`player/`。**

## 四条「写下来会静默失效」的约定（都真踩过）
- **朝向**：从渲染目标采样回来的 pass 都要 `flip_vertical=true`。搞反 = 上下颠倒、
  无 GL 错误 → 有朝向的代码必须配像素断言。
- **stride**：`GL_UNPACK_ROW_LENGTH` 单位是 **texel 不是字节**；1080p 恰好相等把
  bug 藏住，320x240/854x480 必现（周期 8 行绿色横条）。
- **GL 常量**：一个常量别在两个头世界各写一遍 → 后端用 `target()` 说出来。
- **变换顺序**：顶点 shader 是 screen→source **反向映射**，观众最后看到的一步计算上
  最先做 → CROP 写在 ROTATE **之后**。别用算术顺序描述视觉顺序。

## C ABI（M15）代码前必读
- `avbase_c.h` 是 ABI 契约，`-DAVBASE_ENABLE_CAPI=ON` 默认 OFF；独立 target
  `avbase_cabi`。what 码（200/300/100/400/500-502/600/700/800/900/1000/10001）不能猜。
- 全树 `-fno-exceptions -fno-rtti`：**不要写 try/catch**（编译失败）；绑定回调用 lambda
  显式区分「捕获」与「参数」；`SetEventHandler` 要显式写 `avbase::Player::EventHandler`。
- `Status` 是 `base::expected<void, MediaError>`：**无 `.code()`**，先 `has_value()` 守卫
  再 `.error()`。`MediaInfo` 字段 `duration`/`bit_rate`/`streams[]`（`StreamKind`），**无**
  `title`/`video_size`/`rotation`（尺寸/旋转在 `StreamInfo`）。
- **C1 单文件 500 行上限，连注释都算**；头里定义函数必须 `inline`；C `.c` 已在 format
  门禁但 CI 只扫 `*.cc *.h`，别把范围扩成 `*.c`（`cmake/FFmpegVersionProbe.c` 会红）。
- 本机 clang-tidy 对 `media/ffmpeg` **静默零发现**（PCH 格式读不了）→ 必须无 PCH
  scratch 库。**本机干净 ≠ CI 干净。**
- 鉴别力：往 `avbase_c.h` 塞 C++ 默认参数 → `cc -std=c99` 报「C does not support
  default arguments」，`c++` 通过 = 旧腐化形状（C++ 绿，C 宿主断）。

## 诊断门禁（先跑这个，再怀疑产品 bug）
`python3 tools/check_binary_freshness.py --root build/<preset>`（exit 0/1/2）。
**为什么排第一**：第八十五轮拿 10/15 旧二进制诊断出「VideoToolbox 硬解卡死」，重链后
3/3 正常——**那个 bug 不存在**。判新鲜度读 `.ninja_log`（非 `stat`）；每个二进制只跟它
自己链接的库比（依赖取 `build.ninja` 显式段，不含 `||` 后 order-only）。ninja 不跟踪
静态库内容，但 `cmake --build` 对部分目标会报成功却留旧可执行文件——**测量前先确认被测
物的 mtime 晚于输入。

**`buildPresets.targets` 截断陷阱（第九十轮已修）**：`macos-sdl2`/`linux-sdl2`/
`linux-native` 的 buildPreset 曾带 `targets: ["play_sdl2"/"play_native"]`，导致
`cmake --build --preset macos-sdl2`（CI `macos-player` 用的就是这条）**只建 play_sdl2，
不建 player_unittests/headless** → 后续 `ctest` 因找不到可执行文件而红。沙箱看不到 CI，
这条红一直隐形。已移除三处 `targets` 限制（`CMakePresets.json`），现在 `--preset` 与
`cmake --build build/<dir>` 一致，构建全部目标。验证：删 player_unittests 后
`cmake --build --preset macos-sdl2` 会重建它；macos 门禁 786/786。

## 写单测前必读（第八十七/八十八轮）
- **测试绝不依赖工作目录**：用编译期宏 `AVBASE_TESTDATA_DIR`（`tests/CMakeLists.txt`
  逐 target 定义）。同二进制直接跑绿、ctest 红 = 先查 cwd。
- **`PlayerImpl::PrepareSync` 无默认参数**；单测显式 `PrepareSync(base::Seconds(30))`。
- `tests/unit/<dir>/*.cc` 是 glob 收集，新文件不用改 CMakeLists（但 invariants 文件数 +1）。
- **`Signal()` 持锁**：等待者必在 signal 后的动作之后才醒，别把二者当竞态；想造竞态加钩子。
- **注释声称「这个测试能抓住 X」之前，先改回去跑一次**：第八十八轮有注释写了「改回去会
  红」——改回去全绿。注释里的鉴别力声明也会假绿，且更难发现。

## 本 sandbox 网络约束（查 CI 前先读）
- **GitHub HTTP(S) 被代理掐断**（`Proxy CONNECT aborted`/502）→ `gh` 装不上、API 拉不到
  CI。**别再试 `gh`/API。** 唯一通的是 SSH git 协议（`git push`/`ls-remote` 正常）。
- **看不到 CI 的替代方案**：`tools/gate_local.sh` 用同一套命令在本地复刻
  CI 门禁序列（inv/fmt/lint/macos/capi/core/ffmpeg/e2e + 可选 tidy/asan/tsan/ubsan/
  coverage）。`--list` 看 job→CI 映射，本地绿 = CI 会绿。

## 路线图（现状）
L1–L5、docs/14 阶段 0–4、Phase 2 协议验收（HLS/HTTP + RTMP/RTSP/SRT，第八十二轮三协议
实测全绿）、M15 C ABI（已接门禁）均完成。第八十五轮推翻了自己报的「VideoToolbox 硬解
bug」（不存在），并新增 freshness 门禁。
