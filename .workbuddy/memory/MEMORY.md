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

## 第八十轮：Phase 2 收口 + VOD 完成链死锁修复（未推送）
- **VOD 死锁真因**：`VideoRendererImpl::Render` 只在 present 后 re-arm 解码泵；
  合成器因 fps-cap **丢帧**(返回 null)排空时 re-arm 永不发生 → 解码泵饿死、解码流
  堆满 16 个 buffered outputs、EOS 卡在队尾、永不 kCompleted。**关键不变量**：
  re-arm 应发生在「合成器消费了一帧」(`frames_pending` 下降 = present 或 drop),
  **不是**只在 present；hold/repeat(`frames_pending` 不变)不 re-arm。
- **两处修复(缺一不可)**：`decoder_stream.cc` 去 `MaybeDeliver` 的 `!end_of_stream_`
  门禁；`video_renderer_impl.cc` 的 re-arm 从 `if(frame)` 改为
  `compositor_.frames_pending() < pending_before`。
- **被判不合格的变体**：「只要 `frames_pending() < max` 就 re-arm」会过度超前读取、
  把测试期望 present 的帧被 fps-cap 丢掉(**破 8 个单测**),已弃用。
- **验收工具**：`tools/check_live_protocols.py`(自管 MediaMTX 生命周期,RTMP/RTSP/SRT
  判 PASS/MISSING/FAIL + 测延迟 L；退出码 0/1/2 对齐 `check_protocols.py`)；配置
  `tools/mediamtx/mediamtx.yml`。历史会话三协议实时播放成功;本会话沙箱代理拦 GitHub
  下载未能重跑。**Phase 2 全部闭合**(docs/12 §2.4/§2.6 已收口)。

## 第八十二轮：三协议首次全绿 + 验收工具自身三处假 FAIL

- **MediaMTX 二进制本机已有**：`/private/tmp/mediamtx/mediamtx`(v1.21.2)。报「环境阻塞」
  前先 `pgrep -fl mediamtx` / `lsof -p <pid>` 找活体二进制，别先信「下不下来」。
- **实测(12s 预算)**：RTMP L≈3.97s · RTSP L≈2.99s · SRT L≈3.54s，全 PASS exit 0。
- **docs/12 §2.6 结论被数据推翻**：原写「倾向挂 SRT 绕开 LL-HLS」基于「SRT 亚秒级」的
  推测；实测三协议都 3s 上下、**无一条亚秒** → 已改为「三协议量级相当，SRT 无数量级优势」。
- **一次被实验证伪的诊断**：`ffmpeg_demuxer.cc` 无条件给所有网络协议加 `reconnect*`
  曾被我判为 RTMP 打不开的根因；ffprobe 对照实验证伪（未知键不让 open 失败）。**未验证的
  推断直接改产品代码 = 引入测不出来的新 bug**，这类推断必须先做最小对照实验。

## 第八十三轮：M15 C ABI 落地（「真通用门面」）

- **落地**：`player/public/c/avbase_c.h`(436 行 / 60 导出函数) + 三份实现
  （`_player.cc` 473 / `_display.cc` 179 / `_events.cc` 51）+ 共享 `avbase_c_internal.h`
  + 验收 `tools/check_capi.{c,sh}`。独立 target `avbase_cabi`，`-DAVBASE_ENABLE_CAPI=ON`
  （默认 OFF，沿用 docs/06 §971 既有接线约定）。
- **事件走 legacy 四元组** `(what,arg1,arg2,obj)`，直接复用既有 `ToLegacyEvent()`
  （`player/player_event.cc:142`）——docs/05 的「Java 侧零改动」靠它。**what 码
  （200/300/100/400/500-502/600/700/800/900/1000/10001）是 ABI 契约，不能猜**。
- **导出宏复用项目范式**：`avbase_c_export.h` 由 `AVBASE_C_IMPLEMENTATION` 驱动
  （同 `player_export.h`），**不写 `_WIN32`** —— C 头在任何 host 可编译，且避开
  invariant C5「platform macro in a core file」。

**本项目写 C/C++ 边界代码前必须知道的（都真踩）**
1. **全树 `-fno-exceptions`**（Chromium 风格）——C ABI **不需要** try/catch 防穿透，
   写了直接编译失败（"cannot use 'try' with exceptions disabled"）。
2. `Status` 是 `base::expected<void, MediaError>`：**没有 `.code()`**，要 `.error()`
   且成功时先 `has_value()` 守卫。
3. `base::BindOnce(f, arg)` **不能**直接构造 `OnceCallback<void(void*)>`——绑定后签名
   变成 `void(void*, arg)`。用 lambda 显式区分「捕获」与「参数」。
4. `Player::SetEventHandler` 要显式写 `avbase::Player::EventHandler(lambda)` 才转得过去。
5. `MediaInfo` 字段是 `duration`/`bit_rate`/`streams[]`（`StreamKind`，**不是**
   `DemuxerStreamType`），**没有 `title`/`video_size`/`rotation`**——视频尺寸与旋转在
   `StreamInfo` 上。照猜的写会连错 3 处。
6. **C1 单文件 500 行上限，连注释都算**。第八十轮给 `decoder_stream.cc` 加注释把它
   推到 501 行，本轮门禁才报出来。
7. **纯 C 消费者链接**：C 源码用 `cc` 编成 `.o`（这才是真 C 边界），但链接要用
   `c++` 驱动，否则缺 libc++（`std::__1::__shared_weak_count` 未定义）。
8. **`/bin/sh` 没有 `PIPESTATUS`** —— 验收脚本不能用管道取退出码。

**验证**：`tools/check_capi.sh` 17 项全过 exit 0（真实纯 C 程序播到 kCompleted、
事件到达、缓冲区契约正确、非法参数被拒、null 销毁不崩）；单测 393/393；
`check_invariants` 479 文件全过。

## 写 C ABI 相关代码前必须知道的（本项目代价换来的）

**第八十四轮把 C ABI 接了门禁**（`capi` preset + `capi_smoke_test` CTest 用例 +
CI `capi` job），并删掉了 `tools/check_capi.sh`。**先看这一节再动 C ABI 的代码。**

1. **`add_test()` 写在 `enable_testing()` 之前会被静默丢弃** —— configure 成功、
   构建成功、`ctest` 报 `No tests were found!!!` 且**退出码 0**。即「看起来装了门禁，
   一次没跑」。这是本项目最贵的一种失效形状：**它不以失败出现，只以「门禁不存在」出现。**
2. **纯 C 目标链 C++ 库时 CMake 自动选 C++ 链接驱动**（不链任何 C++ 库时才用 `cc`）——
   所以上一轮脚本里「链接必须手写 `c++` 驱动」的 workaround 已经不需要。**实测过，别再手写。**
3. **`avbase_configure_target()` 的整套 flag 对 C99 TU 安全**（`cxx_std_20`、
   `-fno-exceptions -fno-rtti`、`-Wpedantic`、`-Werror` 全部通过）—— 实测过，可直接复用。
   但要**另加 `C_EXTENSIONS OFF`**，否则 `-std=c99` 实际是 `gnu99`，GNU 扩展会让某些
   C++ 写法静默编过。
4. **在头文件里定义函数必须 `inline`** —— 三个 TU 各用一部分，不加就每人报一次
   `-Wunused-function`；且非 inline 定义被多个 TU 包含本来就是重复符号。
5. **每个新 target 都要 `add_library(avbase::xxx ALIAS ...)`** —— 全树约定，缺了报错信息
   指向「拼错了」而不是「少别名」。
6. **`-Wnewline-eof` 在 `-Werror` 下是硬错误** —— 文件末尾必须有换行。
7. **`.c` 文件不在 format 门禁范围**（CI 只扫 `*.cc *.h`）。已把
   `player/public/c/*.c` 精准纳入。**不要改成 `*.c`** —— 全树另一个 `.c` 是
   `cmake/FFmpegVersionProbe.c`（try_run 探针，纳入即首跑红 = R12 门禁失信）。
8. **本机 clang-format 在 `/Library/Developer/CommandLineTools/usr/bin`**（不在默认 PATH）。
   **但别把它加进 PATH 再跑 `cc`** —— 那个 `cc` 不带 macOS SDK，会报
   `'stdio.h' file not found`，看起来像代码坏了其实是环境坏了。

**鉴别力验证怎么做的**（门禁「红得起来」才算数）：
往 `avbase_c.h` 塞一个 C++ 默认参数 → `cc -std=c99` 报
`error: C does not support default arguments`，`c++ -std=c++20` **通过**。
这就是「C ABI 腐化」在旧形状下的完整样子——**C++ 侧全绿，只有 C 宿主会断。**


## 解析本项目输出前必须知道的三件事（都真踩过）

1. **`headless` 的位置更新用 `\r` 覆盖、无换行** —— 一条物理行可含十余个 `position:`
   片段。逐行 `find` 第一个片段 = 把「推进到 11s」读成 `0.000s`。**这条不写在任何接口
   文档里，只能从真实日志看见**；任何解析 headless 输出的代码都要在一行内循环扫。
   （用它判过 RTMP/SRT 假 FAIL，差点去改播放器。）
2. **MediaMTX 的 SRT streamid 分隔符是冒号**（`publish:avbase` / `read:avbase`），写成
   斜杠被拒握手 → 症状与播放器失败一模一样。**同一症状、两个不同原因**是诊断成本最高的
   一类，遇到先并列假设再取证。
3. **peer 是否就绪要用 ffprobe 轮询，不能固定 `sleep`** —— RTMP 要先完成 publish 握手；
   轮询还能把「peer 没就绪」与「播放器失败」拆成 `SERVER_MISSING` / `FAIL` 两种结论。

## 诊断任何「产品有 bug」之前：先跑新鲜度门禁（第八十五轮最贵的一课）

```sh
python3 tools/check_binary_freshness.py --root build/<preset>   # exit 0/1/2
```

**为什么这条排在所有诊断之前**：第八十五轮我拿 `build/macos-sdl2/bin/headless`
（10/15 的旧二进制，库是 10/17）诊断出「**VideoToolbox 硬解 EOS 卡死**」，
逐层排查 Deps / SDL2 / sink / EOS、还写了临时探针程序对比日志 —— 重链后
**3/3 正常播完，那个 bug 从来不存在**。同一轮我报的「ctest 736/736 全绿」
也是旧数据，**真数据 784/784，差的 48 个用例就是铁证**。

- **触发条件比想象中窄**：`touch <某个库源文件> && ninja -C <dir>` 会正确
  重链全部下游。落坑的是「库重建了但当轮没走链接边」。
- **ninja 不跟踪静态库内容**，此时 `cmake --build` 输出 `no work to do`
  **并报成功**。已接进 CI 的 `full` / `macos-player`，**位置在 ctest 之前**。
- **判新鲜度必须读 `.ninja_log`，不能读 `stat`**：静态库内容没变时 ninja
  restat 会把 mtime 回退；且每个二进制只能跟**它自己链接的库**比
  （依赖取自 `build.ninja` 显式输入段，**不含 `||` 后的 order-only 段**）。
  我按 stat 判过一次，误报成「10 个二进制过期」。

## 三条工具/构建纪律（第八十五轮）

- **`--preset X` 不会把已存在的 `build/X` 目录「洗成」X 的定义** —— CMake 只更新
  preset 显式声明的变量，其它缓存值原样留存。实测 `build/ffmpeg` 里
  `AVBASE_ENABLE_CAPI=ON`（ffmpeg preset 根本没声明它，是 `capi` preset 的选项），
  症状是 `bin/` 下多出一个 `avbase_capi_smoke`。**验证 preset 是否纯净必须用全新
  `-B` 目录对照**，看老目录不算数。
- **取 CMakeCache 字段用 `sed -n 's/^KEY:BOOL=//p'`，不要 `grep -oP`** —— macOS 的
  BSD grep 无 `-P`，**静默返回空串**，表格里整列消失像「缓存里没这个变量」。
  （同族已有两条：BRE 的 `\|`、toybox shim 的 `\b`。）
- **恢复变异文件后必须 `touch`**，否则 ninja 会跑上一个突变的二进制
  （`cp` 会连 mtime 一起还原）。第八十五轮做 pull_frames 的鉴别力验证时
  实打实踩到。

## 路线图
L1–L5 全部完成；docs/14 阶段 0–4 全部落地；Phase 2 协议覆盖验收（HLS/HTTP + RTMP/RTSP/
SRT）已闭合并有三协议实测数据（第八十二轮）；M15 C ABI 已落地**并接入门禁**（第八十四轮）。
第八十五轮：修好 `pull_frames` 的 Start 非法转换；新增 `check_binary_freshness.py`
门禁并接进 CI；**推翻了自己上一轮报出的「VideoToolbox 硬解 bug」（它不存在）**。
