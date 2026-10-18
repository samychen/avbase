# MIGRATION · 从 ijkplayer 迁移到 avbase

> 覆盖 [05-迁移对照表](05-迁移对照表.md) 表 10 的**全部 22 条 Δ**（docs/10 §12
> 的发版验收项「MIGRATION.md 覆盖 05 文档的全部 Δ 项」）。每条 Δ 分三栏：
> **设计说法**（05 文档怎么写）、**0.1.0 实况**（2026-10-18 回到代码里逐条核过，
> 附核验位置）、**迁移动作**（你要改什么）。三者不一致时以实况栏为准——设计文档
> 记的是意图，代码记的是事实。
>
> 适用范围：0.1.0 的发版面是 **Linux/macOS 桌面 + C++ SDK**（+ M15 的 C ABI）。
> Android/iOS 绑定（05 表 9）不在 0.1.0，见文末「未实现项」。

---

## 1. 五分钟迁移路径

ijkplayer 的典型调用序列与 avbase 的对应（完整对照见 §2）：

```cpp
// ijkplayer:                                   // avbase:
ijkmp_create(&clazz);                        avbase::Player player;  // 或 PlayerBuilder
ijkmp_set_data_source(mp, url);              player.SetDataSource(url);
ijkmp_set_option(mp, IJKMP_OPT_CATEGORY_PLAYER, "max-fps", 30);
                                                // 三条路任选：
                                                //   PlayerBuilder().SetOption(kPlayer, "max-fps", 30)
                                                //   player.UpdateConfig(config)  // 白名单字段
                                                //   直接写 PlayerConfig 字段（推荐，强类型）
ijkmp_set_log_callback(mp, cb);              avbase::base::logging::SetDelegate(...);
ijkmp_prepare_async(mp);                     player.PrepareAsync();
ijkmp_start(mp);                             player.Start();  // 默认 start_on_prepared=true 时这行是多余的
ijkmp_get_msg(mp, &msg, ...);                player.SetEventHandler(...) 或 AddObserver(o)
ijkmp_seek_to(mp, msec);                     player.SeekTo(pos, SeekMode::kKeyframe, cb);
ijkmp_stop(mp);                              player.StopSync(base::Seconds(5));  // 见 Δ1
ijkmp_shutdown(mp);                          ~Player();  // RAII，见 Δ15
```

三条最容易踩的：

1. **`PrepareSync()` 里的自动起播**：`start_on_prepared` 默认 `true`， autostart
   在 `PrepareSync()` 返回**之前**跑完，你紧接着写的 `Start()` 反而是非法的
   （`kPrepared` 之后没有合法路径再 Start 一次——它会打日志并原样返回）。
   要么不写这个 Start，要么 `start_on_prepared=false` 自己控制。
   （player.h 的 WATCH OUT 注释；examples/pull_frames.cc 有范例。）
2. **析构前用 `StopSync(timeout)` 而不是 `Stop()`**：`Stop()` 异步（Δ1），
   紧接着析构会竞态。
3. **事件回调里可以随便调 Player 的任何方法**（含 `Stop()`/`Reset()`）——
   派发时不持锁（docs/05 表 5 的派发保证 4）。ijkplayer 在这里会死锁。

---

## 2. API 对照（`ijkmp_*` → `Player::*`）

摘自 05 表 2，**右列按 `player/public/player.h` 的真实签名校对**：

| ijkplayer C API | avbase C++ API | 备注 |
|---|---|---|
| `ijkmp_create(clazz)` | `Player player;` / `PlayerBuilder().Build()` | 无全局 class |
| `ijkmp_inc_ref`/`dec_ref` | RAII / `std::unique_ptr<Player>` | — |
| `ijkmp_shutdown(mp)` | `~Player()` | 有界，见 Δ15 |
| `ijkmp_set_data_source(mp, url)` | `Status SetDataSource(std::string_view)` / `SetDataSource(const DataSourceDescriptor&)` | 返回 `Status` |
| `ijkmp_prepare_async(mp)` | `Status PrepareAsync()` | — |
| — | `Status PrepareSync(TimeDelta timeout)` | 🆕 阻塞版，默认 30s |
| `ijkmp_start(mp)` | `void Start()` | 状态机拒绝非法迁移（日志+原样返回） |
| `ijkmp_pause(mp)` | `void Pause()` | — |
| `ijkmp_stop(mp)` | `void Stop()` | 🔧 非阻塞（Δ1） |
| — | `void StopSync(TimeDelta)` | 🆕 阻塞版 |
| `ijkmp_reset(mp)` | `void Reset()` | 回 `kIdle`，可重用 |
| — | `void RunUntilIdle()` | 🆕 CLI/测试用 |
| `ijkmp_seek_to(mp, msec)` | `Result<int64_t> SeekTo(TimeDelta, SeekMode, SeekCB)` | 强类型+回调+mode；返回值是 request id |
| `ijkmp_get_state(mp)` | `PlayerState state()` | — |
| `ijkmp_get_msg(mp,...)` | `SetEventHandler(RepeatingCallback)` / `AddObserver(PlayerObserver*)` | 轮询→回调/观察者 |
| `ijkmp_get_current_position(mp)` | `TimeDelta GetMediaTime()` | 无锁快照 |
| `ijkmp_get_duration(mp)` | `TimeDelta GetDuration()` + `bool is_live()` | Δ7 |
| `ijkmp_get_playable_duration(mp)` | `TimeDelta GetBufferedTime()` | — |
| `ijkmp_set_loop`/`get_loop` | `void SetLoopCount(int)`（-1 无限）+ `config.loop_count` | — |
| `ijkmp_get_video_width/height` | `media::Size video_natural_size()` / `video_coded_size()` | natural 含 SAR |
| `ijkmp_is_playing(mp)` | `bool IsPlaying()` | — |
| `ijkmp_set_volume`/`get_volume` | `void SetVolume(double)` / `double volume()` | 🔧 0..1（Δ6） |
| `ijkmp_set_rate`/`get_rate` | `void SetPlaybackRate(double)` / `playback_rate()` | [0.25, 4.0] |
| — | `void SetMuted(bool)` / `muted()` | 🆕 |
| — | `void StepOnce()` | 🆕 暂停时逐帧 |
| `ijkmp_set_option(...)` | `PlayerBuilder::SetOption(OptionCategory, key, OptionValue)` | 未知 key 报错（Δ2） |
| `ijkmp_get_property_int64/float` | `PlaybackStats GetPlaybackStats()` | 20+ 次调用 → 1 次 |
| `ijkmp_set_property_int64(mp,id,v)` | 语义明确的 setter（如 `ReconnectNow()`） | 一个函数拆多个 |
| `ijkmp_get_meta_l(mp)` | `std::optional<MediaInfo> media_info()` | — |
| `ijkmp_set_stream_selected(mp,type,idx)` | `Status SelectTrack(DemuxerStreamType, int stream_index)` | 轨切换 |
| `ijkmp_set_log_callback(mp,cb)` | `base::logging::SetDelegate(std::unique_ptr<LoggingDelegate>)` | 注入（Δ22） |
| `ijkmp_set_gl_context(mp,ctx)` | `NativeDisplay::Wrap<T>(raw, kind, release)` | 类型安全 |
| `ijkmp_android_set_surface(...)` | `void SetVideoSurface(scoped_refptr<NativeDisplay>)` | Android 侧 M16 后置 |
| `ijkmp_global_init`/`uninit` | `GlobalInit()`（幂等，构造器自动调）/ `GlobalGuard` | — |
| `ijkmp_version()` | `GetVersion()` / `GetFFmpegVersion()` | — |
| `FFP_PROP_INT64_IMMEDIATE_RECONNECT` | `void ReconnectNow()` | 写属性→方法 |
| `FFP_MSG_GET_IMG_STATE` | `Status TakeSnapshot(TimeDelta at, std::string path)` | 🆕 原版只有回调没有 API |
| — | `Status UpdateConfig(const PlayerConfig&)` | 🆕 运行时改白名单字段 |
| — | `std::string DumpDiagnostics()` | 🆕 JSON 全量状态，~1ms |
| — | `Scene* scene()` / `AddSource(...)` | 🆕 多图层/多源合成（docs/14） |

ijkplayer 的 `ffp_*`（约 60 个）全部内化为私有实现，不再对外暴露。

---

## 3. 选项对照（`ff_ffplay_options.h` → `PlayerConfig`）

字符串 key 与原版一致（`OptionCategory::kPlayer` 下），可直接迁移现有
`setOption` 调用点；**未知 key 或越界值从静默忽略变为报错**（Δ2）。完整表见
05 表 4；这里只列迁移时真正会绊人的：

| 原选项 | avbase 字段（key） | 0.1.0 实况 |
|---|---|---|
| `volume` 0..100 | `audio.startup_volume` 0..1 | 已接通（registry 换算） |
| `nodisp` | `render.disable_video_output` | 已接通 |
| `infbuf` | `buffer.unlimited` | 已接通：`max_bytes=0`，包数仍受 256/队列保护 |
| `max-buffer-size` | `buffer.max_bytes` | 已接通，**全播放器预算**按流拆分（见 PERFORMANCE §4） |
| `min-frames` | `buffer.min_frames` | 已接通：预缓冲门控包数下限 |
| `packet-buffering` | `buffer.enabled` | 已接通：三级门控开关 |
| `framedrop` | `video.max_frame_drop` | 已接通 |
| `max-fps` | `video.max_fps` | 已接通；**默认 31**，60fps 源会被降（docs/05 B 类教训） |
| `enable-accurate-seek` | `seek.accurate` | 已接通，作 `kAccurate` 的全局否决位（Δ19） |
| `mediacodec*` / `videotoolbox*` | `video.decoder_preference` / `video.hw_codecs` … | macOS VideoToolbox 可用；Android MediaCodec 随 M16 |
| `ast`/`vst`/`sst` | `*.selected_stream` | 🆕 原版只能 prepare 后设 |

**⚠️ 死选项（10 个，2026-10-18 逐项 grep 复核仍为零读取者）**：可 set/get、
有区间校验，但没有任何代码读它们——设置了不报错也不生效，比不存在更难发现：

`fast`、`rdftspeed`、`sync-av-start`、`video-pictq-size`（`video.frame_queue_size`）、
`no-time-adjust`、`preset-5-1-center-mix-level`、`get-frame-mode`、
`video-mime-type`、`ijkmeta-delay-init`、`render-wait-start`（Δ20 即因此未落地）。

迁移时如果业务依赖其中某个，按「未实现」处理：要么等后续版本，要么走
`UpdateConfig` 白名单之外没有别的入口。核验方法（可复现）：对 registry 路径在
`player/ media/ render/ platform/` 搜**排除** `player_config.h` 与
`option_registry.*` 之外的提及。

---

## 4. 消息对照（`FFP_MSG_*` → `EventType`）

摘自 05 表 5，**右列 payload 字段按 `player/public/player_event.h` 校对**。
派发保证（原版没有）：单线程串行、关键事件不丢、高频事件合并、回调内可重入
Player、`~Player()` 前 Flush 保证收到最后的 `kStateChanged{to=kStopped}`。

| 原常量 | avbase `EventType` | payload（真实字段） |
|---|---|---|
| `FFP_MSG_PREPARED` (200) | `kPrepared` | `PreparedPayload{media_info, stages}` |
| `FFP_MSG_ERROR` (100) | `kError` | `ErrorPayload{error}`（MediaError 三段式） |
| `FFP_MSG_COMPLETED` (300) | `kCompleted` | — |
| `FFP_MSG_VIDEO_SIZE_CHANGED` (400) + `SAR_CHANGED` (401) | `kVideoSizeChanged` | `VideoSizeChangedPayload{coded_size, natural_size, sar}`（Δ8 合并） |
| `FFP_MSG_VIDEO_ROTATION_CHANGED` (404) | `kRotationChanged` | `{degrees, mirrored}` |
| `FFP_MSG_{VIDEO,AUDIO}_RENDERING_START` (402/403) | `kRenderingStarted` | `{type}` |
| `FFP_MSG_{VIDEO,AUDIO}_DECODED_START` (405/406) | `kDecodingStarted` | `{type, video_type, audio_type, codec_name}` |
| `FFP_MSG_OPEN_INPUT` 等 (407–409) | `kStageReached` | `{stage, elapsed_since_prepare}` |
| `FFP_MSG_BUFFERING_START/END` (500/501) | `kBufferingStarted` / `kBufferingEnded` | — |
| `FFP_MSG_BUFFERING_UPDATE` (502) | `kBufferingProgress` | `{buffered_ahead, percent}`（Δ4：实际 1s 一次，见 §5） |
| `FFP_MSG_BUFFERING_{BYTES,TIME}_UPDATE` (503/504) | `kBufferedBytesUpdate` / `kBufferedTimeUpdate` | `BufferedUpdatePayload{cached_time, high_water_mark_time, cached_bytes, high_water_mark_bytes}` |
| `FFP_MSG_SEEK_COMPLETE` (600) | `kSeekCompleted` | `{request_id, requested, actual, load_duration, result}` |
| `FFP_MSG_PLAYBACK_STATE_CHANGED` (700) | `kStateChanged` | `{from, to}` |
| `FFP_MSG_TIMED_TEXT` (800) | `kTimedText` | `{text, pts, duration, x, y, w, h, raw_ass}` |
| `FFP_MSG_ACCURATE_SEEK_COMPLETE` (900) | `kAccurateSeekCompleted` | `{position}` |
| `FFP_MSG_GET_IMG_STATE` (1000) | `kSnapshotCompleted` | `{at, file_path, result}` |
| `FFP_MSG_VIDEO_DECODER_OPEN` (10001) | `kDecoderOpened` | `{type, decoder_name, outputs_opaque_surface}` |
| `FFP_REQ_START/PAUSE/SEEK` (20001–20003) | ❌ 删除，调方法 | Δ9 |
| — | 🆕 `kDecoderFallback` | `{from_decoder, to_decoder, reason}`（Δ12） |
| — | 🆕 `kTrackChanged` | `{type, old_index, new_index}` |
| — | 🆕 `kStats` | `StatsPayload{stats}`（替代轮询 PROP） |
| — | 🆕 `kWaiting` | `{reason}`（kBuffering/kDecoderStalled/kNetworkStalled/kKeyRequired） |
| — | 🆕 `kHdrUnsupported` | `{transfer}` |

**旧消息 handler 不迁移的捷径**：`ToLegacyEvent(event, &out)` 把任意
`PlayerEvent` 降维成 `(what, arg1, arg2, obj)` 四元组，Java/OC 侧
`mEventHandler` 可以原样不动（05 表 9 的同款机制，C ABI 层已在用）。

---

## 5. Δ1–Δ22：故意引入的行为差异（逐条实况）

> 影响列：🔴 必须改代码 ｜ 🟠 建议检查 ｜ 🟡 知悉即可 ｜ 🟢 无感。

### Δ1 `Stop()` 阻塞性 —— 🟠
- 设计：ijkplayer 阻塞到线程退出（可能 ANR）；avbase 立即返回。
- 实况：✅ 如实况。`player.h:94` `void Stop(); // Non-blocking.`；另有
  `StopSync(TimeDelta)` 与「析构前等最后的 `kStateChanged{to=kStopped}`」。
- 动作：把「stop 返回即已停」的假设改成等 `kStateChanged` 或调 `StopSync`。

### Δ2 未知选项从静默忽略变为报错 —— 🟠
- 实况：✅ `option_registry.cc:48-52`：`kInvalidArgument` + 编辑距离
  "did you mean" 提示；越界值同样报错。
- 动作：上线前把全部 `setOption` 调用点过一遍（拼写错误现在会炸出来，这是好事）；
  用 `OptionRegistry::Dump()` 可以导出两份配置做 diff。

### Δ3 `handle_resolution_change` 默认 true —— 🟡
- 实况：✅ `player_config.h:116 bool handle_resolution_change{true}`。
- 动作：无（ijkplayer 默认 0 是会花屏的那个值）。

### Δ4 `BufferingProgress` 间隔 200ms —— 🟡（**实况与设计不符**）
- 设计：1000ms → 200ms，进度条更平滑。
- 实况：⚠️ 0.1.0 **没有 200ms 这个常量**。`kBufferingProgress` 在
  `PlayerImpl::OnStatisticsUpdate()` 里发出，随渲染器统计 tick 走，
  `renderer_impl.h:235 kStatsInterval = base::Seconds(1)`——即**每秒一次**。
  BufferController 本身只有 `Evaluate`/`progress_percent`，无定时器。
- 动作：进度条按 1s 刷新设计；要 200ms 平滑度没有开关（已登记为实现与设计的
  差异，不是配置错误）。

### Δ5 `min-frames` 默认 5 —— 🟢
- 实况：✅ `player_config.h:67 size_t min_frames{5}`，与原版一致。

### Δ6 音量范围 0..1 —— 🟠
- 实况：✅ `startup_volume{1.0}`（double，0..1）；`SetVolume(double)` 注释
  `[0.0, 1.0]`；registry 对旧字符串 key 做 0..100 → 0..1 换算。
- 动作：把 `ijkmp_set_volume(mp, 80)` 改成 `SetVolume(0.8)` 或保持
  `SetOption(kPlayer, "volume", 80)`。

### Δ7 `GetDuration()` 语义 —— 🟢
- 实况：✅ `is_live()` 在 `media/base/media_info.h:62`；直播零时长。
  注意字段名是 `duration_is_estimate`（`media_info.h:77`），05 文档写的
  `duration_estimate` 不存在。
- 动作：判断直播用 `is_live()`，不要测 `duration == 0`。

### Δ8 SAR 与 SIZE 事件合并 —— 🟠
- 实况：✅ `VideoSizeChangedPayload{coded_size, natural_size, sar}`，
  头注释明引 Δ8；原 `FFP_MSG_SAR_CHANGED` 没有对应事件。
- 动作：原来分别处理 400/401 两个消息的接收方，合并成一个分支。

### Δ9 `FFP_REQ_*` 删除 —— 🟡
- 实况：✅ 不存在任何消息触发的控制路径，控制必须走方法（拿得到返回值）。
- 动作：把 `FFP_REQ_START` 等改成直接调 `Start()`/`Pause()`/`SeekTo()`。

### Δ10 精确 seek 超时可观测 —— 🟢
- 实况：✅ `player_impl_accurate_seek.cc:159/175`：超时走
  `MediaError(ErrorCode::kTimeout, "accurate seek timed out", …)`，随
  `SeekCompletedPayload.result` 上报，播放继续。
- 动作：检查 `AsSeekCompleted(e)->result` 而不是假设 seek 必然精确落点。

### Δ11 `ijkurlhook` URL 重写需显式注册 —— 🔴（**0.1.0 未实现**）
- 设计：核心不隐式改 URL，需注册 `UrlRewriteInterceptor` 到 `Deps`。
- 实况：⚠️ **avbase 0.1.0 没有任何 URL 重写组件**——`media/filters/` 下没有
  interceptor 目录，`Deps` 也没有对应字段（`deps.h` 全文可查）。Δ11 描述的
  「显式注册」形态不存在，因为组件本身不在 0.1.0。
- 动作：依赖 `ijkurl:` scheme 的业务，自己实现一个 `DataSource` 包装
  （`deps.data_source`，见 EXTENDING §3），或等后续版本。**这是 0.1.0 最大的一处
  功能缺口，发布说明已单列。**

### Δ12 硬解失败自动降级 —— 🟢
- 实况：✅ `EventType::kDecoderFallback` + `DecoderFallbackPayload`
  （`player_event.h:54/109`）。
- 动作：想感知降级就订阅 `kDecoderFallback`；不订阅则行为等价于「没发生过」。

### Δ13 音频回调线程负载 —— 🟢
- 实况：✅ `audio_renderer_algorithm.h:96` 注释明引 Δ13：`Render()` 只做
  `FillBuffer` + `Scale`，其余（缓冲、时钟、簿记）全在音频回调线程外。
- 动作：无。

### Δ14 时钟读写 seqlock —— 🟢
- 实况：✅ `av_sync_controller.h:190` 注释「Everything else is lock-free
  through Clock's seqlock」，只有音频 diff 滑窗用锁。
- 动作：无（原版的 data race 是 UB，修掉不需要业务配合）。

### Δ15 `~Player()` 有界 —— 🟠
- 实况：✅ `player_config.h:231 shutdown_timeout{Milliseconds(500)}`；超时
  detach + `LOG(ERROR)` + 输出 diagnostics（宁泄漏不卡死）。
- 动作：极端情况下可能有 detached 线程——监控 `DumpDiagnostics()` 与 ERROR 日志，
  不要假设进程内绝无残留线程。

### Δ16 线程名统一前缀 —— 🟢
- 实况：✅ 实测存在：`avbase-event`（event_hub.cc:17）、`avbase-media` /
  `avbase-video` / `avbase-audio`（player_impl.cc:41-43）、`avbase-demux`
  （ffmpeg_demuxer.cc:267，且为 background 优先级）。
- 动作：按线程名过滤日志/抓栈的脚本改用新名前缀。

### Δ17 变速音频算法（SoundTouch → 内置 WSOLA）—— 🟠
- 实况：✅ `media/filters/wsola_internals.cc`（内置，无第三方依赖）；
  `audio.tempo_stretch=false` 可回退简单重采样。
- 动作：音质需 A/B 验证（这是本清单里唯一「听感」相关的差异）；构建不再需要
  单独编译 SoundTouch。

### Δ18 送显调度模型（sink 驱动）—— 🟠
- 实况：✅ `video_renderer_sink.h:57`：
  `Render(deadline_min, deadline_max)`，sink 提供 vsync 窗口；
  `deadline_max` 为空表示无 vsync 信息，退回定时器驱动。
- 动作：自研 sink 必须实现 deadline 版的 `RenderCallback::Render`；
  只喂帧不提供 deadline 的旧写法在 avbase 没有对应物。

### Δ19 seek 精度两段式显式化 —— 🟢
- 实况：✅ `SeekMode::kAccurate` 是 per-call opt-in；`seek.accurate` 默认
  `true` 作全局否决位（`player_config.h:90`），关掉时精确请求降级为关键帧
  落点 + 一次性 WARNING（player_impl.cc:188）。
- 动作：默认播放行为与原版一致（关键帧）；要精确 seek 的地方显式传
  `SeekMode::kAccurate`。

### Δ20 首帧渲染时机语义明确 —— 🔴（**0.1.0 未实现**）
- 设计：`render_wait_start` 明确 prepared 前/后渲染首帧。
- 实况：⚠️ `render.render_wait_start` 是**死选项**（§3 列表内，全树零读取者），
  设计语义未落地。当前首帧时机由 `start_on_prepared` 与 sink 自身行为决定。
- 动作：不要依赖这个选项；需要「prepared 前就出首帧」的宿主，0.1.0 只能实测
  确认现状。

### Δ21 `AVPacket`/`AVFrame` 不暴露 —— 🟢
- 实况：✅ `player/public/*.h` grep `AVPacket|AVFrame` 零命中；帧经
  `StorageType` + `storage_as<T>()` 访问。
- 动作：原来直接摸 `AVFrame` 的代码改用 `VideoFrame` 接口；硬解零拷贝路径
  不需要第二套 overlay 机制。

### Δ22 日志注入而非硬编码 stderr —— 🟠
- 实况：✅ `base::logging::SetDelegate(unique_ptr<LoggingDelegate>)`
  （base/logging.h:51）；FFmpeg 日志经 `media/ffmpeg/log_bridge.cc` 汇入同一
  委托。
- 动作：依赖默认 stderr 的日志采集脚本改接 delegate 或重定向。

---

## 6. 属性对照（`FFP_PROP_*` → `PlaybackStats`）

一次 `GetPlaybackStats()` 拿全部（05 表 6 的字段名已按
`player/public/playback_stats.h` 校对）：`video_decode_fps` /
`video_output_fps` / `playback_rate` / `volume` / `drop_frame_rate` /
`av_delay` / `av_diff` / `selected_{video,audio,text}_stream` /
`video_decoder` / `audio_decoder`（`VideoDecoderType`/`AudioDecoderType` 枚举，
含 `kVideoToolbox`）/ `video{cached_duration,cached_bytes,cached_buffers}` /
`audio{…}` / `bit_rate` / `tcp_speed` / `async{buf_backwards,buf_forwards,
buf_capacity}` / `traffic_byte_count` / `cache{physical_pos,file_forwards,
file_pos,count_bytes,logical_file_size}` / `latest_seek_load_duration` /
`buffering{buffering_count,total_buffering_time,last_buffering_time,
current_hwm_step,active_high_water_mark}` / `frames_presented` /
`frames_dropped` / `frames_dropped_late` / `frames_dropped_fps` /
`frames_repeated` / `frames_dropped_by_sink` / `frames_submit_failures` /
`measured_refresh_hz` / `audio_glitches` / `render`（upload/compose/blit/present
四段） / `stages` + `first_frame_latency` / `heap_bytes_estimate` /
`events_dropped_by_overflow`。

写属性类（原版的 `ijkmp_set_property_int64`）全部改成方法：
`IMMEDIATE_RECONNECT` → `ReconnectNow()`；`SHARE_CACHE_DATA` →
`DataSourceConfig` + `UpdateConfig()`。

---

## 7. 未实现项（迁移前必读）

| 项 | 状态 | 替代 |
|---|---|---|
| Android/iOS binding（05 表 9） | ❌ 0.1.0 无 | C ABI（`player/public/c/avbase_c.h`，M15）是跨语言挂点 |
| `ijkio` 边下边播缓存（M18） | ❌ | 自研 `DataSource`（`deps.data_source`） |
| URL 重写 hook（Δ11） | ❌ | 自研 `DataSource` 包装 |
| `ijkurl:`/`ijksegment` 等 scheme | ❌ | 交给宿主或自研 DataSource |
| SoundTouch 变速 | ❌ 故意 | 内置 WSOLA（Δ17） |
| MediaCodec 硬解（Android） | ❌ M16 | 桌面侧 VideoToolbox 可用 |
| DRM（`CdmContext`） | ❌ 占位 | `video_decoder.h:31` 明记未实现 |
| play_embed 嵌入示例 | ❌ | COOKBOOK 第 1 条承担最小集成职责 |

---

## 8. 迁移验证清单

1. **配置无死键**：`OptionRegistry::GetInstance().Dump(config, kPlayer)` 导出，
   对照 §3 死选项表；对每个曾用选项 grep 一次是否真生效（docs/05 文末的核验
   方法）。
2. **事件无遗漏**：开着 `SetEventHandler` 跑一遍，`kStateChanged` 序列是否
   符合预期；旧 handler 先接 `ToLegacyEvent` 对照 (what, arg1, arg2)。
3. **seek 契约**：`AsSeekCompleted(e)->request_id` 与调用返回值匹配；
   `result.code == ErrorCode::kTimeout` 的路径有处理。
4. **停机有界**：`StopSync` 超时值与 `shutdown_timeout` 匹配；压测里监控
   `DumpDiagnostics()`。
5. **性能对照**：PERFORMANCE.md §3 的 `bench_render.sh` 跑迁移前后两版。

---

## 附：与 05 文档的差异登记（本篇新增）

| # | 05 文档说法 | 0.1.0 实况 | 处理 |
|---|---|---|---|
| 1 | Δ4 进度间隔 200ms | 随 `kStatsInterval`=1s | §5 Δ4 栏 |
| 2 | Δ11 显式注册 UrlRewriteInterceptor | 组件不存在 | §5 Δ11 栏 + §7 |
| 3 | Δ20 `render_wait_start` 语义明确 | 死选项，未落地 | §5 Δ20 栏 + §3 |
| 4 | Δ7 `duration_estimate` | 字段名 `duration_is_estimate` | §5 Δ7 栏 |
| 5 | 表 5 `kDecoderOpened{hw_surface}` | 字段 `outputs_opaque_surface` | §4 表 |
