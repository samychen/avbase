# 代码审查报告 — avbase

- 日期：2026-10-10
- 范围：全仓 C++ 源码 364 文件 / 58,210 行（`base/` `media/` `player/` `platform/` `tools/` `examples/`）
- 方法：分模块通读 + 逐条核对到 `file:line`；关键结论用编译/链接实测复现
- 原则：只报**真实缺陷**，不报风格；每条区分「已验证」与「存疑」

---

## 摘要

基础库与 FFmpeg 封装层的工程质量在同类项目里属上乘：RAII 封装、错误路径清理、`TaskQueue`
唤醒协议、refcount 内存序、以及一整套 CI 门禁（C1/C4/C5/C16/C25/C26/C27）都经得起推敲，
注释里记录的"已修 bug"确实修对了。**真正的问题不在算法，而在三处系统性缺口**：

1. **跨序列回调的生命周期**：`PipelineImpl` 的 hop 已系统改用 `weak_factory_`，但**漏了一处**
   （`pipeline_track_select.cc` 仍用 `Unretained`）；上层 `player/` 与 `event_hub` 则完全没有
   weak 概念，公开 API 承诺的"任意线程调用"因此在多线程下可产生 UAF。
2. **`base/types/expected.h` 的存储实现**：union 默认构造 + 构造体二次 placement-new，导致
   每个 `expected` 漏一个 `T` 且强制 `T` 可默认构造；`value()` 又未按注释终止。
3. **离线转码的时间基**：编码器 `time_base` 与送入帧的 `pts` 单位不一致（一个 1/1、一个微秒），
   重编码视频时间轴被放大 ~10⁶ 倍；现有测试恰好不断言重编码时间戳，属"测试盲区里的确定缺陷"。

`bind.h` 把按值绑定的实参当右值转发、导致第二次调用拿到被移空的值，已用独立程序**实测复现**。

---

## Critical（必须修）

| # | file:line | 问题 | 证据/影响 | 建议 |
|---|---|---|---|---|
| C1 | `media/filters/pipeline_track_select.cc:39-42` | 跨序列 `PostTask` 绑定 `base::Unretained(this)` | `PipelineImpl` 头 `pipeline_impl.h:236-243` 明确写了"调用方可能在 API 返回瞬间销毁 Player，post 已在途"；兄弟 hop（`pipeline_impl.cc:296/324/327/368`）**全部**用 `weak_factory_`，此处漏改 | 改绑 `weak_factory_.GetWeakPtr()` |
| C2 | `player/public/player.h:130` vs `player/player.cc:212` | `Subscription` 注释声明"RAII，析构即注销"，但 `~Subscription() = default`，只有显式 `Reset()` 才 `RemoveObserver` | 析构不注销 → `PlayerObserver*` 留在 `EventHub`，后续事件派发到已销毁对象 | 析构调用 `Reset()`；补测试 |
| C3 | `player/event_hub.cc:97-99` | 在锁内快照裸指针、**释放锁后**逐个回调 | 观察者 A 的回调销毁观察者 B（或另一线程 `RemoveObserver`+析构 B）→ UAF | 用弱句柄；或"回调进行中"计数，`RemoveObserver` 等待 drain |
| C4 | `player/player_impl_events.cc:246-256` | `GetMediaTime/GetBufferedTime/GetDuration` 无锁解引用 `pipeline_`，而 `Reset()`（`player_impl_stop.cc:93`）在任意线程 `pipeline_.reset()` | 违反 `player.h:42`"任意线程可调"与 `:107`"queries 全线程安全"的成文契约 | 用 `state_lock_` 保护，或改持 `shared_ptr` 取本地副本 |

---

> **修复状态（2026-10-10，第四十二轮）**：C1–C4 已全部修复——C1 换绑 weak 句柄；C2
> `~Subscription()` 调 `Reset()`；C3 观察者表改 `shared_ptr` + `alive` + `RemoveObserver`
> 等待在途 Dispatch 排空（新增 `event_hub_unittest.cc` 4 用例，均经负向测试证明承重）；
> C4 `pipeline_`/`renderer_factory_` 改 `shared_ptr` + 快照访问器（~25 处使用点）。修复
> 过程中被新测试逼出一个**既有潜伏链接缺陷**（no-ffmpeg 下 `WriteJpegSnapshot` 未定义），
> 一并以 `AVBASE_ENABLE_FFMPEG` 条件编译修复。High 各项待后续轮次消化。

> **修复状态（2026-10-10，第四十三轮）**：H1–H7 已全部修复，每条均配承重测试并经
> 负向测试（回滚修复必红）验证——H1 `Clock` 暂停冻结 + `SetSpeed` 按旧速率外推重锚
> （新增 `clock_unittest.cc` 6 用例，旧代码下 4 个必红）；H2 音频致命解码错误经新增
> `set_error_cb` 上报 `RendererImpl::ReportError`（初始与轨切换两处接线，+1 用例）；
> H3 在飞 seek 期间的新目标改为**排队**（最新意图胜出）并在完成后启动，两个完成回调
> 均触发（`MidFlightSeekIsNotDropped`：旧代码下落在第一目标、负向红）；H4 `BindState`
> 增 `kIsOnce` 策略——Repeating 回调的存储值按左值传递（+2 用例：二次 Run 完整性 +
> move 计数为零，旧代码下均红）；H5 union 空构造（`expected<NonDefaultConstructible,E>`
> 可编译、构造析构平衡，旧代码下编译失败 + 计数失衡双承重）；H6 `value()`/`error()`
> 三个 ref 限定重载各加 `CHECK`；H7 编码器 `time_base = {fps_den, fps_num}` + pts
> `av_rescale_q` 微秒→刻度，`AvFrameToVideoFrame` 按调用方传入的 `pkt_timebase` 换算
> （不再硬编码 /90000；+4 用例，旧代码下 3 个必红）。

---

## High（应修）

| # | file:line | 问题 | 证据/影响 | 建议 |
|---|---|---|---|---|
| H1 | `media/filters/legacy/clock.cc:127`（及 `:93`） | 暂停不冻结时钟 | `rate = s.speed > 0.0f ? speed : 1.0`，`speed==0` 被当 1.0 → `Get()` 在暂停期间继续推进，与 `renderer_impl_controls.cc:30-33`"speed 0 保持锚点"的注释相反；`SetSpeed` 重锚用 `elapsed * 1` 而非 `before.speed` → 恢复时按暂停时长前跳 | `Get()` 直接用 `s.speed`；重锚用旧速率或直接 `Get()` |
| H2 | `media/filters/audio_renderer_impl.cc:330-340` | 音频致命解码错误被吞 | 非 `kDecodingAborted`、非 EOS 的 `!status.is_ok()` 只 `LOG(ERROR) << "...reporting to the pipeline"` 后 `return`，既不 `client_->OnError` 也不置 `ended_` → 音频泵停摆、UI 无限缓冲、日志与行为自相矛盾 | 走 `RendererImpl::ReportError(...)` 并终止该腿 |
| H3 | `media/filters/pipeline_impl.cc:306-313,353` | 并发 seek 被静默丢弃 | `seek_in_flight_` 期间再次 `Seek()` 立即回掉回调并置 `pending_seek_superseded_=true`，而该标志除 `:353` 置回 false 外**从未被读**，新目标既未保存也未重启；注释"this one starts the moment it finishes"不成立 | 保存 pending（time, cb）并在 `FinishSeekIfBothDone` 末尾启动；或明确返回 `kInvalidState` |
| H4 | `base/functional/bind.h:148-151` | 按值绑定的实参被当右值转发，第二次调用拿到被移空的值 | **实测复现**：`BindRepeating(&Take, std::string("hello"))` 连调两次，第二次 `Take` 收到空串（探针 exit=1）。根因：`operator()` 里 `std::forward<B>(bound_args)` 对值类型产出右值，`UnwrapArg(T&&)` 原样转发 | generic `UnwrapArg` 对存储值返回左值引用；仅 `Unwrap*`/`WeakPtr` 用转移重载 |
| H5 | `base/types/expected.h:173-174` + `:80-83` | union 双重构造 + 泄漏 + 强制默认构造 | `union Storage { constexpr Storage() : value() {} }` 已构造 `T`；构造函数体又 `new (&storage_.value) T(...)`；`Destroy()` 只析构一个 → 每个 `expected` 漏一个 `T`，且 `T` 必须可默认构造（`expected<NonDefaultConstructible,E>` 无法编译） | 去掉 `Storage() : value()`，union 成员由外层手动 placement-new 管理 |
| H6 | `base/types/expected.h:150-153` | `value()` 未按注释终止 | 注释写"Terminates on error"，实现直接返回 inactive union 成员 → 读垃圾/UB | 加 `CHECK(has_value_)`；三个 ref 限定重载都要 |
| H7 | `media/transcode/ffmpeg_video_encoder.cc:102` + `:216`，`media/transcode/ffmpeg_transcode_streams.cc:81-87` | 转码视频时间基与 pts 单位不一致 | `time_base = {1, fps_den}`（30fps → `{1,1}`，即 1 秒/刻度）但 `pts = InMicroseconds()`（33333）→ 等价每帧间隔 33333 秒；`time_base()`（`:171-176`）又作为 muxer 的 `src_tb`（`ffmpeg_transcode_streams.cc:370-372`），`av_packet_rescale_ts` 无法纠正单位。另 `AvFrameToVideoFrame` 硬编码 `/90000`，忽略 `pkt_timebase=stream->time_base`（`:47`） | `time_base` 应为 `{fps_den, fps_num}` 且 `pts` 用帧序号；`AvFrameToVideoFrame` 用 `av_rescale_q(pts, pkt_timebase, {1,1000})`。注：转码线当前**零生产调用方**，属潜伏缺陷 |

---

## Medium（建议修）

> **修复状态（2026-10-10，第四十四轮）**：M1–M15 全部修复（M2 已随第四十二轮 C3 的
> 单调 `next_observer_id_` 顺带解决）。要点：M1 用除法预检消除溢出 UB 并新增
> `SaturatingSub`；M3 状态机补 `kIdle/kInitialized→kStopping` 边、事件用真实前态；
> M4 `Reset()` 以 `kAborted` 回调在途 seek 并清表；M7/M11 补错误分支释放；M8 EAGAIN
> 排空重发；M9 按实际声道掩码映射；M10 六处分配判空；M14 drain 复用输出路径；
> M15 `buffersrc` 改 `KEEP_REF` + pts 换算；M5/M6 SDL 目标刷新与子系统配对。

| # | file:line | 问题 | 建议 |
|---|---|---|---|
| M1 | `base/time/time.h:84`、`:163`、`:70` | 有符号溢出 UB：`operator*`/`SaturatingMul` 先算 `a*b` 再校验（UBSan 可命中）；`operator-=` 对 `INT64_MIN` 取负 | 先除后乘判阈值，或用 `__builtin_mul_overflow`；`-=` 改为 `*this += -other` |
| M2 | `player/event_hub.cc:30` | observer id = `size()+1`，删除后复用 → `RemoveObserver` 可能删错/删不掉 | 单调递增计数器 |
| M3 | `player/player_impl_stop.cc:32-47` | 从 `kIdle`/`kInitialized` 调 `Stop()` 时状态机拒绝 `kStopping`/`kStopped`（`state_machine.cc:19-22`），机器停在原态，却发出 `from=kStopped,to=kStopped` 事件 | 走合法表或 `ForceReset`；事件 `from` 用真实前态 |
| M4 | `player/player_impl_stop.cc:91-105` | `Reset()` 未清 `pending_seeks_`/`accurate_seek_targets_`（`player_impl.h:191/196` 确认存在），也未重置 `accurate_seek_`/`buffer_controller_`/`retry_source_` | Reset 中以 `kAborted` 回调在途 seek 并清表 |
| M5 | `platform/sdl2/sdl2_video_sink.cc:110-130` | `SetOutputTarget` 更新了 `display_`/`gl_context_`/`overlay_slot_` 但**未更新 `renderer_`**（仅在 `Start():64` 赋值）→ 切到无 GL 的 surface 后仍用旧 `SDL_Renderer` | 同步刷新 `renderer_`，或在 Present 时从 surface 重读 |
| M6 | `platform/sdl2/sdl2_audio_sink.cc:42-44` | `SDL_InitSubSystem(SDL_INIT_AUDIO)` 忽略返回值，且全仓**无配对** `SDL_QuitSubSystem`（`git grep` 确认只有这一处 Init） | 记录是否本次 init，`Stop`/析构时 Quit；检查返回值 |
| M7 | `media/ffmpeg/ffmpeg_hw_video_decoder.cc:337-341,361-363` | `held_for_release = av_frame_clone(frame)` 在两条错误分支泄漏（`!readback_frame`、`WrapNativeBuffer` 返回 null 时 `BindOnce` 闭包析构而未运行） | 错误返回前 `av_frame_free(&held_for_release)`；或先用 `BufferPtr` 接管 |
| M8 | `media/transcode/ffmpeg_transcode_job.cc:236,282` | `avcodec_send_packet(...)<0` 把 `AVERROR(EAGAIN)` 当致命直接丢包 | 显式区分 `EAGAIN`：先 `receive_frame` 排空再重发（对照 `ffmpeg_video_decoder.cc:355` 的正确写法） |
| M9 | `media/ffmpeg/ffmpeg_audio_decoder.cc:153-156` | 声道数非 1/2（如 5.1）时 `channel_layout_` 被强制成 `kStereo`，与 `channels_` 不符；而 `ChannelLayout` 枚举**本就有** `k5_1`/`k7_1`（`audio_parameters.h:27-39`） | 按 `av_channel_layout` 实际掩码映射 |
| M10 | `media/transcode/ffmpeg_transcode_job.cc:191-192`、`ffmpeg_video_encoder.cc:184`、`ffmpeg_encode_muxer.cc:168`、`ffmpeg_audio_encoder.cc:89,117`、`ffmpeg_image_snapshot.cc:60` | 一组 `av_packet_alloc`/`av_frame_alloc`/`av_mallocz` 未判空即解引用（OOM → 空指针写） | 统一"分配即判空，失败返回 `kOutOfMemory`" |
| M11 | `media/ffmpeg/data_source_io.cc:58-67` | `avio_alloc_context` 失败时 `io_buffer_`（`av_malloc`）未 `av_free`，泄漏 32KB | `if (!pb_) { av_freep(&io_buffer_); ... }` |
| M12 | `media/filters/pipeline_impl_host.cc:165,168` | live-chase 路径给 `Flush`/`StartPlayingFrom` 绑 `Unretained(this)`，与 `pipeline_impl.cc:323-327` 的 weak 写法不一致（同类问题，存疑：取决于回调线程与 FIFO） | 统一改 `weak_factory_.GetWeakPtr()` |
| M13 | `base/functional/callback.h:165` | `RepeatingCallback::Run() &&` 把 `shared_ptr` 赋给 `unique_ptr` → 一旦实例化即编译失败（当前无人调用，潜伏） | 删除该 `&&` 重载或改 `auto invoker = std::move(invoker_);` |
| M14 | `media/ffmpeg/ffmpeg_audio_filter.cc:205-213` | EOS drain 拉出的帧只 `av_frame_free`，未转成 `AudioBuffer` 输出（非 EOS 分支 `:246-280` 会输出）→ 带延迟的滤镜图丢尾音 | drain 分支复用非 EOS 分支的输出逻辑 |
| M15 | `media/ffmpeg/ffmpeg_video_filter.cc:148-160` | 以 `flags=0` 把**非引用计数**帧（`buf[]` 空、`data` 借用 `VideoFrame`）推入 buffersrc；且 `pts` 用微秒而 abuffer `time_base=1/90000` | 加 `AV_BUFFERSRC_FLAG_KEEP_REF` 并保证输入帧存活；pts 用 `av_rescale_q(..., {1,90000})` |

---

## Low（可选）

> **修复状态（2026-10-10，第四十四轮）**：L1–L9 全部处理——L1 补 `<new>`；L2 零乘
> 短路；L3 `HasObserver` 加 `alive`；L4 失效后重武装；L5 文档化（保留 manual 默认，
> 与 Chromium 一致）；L6 示例改 `StopSync()`；L7 槽位提交序号保证 FIFO（负向测试
> 承重）；L8 EOS 幂等；L9 `strtoull` 校验。

| # | file:line | 问题 | 建议 |
|---|---|---|---|
| L1 | `base/types/expected.h:114,166` | 用 placement new 却不自包含 `#include <new>`（实例化时报 `no matching operator new`） | 头部补 include |
| L2 | `base/time/time.h:80-83` | `Zero() * INT64_MAX` 走 `is_infinte()/a==MAX` 分支返回 `Max()`（应为 0） | 该分支先特判 `micros_==0` |
| L3 | `base/observer_list.h:59-62` | `HasObserver` 不看 `alive`，与 `size()/empty()` 语义不一致；迭代期延迟移除后 `AddObserver` 的 `CHECK(!HasObserver())` 会误报 | 谓词加 `e.alive &&` |
| L4 | `base/memory/weak_ptr.h:151-158` | `InvalidateWeakPtrs()` 后 `GetWeakPtr()` 返回永久失效（Chromium 会重建新 flag） | 若业务要调用需对齐语义 |
| L5 | `base/synchronization/waitable_event.h:26` | 默认 `kManualReset`（Chromium 是 `kAutoReset`），现有调用方恰好正确，属易踩陷阱 | 文档化或改默认 |
| L6 | `player/player_impl_stop.cc:91-105` | 与 M4 同源；`Stop()` 非阻塞后调用方若立即拆资源会 UAF（示例 `examples/play_sdl2.cc:369-380` 即如此） | 用 `StopSync()` |
| L7 | `media/base/video_frame_queue.cc:173-181` | `Reserve` 取最低空闲槽、`Pop` 取最低已填槽 → 多帧在途时非 FIFO（该队列当前未被生产代码实例化） | 维护提交序号/环形头指针 |
| L8 | `media/base/decoder_buffer_queue.cc:235-240` | `MarkEndOfStream()` 不检查 `closed_`/`aborted()`，重复调用压入多个 EOS 标记 | 加幂等保护 |
| L9 | `tools/inspect/inspect_common.cc:73` | `--limit` 用 `atol` 无校验，解析失败静默为 0，负值经 `size_t` 回绕成巨值 | 用 `strtoull` 并校验 errno/范围 |

---

## 安全发现

本仓是原生媒体库，**经典 Web 攻击面无适用项**，扫描结果如下：

| 检查项 | 结果 |
|---|---|
| 硬编码密钥/令牌（`git grep` 全仓 + 过滤测试/占位符） | 未发现 |
| 外部命令执行（`system`/`popen`/`exec*`） | 无 |
| 无界 C 字符串函数（`strcpy`/`strcat`/`sprintf`/`gets`） | 无 |
| 注入 / XSS / CSRF / 反序列化 | 不适用（无网络服务、无脚本执行、无用户 HTML 渲染） |

**实际安全相关问题集中在内存安全**，已归入上表：

- **整数溢出 UB**（M1）— `base/time/time.h`，可被编译器优化掉溢出检测；这是本仓唯一"可由输入到达"的 UB（超长时间戳/倍速相乘）。
- **OOM 空指针解引用**（M10）— 一组 FFmpeg 分配未判空。
- **UAF 类**（C1–C4、M5）— 全部源于跨线程生命周期，不是输入可控，而是时序可控，多线程下可稳定触发。

---

## 性能说明

整体无明显热点问题，以下几处是**可量化**的：

1. `player/player_impl_events.cc:246-256`：`GetMediaTime/GetBufferedTime/GetDuration` 是查询热路径，当前每次调用都做一次 `pipeline_` 判空 + 转发；若改为加锁，注意不要变成高频锁竞争（建议 `shared_ptr` 本地副本而非全局锁）。
2. `platform/sdl2/sdl2_video_sink.cc:164-179`：硬解帧走 `ToI420()` 每次 present 一次全帧读回。注释已承认这是过渡方案（零拷贝 NV12 纹理导入为终态），当前是"正确但有成本"，不是缺陷。
3. `media/ffmpeg/ffmpeg_encoder*`：编码路径 `av_packet_alloc`/`memcpy` 每包一次，属必要开销。

---

## 做得好的地方

- **FFmpeg RAII 封装**：`FormatCtxPtr`/`CodecCtxPtr`/`FramePtr`/`PacketPtr` 的 deleter 正确；`avio_context_free` vs `avformat_close_input` 的归属区分、`AVFMT_FLAG_CUSTOM_IO` 的用法都对（`data_source_io.cc:54-71`）。
- **`base::TaskQueue` 唤醒协议**正确（`quit_` 持锁置位+广播，无 lost-wakeup）。
- **流水线的 weak 化是系统性的**（`pipeline_impl.cc` 的 hop 全部弱引用）——C1 正是"系统性里漏了一处"的证据，价值高于孤立缺陷。
- **注释即事故档案**：`clock.cc:135-142`（Bug #32）、`player_impl_stop.cc:56-73`（StopSync 竞态）等把"为什么这么写"记录得可追溯，这在同类项目里罕见。
- **门禁与负向测试纪律**：C1/C4/C5/C16/C25/C26/C27 + "写完必须种违反验证它会响"，是把架构约束固化成可执行检查的正确做法。

---

## 建议的修复顺序

1. **第一批（内存安全，收益最高）**：C1、C3、C4、C2 → 统一"跨序列 hop 一律 weak、观察者注销一律 RAII 且回调安全"。
2. **第二批（基础库确定性缺陷）**：H4、H5、H6、M1 → `bind` + `expected` + `time` 三处，配套补单测（`bind` 的探针可直接做成用例）。
3. **第三批（功能正确性）**：H1、H2、H3、M3、M4 → 时钟与 seek/stop 状态机。
4. **第四批（转码线，可与 Phase 5 入口一起做）**：H7、M8、M10 → 时间基 + EAGAIN + 判空。
5. 其余 Low 项可按触碰频率顺带收敛。
