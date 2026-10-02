# 实施进度（活文档，每个里程碑结束时更新）

> 设计文档：[README](../README.md) ｜ 里程碑定义：[08 实施路线图](08-实施路线图与风险.md)

## 当前状态：**M0–M11 ✅（播放链路 + SDL2 出画）· M9 收口中（三级 HWM/精确 seek/RetryDataSource/饥饿信号 ✅）· Phase 0 更名 avbase ✅ · Phase 3 硬解零拷贝 ✅（VideoToolbox 实测）· 音/字幕轨切换 ✅ · fuzz 目标 ✅**

最后更新：2026-10-03（第十八轮）—— **Phase 4.5 开篇：libFuzzer 目标 + 确定性 standalone 驱动。**
`tests/fuzz/fuzz_demuxer.cc` 把每份输入推过真实的 demux 前门（MemoryDataSource →
AVIOContext 桥 → avformat 探测 → 有界 Read → Stop），全部等待限时、字节拷贝与
demuxer 同批在 media 线程上销毁（harness 特意泄漏，避开退出期静态析构顺序雷）。
驱动双轨：libFuzzer（CI Linux，覆盖引导）+ 固定种子 standalone（本机 asan 可跑，
325 输入全干净）。CI 新增 fuzz-smoke job。顺带修复 `expected.h` 的
`#if defined()` 宏序雷（BuildConfig 恒定义 0 的名字会劫持 include 顺序敏感的 TU）。
ffmpeg 439/439、no-ffmpeg 376/376、asan 439/439 连续两轮全绿，invariant 全过
（282 文件）。
> 第十七轮：Phase 4.2 字幕文本腿——kText 轨选择与 TimedText 事件

## 第二十一轮（本轮）：直播追帧端到端验收（docs/12 §2.1 闭合）

`PipelineSeekTest.LiveSourceChasesToTheEdge`：合成源标记 live（duration 10s 作为
边缘替身）、延迟目标 2s、播放头起步落后 10s——**首个统计节拍触发追帧**（日志：
behind=7942ms, skipping to 9s），播放头落到边缘附近（断言 ≥8.5s），随后帧持续
呈现（无死锁、无错误）。此为 docs/12 §2.1 的端到端验收路径；真"增长边缘"demuxer
（时戳随墙钟推进）留作增强，当前用有限边缘替身先钉住触发→skip→续播全链。
过程修正：共享夹具（dcc876c）的 PumpRound/PlayAndWaitForSinks 已无 bus 参数。

**§2.1 状态：✅ 闭合**（判定核 + 接线 + 端到端验收；协议覆盖验收见 §2.4，另列）。

## 第二十轮（本轮）：直播追帧判定核（docs/12 §2.1 判定侧）

`media/filters/live_edge_policy.{h,cc}`：追帧决策纯函数（DecideNextFrame 模式）——
`ShouldChase(is_live, latency_hint, behind)`：仅直播 + 正向延迟目标 + 严格大于
（规范卷 §3"落后 >8s"）才追；`SuggestedTarget` 落点在边缘减去半个目标（留出
整个目标的余量，防止统计节拍上乒乓）。接线：`PipelineImpl::SetLatencyHint` 现在记录
hint（此前只转发渲染器）；统计节拍（1s，host TU）上检测 直播边缘(duration)−播放头
超过目标即自发起两阶段 skip（renderer flush + demuxer 从最新处续读——**非物理 seek**，
直播容器不能回退），复用既有 seek_in_flight_ 防重入与完成汇合。4 个纯函数单测。

**验收边界**：判定核 + 接线已测；docs/12 §2.1 的端到端验收（合成直播源落后 8s
一个阈值周期内追平、无死锁）需要合成直播 demuxer 假件（定时产流、时戳随墙钟
增长），是下一轮主体。

## 第十九轮（本轮）：LiveDataSource——直播字节源基建（docs/12 §2.2 前半）

`media/filters/live_data_source.{h,cc}`：可增长的直播字节源——生产者 `Append()`
（网络回调/测试 rig），消费者经 DataSource 桥顺序读，**在直播边缘阻塞等待**；
`Close()` 把边缘变成 EOF；`Abort()` 以 10ms 分片及时解锁（Δ15）。三个语义决定：
**部分读立即返回**（直播消费者不能等可能永远不来的整段——只读请求超出已有字节且
未 Close 时若整读阻塞会饿死）；非 seekable + streaming（mpegts 组合，可自动化）；
异步 Read 走私有 worker（契约：回调绝不内联）。6 个单测（模式校验/边缘阻塞直至
生产者追加/部分读不等待/Close 变 EOF/Abort 界定等待/异步投递），无 FFmpeg 依赖走
no-ffmpeg 门禁。

过程坑（更名轮的连锁）：导出宏已是 `AVBASE_MEDIA_EXPORT`、命名空间已是
`avbase::media`、filters 列表是手写的（base/legacy/renderers 才 GLOB）、
ConditionVariable 需显式以锁地址构造——四处全部撞了一遍。

## 第十八轮（本轮）：Phase 4.5 开篇——libFuzzer 目标 · 确定性驱动 · CI smoke

依据 avbase 升级计划 Phase 4.5（"对 platform/ffmpeg 输入做结构模糊，语料入库常跑"）。

### 落地面

| 项 | 说明 |
|---|---|
| `tests/fuzz/fuzz_demuxer.cc` | 共享 harness（无 main）：每份输入 = MemoryDataSource（**堆拷贝**——MemoryDataSource 不复制字节，libFuzzer 输入缓冲返回即失效）→ FFmpegDemuxer::Initialize → 对存在的音/视频流各做 ≤8 轮有界 Read → Stop → 与字节拷贝同批在 media 线程 FIFO 销毁。所有等待限时（init 10s / read 5s / drain 5s），挂死的输入被跳过而不是楔死 fuzzer |
| 双驱动 | `avbase_fuzz_demuxer`（libFuzzer，覆盖引导；运行时按 `check_cxx_source_compiles` 探测，没有则 EXCLUDE_FROM_ALL——AppleClang 不带 libFuzzer runtime）+ `avbase_fuzz_demuxer_standalone`（固定种子 mt19937 四类确定性变异：位翻转/截断/填字节/拼接，65 输入/种子，本机任何 sanitizer 配置可跑，崩溃可从命令行复现） |
| 语料 | `tests/fuzz/seeds/` 入库：3 个完整容器 + 截断头 + 损坏前缀；每次 run 先原样回放全部种子（回归即阻断） |
| CI | `fuzz-smoke` job（ubuntu-24.04，clang + fuzzer+asan）：60 秒覆盖引导 run + 种子回放；macOS 排除（无 runtime） |
| CMake | 根 CMake 的 tests 目录门控放宽为 `AVBASE_BUILD_TESTS OR AVBASE_BUILD_FUZZ`（fuzz-only 构建是合法配置）；测试套件段按 BUILD_TESTS 收进守卫 |

### 过程中抓到并修掉的两颗雷

1. **`base/types/expected.h` 的宏序雷**：命名空间分支用 `#if defined(AVBASE_HAVE_STD_EXPECTED)`，而 BuildConfig.h 的同名 `#cmakedefine01` **恒被定义（无 <expected> 的工具链上恒为 0）**——先含 BuildConfig 再含 expected.h 的 TU 会被推进 std::expected 分支后编译失败。改为带值判断的自包含 `AVBASE_USE_STD_EXPECTED`（include 顺序不再敏感）。
2. **退出期静态析构顺序**：harness 静态 Thread 在 logging 静态互斥量之后销毁，Stop() 里的 LOG 拿已销毁的锁（abort）。fuzzer 基建改为刻意泄漏（`new` 不 delete）——进程级资源的标准处置。

### 验证

- standalone 驱动：默认构建 + asan 配置各一轮，**325 输入全部干净退出**。
- libFuzzer 二进制本机不可链接（无 runtime），由 CI Linux 验证；fuzz-smoke job 已入 workflow。
- 回归：ffmpeg 439/439、no-ffmpeg 376/376、asan 439/439 ×2；invariant 全过（282 文件）。

## 第十六轮（本轮）：Phase 4.2 运行时音轨切换——SelectAudioTrack 全链

### 落地面

| 层 | 改动 |
|---|---|
| MediaResource | 新 `GetStreams(type)` 虚函数（默认单流实现向后兼容），FFmpegDemuxer 覆写为容器序全枚举。流身份 = `DemuxerStream::stream_index()`（容器流号，与 MediaInfo::streams 一致） |
| Demuxer 路由 | 新 `SetActiveStream(type, index)`：**只对被消费的流施加背压**，其余同型流的包直接丢弃。这是切轨的前置——否则旧流的孤儿队列把自己灌到水位线上，demux 循环自锁，新流饿死 |
| RendererImpl | `OnTracksChanged(kAudio, stream, cb)` 从桩变真实现：S1 编排——旧音频渲染器 S4 上停止+排干（`StopAndDrainForTeardown`：经 DecoderStream::Flush 以 kDecodingAborted 完成未决读）→ 重建（新 AudioParameters 取自新流配置，音量/静音/倍速/变速不变调设置从旧实例的原子量捕获重放）→ StartPlayingFrom(当前媒体时间) 续接时钟。kText/kVideo 维持可操作的 kNotImplemented |
| Pipeline | facade 新 `SelectAudioTrack(index, cb)`（默认实现如实回报 kTrackSwitchError）；PipelineImpl 编排流查找 + `SetActiveStream` 先行 + 渲染器交接 |
| 状态码 | `PipelineStatus::kTrackSwitchError` + 全映射（"切轨失败不是播放失败"），ToString/ToMediaError 同步 |
| Player | `SelectTrack` 接线：状态门（kPrepared/kStarted/kPaused）→ MediaInfo 索引校验 → 异步交接；结果以 `kTrackChanged{type,old,new}` 事件上行（实现独立成 `player/track_selection.cc`，两个既有文件都在 C1 天花板上） |

### 排掉的三颗雷（每颗都是测试抓的，不是读代码读出来的）

1. **demux 循环孤儿队列自锁**（切轨必现）：背压等待的三个唤醒者（Flush/Stop/seek）不含
   "消费者消失"。修复即上面的活跃流路由；`PlaysAudioOnlyTwoTrackFileToEosWithoutSwitch`
   是它的无切轨回归锚。
2. **S1 阻塞等待 S4 死锁**（首稿）：切轨的拆除在 S4，而 S4 的未决读回复经 media runner
   投回 S1——S1 等待即死锁。改为全异步交接，S1 零等待。
3. **退役渲染器 UAF**（ASan 抓的）：`delete on quiescence` 之后，更早排队、以
   Unretained 绑定旧对象的控制任务（StartPlayingFrom/SetVolume…）才执行。改为退役表：
   旧对象停止后转入 graveyard（所有入口在 stopping_ 下早退，成为惰性对象），
   ~RendererImpl 在 S4 统一销毁。AudioRendererImpl 析构的生命周期纪律从此与切轨解耦。

### 连带修复的夹具雷（throttle 夹具同样携带，靠 DISABLED 躲着）

- `MemoryDataSource` 不拷贝字节，throttle/track 夹具把局部 vector 传进去即悬垂——
  ASan 在切轨测试上炸出来。字节改挂 fixture 成员。
- 夹具 `PumpRound` 在 pipeline 停止后继续拉取已随 renderer 销毁的 sink——加
  IsRunning 门 + `set_render_runner` 把拉取序列化到渲染线程（seek 夹具早有此步）。
- `FakeAudioSink::Stop` 现在清回调指针，兑现"Stop 后不再 Render"的真 sink 契约。

### 验证

- 新 `two_audio_tracks.m4a`（双 AAC 轨，lavfi 合成）+ 5 个管线级测试 × 重复 4 遍全过：
  枚举、无切轨播放到 EOS、中途切换后到 EOS、往复切换、非法索引失败但管线健康。
- ffmpeg 435/435（+5）；no-ffmpeg 376/376；**asan 435/435（0 泄漏）**；
  invariant 全过（278 文件）；headless 回归正常。
- 未做：字幕轨(kText，基座只出文本+时间的文本腿)、视频轨切换、
  `kHardwareOnly` 生产语义、直播追帧（需直播源基建）。

## 第十七轮（本轮）：Phase 4.2 字幕文本腿——kText 轨选择与 TimedText 事件

### 落地面

| 层 | 改动 |
|---|---|
| media/base | `TimedTextCue{text,ass,pts,duration}`（基座只出文本+时间，渲染归宿主）；`TextDecoderConfig{codec_name,language,extra_data}`；`TextDecoder`/`TextDecoderFactory`——**刻意同步**：字幕包极小、解码器直通，S1 上无节流问题，异步契约是无消费者的仪式 |
| DemuxerStream | `text_decoder_config()` 虚函数（基类默认空配置，单字幕-free 实现零改动）；FFmpegDemuxerStream 从 codecpar 填充（含语言元数据） |
| FFmpegTextDecoder | avcodec_decode_subtitle2；ASS 括号标记剥离出纯文本，原始 markup 进 `cue.ass` 供会排版的宿主；pts 取 **buffer 时间戳**（demuxer 已按容器时基盖戳——MKV 的 packet time_base 不可信，实测差 1000 倍）；时长回退到容器 BlockDuration |
| Demuxer 路由 | 文本流**常驻**有界丢弃最旧队列（`DecoderBufferQueue::TryPushDropOldest`），不参与水位背压：字幕包一次性到达，按 A/V 路由要么楔死循环要么让迟到订阅全失 |
| RendererImpl | S1 文本泵（Read→解码→`client_->OnTimedText`）；世代号使跨切换/flush 的在途读回复失效；kText 选择分支替换"谎报桩" |
| 事件链 | RendererClient/Pipeline::Client 新 `OnTimedText`（默认 no-op）；PlayerImpl 转成 `kTimedText` payload；`SelectTrack(kText)` 接线（`player/track_selection.cc` 与 `media/filters/pipeline_track_select.cc` 各自独立成文件，宿主都在 C1 天花板） |

### 生命周期三连雷（teardown 的历史债，本轮一并封口）

1. **Stop 异步化 vs IsRunning 语义**：FinishStop 在 Flush 完成后才落 kStopped，
   `IsRunning()` 必须包含 kStopping，否则测试在 kStopping 就销毁管线（DCHECK 抓的）。
2. **管线停止需先 Flush 渲染器**：裸 reset 留下 demuxer→S1→S4 回复链在途，落在已释放
   的 AudioRendererImpl 上锁已释放互斥量。DoStop 改为先 Flush（内联完成全部在途读）
   再销毁；~RendererImpl 的音频销毁合并为"排干+销毁"单任务。
3. **泵自续任务弱绑定**：AudioRendererImpl 的 pump_again 以 Unretained 入队，可与销毁
   任意交错（DecoderStream 内部早已全弱绑定，唯独渲染器自己的泵漏了）。改 WeakPtr 后
   该类任务在对象销毁后自动变空操作——这是对这一族竞态的**类级封口**，不依赖调用方顺序。

（另：`FakePipelineClient::OnTimedText` 持锁调 Record 自死锁，样例采样器抓的；已修。）

### 验证

- 新 `audio_two_subs.mkv`（1 音轨 + eng/chi 双 srt）+ 4 个管线级测试 × 重复 4 遍全过：
  未选轨零 cue 且音频正常 EOS、选轨后 cue 文本/顺序/pts/时长逐项断言、往复切换、
  非法索引干净失败。
- ffmpeg 439/439（+4）；no-ffmpeg 376/376；asan 439/439 连续 4 轮全绿；
  invariant 全过（280 文件）；headless 回归正常。
- 未做：WebVTT/ASS 样本覆盖（解码路径同型）、字幕样式信息透传（raw_ass 已带原文）、
  直播字幕的过期 cue 丢弃策略。

## 第十五轮（本轮）：Phase 3 硬解与零拷贝——NativeBuffer · VideoToolbox 全链 · 颜色空间

依据 avbase 升级计划 Phase 3（1–2 人 × 4 周的量，本轮落地的是其全部结构 + macOS 一条
实测链；Linux/Windows 的实测验收依赖对应硬件，留给有设备的环境）。

### 落地内容

| 计划项 | 实现 | 验证 |
|---|---|---|
| VideoFrame NativeBuffer 分支 | `NativeHandleKind{kVaapiSurface,kD3D11Texture,kCVPixelBuffer}` + `NativeHandle{kind,id,subresource}`（§6.2：消费者永不盲转）；`WrapNativeBuffer()` 带 `release_cb`（最后引用释放生产端后备存储）与 `to_i420_cb`；`ToI420()` 显式回读——owned I420 返回自身新引用，否则走生产端回读，无回读路径返回 nullptr | 6 个新单测：句柄类型、释放恰好一次、克隆引用共享句柄、无回读返回 null、回读可重复且不动原帧、owned I420 返回同帧 |
| FFmpeg 硬解路径 | `platform/ffmpeg/ffmpeg_hw_video_decoder.{h,cc}`：`av_hwdevice_ctx_create` + `avcodec_get_hw_config` + get_format 回调（拒答软格式——静默降级是谎报硬解）；hw 帧经 `WrapNativeBuffer` 直出，帧句柄按设备取自 `data[3]`（vt: CVPixelBufferRef / vaapi: VASurfaceID / d3d11: texture+subresource）；中途掉出 hw 路径即报错交给回退链，绝不冒充软解 | 本机实测：`pull_frames` 8/8 帧 `handle=cvpixelbuffer`；headless 全部测试媒体 kCompleted（硬解优先链下） |
| 显式回读 | `platform/ffmpeg/hw_frame_readback.cc`：`av_hwframe_transfer_data` + sws→I420，只在 `ToI420()` 背后被调 | 单测（回调机制）+ SDL2 sink 改造后可在真实播放中走通 |
| 平台工厂 | `platform/{videotoolbox,vaapi,d3d11}/` 各一个 factory target：capability `hardware=true, outputs_opaque_surface=true, priority=10`；codec 级掩码门（`CodecAllowedByMask`），被排除的 codec 是"not me"而非错误 | CMake 选项 `AVBASE_ENABLE_VIDEOTOOLBOX/VAAPI/D3D11`（第一个 Apple 默认 ON）；工厂返回 nullptr → DecoderStream 现成的 "declined" 路径 |
| 选择器接线 | `player_impl` 默认工厂列表：硬解工厂（按偏好）插在软解 FFmpeg 之前，宿主注入仍在最前；掩码过滤在工厂内（DecoderStream 对 null 解码器有现成的 fallback）；`hw_codecs` 默认值从 0 改为 kAll（0 意味着禁用全部硬解，与 kAuto 的"硬解优先"矛盾） | 既有 decoder_stream 回退单测 + 端到端 |
| 颜色空间兜底 | 新 `media/base/video_color_space.{h,cc}`：四轴类型 + `GuessColorSpaceFallback` 数据表（HDR→BT2020/PQ；≥600 行→BT.709；SD→SMPTE170M；每行整组指定）；`platform/ffmpeg/color_space_bridge` 从 AVFrame 映射，无标签时按**容器**信息兜底（风险 §14.4），软硬解路径统一接入 | 8 个新单测含边界行 599/600 与"永不半指定"不变量 |
| examples/pull_frames | 自定义 VideoRendererSink 捕获合成层输出帧并逐帧打印句柄类型——零拷贝取帧的活文档 | 本机实测 8 帧 GPU 直出 |

### 关键设计决定

1. **get_format 拒答软格式**：hw 解码器若在 get_format 接受软格式，FFmpeg 会静默降级软解
   而管线以为自己在硬解——谎报比失败更糟；失败交回退链，事件流里能看到。
2. **VAAPI/D3D11 本轮编译骨架 + 实测顺延**：解码器与工厂代码完整，但零拷贝**显示**链
   （VASurface→EGLImage、D3D11-GL interop）是 sink 侧工作，且验收要求对应硬件；解码侧
   的 GPU 传递对三个平台是同一套代码。
3. **SDL2 sink 显式回读**：SDL2 是 CPU blit，遇到不透明帧走 `ToI420()` 并告警一次——
   硬解播放因此保持正确（每帧一次回读），零拷贝显示路径出现后此处替换。
4. **`pull_frames` 不调 `ToI420()`**：示例的职责是证明零拷贝路径存在，回读反而模糊焦点。

### 与计划的偏差（登记）

- `platform/ffmpeg` 是唯一见 libav 的 target（C8），所以 FFmpeg-hwaccel 介质的硬解解码器
  必须住在那里；三个平台目录各持一个薄工厂 target 链接它。计划设想的
  "platform/vaapi/ 等目录装硬解"在原生（非 FFmpeg）解码出现前受 C8 约束。
- `DecoderPreference::kHardwareOnly` 的"宁败不回退"语义仍只在 Selector 层实现；
  生产接线按 kAuto/kHardwareFirst 排序，DecoderStream 的配置时重排序是 M9 后续项。

### 验证

- ffmpeg preset：430/430（新增 14 个）；no-ffmpeg：376/376（media/base 不依赖 FFmpeg）。
- `check_invariants` 全规则通过（276 文件），C23 基线 286 行未增长（新文件全部 ≤80 列）。
- asan 配置构建+测试（后台）。
- 本机 VideoToolbox 实测：`pull_frames` 输出 `handle=cvpixelbuffer`（零拷贝 GPU 帧到达
  合成层边界），headless e2e 全部通过（corrupt_header 按设计失败于 DecodeFailed）。
- 未跑：4K 硬解 CPU 占用对照、Linux VAAPI-EGL 与 Windows D3D11-GL interop 零拷贝显示链
  （无设备，Phase 3 验收的环境相关部分）。

## 第十四轮（本轮）：Phase 0 仓库改造——更名 avbase · LGPL 清点 · 文档合并

依据 avbase 升级计划（Phase 0，纯工程与法务，要求更名做成独立 commit）。

### 已完成

| 计划项 | 结果 |
|---|---|
| 更名 ijkpp → avbase | ✅ 独立 commit（`4c655fc`）。305 个文件、三种大小写变体（`ijkpp`×1805 / `Ijkpp`×42 / `IJKPP`×1239）。四个 cmake 模块 git mv。**"ijkplayer" 不含 "ijkpp" 子串，267 处上游指称原样保留**——溯源注释与 docs/05 的对照关系不受影响。git 历史全保留（无 squash）。宿主侧的仓库目录改名（GitHub repo rename）需要人工操作，内容层面已全部就位 |
| LICENSE | ✅ 第九轮已落 BSD-3 全文 + 第三方/衍生说明节，本轮仅随更名改抬头。README 声明一致 |
| CI 矩阵 | ✅ 第十轮已加 macOS 档（`macos-14`：no-ffmpeg + ffmpeg 两个 preset），计划项提前完成，无需改动 |
| 文档合并 | ✅ avbase_design.md §5/§7/§8 并入 [11-行为规范卷](11-行为规范卷.md)（线程表 / 背压级联与 seek 序列 / 三级水位表，标注为 Phase 1–2 的验收依据）；docs/05 顶部标注为历史卷（golden 对拍与常量溯源仍引用）；README 标题与定位改为"企业播放器基座"，补项目沿革段 |
| git 历史 | ✅ 更名 commit 与其余改动分离，历史未重写 |

### 更名的两个连带修复（都是"改名暴露既有事实"，不是新 bug）

1. **`ThreadTest.LongNameIsTruncatedNotRejected` 自相矛盾**：原期望截断产物
   `ijkpp-this-name`（15 字符，恰好等于 `kMaxThreadNameLen`）；前缀换成 6 字符的
   `avbase-` 后同样输入截断为 `avbase-this-nam`，与 `EXPECT_LE(15u)` 冲突。
   期望值改为新前缀下的 15 字符产物。
2. **C23 列宽棘轮**：`avbase` 比 `ijkpp` 宽一列，把 45 行顶过 80 列（检查器按字符计，
   中文注释不误伤）。全部手工收窄——按"动过的文件回归干净"的棘轮精神，而不是加基线；
   基线 310→286（-24）。

### LGPL 标记清点（计划项 3，"更名窗口内做完"）

| 文件 | 自述 | LICENSE 声明 | 结论 |
|---|---|---|---|
| `media/filters/legacy/*`（6 个） | 逐行移植 ff_ffplay.c，LGPL-2.1 头 + 隔离目录 + 阈值不可改 | LICENSE §1 逐一列名 | ✅ 一致，合规隔离成立 |
| `media/filters/ffmpeg_demuxer.cc` | "demux 循环的**形状**追随 read_thread()；seek flags 同" | LICENSE §3：全部 ffmpeg_* 为原创 BSD | ✅ 低风险：高层设计思路不受版权保护，seek flags 是 FFmpeg API 常量，无表达式复制 |
| `media/filters/video_renderer_impl.h` | 仅指路注释（"节拍算法住在 legacy/"） | 同上 | ✅ 无移植事实 |
| **`media/filters/ffmpeg_audio_decoder.{h,cc}`** | 头注释自称 **"Ported from ff_ffplay.c audio_decode_frame() (LGPL-2.1-or-later)"**，"restructured behind Chromium's AudioDecoder interface" | LICENSE §3 声明所有 `ffmpeg_*` 为**原创 BSD** | 🔴 **矛盾，待决策** |

🔴 项的两个事实方向冲突：代码结构（avcodec send/receive 解码器）追随的是
Chromium `FFmpegAudioDecoder`（BSD），与 ffplay `audio_decode_frame`（滤镜图取帧 +
swr 重采样的消费端胶水）结构上并不相似，"Ported" 措辞疑似**夸大**；但**头注释白纸黑字
的移植声明不能由写代码的人单方面抹掉**，而按项目自己的规则（legacy/README 准入：
"自认移植的文件应进隔离区"）把它搬进 legacy/ 又会打断"基座纯 BSD"的更名目标。
这是法律判断，本轮**不动代码、不动 LICENSE**，只登记矛盾。两条出路（均需法务/作者确认）：

- **A（推荐，若作者确认是措辞夸大）**：改写头注释为准确的出处描述（结构追随 Chromium，
  原创实现），LICENSE 不变，基座保持纯 BSD-3。
- **B（保守）**：文件搬入 `media/filters/legacy/` 并进 LICENSE §1 例外清单，声明为
  LGPL-2.1；代价是 §1 "隔离目录 = 全部衍生作品" 的清单式边界被稀释。

> **决策：A（同轮拍板）。** 作者确认当初的 "Ported" 是措辞夸大——该文件与 ffplay 的
> 关系是**角色对位**（PROGRESS 第八轮自己就记录了关键差异：输出编解码器原生格式、
> 转换留给消费者，与 `audio_decode_frame` 的回调线程 swr_convert 正相反），不是表达式
> 移植。头注释已改写为准确出处（结构追随 Chromium FFmpegAudioDecoder，原创实现，
> 附一句更名记录防回潮），LICENSE 不变，**基座自此为纯 BSD-3 + legacy/ 一处隔离区**。
> 全仓 `LGPL|Ported from` 扫描：`filters/` 下仅剩两处指向 legacy/ 的指路注释，
> 语义准确，保留。

### 连带登记：`renderer_impl.cc` 的 C1 豁免

更名跑 invariant 时暴露 `media/filters/renderer_impl.cc` 540 行 > 500 限——**分支既有
违规**（`daf6373` 起 512 行，饥饿信号各 commit 推到 540），与更名无关，CI 未及发现。
按 C2 豁免的先例登记进 `LINE_LIMIT_ALLOWLIST` 并写明理由（视频/音频两半互为镜像，
按行数硬拆只会复制生命周期状态机）；M9 BufferController 吸收
`CheckBufferingTransitions()` 时应回头重审。

### 验证

- no-ffmpeg preset 全量构建 + 376/376 测试通过（本机 macOS，GTest 经 Homebrew 补装——
  此前 287 个用例只在 Linux CI 跑过）。
- `check_invariants.py` 全规则通过（258 文件），C23 基线只降不升。
- FFmpeg 配置与 sanitizer 配置本机未跑（依赖 Homebrew ffmpeg/SDL2 环境较重），
  以 Linux CI 为准。

## 第十三轮（本轮）：DataSource 桥（M4 余项收口）+ 饥饿信号（M9 第三块）

| 组件 | 说明 | 备注 |
|---|---|---|
| `platform/ffmpeg/data_source_io.{h,cc}` | **DataSource → AVIOContext 桥**，M4 余项收口：内存缓冲 / 宿主 `DataSource` 经自定义 AVIOContext 进入 FFmpegDemuxer（kUri/kFd 仍走协议层）。桥自持字节位置——`avio_tell()` 是"FFmpeg 消费到哪"，不是"下一字节从哪来"，混用会毁掉每次 seek；读路径只用 `ReadBlocking`（D3 契约），demuxer 的 interrupt flag 短路阻塞读，Stop() 会 `Abort()` 源 | 2 端到端用例（探针/尺寸/时长过桥）；AVFMT_FLAG_CUSTOM_IO 下 `avformat_close_input` 不管 pb，自定义 pb 的正确终结符是 `avio_context_free`（`avio_closep` 会把 opaque 当 URLContext 释放）；`avio_alloc_context` 的缓冲归 FFmpeg 所有，必须 `av_malloc` |
| `renderer_impl` 饥饿信号 | `CheckBufferingTransitions()`（10ms 时钟节拍，S1）：所辖流全部干涸 → kHaveNothing，数据恢复 → kHaveEnough，只有边沿过界。**这是 BufferController 循环的第一个真实触发者**——此前 `OnBufferingStart` 没有任何调用方，三级 HWM 永远停在第一步 | 无新增用例（管线 seek 夹具的手动泵天然制造干涸边沿，409 全绿即回归证据）；防滞回（trickle 振荡）留待限速源用例观察 |
| `player` 侧 | `CreateDemuxer` 接受全部描述符种类；demuxer 内部对不支持的种类报可操作错误 | |

### 本轮补强：饥饿边沿的确定性 + 管线级断言

首版饥饿检测纯靠 10ms 采样，管线 seek 夹具证明它会漏掉短于采样周期的干涸窗口
（关键帧 seek 的边沿捕到了，精确 seek 的没捕到）。修正分两层：

- **Flush 即边沿**：flush 按定义清空所有队列，kHaveNothing 在 Flush 里同步发射，
  不与 demuxer 的回填竞速；恢复边沿仍由采样报告。
- **管线级断言**：`PipelineSeekTest` 两个用例现在都断言 seek 后
  kHaveNothing→kHaveEnough 边沿到达客户端——没有渲染器的饥饿信号，
  `have_nothing` 永远为假，facade 的 HWM 永远不前进（本断言就是防回归的哨兵）。
- 过程教训（同类第三次）：一次多段 python 补丁在中间 assert 失败时**整脚本不落盘**，
  重试脚本只补了报错之后的两段，漏掉了报错之前已"看起来成功"的第一段——
  `rendering_` 标志静默丢失，测试失败信息（have_nothing=false）与根因（标志未置位）
  相距十万八千里。**多段编辑必须每段独立落盘验证。**

### 遗留（M9 内，更新）

1. **饥饿竞态诊断结果（第十四轮闭合一半）**：(a) 精确 seek 的 kHaveEnough 丢失
   **根因已修**——`StartPlayingFrom` 重置 `starved_reported_`，把 Flush 刚发布的
   DRY 边沿状态吞掉；seek 重启后数据若先于首个 10ms tick 到达，RECOVER 永不发布。
   重置本身语义错误（起播时的干涸是合法的 kHaveNothing，ffplay 同义），移除后
   12/12 稳定；(b) 限速源用例 **已转正（第十四轮）**。两级修复：音频干涸判据撞上 WSOLA 残余
   OLA 窗口（960 帧）——地板修到四个周期仍不够；正解是**子渲染器自己发布
   starved 判决**（AudioRendererImpl 在设备回调里发布：短供/空供置位、满供清除，
   暂停不计入；RendererImpl 只做合并，不再猜测队列深度），事件驱动彻底取代采样，
   8/8 压力全稳。过程教训：Render 已被后续轮次重构出 DrainRing，补丁引用了
   不存在的 `requested` 变量——**构建失败时压测结果无效**（跑的是旧二进制），
   修复后必须重跑全部数字。
2. **限速源卡顿-恢复集成用例**：断言层已就位（边沿到达客户端），差的是
   FFmpeg+ThrottledDataSource 的夹具变体（进 media_ffmpeg_unittests）——
   FFmpegDemuxer(ThrottledDataSource(Memory(file))) + 假 sink 的管线级驱动，
   断言 kHaveNothing→kHaveEnough 的边沿与 hwm_step 递进。下一轮第一优先。
2. **RetryDataSource 已落地（第十四轮）**：`media/filters/retry_data_source.{h,cc}`——
   连续失败重试同一请求（成功即重置连败计数，间歇抖动不会累积成放弃），可中止的
   分片睡眠兜住 Δ15，异步 Read 走私有 worker 且绝不内联回调；4 个单测（恢复、
   预算耗尽、异步投递、Abort 及时解锁）走 media_unittests（无 FFmpeg 依赖）。
   剩 LiveDataSource / UrlRewriteInterceptor。
3. docs/07 §5 其余管线级断言。
4. 精确 seek 的 golden 验证等 M10/Q8。

---

## 第十二轮（本轮）：M9 精确 seek——丢弃窗口 · 到达检测 · SeekController

`SeekMode::kAccurate` 的 DoD（"精确 seek ±1 帧"）全链落地。此前它按桩返回警告并降级
关键帧 seek；本轮把它做成真实现。

### (1) 设计与接线

| 层 | 内容 |
|---|---|
| `media/base/renderer.h` | + `BeginAccurateSeek(target, reached_cb)` / `EndAccurateSeek()`（纯追加，默认 no-op，与 SetPaused 同例）。**窗口在 Flush() 中存活**（compositor 的 Flush 刻意不重置 accurate 状态），所以允许在关键帧 seek 之前开窗——解码重启时新世代已经被框住 |
| `renderer_impl_controls.cc` | RendererImpl 实现：S1 开窗（S3 打开 compositor 规则）、S6 呈现钩子 `OnVideoFramePresented` 同时喂视频时钟与目标检测（原子 target 只被观察，判定全在 S1）、**无视频流时立即回报 reached**（否则只能靠超时）；End 的每条路径（到达/超时/取代）都必须关窗，否则窗口会永远吃帧 |
| `pipeline_impl.{h,cc}` | 具体侧转发（不入冻结的 Pipeline 接口——Seek() 的"只做关键帧"契约保持精确，框帧策略属于 facade） |
| `player/seek_controller.{h,cc}` | 策略核（照 BufferController 的纯函数模式）：`Evaluate(reached, deadline, now)` 纯决策——**到达胜过过期**（期限瞬间落地是成功不是竞态）；单槽规则（新 seek 取代在等 seek，旧回调以 kAborted 完成而非悬空）；全部状态在媒体序列 |
| `player_impl.cc/events` | `SeekTo(kAccurate)`：登记目标 → S1 上 BeginAccurateWaitOnMedia（先取代旧的）→ pipeline Seek；`OnMediaSeekDone` 对 accurate 请求**不完成**（显示还在追赶）；到达 hop / 超时任务汇入 `CompleteAccurateSeek`：先关窗（帧恢复流动）再发事件——kAccurateSeekCompleted + kSeekCompleted（result 携带 kTimeout，Δ10 可观测），用户回调以 Ok/kTimeout 收尾 |

### (2) 验证

```
✅ seek_controller_unittest 5 例（到达胜过期、期限边界、Begin/End 身份、取代覆盖）
✅ 管线级 AccurateSeekPresentsNothingBeforeTheTarget：从开窗起**没有任何 < 目标帧的
   帧到达显示**（比关键帧 seek 测试更强——那只能约束上界），reached 回调必须触发
✅ headless --seek 1.5 --accurate（真实 FFmpeg）：落地后首个采样 1.52s（关键帧落在
   1.33s 之前），继续播到 kCompleted；--seek 1.4 → 1.42s
✅ 407/407（FFmpeg）· 372/372（no-ffmpeg）· invariants 252 文件全过
```

### (3) 发现与修正

- `End()` 之后读 `request_id()/target()` 拿到的是重置值——先取值再 End（首版两处顺序
  反了，编译期发现不了，单测的 ReplacingAWaitReplacesItsIdentity 钉住）。
- 取代路径首版漏发用户回调（只清表）——"新 seek 取代旧的"必须以 kAborted 回答，
  与 demuxer 层被取代 seek 的契约同形。

### (4) 遗留（接第十一轮的 M9 清单）

1. ~~SeekController 精确 seek~~ → **本轮完成**。金色验证（与原版对齐）仍等 M10/Q8。
2. `RetryDataSource` / `LiveDataSource` / `UrlRewriteInterceptor`（故障注入假件已就绪）。
3. **管线级 HWM 集成用例的前置缺口（本轮确认）**：ThrottledDataSource 是
   `media::DataSource`，而 `DataSource → demuxer 的 AVIOContext 桥`（M4 余项）未建，
   合成 demuxer 也不收 DataSource——用例要等桥落地；更深一层，`kHaveNothing` 饥饿信号
   还没有从 renderer 端到端接线（BufferController 的 OnBufferingStart 目前没有真实触发者）。
   桥 + 信号两者是下一轮的第一优先。
4. docs/07 §5 其余管线级断言（丢帧数、变速、故障注入、循环、纯音/纯视频）。

---

## 第十一轮（本轮）：M9 开篇——限速假件 · 三级 HWM · 管线级 seek 夹具

M9（缓冲 + Seek 完整版）的前两步，加上上一轮收尾的测试基建：

| 组件 | 说明 | 备注 |
|---|---|---|
| `tests/unit/media_filters/pipeline_seek_unittest.cc` · `tests/support/{fake_sink_factories,fake_pipeline_client}` | 管线级 seek 契约（docs/07 §5 首条）："SeekTo(5s) 后落地帧 ∈ 150–152" + "落地后旧世代不再出现"；`DefaultRendererFactory` 组装 + S1/S3/S4 三条真线程 + 手动泵双 sink | 压力 100/100、`ctest -j8` 4 轮 349/349；连带抓出 #51/#52。确定性规则：等待有界单向 · 渲染只由泵驱动 · 泵轮节奏落后媒体节奏（16ms 睡眠让外推时钟超前，落地帧漂到 155–158）· Play 等 kHaveMetadata |
| `tests/support/throttled_data_source.{h,cc}` | M9 的测量仪器：限速供数（墙钟预算 + 封顶突发）+ `FailFrom/ClearFailure` 故障注入 + `Abort()` 及时解锁 + `bytes_served/stalls` 观测 | 7 用例。设计陷阱：默认突发 = 1s × 速率，小于速率的文件在突发内跑完、节流器形同虚设 → `set_max_burst_bytes()` 旋钮 |
| `player/buffer_controller.{h,cc}` | 三级 HWM 决策核心（照 `DecideNextFrame` 纯函数模式）：`first`(100ms) 起播 · `next`(500ms) 首次恢复 · `last`(4s) 后续 + 上限；每完成一个缓冲周期升一级，**seek 完成回卷到 first**（新位置对旧缓冲一无所知） | 10 用例。挂接 `PlayerImpl`（媒体序列，`SEQUENCE_CHECKER` 免锁）：`kBufferingEnded` 载荷携带本周期实际等待的 mark；缓冲期间统计 tick 发 `kBufferingProgress`；`OnMediaSeekDone` 执行重置 |

**TSan 浸泡与三个真缺陷**（#51/#52/#53，详见 §(3) 表）：30 分钟 232 轮 0 报告（修 #51 前第 68 轮 SEGV）。

### (5) 遗留（M9 内）

1. `SeekController` 精确 seek ±1 帧（`HoldReads` 世代机制为地基，待做）。
2. `RetryDataSource` / `LiveDataSource` / `UrlRewriteInterceptor`（故障注入假件已就绪）。
3. 管线级 HWM 集成用例：限速源下观察 `hwm_step` 递进与起播/恢复判定（DoD"限速 50KB/s 下
   卡顿-恢复循环正常"），假件已就绪、用例未写。
4. docs/07 §5 其余管线级断言（丢帧数、变速、故障注入、循环、纯音/纯视频）。

---

## 第十轮：播放链路打通

本轮把"能构建、能测"推进到"能播放"。新增实现约 2,900 行：`PipelineImpl`（管线编排）、
`DefaultRendererFactory`、media 层 null 双 sink、`FFmpegVideo/AudioDecoderFactory`、
player 层 `PlayerImpl`/`StateMachine`/`EventHub`，以及 platform/sdl2 双后端（M11 的
代码提前落地）与两个示例。**验证边界**：macOS 本机（Xcode 工具链）；Linux 真机
X11/Wayland 未验证；Golden Test 未做（M10/Q8 未动）。

### (1) 新增组件

| 组件 | 说明 |
|---|---|
| `media/filters/pipeline_impl.{h,cc}` · `pipeline_impl_host.cc` | Pipeline 的唯一实现：demuxer 先于 renderer 初始化（并行会让 renderer 拿到零条流）、seek 为"renderer flush ∥ demuxer seek"两阶段完成、Stop 按文档 §10.1 顺序销毁。GetMediaTime 直读共享的 AvSyncController（seqlock），renderer 销毁后仍可安全应答。**C1 拆两个 TU**：`pipeline_impl_host.cc` 承接 stream-id 查询、M9 轨选桩与 Demuxer::Host / RendererClient 回调 |
| `media/renderers/default_renderer_factory.{h,cc}` | 把 Deps 的 sink/decoder 工厂组装成 RendererImpl；sink 工厂缺省时回落 media 层 null sink——零配置 headless 播放的落点 |
| `media/filters/null_{video,audio}_sink.{h,cc}` | 放在 media/filters 而非 platform/null（Chromium 的 media/video/null_video_sink 先例）：headless 是框架能力不是平台能力。null 音频 sink 真实按设备周期消费（音频时钟才能前进），null 视频 sink 以 60Hz 驱动 compositor |
| `media/filters/ffmpeg_decoder_factories.{h,cc}` | 软解工厂（.cc 进 platform_ffmpeg target），排在注入的硬解工厂之后（Δ12 回落链的尾巴） |
| `player/player_impl.{h,cc}` · `player_impl_events.cc` · `state_machine.{h,cc}` · `event_hub.{h,cc}` | 门面实现：S1/S3/S4 三线程 + 事件线程、PlayerState 转移表（非法转移拒绝并给出可操作错误）、seek 按 request_id 匹配回调、loop 循环播放、~Player 有界等待（Δ15）。**C1 拆两个 TU**：`player_impl_events.cc` 承接 seek 完成、运行时控制、观察者回调与状态访问器 |
| `platform/sdl2/`（M11 提前） | Sdl2VideoSink（宿主持窗口/renderer，sink 线程独占绘制 YUV 纹理）+ Sdl2AudioSink（SDL 音频回调只做 planar→interleaved 拷贝，Δ13）+ `cmake/FindSDL2.cmake`。宿主窗口嵌入模式（surface.h 的两条线程规则） |
| `examples/headless.cc` · `play_sdl2.cc` | headless 支持 --seek/--rate/--timeout，是 CI e2e 的雏形；play_sdl2 约 150 行（docs/10 的"main 行数 = API 易用性"度量）——C2 把参数解析、建窗与事件泵移出 `main()`，承载该度量的是文件而不是 main 本身 |

### (2) 对既有文件的修改（全部有理由）

| 文件 | 修改 | 理由 |
|---|---|---|
| `media/base/demuxer.h` | `Demuxer : public MediaResource` | media_resource.h gap 1 预言的改动；RendererImpl::Initialize 需要 MediaResource 视图，而无 RTTI 无法跨无关基类转换 |
| `media/base/renderer.h` | + `SetPaused`/`SetOutputTarget`（默认 no-op，实现移出 .h）；`media/base/pipeline.h` + `Play`/`Pause`/`SetOutputTarget` | Chromium Pipeline 本有 Play/Pause（DRAFT 时被裁掉）；暂停不能走 `SetPlaybackRate(0)`——AudioRendererAlgorithm 明确拒绝 rate 0 |
| `renderer_impl.{h,cc}` | 暂停/换窗转发、外部时钟锚定、视频时钟喂入（Render 回调调 `OnVideoFramePresented`——此前**无人喂视频时钟**，纯视频流永远没有有效主时钟）、EOS 传播（CheckForEnded + 周期复查）、init 回调改为成员持有并 hop 到 S1、Flush 完成回调 hop、dtor 在子渲染器自己的 sequence 上销毁它们 | 逐条见 §(3) 的 bug 列表。**C1 拆两个 TU**：`renderer_impl_controls.cc` 承接启动后的控制面；**C2**：`Initialize()` 的子渲染器装配移到 `CreateSubRenderers()` |
| `audio_renderer_ring.cc` | **C1 第二次触顶后拆 TU**：`PreStretch()`/`DrainRing()`（把算法帧搬进就绪环、设备再从环里取）搬到新 TU，`OutputFramesToMediaTime()` 提为私有静态跨 TU 共用。缝是真的：这里全在 S4，`Render()` 留在原地跑在 S7，两者只共享 `handoff_lock_` 下的环 | 拆后主文件 513 → 422 行 |
| `audio_renderer_impl.{h,cc}` | `SetPaused`（设备路径门控）、`kOk+null`（流排空）标记 EOS、消费方唤醒；取环循环移到 `DrainRing()`（C2） | #36/#41/#42 |
| `video_renderer_impl.{h,cc}` | 自然 EOS（kOk+null）此前被当作"无事发生"无限泵；帧呈现回调；消费方唤醒 | #36 |
| `decoder_stream.cc` | demuxer 读回调整 hop 到自身 sequence | #40 |
| `ffmpeg_demuxer.{h,cc}` | open 时安装 log bridge（否则 FFmpeg 失败原因不可见）；网络专用选项按 URI 协议门控（reconnect*/user_agent/headers 传给本地文件只会变成 Δ2 的"未消费选项"噪音） | |
| `player/player.cc` | 改为纯转发到 PlayerImpl；RunUntilIdle/StepOnce/TakeSnapshot/UpdateConfig/SelectTrack/ReconnectNow 保持 kNotImplemented 并指明里程碑（M9+） | |
| `base/synchronization/lock.h` | `Lock` 补 `CAPABILITY("mutex")`、`AutoLock` 补 `SCOPED_LOCKABLE` + `EXCLUSIVE_LOCK_FUNCTION`；新增 `EXCLUSIVE_LOCKS_REQUIRED` 宏 | 注解此前不成立：`GUARDED_BY` 从未真正生效，且每处 `GUARDED_BY` 自己又产生一条诊断 |
| `cmake/AvbaseCompilerFlags.cmake` | `-Wuseless-cast` 移入 GCC 专有列表 | clang 报 unknown warning option，`-Werror` 下 debug 预设无法编译 |
| 24 个文件的头注释 | 修复第十轮脚本把新段落插进旧句子中间留下的断句、重复的 `(promoted from DRAFT, tenth round)` 与重复空注释行 | 纯重排与合并，除重复片段外未改字面 |
| `.github/workflows/ci.yml` · `CMakePresets.json` | 骨架 job 变真：新增 `ffmpeg`（发行版 FFmpeg，359 用例）、`e2e-headless`（5 个样本播到结束）、`sdl2-build`（Linux 上编译 SDL2 后端）；新增 `ffmpeg` 预设，`linux-sdl2` 补上 FFmpeg | 带 FFmpeg 的配置与端到端此前从未在 CI 跑过（`ffmpeg-matrix` / `e2e-linux` 一直是 `if: false`）；`linux-sdl2` 因缺 FFmpeg 连 `play_sdl2` 都建不出来 |
| `base/synchronization/lock.cc` · `tests/unit/base/synchronization_unittest.cc` | `ObservedOrder()` 补 `thread_local`；新增 `LockTest.ConcurrentOrderRecordingIsRaceFree` | #44 |
| `tests/support/synthetic_demuxer.{h,cc}` | 合成源（M10 的第一片，docs/07 §5）：10s/30fps/48kHz 的可脚本化 `Demuxer`，**编码即契约**——每个包的负载前 4 字节是它自己的序号（`ReadIndexPayload`），时间戳按序号整数算出（不是累加帧时长，所以第 150 帧恰好是 5.000s），音频音高 = 440 + 该包所落的整秒，关键帧每 30 帧一个，`StartPlayingFrom()` 落在"不晚于请求"的关键帧上、回传实际位置并 **bump 流 serial**（新世代，`demuxer_stream.h` 的契约，也是 #50 的判据） | 自测 6 例（序号/节奏/关键帧/seek 落点/音频秒边界/EOS/媒体信息）全过且耗时 **0 ms**；帧号画进像素与故障注入（`fail_read_at_packet` 等）**故意未实现**——没有消费方的钩子只会让替身开始说谎 |
| `tests/support/*` · 三个渲染器套件（`renderer_impl` · `video_renderer_impl` · `audio_renderer_impl`） | 渲染器直接单测：脚本化输入、可编排解码器、手动拉动的双 sink、记录型 renderer client（`renderer_client.h` 里点名"not written yet"的那个）。14 个用例覆盖启动契约（只报一次、绝不内联、视频 sink 在解码器就绪时开、音频设备在 StartPlayingFrom 才开）、结束契约（双流排空 + 尾帧必须发布）、暂停门控（视频不出新帧；音频报静音且不计 underrun）、以及 flush 后的串号隔离（两侧都不许放行旧 serial 的帧） | 第十轮的 12 个渲染器 bug 全靠端到端发现；这套件当场抓出 #47/#48/#49，三处产品修复随它一起进 |
| `tests/unit/media_filters/pipeline_seek_unittest.cc` · `tests/support/{fake_sink_factories,fake_pipeline_client}.{h,cc}` | 管线级 seek 契约（docs/07 §5 首条）："SeekTo(5s) 后落地帧 ∈ 150–152"+"落地后旧世代不再出现"，`DefaultRendererFactory` 组装 + S1/S3/S4 三条真线程 + 手动泵双 sink，注入点就是 `Pipeline::Start()` 的 demuxer 参数（产品代码零改动） | 压力 100/100；连带抓出 #51/#52。等待全部有界单向、渲染只由泵驱动，是它和被撤回首版的全部差别 |
| `tools/check_invariants.py` | 门禁收敛：17 条 → **7 条结构规则**（C1 文件规模 · C4/C5/C22 分层 · C23 列宽棘轮 · C24/C25 构建覆盖），删 10 条风格类（C18 抛异常/C20 命名空间失衡本就是编译器必报；C7/C9/C14/C17/C21 是 taste；C2 函数长度、C8 版本守卫、C11 目录配对按"违规即藏缺陷"标准不再保留）。**规则 ID 不重编**，历史可 grep | 文件 680 → 509 行；探针验证：被砍规则不再报、游离 `.cc` 仍被 C25 拒绝（exit 1）；docs/06 §9 与 docs/07 检查单同步 |
| 10 个 `CMakeLists.txt` · `cmake/AvbaseCheckInvariants.cmake` · `tools/check_invariants.py` | 源文件列举定成一条规则（docs/06 §7.6）：目录成员 == 目标成员处改用 `file(GLOB ... CONFIGURE_DEPENDS)`（`media/base` · `media/renderers` · `media/filters/legacy` · `player` · `platform/{ffmpeg,sdl2}` · `tools/inspect` · `tests/unit/{base,media_base,player}`），其余四处（`media/filters` · `tests/unit/media_filters` · `base` · `examples`）保持显式并在文件里写明理由。**新增门禁 C25**：每个 `.cc` 必须被某个目标覆盖（显式列表，或规则自己展开的 glob），否则非零退出 | 新增文件不必再改 CMake，Ninja 在构建时重跑 glob（`[0/N] Re-checking globbed directories...`）。C24 管"列出的文件存在"，C25 管"存在的文件被编译"——两个方向都不再静默。`aux_source_directory` 明确不用：不递归，且新增文件不触发 CMake 重配（CMake 官方文档警示的正是这一点）。实测 108 个 (target, source) 与改动前逐一相同 |

### (3) 本轮抓到的 bug（接通播放 = 第一条真正跑全链路的路径，收获很大）

| # | 问题 | 性质 |
|---|---|---|
| 35 | `PlayerStateMachine::TransitionTo` 只校验不赋值——状态机形同虚设，每次调用都停在 kIdle | 🔴 实现 bug（本轮新写） |
| 36 | 视频"自然 EOS"（kOk+null）被当作无事发生、无限泵；音频流排空（kOk+null）同样未标记 ended → 播完永不 kCompleted | 🔴 两个子渲染器各一个 |
| 37 | RendererImpl 的 init 回调经视频路径传递，video 先完成时闭包随返回栈帧销毁 → pipeline 永远等不到 ready | 🔴 回调生命周期 |
| 38 | init 回调在最后完成的子渲染器线程（S4）内联执行，PipelineImpl 的 sequence checker 当场击毙——checker 第一次抓住真问题 | 🔴 跨 sequence |
| 39 | RendererImpl dtor 在 S1 销毁子渲染器，DecoderStream 的 checker 拒绝 → 销毁必须发生在各自 sequence（post+Wait，外部由 Δ15 兜底） | 🔴 销毁顺序 |
| 40 | `DemuxerStream::Read` 回调直达 DecoderStream（S1→S3）：不只是 DCHECK 违规，`pending_reads_`/`decoded_outputs_` 是真实数据竞争 | 🔴 数据竞争 |
| 41 | 解码泵内联递归：DecoderStream 缓存同步交付时每帧一层栈，真实文件直接栈溢出（SIGBUS on S4） | 🔴 |
| 42 | 泵在背压处停止后无人唤醒（消费方清空压力没有人通知生产者）——ffplay 用 condvar，这里用 Render 回调里的 post | 🔴 逻辑缺口 |
| 43 | RendererImpl::Flush 给音频路径传空 closure，AudioRendererImpl::Flush 无条件 Run → "null callback" CHECK | 🟠 |
| 44 | 锁序检查器的"已观测顺序"表是**所有线程共享**的 `std::vector`（旁边那张 held 栈写了 `thread_local`，这张漏了），而记录发生在取锁之前 → 并发 `push_back` 直接踩坏堆。表现：`headless` 在 6 路并发下约 1/36 次**静默** `SIGABRT`/`SIGTRAP`（`libmalloc: pointer being freed was not allocated`，栈落 `TaskQueue::PostDelayedTaskImpl` → `AutoLock` → `AssertAcquiredInOrder`），播放本身看不出任何异常 | 🔴 数据竞争（**早于第十轮**，由"把 CI e2e 变成真 job"时的抖动调查发现） |
| 44 | RendererImpl::Flush 完成回调从 S3 直达 PipelineImpl | 🔴 跨 sequence |
| 45 | PipelineImpl 并行初始化 demuxer 和 renderer，后者拿到零条流报 kMissingDemuxerStreams（首跑的"StreamNotFound"假象） | 🔴 编排顺序 |
| 46 | **macOS 移植四件**：根目录 `VERSION` 文件在大小写不敏感 FS 上遮蔽 libc++ 的 `<version>`（改名 VERSION.txt）；`pthread_setname_np` 平台差异（统一截断 15 字符）；FindFFmpeg 的 pkg-config 分支 include 目录经 `PkgConfig::` 中转后丢失（改为从 PC_* 变量直构）；`avbase-inspect` 因 FFmpeg PRIVATE 链接拿不到头（显式链接） | 🟠 平台 |
| 50 | **seek 后新世代一帧都到不了显示**：`RendererImpl::Flush` 用 `av_sync_->master_serial()`（**seek 前**的音频时钟 serial）去 flush 视频解码流，而真实 demuxer 的队列在 seek 时会 bump serial（`DecoderBufferQueue::Flush()`，注释写明"这就是关键"）——于是解码流按上一代过滤，把新世代的包全部丢弃，seek 之后视频永久停摆。headless 端到端看不见：null sink 不数帧，kCompleted 只要求 EOS | 🔴 编排缺陷（合成源 + 管线级夹具发现；修法：子渲染器在 `StartPlayingFrom`（demuxer 已完成 seek）采纳新世代，走 `DecoderStream::AdoptSerial()`；先试过用 `Flush()` 采纳——它会以 kDecodingAborted 收掉在途读、泵不再被重新武装，真实 seek 3/4 挂） |
| 47 | **init 回调内联**：`RendererImpl::Initialize` 的缺流错误路径直接 `std::move(init_cb).Run(...)`，违反 renderer.h 的"绝不内联、调用方可在回调里销毁状态"——调用方会在 `Initialize()` 还在栈上时被重入 | 🔴 契约违背（新单测发现；已改为与成功路径同一条 hop） |
| 48 | **音频 EOS 尾帧永不发布**：`MarkEndOfStream()` 只翻标志不搬帧，而 `PreStretch()` 只在 `OnDecoderOutput` 与"恢复暂停"时调用。解码器一次输出的帧数大于设备周期时，EOS 到达那一刻环是满的 → 泵因背压停摆、此后无人搬运 → `buffered_frames()` 永不归零 → `CheckForEnded()` 永不报 `OnEnded`（"播完了但不结束"）。设备周期与解码粒度相同时不触发，这正是端到端一直没遇到它的原因 | 🔴 逻辑缺口（新单测发现；修法：`PumpDecoder()` 在 `ended_` 后仍搬运一次尾帧） |
| 49 | **初始化上报跨 sequence**：`OnVideoInitialized`/`OnAudioInitialized` 由子渲染器在 S3/S4 上调用，却直接写 S1 的 `video_initialized_`/`audio_initialized_` 并互相读。第十轮只给 "ended" 与 init 完成两条路径加了 hop，这条漏了 | 🔴 数据竞争（TSan 在真线程夹具下报出；已加 hop） |
| 51 | **析构后时钟推送链继续跑**：`RendererImpl::PushMasterClock`/`PushStatistics` 两条 10 ms 自续定时链用 `Unretained(this)` 绑定，对象销毁后队列里残留的任务照常执行，读到已释放的 `Clock` → SEGV。TSan 浸泡第 68 轮抓到（崩在 `~TaskEnvironment` 收尾的 `RunUntilIdle`，普通构建下是无征兆的 UAF） | 🔴 生命周期（修法：两条链与首次定时都改绑 `WeakPtrFactory`，失效任务由绑定层跳过；成员置于末位并注明） |
| 52 | **flush 与重启的窗口烧掉整个新世代**：`VideoRendererImpl::Flush` 写入的是过期 serial（`av_sync_->master_serial()`），此后到 `StartPlayingFrom` 采纳之间，自由运行的消费泵驱动 `DecoderStream` 继续读 demuxer——此时 demuxer 已 bump 到新 serial，于是新世代每个包都按"stale"丢弃，而 EOS 包按契约不过滤序列 → `end_of_stream_` 直接置位：流还没开始就结束了，"ended" 提前、落地帧永不出现。实测约 3% 的管线级 seek 命中（插桩日志：连续 `stale drop buf_serial=1 mine=0` 后紧跟 `EOS buffer accepted, mine=0`） | 🔴 编排缺陷（管线级夹具压力 100 轮发现；修法：`DecoderStream::HoldReads()`——flush 合闸、`StartPlayingFrom` 采纳后开闸，两侧子渲染器同样处理） |
| 53 | **统计路径跨序列读未保护状态**：`RendererImpl::GetStatistics()`（S1）经 `buffered_frames()` 读 `AudioFrameQueue::frames_`，与 S4 的 `SeekFrames`/`Append` 并发。该队列"单序列无锁"是有意设计（头文件注记：音频回调 100 µs 预算不允许加锁），破坏契约的是统计读这一侧 | 🟠 数据竞争（TSan 浸泡第 1 轮报出；修法：`frames_` 改 `std::atomic<int>`——不加锁、保留单序列设计，读方 relaxed） |

### (4) 验证结果（macOS 24.5 arm64 / AppleClang 21 / Homebrew FFmpeg 7.1.1 / SDL2 2.32.6）

```
✅ headless: small_h264_aac_3s.mp4  → completed at 2.99s（对照 ffprobe 3.000s）
✅ headless --seek 1.5             → seek ok → 续播 → completed at 2.11s
✅ headless: audio_only.m4a        → completed at 0.71s（纯音频路径）
✅ headless: video_only.mp4        → completed at 1.00s（纯视频路径，视频时钟经
                                      Render 回调喂入后主时钟正确回退）
✅ headless: truncated_tail.mp4    → completed at 2.88s（截断文件优雅播到 EOF）
✅ corrupt_header.mp4              → prepare 失败，错误含 DecodeFailed/建议（不崩溃）
✅ play_sdl2（真窗口 + 音频）       → 播完 kCompleted，退出码 0
✅ ctest: 373/373（mac 配置）· 338/338（no-ffmpeg）——第十轮末尾各 +1：锁序竞态回归用例（#44）；
   渲染器直接单测再 +14（RendererImpl 6 · VideoRendererImpl 3 · AudioRendererImpl 5）
✅ TSan：三个渲染器套件 14 用例在 `tsan` 预设下 **0 报告**（此前该路径报出 #49 与
   假 sink 自身的 3 处竞争，两者都已修）；**30 分钟浸泡**（渲染器三套件 + 管线级 seek
   连续循环）修 #51 前在第 68 轮 SEGV、修后完整跑完 **0 报告**；TSan 下
   `headless --seek 1.5` 连跑 **20 次全部 exit=0**
✅ 严格告警配置覆盖 FFmpeg 层：`debug`（Debug + 严格告警 + `-Werror` + FFmpeg）实测
   **0 警告**；此前该预设沿用 `AVBASE_ENABLE_FFMPEG=OFF`，FFmpeg 适配层
   （`platform/ffmpeg/*` 与 `media/filters/ffmpeg_*.cc`）从未进过严格门禁。
   合成源是这条门禁扩宽后写下的第一批代码：同样 0 警告
✅ 合成源自测 6 例 + 合成解码器 4 例（348/348 · 383/383 内含），耗时 **0 ms**
✅ 真实 seek 稳定性：修 #50 后 `headless --seek 1.5` 连跑 **6 次全部 exit=0**；而用
   `Flush()` 采纳世代的那一版连跑 4 次里 3 次 exit=1
✅ 管线级 seek 夹具（重建版）：压力 **100/100**，`ctest -j 8` 全量并行 4 轮
   **349/349 ×4**；首轮无闸门版本 100 轮 18 失败（泵速失配：音频每轮只推 5.3 ms 媒体
   时间而显示间隔 16 ms，落地帧永远"未到期"），对齐泵速后仍 3/100——那 3 次就是 #52
✅ 伪证检查：临时撤掉 #48 的修复后，`EndedPublishesTheTailTheRingCouldNotTake` 与
   `EndedIsReportedAfterBothStreamsDrain` 双双报红，恢复后全绿——两条结束用例确实咬住了
   那个缺陷，而不是恰好通过
✅ check_invariants 全过（220 文件）；C23 列宽基线 323 → 310（净减 13 行，棘轮只降不升）；
   新增 C25（源文件必须被某个目标覆盖）后复跑全过，且从零 configure 的构建目录同样 324/324
✅ 零警告（编译器 + 链接器）：RelWithDebInfo 的 no-ffmpeg / FFmpeg+SDL2 与
   Debug + 严格告警 + `-Werror` 三套配置实测。这份成绩单在第十轮末尾一度是虚的：
   `-Wthread-safety` 报的 660 条里有 642 条源于 `base::Lock` 缺 capability 注解，
   `-Wuseless-cast` 又是 GCC 专有选项却无条件传给了 clang（`-Werror` 下 debug 预设
   一行都编不过）。两处根因修好后 660 → 0，中途暴露的 18 条真问题逐条修掉。
```

### (5) 本轮未做 / 遗留

1. ~~**管线级 seek 断言：夹具稳定化**~~ → **已落地（重建版）**。`Pipeline::Start()` 接收一个
   demuxer（冻结签名就是为此），媒体层不改产品代码即可注入合成源。重建版与被撤回的首版差在
   四条确定性规则：**每个等待有界且单向**（轮询必须到达的状态，超时打印完整事件日志）；**渲染
   只由手动泵驱动**（没有自由跑的时钟可竞速）；**泵轮对齐节奏**（每轮 3 个音频周期 ≈ 16 ms
   媒体时间 = 一个显示间隔——首轮 18% 失败正是失配所致）；**Play 等的是 kHaveMetadata**
   （`OnDurationChange` 在 kReady 之前到达，用它触发 Play 会打在 kStarting 上被吞掉）。
   稳定后当场抓出 **#51/#52**（见 §(3)）。`tests/unit/media_filters/pipeline_seek_unittest.cc`
   （1 例）+ `tests/support/{fake_sink_factories,fake_pipeline_client}` 已进库。又补两条夹具
   层的教训，都在 TSan/压力下现形：**音频假 sink 的拉动必须封送回渲染器序列**（S4）——内联在
   gtest 主线程跑渲染回调会让渲染器状态被两个 sequence 同时触碰（TSan 报
   `SyntheticDemuxer::MakeAudioPacket` 竞争；`FakeAudioSink::set_render_runner` 即为此而设，
   RendererImpl 风格的套件单泵无需启用）；**泵轮的真实节奏要落后于媒体节奏**——音频时钟在两次
   消耗回报之间按挂钟外推，轮内睡 16 ms（真实 ≈25 ms）会让外推时钟超前，落地帧漂到 155–158
   被判晚丢弃；改成 4 ms 后时钟滞后（落后只会 hold，安全），100/100；**假件必须像真件一样
   线程安全**——`HoldReads()` 让两条流首次真正并发读取后，TSan 当场抓住 `SyntheticDemuxer`
   的共享游标在 S3/S4 竞争（真 demuxer 内部有锁，替身不能在这条契约上撒谎），已按真件语义
   加锁。
   仍欠：
   docs/07 §5 其余条目（丢帧数、变速、故障注入、循环、纯音/纯视频），以及 S1 泵依赖
   `TaskEnvironment` 的说明——管线任务全走 post，"只睡不泵"的夹具会拿着空事件日志干等。
   夹具形状备忘不变：`RendererImpl` 套件必须用真线程（析构 post 到 S3/S4 后阻塞等待），
   两个子渲染器套件单线程即可。
2. 首播 seek 曾触发一次 NAL 损坏（改为首播不重复寻址后消失）——根因未深究，
   真实 seek 已验证干净，但 seek 后串号隔离的黄金验证要等 M10。
3. `RunUntilIdle`（需要 message_pump_epoll，R2 降级债）、精确 seek（M9
   SeekController）、轨选切换、快照、UpdateConfig 白名单、`option_registry.inc`
   接线（A10 仍 9/66）——全部按桩返回 kNotImplemented 并指明里程碑。
4. SDL2 后端现在**在 CI 的 Linux 上编译**（`sdl2-build` job），但"跑起来"仍只有
   macOS：Linux 真机 X11/Wayland 与 `e2e-linux`（xvfb + llvmpipe + 双后端）留到
   M11/M12。带 FFmpeg 的 359 用例与 headless 端到端已进 CI（`ffmpeg` / `e2e-headless`
   两个 job）；覆盖率门禁仍欠。
5. macOS 首次构建暴露的文档失真已按本轮实测修正 README/BUILDING 的对应行。
6. C1/C2 清理把三个实现文件各拆成两个 TU（`pipeline_impl_host` · `renderer_impl_controls`
   · `player_impl_events`），并把音频取环、渲染器装配与两个示例的 `main` 抽成辅助函数。
   一处代价记在这里：`pipeline_impl_host.cc` 的 `OnDemuxerError`/`OnError`/`OnEnded` 各写
   一次 `state_`/`playing_`，与 M8 那条"具体状态机应与它驱动的 Pipeline 同 TU"的理由
   相反；M9 重排时把这三处收回 `pipeline_impl.cc` 即可闭合。
7. 警告门禁修好之后 `-Wthread-safety` 才开始说真话：它抓出 7 处 `state_lock_` 保护成员
   在锁外读写（`PlayerImpl` 的 `PrepareAsync` 两处、`source_`、`display_` 三处、
   `CreateDemuxer` 一处），以及 `FFmpegDemuxerStream::FulfilPendingReadLocked` 这类
   "约定持锁却没写注解"的助手。两件都已完成（该助手现在写着 `EXCLUSIVE_LOCKS_REQUIRED`）。
   **顺带查出一个门禁空洞并修掉**：严格告警 + `-Werror` 只在 `debug` 预设里，而它沿用
   `AVBASE_ENABLE_FFMPEG` 的默认 OFF——FFmpeg 适配层从未被严格配置编过，所以"零警告"
   只覆盖了不含 FFmpeg 的那半棵树，而那一层恰恰是 `-Wthread-safety` 最常说话的地方之一。
   `debug` 现在打开 FFmpeg（实测 Debug + 严格告警 + `-Werror` + FFmpeg：0 警告、
   373/373 通过），理由写进 BUILDING §3.3。**strict job 已进 CI**（`strict`，clang 编译，
   从零 configure + 构建 + `ctest --preset debug` 384/384 且 DCHECK 开启——这条配置此前
   从未跑过完整测试）：用 clang 而非发行版 GCC 是有意的，严格集会连带打开 GCC 专有的
   `-Wuseless-cast`，该旗标从未对本树实测过，按 R12"不要上线一门必红的门禁"，GCC 半边
   留给能在本地实测的人（job 注释写明了缘由）。
8. glob 改的是"新增文件不用改 CMake"，代价落在 DRAFT 的放法上（docs/06 §7.6）：留在一个
   被 glob 目录里的 DRAFT `.cc` 会被编译，所以它要待在没有任何 glob 能到达的子目录里
   （glob 不递归）直到能编译为止，并在 `DRAFT_FILES` 里登记。显式列表的四个目录不受此限。


---

> **历史轮记录(第 1–9 轮、早期专题)**已移至
> [archive/PROGRESS-rounds-1-9.md](archive/PROGRESS-rounds-1-9.md)——那里的教训
> 仍然有效(34 个编号 bug 的成因都在轮次记录里),只是不再属于"当前状态"。
> 里程碑级的未完成清单见下表。

> **本表为第九轮快照,条目级的最新去向以 [12-剩余工作清单](12-剩余工作清单.md) 为准**;
> 里程碑级的进度对照仍见下表(下次里程碑收口时整体重写)。

## 未完成（按里程碑）

> ⚠️ **本表与下方"工具与门禁现状"表是第九轮快照，仅作里程碑对照的起点**——
> M7/M8/M11 与 M9 大部分已在第十至十七轮完成（见文首状态块与各轮记录），
> 两表待下一次里程碑收口时整体重写。
> **第九轮修订**：本表此前混着几轮的历史行，同一条 M4/M5 既出现在 ✅ 行也出现在
> ⬜ 行（"**M4 ← 下一个关键路径** ⬜"与上面"M4 完成"直接矛盾），读者无法判断真实
> 进度。本轮按当前代码状态重写一遍；各轮的过程记录仍保留在上文对应小节里。

| M | 内容 | 状态 |
|---|---|---|
| ✅ M0 | 工程基建（CMake / Presets / flags / FindFFmpeg / CI） | 完成 |
| ✅ M1 | `base/` 核心件 | 完成，**但已落到 R2 的 L1+L2 降级**（见上文"(4) R2 降级债"） |
| ✅ M2 | `base/task/*` + `base/threading/*` + `TaskEnvironment` | 完成（L2：`TaskQueue` 驱动，无 `message_pump_epoll`） |
| ⬜ M1+ | `base/` 补充件：`feature_list` · `circular_deque` · `trace_event` · `strings` · `files`（含 `ScopedLibrary`，**M12 dlopen 弱依赖的前置**）· `LockOrderChecker`（**R5 应对⑤的前置**） | 非关键路径，但后两项已被 M12/M8 依赖 |
| ✅ M3 | `media/base/` 值类型与两个队列 + `AudioBuffer` + `decoder_config` | 完成 |
| ⬜ M3 余项 | `AudioRendererAlgorithm`（WSOLA） | **单点阻塞 M7 的音频半边** |
| ✅ M4 | `platform/ffmpeg/` + `FFmpegDemuxer` + `ffmpeg_glue` + 5 个测试媒体 | 完成，12 个端到端测试对真实媒体全绿 |
| ⬜ M4 余项 | `DataSource` 后端的 `AVIOContext` 桥（内存 / fd / 自定义源） | ⬜（`avbase-inspect probe` 已在第八轮完成） |
| ✅ M5 | `DecoderStream<Traits>` + `FFmpegVideoDecoder` / `FFmpegAudioDecoder` + `DecoderSelector` | 完成 |
| ✅ M6 | `AvSyncController`（seqlock）· `VideoFrameCompositor` · `Clock` | 完成（第九轮移入 `media/filters/legacy/`） |
| ⬜ M6 余项 | `DisplayGeometry`（letterbox / SAR / DPI，docs/09 §4.4） | ⬜ M12 需要 |
| ⬜ **M7** | `VideoRendererImpl` · `AudioRendererImpl` · `RendererImpl` · `TextRenderer` · `media/audio/*` · WSOLA | ⬜ **下一个关键路径**；接口头第九轮已 DRAFT 冻结 |
| ⬜ M8 | `pipeline_impl` · `player_impl` · `state_machine` · `event_hub` · `seek_controller` · `buffer_controller` · `diagnostics` | ⬜ SDK 的 12 个头已冻结；**`media/base/` 的 7 个管线接口头第九轮已 DRAFT 冻结**；`player.cc` 的 10 个方法仍返回 `kNotImplemented` |
| ⬜ M8 余项 | `tools/gen_options.py` + `option_registry.inc` | ⬜ **`OptionRegistry` 现仅 8 个 key / 规划 60+**，直接卡住验收标准 A10 与 docs/10 Level 3 迁移路径 |
| ⬜ M9 | 三级 HWM `BufferController` · 精确 seek · `RetryDataSource` · `LiveDataSource` · `UrlRewriteInterceptor` | ⬜ |
| ⬜ M10 | `platform/null` · `tools/ijkplayer-recorder` · 15 份 golden JSONL · `golden_record/diff.py` · `tolerance.yaml` · `synthetic_demuxer` | ⬜ **被开放问题 Q8（golden 基线锁哪个 ijkplayer 版本）卡住** |
| ⬜ **M11** | `platform/sdl2`（5 文件）· `FindLinuxMediaDeps.cmake` · `examples/play_sdl2` · `tools/verify_e2e.py` · CI `e2e-linux` | ⬜ **Linux 首次出画**；`platform/CMakeLists.txt` 里该开关目前仍是 `FATAL_ERROR` |
| ⬜ **M12** | `platform/linux/`：`gl/` 8 文件 · `window/` 4 文件（含 Wayland 协议代码生成）· `audio/` ALSA+Pulse · `zero_copy/` dmabuf · dlopen loader（11 个库）· `play_native` + `play_embed` | ⬜ 单项最大（~3800 行 / 3 周） |
| ⬜ M13 | Fuzz ×5（24h）· Stress ×11（含 10 小时 long_play）· Bench ×16 + 趋势守护 · 覆盖率门禁 · 9 份 SDK 文档 · cpack/vcpkg/conan · V1–V10 · 28 项质量门禁 → **发布 0.1.0** | ⬜ 另需补上 R2 降级债（docs/08 说"M13 后补齐"，但 M13 checklist 里没有这一项） |
| ⏳ M14–M18 | VAAPI + 完整 HDR · C ABI · Android · iOS · `ijkio` 缓存 | 后置增量（共 11 周） |
| 🚫 不做 | DRM/CDM · libass 排版 · `ijkavformat` patch 族 · `SDL_VoutOverlay` · SoundTouch · Abseil/spdlog/fmt · Mojo/blink/cc/viz · Java/OC UI · Windows/macOS 出画 | docs/08 §4，净减 ~9000 行 |

**工具与门禁现状**（第九轮实测）：

| 项 | 状态 |
|---|---|
| `tools/check_invariants.py` | ✅ 14 条规则，173 文件全通过；本轮更新 `LINE_LIMIT_ALLOWLIST` 的 legacy 路径 |
| `tools/extract_constants.py` | ✅ **本轮新增**，26 个条目；`--selftest` 两方校验 24 项一致 + 1 项 Δ4 声明偏离 + 1 项待 M9；`--ijkplayer` 三方校验已用合成 ffplay 源码树验证（含 0.04→0.4 手抄错、宏缺失、fork 差异三种失败模式） |
| `tools/inspect/`（`avbase-inspect`） | ✅ probe / decode / sync；⬜ doctor / play / dump / golden |
| `tools/gen_options.py` · `golden_record.py` · `golden_diff.py` · `verify_e2e.py` · `build_linux.sh` · `ijkplayer-recorder/` | ⬜ 全部未建 |
| CI | ✅ quick + full 矩阵（本轮已把 `extract_constants --selftest` 加进两个 job）；⬜ `ffmpeg-matrix` 与 `e2e-linux` 仍是 `if: false`（前者注释写 "enabled at M4"，**M4 早已完成**）；⬜ coverage job 是 `lcov --summary \|\| true`，不会 fail；⬜ `check-format` / `check-cpplint` / `check-no-vendor-leak` |
| 许可证 | ✅ **本轮落地**：根 `LICENSE`（BSD-3 + 4 节第三方说明）· `media/filters/legacy/`（LGPL-2.1 全文 + README 准入规则 + 6 个文件头重写）；⬜ **法务确认（Q1/R8）仍未做** |
| 仓库卫生 | ⬜ `.clang-tidy` · `.editorconfig` 仍缺（STYLE.md 与 README §8 都列了） |

