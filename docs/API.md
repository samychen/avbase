# avbase API 参考

> **本文件由工具生成，勿手改。** 管线：`tools/gen_api_docs.sh` →
> Doxygen（`docs/Doxyfile`，XML 后端，INPUT_FILTER 把 public 头的
> `//` 注释转成 doxygen 语法）→ `tools/doxy_xml_to_md.py`。
> 验收标准：Doxygen 运行**零 warning**（docs/10 §12）。改注释请改
> `player/public/` 下的头文件后重新生成。

## 目录

- player/public/c/avbase_c.h
- player/public/deps.h
- player/public/global.h
- player/public/option_registry.h
- player/public/playback_stats.h
- player/public/player.h
- player/public/player_config.h
- player/public/player_event.h
- player/public/player_export.h
- player/public/scene.h

## player/public/c/avbase_c.h

- `#define AVBASE_OK 0`
  kOk – also the only value for which the other out-params are meaningful.
- `#define AVBASE_ERR_FAILED 1`
  Generic failure. Prefer the specific codes: a host that can tell "file not
  found" from "codec unsupported" can show the user something useful.
- `#define AVBASE_ERR_INVALID_ARGUMENT 2`
- `#define AVBASE_ERR_INVALID_STATE 3`
- `#define AVBASE_ERR_NOT_IMPLEMENTED 4`
- `#define AVBASE_ERR_OUT_OF_MEMORY 5`
- `#define AVBASE_ERR_ABORTED 6`
- `#define AVBASE_ERR_TIMEOUT 7`
- `#define AVBASE_ERR_SOURCE_OPEN_FAILED 8`
- `#define AVBASE_ERR_SOURCE_NOT_FOUND 9`
- `#define AVBASE_ERR_SOURCE_READ_FAILED 10`
- `#define AVBASE_ERR_DECODER_NOT_FOUND 11`
- `#define AVBASE_ERR_DECODER_OPEN_FAILED 12`
- `#define AVBASE_ERR_DECODE_FAILED 13`
- `#define AVBASE_ERR_DECODER_UNSUPPORTED_CODEC 14`
- `#define AVBASE_ERR_SINK_NOT_ATTACHED 15`
- `#define AVBASE_ERR_STREAM_NOT_FOUND 16`
- `#define AVBASE_ERR_TRACK_NOT_FOUND 17`
- `#define AVBASE_ERR_CONFIG_INVALID 18`
- `#define AVBASE_ERR_EOS 19`
- `#define AVBASE_DISPLAY_KIND_NONE 0`
  Mirrors media::NativeDisplayKind. Frozen: append only.
- `#define AVBASE_DISPLAY_KIND_X11_WINDOW 1`
- `#define AVBASE_DISPLAY_KIND_WAYLAND_SURFACE 2`
- `#define AVBASE_DISPLAY_KIND_SDL2_WINDOW 3`
- `#define AVBASE_DISPLAY_KIND_DRM_MASTER 4`
- `#define AVBASE_DISPLAY_KIND_GBM_DEVICE 5`
- `#define AVBASE_DISPLAY_KIND_ANDROID_NATIVE_WINDOW 6`
- `#define AVBASE_DISPLAY_KIND_ANDROID_SURFACE 7`
- `#define AVBASE_DISPLAY_KIND_CAMETAL_LAYER 8`
- `#define AVBASE_DISPLAY_KIND_CAEAGL_LAYER 9`
- `#define AVBASE_DISPLAY_KIND_HWND 10`
- `#define AVBASE_DISPLAY_KIND_GLFW_WINDOW 11`
- `#define AVBASE_DISPLAY_KIND_EGL_WINDOW 12`
- `#define AVBASE_STREAM_UNKNOWN 0`
  Mirrors media::DemuxerStreamType. Frozen: append only.
- `#define AVBASE_STREAM_AUDIO 1`
- `#define AVBASE_STREAM_VIDEO 2`
- `#define AVBASE_STREAM_TEXT 3`
- `#define AVBASE_STATE_IDLE 0`
  Mirrors PlayerState. Frozen: append only.
- `#define AVBASE_STATE_INITIALIZED 1`
- `#define AVBASE_STATE_PREPARING 2`
- `#define AVBASE_STATE_PREPARED 3`
- `#define AVBASE_STATE_STARTED 4`
- `#define AVBASE_STATE_PAUSED 5`
- `#define AVBASE_STATE_BUFFERING 6`
- `#define AVBASE_STATE_STOPPING 7`
- `#define AVBASE_STATE_STOPPED 8`
- `#define AVBASE_STATE_COMPLETED 9`
- `#define AVBASE_STATE_ERROR 10`
- `#define AVBASE_STATE_END 11`
- `using AvbaseStatus = int32_t;`
  Mirrors avbase::ErrorCode as a flat int. The C++ enum is a uint16_t list of 30 values; the numeric values are part of this ABI, so new codes may only be APPENDED here.
- `using AvbasePlayer = struct AvbasePlayer;`
  Opaque. Create with avbase_player_create(); destroy with avbase_player_destroy(), which is bounded by PlayerConfig's shutdown timeout and therefore cannot hang the caller.
- `using AvbaseNativeDisplay = struct AvbaseNativeDisplay;`
  An opaque, host-owned window handle. Mirrors the kinds in media/base/native_display.h; see NativeDisplay::FromX11Window et al.
- `using AvbaseSource = struct AvbaseSource;`
  An opaque handle for a secondary media source registered with avbase_player_add_source(). Stable across Stop/Reset.
- `using AvbaseMediaInfo = struct AvbaseMediaInfo;`
  Fills a caller-allocated buffer with the media's duration, dimensions and track counts. A snapshot, not a live view. |out| may be null only if all pointers passed in are null (a no-op way to probe support).
- `using AvbaseEventCallback = void(*);`
  |obj| is owned by the library and valid only for the duration of the call.
- `const char * avbase_c_version(void)`
  Version of the C ABI itself, not of the engine. Bumped ONLY when an existing symbol changes meaning or disappears; adding symbols is a minor bump. A host compiled against an older header keeps working against a newer library, so this exists to let it decide whether to feature-detect instead. Side effects: none.
- `const char * avbase_engine_version(void)`
  Mirrors avbase::GetVersion() ("0.1.0"). Side effects: none.
- `void avbase_global_init(void)`
  Mirrors avbase::GlobalInit(). Optional: every avbase_player_create() calls it. Call it yourself only to guarantee it happens before your first X11 or FFmpeg call. Side effects: idempotent global init (FFmpeg/X11 once per process).
- `const char * avbase_status_name(AvbaseStatus status)`
  Stable, never-null, never-freed name of a code, e.g. "kSourceNotFound". The name is the enum SPELLING on purpose: it is what a bug report will say, and it stays readable across a mapping to a legacy code in a Java handler. Side effects: none.
- `const char * avbase_status_message(AvbaseStatus status)`
  The engine's own message for |status|: a human sentence naming the likely cause, or "" when it has nothing to add. Never null. Side effects: none.
- `AvbasePlayer * avbase_player_create(void)`
  Creates a player with the default platform backends and an event handler already attached, so no call can race a subscription. Returns null only when the process is out of memory. Side effects: allocates a player (release with avbase_player_destroy).
- `void avbase_player_destroy(AvbasePlayer *player)`
  Destroying a null player is a no-op (not an error) so that cleanup paths can call it unconditionally after a failed create. Side effects: releases every pipeline resource, bounded by the shutdown timeout.
- `void avbase_player_stop_sync(AvbasePlayer *player, int64_t timeout_ms)`
  Bounded by PlayerConfig::shutdown_timeout; does not hang. Blocking Stop: returns once the pipeline has really stopped. Side effects: emits kStateChanged(kStopped).
- `AvbaseStatus avbase_player_set_data_source(AvbasePlayer *player, const char *uri)`
  SetDataSource(uri). |uri| is copied; the caller keeps ownership. Side effects: moves the machine to kInitialized.
- `AvbaseStatus avbase_player_prepare(AvbasePlayer *player)`
  PrepareAsync(): setup starts and this returns immediately; kPrepared (what =200) arrives through the event callback. Side effects: emits kPrepared, or kError on failure.
- `AvbaseStatus avbase_player_prepare_sync(AvbasePlayer *player, int64_t timeout_ms)`
  Blocking PrepareAsync, bounded by |timeout_ms|; a successful return may already be kStarted when start_on_prepared is set. Side effects: emits kPrepared, or kError on failure.
- `void avbase_player_start(AvbasePlayer *player)`
  Starts playback (legal from kPrepared/kPaused/kCompleted); illegal states are logged and ignored, never fatal. Side effects: emits kStateChanged(kStarted).
- `void avbase_player_pause(AvbasePlayer *player)`
  Pauses a running player (legal from kStarted only). Side effects: emits kStateChanged(kPaused).
- `void avbase_player_stop(AvbasePlayer *player)`
  Stops playback and tears the pipeline down; idempotent. Side effects: emits kStateChanged(kStopped).
- `void avbase_player_reset(AvbasePlayer *player)`
  Back to kIdle and reusable. Releases the current media. Side effects: emits kStateChanged(kIdle); drops every buffered packet.
- `void avbase_player_step_once(AvbasePlayer *player)`
  Single-frame advance while paused. Side effects: emits kSeekRenderingStarted for the stepped frame.
- `void avbase_player_seek_to(AvbasePlayer *player, int64_t position_ms)`
  Fire-and-forget seek; completion arrives as an event. |position_ms| is relative to the media start. Deliberately no return value: the C++ API's Result<int64_t> exists to hand back a request id for correlation, and that correlation is already carried by the event payload's request_id – a host that needs it should use the richer C++ API. Side effects: emits kSeekCompleted (what=600, arg1 = actual ms).
- `void avbase_player_set_playback_rate(AvbasePlayer *player, double rate)`
  |rate| is clamped to [0.25, 4.0] as the C++ API does. Side effects: takes effect on the next rendered frame.
- `void avbase_player_set_volume(AvbasePlayer *player, double volume)`
  |volume| is clamped to [0.0, 1.0]. Side effects: applies to the audio sink immediately.
- `void avbase_player_set_muted(AvbasePlayer *player, int muted)`
  Mutes or unmutes without touching the volume level. Side effects: applies to the audio sink immediately.
- `void avbase_player_set_loop_count(AvbasePlayer *player, int count)`
  |count| < 0 means infinite. Side effects: emits kCompleted (what=300) once per finished pass.
- `AvbaseStatus avbase_player_select_track(AvbasePlayer *player, int stream_type, int stream_index)`
  |stream_type| is an AVBASE_STREAM_* value below. |stream_index| is the index reported by the prepared media, NOT an avbase_source index. Side effects: emits kTrackChanged; decoders are rebuilt lazily.
- `void avbase_player_set_video_surface(AvbasePlayer *player, AvbaseNativeDisplay *display)`
  Attaches |display| as the video output, or detaches when null (audio keeps playing – the documented background-playback case). The player holds a reference; destroying the display handle afterwards is safe. Side effects: re-targets the video sink on the next frame.
- `AvbaseStatus avbase_player_take_snapshot(AvbasePlayer *player, int64_t at_ms, const char *file_path)`
  Writes the frame at |at_ms| to |file_path| (PNG); completion arrives as kSnapshotCompleted (what=1000, arg2 = Status). Side effects: writes one file; emits kSnapshotCompleted.
- `void avbase_player_reconnect_now(AvbasePlayer *player)`
  Forces the network retry path now. Replaces ijkplayer's write-only FFP_PROP_INT64_IMMEDIATE_RECONNECT property. Side effects: re-runs the reconnect ladder.
- `int avbase_player_state(const AvbasePlayer *player)`
  Mirrors PlayerState. See AVBASE_STATE_* below. Side effects: none.
- `int64_t avbase_player_media_time_ms(const AvbasePlayer *player)`
  Current playback position, including any seek in flight. Side effects: none.
- `int64_t avbase_player_buffered_time_ms(const AvbasePlayer *player)`
  How much media is buffered ahead of the playhead. Side effects: none.
- `int64_t avbase_player_duration_ms(const AvbasePlayer *player)`
  Total duration in ms, or zero for a live source. Side effects: none.
- `int avbase_player_is_live(const AvbasePlayer *player)`
  Nonzero when the source is live (no seekable timeline). Side effects: none.
- `int avbase_player_is_playing(const AvbasePlayer *player)`
  Nonzero while playback is actively running. Side effects: none.
- `int32_t avbase_player_video_width(const AvbasePlayer *player)`
  Display width after rotation/aspect corrections. Side effects: none.
- `int32_t avbase_player_video_height(const AvbasePlayer *player)`
  Display height after rotation/aspect corrections. Side effects: none.
- `int32_t avbase_player_video_rotation(const AvbasePlayer *player)`
  Clockwise rotation the host must apply, in degrees. Side effects: none.
- `double avbase_player_playback_rate(const AvbasePlayer *player)`
  Effective rate, after clamping. Side effects: none.
- `double avbase_player_volume(const AvbasePlayer *player)`
  Effective volume, after clamping. Side effects: none.
- `int avbase_player_muted(const AvbasePlayer *player)`
  Current mute state. Side effects: none.
- `int32_t avbase_player_title(AvbasePlayer *player, char *out, int32_t capacity)`
  Copies at most |capacity| bytes (including the NUL) of the human-readable media title into |out| and returns the number of bytes the full string needs, excluding the NUL. A return value >= |capacity| means it was truncated, so the caller can size a buffer and retry – guessing a size is how the legacy path used to truncate paths silently. Side effects: none.
- `int32_t avbase_player_dump_diagnostics(const AvbasePlayer *player, char *out, int32_t capacity)`
  Full internal state as JSON (~1 ms). The first thing to attach to a bug report. Same copy-into-your-buffer contract as the title. Side effects: none.
- `AvbaseStatus avbase_player_get_media_info(const AvbasePlayer *player, AvbaseMediaInfo *out)`
  Available only after avbase_player_prepare() has reported kPrepared; returns AVBASE_ERR_INVALID_STATE before that. Side effects: none.
- `AvbaseSource * avbase_player_add_source(AvbasePlayer *player, const char *uri, const char *name, int32_t z_order)`
  Registers a SECONDARY source and returns its layer handle, or null when the player is no longer in a state that accepts one. Call before prepare().

  The returned handle stays valid across stop/reset (the scene keeps it) but must be released with avbase_source_release(). Source 0 is the player's own avbase_player_set_data_source(); each call adds the next id. Side effects: allocates a scene layer visible after the next composite.
- `void avbase_source_release(AvbaseSource *source)`
  Releases a source handle. Null is a no-op. Does not unregister the source; it only drops this reference (matching the C++ shared_ptr semantics). Side effects: none (the source outlives the handle).
- `AvbaseStatus avbase_source_set_uri(AvbaseSource *source, const char *uri)`
  Re-points a registered source at a new URI; takes effect at the next prepare. Side effects: invalidates the source's demuxer until re-prepared.
- `AvbaseNativeDisplay * avbase_display_from_x11(void *x_display, uintptr_t window)`
  X11: |display| is a `Display*`, |window| is an XID. The pointers are copied, not retained: the window must outlive the display handle. Side effects: allocates a display handle (release with avbase_display_release).
- `AvbaseNativeDisplay * avbase_display_from_wayland(void *wl_display, void *wl_surface, int32_t width, int32_t height)`
  Wayland: |surface| is a `wl_surface*`. Wayland cannot report a surface size, so |width|/|height| must be supplied by the embedder. Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_from_sdl2(void *sdl_window)`
  SDL2: |sdl_window| is an `SDL_Window*`. Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_from_glfw(void *glfw_window)`
  GLFW: |glfw_window| is a `GLFWwindow*`. Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_from_egl(void *egl_surface)`
  EGL: |egl_surface| is an `EGLSurface` as an opaque pointer. Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_from_hwnd(void *hwnd)`
  Windows HWND. Kept explicit rather than folded into the Wrap() escape hatch because it is the one kind every Windows host asks for. Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_from_android_native_window(void *window)`
  Android ANativeWindow / Surface. Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_from_android_surface(void *surface)`
  Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_from_cametal_layer(void *layer)`
  iOS: a CAMetalLayer or CAEAGLLayer. Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_from_caeagl_layer(void *layer)`
  Side effects: allocates a display handle.
- `AvbaseNativeDisplay * avbase_display_wrap(void *raw, int kind, void(*release)(void *raw, void *user_data), void *user_data)`
  The generic door: |kind| is an AVBASE_DISPLAY_* value and |release| is an optional callback run on the render sequence when the last reference dies (may be null). This is how a host with its own window abstraction plugs in without asking this library for a new entry point per toolkit.

  |release| receives BOTH |raw| (the pointer that was wrapped) and |user_data| (whatever the host passed). Passing user_data separately is the whole point: a C host that needs cleanup state has nowhere else to keep it, and deriving it from |raw| would be a guess. It runs on the render sequence, so it must not block. Side effects: allocates a display handle; |release| runs when the last reference dies.
- `AvbaseNativeDisplay * avbase_display_none(void)`
  A valid "render nowhere" target; the sink discards frames but audio keeps playing. Side effects: allocates a display handle.
- `void avbase_display_release(AvbaseNativeDisplay *display)`
  Drops one reference; null is a no-op. Side effects: frees the handle when the last reference dies.
- `int avbase_display_kind(const AvbaseNativeDisplay *display)`
  The AVBASE_DISPLAY_KIND_* this handle was created with. Side effects: none.
- `int avbase_display_is_valid(const AvbaseNativeDisplay *display)`
  0 when the display can render, non-zero when it is None() or failed to be created. Never use the pointer itself for this: a null pointer here means "could not create", and None() is a VALID non-rendering target – the two are different states and conflating them is what makes "the PiP is not
  showing" undiagnosable (see the note on Player::scene()). Side effects: none.
- `int32_t avbase_display_debug_string(const AvbaseNativeDisplay *display, char *out, int32_t capacity)`
  Copy-into-your-buffer contract, same as the title: returns the full length excluding the NUL, and a value >= |capacity| means truncated. Side effects: none.
- `void avbase_player_set_event_callback(AvbasePlayer *player, AvbaseEventCallback cb, void *user_data)`
  Replaces any previous callback (pass null to remove it). The callback runs on the event sequence, never on a pipeline sequence, and it is safe to call any avbase_player_* function from inside it – including stop and reset. That is the deadlock fix over ijkplayer's message_loop; do not reintroduce a "must not reenter" rule here. Side effects: starts event delivery to |cb|.
### `struct` AvbaseMediaInfo

*player/public/c/avbase_c.h:305*

Fills a caller-allocated buffer with the media's duration, dimensions and track counts. A snapshot, not a live view. |out| may be null only if all pointers passed in are null (a no-op way to probe support).

- `int64_t duration_ms`
- `int64_t bitrate`
- `int is_live`
  Live streams report a zero duration, so this flag – not the duration – is how a host tells them apart.
- `int seekable`
- `int32_t video_width`
- `int32_t video_height`
- `int32_t video_rotation`
- `int32_t video_track_count`
  Counts, so a host can size a track menu without parsing anything. Taken from the FIRST stream of each kind for the size/rotation fields.
- `int32_t audio_track_count`
- `int32_t subtitle_track_count`

## player/public/deps.h

### `class` avbase::EventDispatcher

*player/public/deps.h:35*

Routes events to the host's own loop (Android Looper, GLib main context, Qt event loop). The default dispatcher is a dedicated FIFO thread, which preserves event order and guarantees that no pipeline sequence is ever blocked inside a user callback.

- `EventDispatcher(const EventDispatcher &)=delete`
- `EventDispatcher & operator=(const EventDispatcher &)=delete`
- `void Post(base::OnceClosure task)=0`
  Queues |task| on the sequence this dispatcher represents; it runs after the current task returns. Any thread. Errors: none. Side effects: none until the task runs.
- `void Flush()=0`
  Blocks until every queued task has run. Called from ~Player() so the host still receives the final StateChanged{to=kStopped}.

### `struct` avbase::Deps

*player/public/deps.h:56*

Everything avbase needs from the outside world. Every field may be null, in which case the auto-detected platform default is used — that is what makes `Player player; player.SetDataSource(url);` work with zero setup (rule E1).

- `std::vector<base::scoped_refptr<media::VideoDecoderFactory>> video_decoder_factories`
  OWNERSHIP VOCABULARY FOR THIS STRUCT (see STYLE.md §3 and the three verbs at the bottom of base/memory/scoped_refptr.h).

  A field whose type derives from base::RefCountedThreadSafe MUST be held in base::scoped_refptr. media::DataSource, media::VideoDecoderFactory and media::AudioDecoderFactory all do, and all three carry REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE() with protected constructors and protected destructors. Holding one in std::shared_ptr is not a style preference but two distinct failures: the field cannot be assigned at all (shared_ptr needs an accessible destructor, and these are protected), and if it somehow could be, the two smart pointers would keep two independent counts on one object – a double free or a use-after-free.

  These three were std::shared_ptr until the ninth round. The evidence that this was drift rather than a decision: this header already included base/memory/scoped_refptr.h without using it, the comment in deps.cc already said "Deps holds scoped_refptrs to forward-declared interfaces", and Player::SetVideoSurface() in the same frozen surface already takes scoped_refptr<NativeDisplay> – another RefCountedThreadSafe type.

  The remaining shared_ptr fields below (sink factories, tick clock, event dispatcher) name types that are NOT ref-counted, so shared_ptr is functional there. They still deviate from STYLE.md §3, which marks shared_ptr "not recommended"; changing them is churn on a frozen header without a defect to fix, so it is left to the M8 interface review as a recorded decision rather than done silently here.
- `std::vector<base::scoped_refptr<media::AudioDecoderFactory>> audio_decoder_factories`
- `base::scoped_refptr<media::TextDecoderFactory> text_decoder_factory`
  Text leg factory (Phase 4.2). scoped_refptr: TextDecoderFactory is refcounted like the decoder factories above.
- `std::shared_ptr<media::VideoRendererSinkFactory> video_sink_factory`
- `std::shared_ptr<media::AudioRendererSinkFactory> audio_sink_factory`
- `base::scoped_refptr<media::DataSource> data_source`
  Custom byte source (encrypted streams, an in-app downloader, a cache). When set, avbase reads through it instead of opening the URI itself. Build one with base::MakeRefCounted<media::MemoryDataSource>(...), or with your own DataSource subclass; M9's RetryDataSource and M18's CacheDataSource are decorators that wrap an existing scoped_refptr.
- `std::shared_ptr<const base::TickClock> tick_clock`
  Injectable monotonic clock. Null means base::DefaultTickClock. Tests use base::SimpleTestTickClock, which is what makes the scheduling logic deterministically testable (design principle P6).
- `std::unique_ptr<base::logging::LoggingDelegate> logging_delegate`
- `std::shared_ptr<EventDispatcher> event_dispatcher`
- `Deps()`
- `Deps(const Deps &)=delete`
- `Deps & operator=(const Deps &)=delete`
- `Deps(Deps &&) noexcept`
- `Deps & operator=(Deps &&) noexcept`
- `~Deps()`
- `static Deps CreateDefault()`
  Fills in every auto-detectable default for the current platform.

## player/public/global.h

### `class` avbase::GlobalGuard

*player/public/global.h:31*

RAII helper for programs that want symmetric init/shutdown.

- `GlobalGuard()`
- `GlobalGuard(const GlobalGuard &)=delete`
- `GlobalGuard & operator=(const GlobalGuard &)=delete`
- `~GlobalGuard()=default`

- `void GlobalInit()`
  One-time, idempotent process-wide initialisation: bridges the vendor log, initialises FFmpeg exactly once, and calls XInitThreads() when the process will use X11.

  Every Player constructor calls this, so you normally never need to. Call it explicitly only when you must guarantee it happens before your own first X11 call (see docs/archive/09 §10 L1).

  Thread safety: safe to call concurrently; guarded by std::call_once.
- `const char * GetVersion()`
  Version of avbase, e.g. "0.1.0".
- `const char * GetFFmpegVersion()`
  Version of the FFmpeg avbase was built against, e.g. "7.1", or "none" when built with AVBASE_ENABLE_FFMPEG=OFF.
## player/public/option_registry.h

### `class` avbase::OptionRegistry

*player/public/option_registry.h:51*

String-keyed configuration, kept for migrating existing setOption() call sites. Unlike ijkplayer, an unknown key or an out-of-range value is an error rather than a silent no-op (behaviour difference Δ2).

The table is generated from player_config.h by tools/gen_options.py so the typed fields and the string keys can never drift apart (rule C13).

- `OptionRegistry(const OptionRegistry &)=delete`
- `OptionRegistry & operator=(const OptionRegistry &)=delete`
- `Status SetInt(PlayerConfig *config, OptionCategory category, std::string_view key, int64_t value) const`
  Writes |value| into |config| for the option |key| of |category|. Errors: kInvalidArgument for an unknown key (the message carries a "did you mean" suggestion) or a value outside the option's range. Side effects: mutates |config| only on success.
- `Status SetDouble(PlayerConfig *config, OptionCategory category, std::string_view key, double value) const`
  Double form of SetInt; same contract. Errors: kInvalidArgument for an unknown key or out-of-range value. Side effects: mutates |config| only on success.
- `Status SetString(PlayerConfig *config, OptionCategory category, std::string_view key, std::string_view value) const`
  String form of SetInt; same contract. Errors: kInvalidArgument for an unknown key or unparsable value. Side effects: mutates |config| only on success.
- `Status SetValue(PlayerConfig *config, OptionCategory category, std::string_view key, const OptionValue &value) const`
  Typed-value form of SetInt; dispatches on the OptionValue's kind. Errors: kInvalidArgument for an unknown key or a kind the option does not accept. Side effects: mutates |config| only on success.
- `std::map<std::string, std::string> Dump(const PlayerConfig &config, OptionCategory category) const`
  Inverse of the setters: exports a config as string key/value pairs, for logging and for diffing two players' configurations.
- `std::vector<OptionDescriptor> Describe() const`
  Full machine-readable description; also the source for docs/API options.
- `std::string SuggestNearestKey(std::string_view key) const`
  "did you mean ..." helper used by the kInvalidArgument error message.
- `static const OptionRegistry & GetInstance()`
  Returns the process-wide OptionRegistry. Thread safety: any thread; the instance is constructed once and then read-only, so the read path needs no locking. Errors: none (never fails). Side effects: none – first-call construction is deferred and idempotent.

- `enum class OptionCategory`
  Mirrors ijkplayer's IJKMP_OPT_CATEGORY_*.
  - `kPlayer` `= 0`
  - `kFormat`
  - `kCodec`
  - `kScaler`
- `using OptionValue = std::variant<bool, int64_t, double, std::string>;`
### `struct` avbase::OptionDescriptor

*player/public/option_registry.h:27*

- `enum class Type`
  - `kBool`
  - `kInt`
  - `kInt64`
  - `kDouble`
  - `kString`
  - `kEnum`
- `std::string key`
- `OptionCategory category {OptionCategory::kPlayer}`
- `enum avbase::OptionDescriptor::Type kInt`
- `OptionValue min`
- `OptionValue max`
- `OptionValue default_value`
- `std::string config_field`
  e.g. "buffer.max_bytes"
- `std::string doc`

## player/public/playback_stats.h

### `struct` avbase::AsyncStats

*player/public/playback_stats.h:29*

- `base::TimeDelta buf_backwards`
- `base::TimeDelta buf_forwards`
- `base::TimeDelta buf_capacity`

### `struct` avbase::BufferStats

*player/public/playback_stats.h:23*

Replaces the 30+ FFP_PROP_* integer properties that ijkplayer callers had to poll one at a time, each taking a mutex. One call, one immutable snapshot. See docs/05 table 6 for the field-by-field mapping.

- `base::TimeDelta cached_duration`
- `size_t cached_bytes {0}`
- `size_t cached_buffers {0}`

### `struct` avbase::BufferingStats

*player/public/playback_stats.h:43*

- `uint64_t buffering_count {0}`
- `base::TimeDelta total_buffering_time`
- `base::TimeDelta last_buffering_time`
- `int current_hwm_step {0}`
- `base::TimeDelta active_high_water_mark`

### `struct` avbase::CacheStats

*player/public/playback_stats.h:35*

- `int64_t physical_pos {0}`
- `int64_t file_forwards {0}`
- `int64_t file_pos {0}`
- `int64_t count_bytes {0}`
- `int64_t logical_file_size {0}`

### `struct` avbase::PlaybackStats

*player/public/playback_stats.h:65*

- `double video_decode_fps {0.0}`
- `double video_output_fps {0.0}`
- `double playback_rate {1.0}`
- `double volume {1.0}`
- `double drop_frame_rate {0.0}`
- `base::TimeDelta av_delay`
- `base::TimeDelta av_diff`
- `int selected_video_stream {-1}`
- `int selected_audio_stream {-1}`
- `int selected_text_stream {-1}`
- `media::VideoDecoderType video_decoder {media::VideoDecoderType::kUnknown}`
- `media::AudioDecoderType audio_decoder {media::AudioDecoderType::kUnknown}`
- `BufferStats video`
- `BufferStats audio`
- `BufferingStats buffering`
- `int64_t bit_rate {0}`
- `int64_t tcp_speed {0}`
- `AsyncStats async`
- `int64_t traffic_byte_count {0}`
- `CacheStats cache`
- `base::TimeDelta latest_seek_load_duration`
- `uint64_t frames_presented {0}`
- `uint64_t frames_dropped {0}`
- `uint64_t frames_dropped_late {0}`
- `uint64_t frames_dropped_fps {0}`
- `uint64_t frames_repeated {0}`
- `uint64_t frames_dropped_by_sink {0}`
  Frames handed to the display layer that it refused because it had nowhere to draw them (no surface, a surface of the wrong kind). A subset of frames_presented, NOT a part of frames_dropped – see the field comment in media/base/renderer_client.h for why the two are different events.
- `uint64_t frames_submit_failures {0}`
  Frames that reached the display layer and could not be shown: upload or present failed, or a hardware frame could not be imported. Nonzero here is a display-path fault, not a slow decode.
- `double measured_refresh_hz {0.0}`
  The rate this display layer actually achieved: the average spacing of its own presents, in Hz. Unlike the display interval a sink is constructed with, this is measured from what happened, so a value below the panel's nominal rate means presents are not arriving every interval (starved, or paced slower) – not that the display is slow. 0 until one is measured.
- `media::AudioGlitchInfo audio_glitches`
  Underrun accounting straight from the audio sink. Turns "sometimes it
  crackles" into a number you can alert on.
- `media::RenderPerfStats render`
  What each presented frame actually cost, measured on the GL render thread and split by stage (upload / compose / blit / present). Distinct from video_output_fps above, which the player derives from media timestamps: this one describes the rendering itself and survives a sink swap.
- `StageTimings stages`
- `base::TimeDelta first_frame_latency`
- `size_t heap_bytes_estimate {0}`
- `uint64_t events_dropped_by_overflow {0}`
- `std::string ToJson() const`
  Serialises the stats as JSON, for logs and monitoring pipelines. Errors: none. Side effects: none.

### `struct` avbase::StageTimings

*player/public/playback_stats.h:54*

First-frame latency decomposition. ijkplayer emitted FFP_MSG_OPEN_INPUT / FIND_STREAM_INFO / COMPONENT_OPEN for exactly this purpose but left the caller to stitch the timestamps together.

- `base::TimeDelta prepare_to_open_input`
- `base::TimeDelta open_input_to_stream_info`
- `base::TimeDelta stream_info_to_decoder_open`
- `base::TimeDelta decoder_open_to_first_packet`
- `base::TimeDelta first_packet_to_first_frame`
- `base::TimeDelta first_frame_to_presented`
- `base::TimeDelta total_first_frame`
- `base::TimeDelta audio_first_rendered`

## player/public/player.h

### `class` avbase::Player

*player/public/player.h:75*

The SDK entry point: one Player owns a complete pipeline (demuxer, decoders, A/V sync, output sinks).

Thread safety: every method may be called from any thread and none block, with the single exception of PrepareSync() and StopSync(). Destruction is bounded by PlayerConfig::shutdown_timeout, so ~Player() cannot hang the caller (behaviour difference Δ15).

WATCH OUT: do NOT give a Player static storage duration with automatic destruction. ~Player() joins the event thread (and, via the pipeline, the media/video/audio threads). Running that join from C++ static-destruction phase – a `static Player` torn down at program exit – crashes empirically (bench/bench_event_hub.cc measured SIGABRT/SIGILL on exit for the analogous static EventHub). If a Player must outlive everything, leak it (e.g. `new Player(...)` and never delete) rather than letting its destructor run at atexit.

Minimal use:

```cpp
avbase::Player player;
player.SetEventHandler([&player](const avbase::PlayerEvent& e) {
  if (e.type == avbase::EventType::kPrepared) player.Start();
});
AVBASE_RETURN_IF_ERROR(player.SetDataSource("video.mp4"));
AVBASE_RETURN_IF_ERROR(player.PrepareAsync());

```

The event handler runs on the event dispatch sequence, never on a pipeline sequence, and it is safe to call any Player method from inside it — including Stop() and Reset(). That fixes a real deadlock in ijkplayer, whose message_loop dispatches while holding the player mutex.

- `using SeekCB = base::OnceCallback<void(Status)>;`
- `using EventHandler = base::RepeatingCallback<void(const PlayerEvent &)>;`
- `Player()`
  Uses auto-detected platform backends. Inject custom ones via the Deps overload or PlayerBuilder.
- `explicit Player(const PlayerConfig &config)`
- `Player(const PlayerConfig &config, std::unique_ptr< Deps > deps)`
- `Player(const Player &)=delete`
- `Player & operator=(const Player &)=delete`
- `~Player()`
- `Status SetDataSource(std::string_view uri)`
- `Status SetDataSource(const DataSourceDescriptor &descriptor)`
  Descriptor form: same contract, for callers that already resolved the source (memory buffer, custom downloader, secondary source). Errors: kInvalidArgument for a malformed descriptor. Side effects: moves the machine to kInitialized.
- `Status PrepareAsync()`
  Starts demux/decoder setup and returns immediately; kPrepared arrives through the event handler (or the return of PrepareSync). Errors: kInvalidState from any state but kIdle/kInitialized. Side effects: emits kPrepared, or kError on failure.
- `Status PrepareSync(base::TimeDelta timeout=base::Seconds(30))`
  Blocking form of PrepareAsync with |timeout|. The autostart runs inside it (see Start's WATCH OUT), so a successful return may already be kStarted. Errors: kTimeout when |timeout| elapses first; kInvalidState from any state but kIdle/kInitialized. Side effects: emits kPrepared, or kError on failure.
- `void Start()`
  Legal only from kPrepared, kPaused and kCompleted; from any other state it logs "Player::Start in state <s> is not legal" and returns without changing anything (that rejection is deliberate, see player/state_machine.h).

  WATCH OUT: with the default start_on_prepared=true, the autostart runs inside PrepareSync() before it returns, so the Start() you write right after PrepareSync() is the ILLEGAL one, not the missing one. Guard it (examples/pull_frames.cc) or set start_on_prepared=false if you mean to start playback yourself. ijkplayer cannot hit this at all: its start only clears pause_req and is not a state transition.

  Errors: none – an illegal state is logged and ignored (the rejection is deliberate, see player/state_machine.h). Side effects: emits kStateChanged(kStarted); from kCompleted replays from the beginning.
- `void Pause()`
  Pauses a running player; legal only from kStarted. Errors: none – an illegal state is logged and ignored. Side effects: emits kStateChanged(kPaused).
- `void Stop()`
  Stops playback and tears the pipeline down, keeping the configured backends. Idempotent: stopping an already-stopped player is a no-op. Errors: none. Side effects: emits kStateChanged(kStopped).
- `void StopSync(base::TimeDelta timeout)`
  Blocking Stop: returns once the pipeline has really stopped, bounded by |timeout| so it cannot hang a shutdown path. Errors: kTimeout if the pipeline does not stop within |timeout|. Side effects: emits kStateChanged(kStopped).
- `void Reset()`
  Releases the current source and returns to kIdle, keeping config and backends: the player is reusable afterwards. Errors: none. Side effects: emits kStateChanged(kIdle); drops every buffered packet.
- `void RunUntilIdle()`
  Blocks until playback reaches a terminal state. Convenience for CLIs and tests; production hosts use SetEventHandler() with their own loop. Errors: none (the terminal error, if any, arrives as an event). Side effects: none beyond what playback itself emits.
- `Result<int64_t> SeekTo(base::TimeDelta position, SeekMode mode, SeekCB cb)`
  Runs |cb| on the event sequence when the seek finishes; never inline. Returns the request id, which also appears in SeekCompletedPayload so a caller doing rapid seeks can match results to requests. SeekMode::kAccurate is additionally gated by config.seek.accurate ("enable-accurate-seek"). With that switched off an accurate request does not fail: it lands on the keyframe, and requested-vs-actual in the completion is how you tell. Errors: kInvalidState when not seekable in the current state. Side effects: emits kSeekCompleted (or kAccurateSeekCompleted); the callback runs on the event sequence, never inline.
- `void SeekTo(base::TimeDelta position)`
  Fire-and-forget seek to the nearest keyframe at or before |position|. Errors: kInvalidState when not seekable in the current state. Side effects: emits kSeekCompleted with the landed position.
- `void StepOnce()`
  Advances exactly one frame while paused; no-op unless kPaused. Errors: none – an illegal state is a no-op, not a failure. Side effects: emits kSeekRenderingStarted for the stepped frame.
- `void SetPlaybackRate(double rate)`
  Sets the playback rate; the valid range is [0.25, 4.0] and out-of-range values are clamped, not rejected. Errors: none. Side effects: takes effect on the next rendered frame; audio is time-stretched to keep pitch (docs/04 §5).
- `void SetVolume(double volume)`
  Sets the output volume in [0.0, 1.0]; out-of-range values are clamped. Errors: none. Side effects: applies to the audio sink immediately.
- `void SetMuted(bool muted)`
  Mutes or unmutes output without touching the volume level. Errors: none. Side effects: applies to the audio sink immediately.
- `void SetLoopCount(int count)`
  Sets how many times playback repeats after the first pass; -1 means infinite. Errors: none. Side effects: emits kCompleted once per finished pass.
- `Status SelectTrack(media::DemuxerStreamType type, int stream_index)`
  Selects one track of |type| by its index in media_info(); disabled tracks keep their place, so switching back is cheap. Errors: kTrackNotFound for an index the media does not have. Side effects: emits kTrackChanged; decoders are rebuilt lazily.
- `void SetVideoSurface(base::scoped_refptr< NativeDisplay > display)`
  May be called while playing. Pass a null display to render nowhere while keeping audio (useful for background playback). Errors: none. Side effects: re-targets the video sink on the next frame; emits kRenderingStarted when output resumes.
- `Status TakeSnapshot(base::TimeDelta at, std::string file_path)`
  Writes the frame at |at| to |file_path| (PNG). The write happens on a worker sequence; completion arrives as an event. Errors: kInvalidState when the position is not decodable yet. Side effects: emits kSnapshotCompleted (success or failure).
- `void ReconnectNow()`
  Forces the retry path of NetConfig::reconnect immediately. Replaces ijkplayer's write-only FFP_PROP_INT64_IMMEDIATE_RECONNECT property. Errors: none – without a live source the request is dropped. Side effects: re-runs the reconnect ladder (docs/04 §7).
- `PlayerState state() const`
- `std::optional<MediaInfo> media_info() const`
  Media metadata once prepared (duration, streams, rotation...); nullopt before that. Errors: none. Side effects: none.
- `base::TimeDelta GetMediaTime() const`
  Current playback position, including any seek in flight. Errors: none. Side effects: none.
- `base::TimeDelta GetBufferedTime() const`
  How much media is buffered ahead of the playhead. Errors: none. Side effects: none.
- `base::TimeDelta GetDuration() const`
  Total duration, or zero for a live source. Errors: none. Side effects: none.
- `bool is_live() const`
  Whether the current source is live (no seekable timeline). Errors: none. Side effects: none.
- `bool IsPlaying() const`
  Whether playback is actively running (kStarted). Errors: none. Side effects: none.
- `media::Size video_natural_size() const`
  Display size after rotation/aspect corrections. Errors: none. Side effects: none.
- `media::Size video_coded_size() const`
  Coded size before rotation/aspect corrections. Errors: none. Side effects: none.
- `int video_rotation() const`
  Clockwise rotation the host must apply, in degrees. Errors: none. Side effects: none.
- `double playback_rate() const`
  Effective rate, after clamping. Errors: none. Side effects: none.
- `double volume() const`
  Effective volume, after clamping. Errors: none. Side effects: none.
- `bool muted() const`
  Current mute state. Errors: none. Side effects: none.
- `PlaybackStats GetPlaybackStats() const`
  Current counters (frames, drops, buffered ranges) as one snapshot. Errors: none. Side effects: none.
- `std::string DumpDiagnostics() const`
  Full internal state as JSON, ~1 ms, safe to call at any time. The first thing to attach to a bug report; see docs/10 §10.
- `Scene * scene()`
- `std::shared_ptr<SceneSource> AddSource(const DataSourceDescriptor &descriptor, std::string name, int z)`
- `void SetEventHandler(EventHandler handler)`
  Installs the event handler. Replaces any previous one; the handler runs on the event dispatch sequence and may call any Player method (the class comment explains why that cannot deadlock). Errors: none. Side effects: none until the first event.
- `Subscription AddObserver(PlayerObserver *observer)`
  Subscribes |observer| to every PlayerEvent; keep the returned handle alive for as long as the observer should receive them. Errors: kInvalidArgument for a null observer. Side effects: starts event delivery to |observer|.
- `Status UpdateConfig(const PlayerConfig &config)`
- `const PlayerConfig & config() const`
  The config in force; the reference stays valid for the player's lifetime and is never mutated in place. Errors: none. Side effects: none.

### `class` avbase::Player::Subscription

*player/public/player.h:311*

RAII subscription handle: unsubscribes on destruction.

- `Subscription()`
- `Subscription(Subscription &&) noexcept`
- `Subscription & operator=(Subscription &&) noexcept`
- `Subscription(const Subscription &)=delete`
- `Subscription & operator=(const Subscription &)=delete`
- `~Subscription()`
- `void Reset()`
  Drops the subscription early; the destructor does the same. Errors: none. Side effects: stops event delivery to the observer.
- `bool active() const`
  Whether the subscription still receives events. Errors: none. Side effects: none.

### `class` avbase::PlayerBuilder

*player/public/player.h:356*

Builder form, for injecting custom backends and for migrating string options. Not thread-safe: configure, Build(), then hand the Player to its owner.

- `PlayerBuilder()`
- `PlayerBuilder(const PlayerBuilder &)=delete`
- `PlayerBuilder & operator=(const PlayerBuilder &)=delete`
- `~PlayerBuilder()`
- `PlayerBuilder & SetConfig(PlayerConfig config)`
  Takes ownership of |config| (a copy is kept). Errors: none (validation happens in Build). Side effects: none until Build.
- `Status SetOption(OptionCategory category, std::string_view key, const OptionValue &value)`
  Migrates one ijkplayer-style string option; unknown keys are rejected with a "did you mean" suggestion (Δ2). Errors: kInvalidArgument for an unknown key or malformed value. Side effects: records the option for Build.
- `PlayerBuilder & SetDeps(std::unique_ptr< Deps > deps)`
  Installs custom backends; the defaults are used for every factory the Deps leaves null. Errors: none. Side effects: none until Build.
- `PlayerBuilder & SetLogLevel(base::logging::LogSeverity severity)`
  Sets the initial log severity. Errors: none. Side effects: applies from the first log call after Build.
- `Result<std::unique_ptr<Player>> Build()`
  Produces the Player. Exactly one Build per builder. Errors: kInvalidState on a second Build; configuration errors as kInvalidArgument (see PlayerConfig::Issues()). Side effects: hands the configured backends to the new Player.

## player/public/player_config.h

- `enum class SyncMasterType`
  - `kAudio` `= 0`
  - `kVideo`
  - `kExternal`
- `enum class OverlayFormat`
  - `kRgb32` `= 0`
  - `kRgb24`
  - `kRgb16`
  - `kI420`
  - `kYv12`
  - `kNative`
- `enum class SeekMode`
  - `kPreviousKeyframe` `= 0`
  - `kAnyFrame`
  - `kAccurate`
- `enum class AudioBackend`
  - `kAuto` `= 0`
  - `kAlsa`
  - `kPulse`
  - `kPipeWire`
  - `kSdl2`
  - `kOpenSLES`
  - `kAAudio`
  - `kCoreAudio`
- `enum class LinuxVideoBackend`
  - `kAuto` `= 0`
  - `kSdl2`
  - `kGl`
- `enum class HdrToneMapping`
  - `kNone` `= 0`
  - `kSimple`
  - `kAuto`
- `std::vector<ConfigIssue> ValidateConfig(const PlayerConfig &config)`
  Returns every problem at once, so a caller can fix them in a single pass instead of discovering them one rebuild at a time. Side effects: none (pure validation).
### `struct` avbase::AudioConfig

*player/public/player_config.h:131*

- `bool disabled {false}`
  "an"
- `double startup_volume {1.0}`
  "volume" (0..100 -> 0..1, Δ6)
- `AudioBackend backend {AudioBackend::kAuto}`
  "opensles" etc.
- `bool tempo_stretch {true}`
  "soundtouch"

  True by default: uses the built-in WSOLA AudioRendererAlgorithm instead of ijkplayer's SoundTouch dependency (behaviour difference Δ17).
- `bool preserves_pitch {true}`
- `int frame_queue_size {9}`
- `int selected_stream {-1}`
  "ast"
- `double preset_5_1_center_mix_level {
      0.70710678}`
  "preset-5-1-center-mix-level"
- `std::string filter_graph`
  "af"
- `std::string output_device_id`

### `struct` avbase::BufferConfig

*player/public/player_config.h:64*

- `bool enabled {true}`
  "packet-buffering"
- `size_t max_bytes {15ULL * 1024 * 1024}`
  "max-buffer-size"
- `size_t min_frames {5}`
  "min-frames"
- `bool unlimited {false}`
  "infbuf"
- `base::TimeDelta max_cached_duration {base::Seconds(3)}`
- `base::TimeDelta first_high_water_mark {base::Milliseconds(100)}`
  Three-tier high water mark: each stall->recovery cycle steps to the next, and a seek resets to the first. Ported unchanged from ijkplayer.
- `base::TimeDelta next_high_water_mark {base::Seconds(1)}`
- `base::TimeDelta last_high_water_mark {base::Seconds(5)}`
- `bool sync_av_start {true}`
  "sync-av-start"

### `struct` avbase::ConfigIssue

*player/public/player_config.h:246*

- `std::string field`
  "buffer.first_high_water_mark"
- `std::string problem`
  "must be <= buffer.next_high_water_mark"
- `std::string suggestion`

### `struct` avbase::DataSourceConfig

*player/public/player_config.h:188*

- `bool enable_cache {false}`
  ijkio master switch (M18)
- `std::string cache_dir`
- `size_t cache_max_bytes {512ULL * 1024 * 1024}`

### `struct` avbase::DemuxConfig

*player/public/player_config.h:53*

- `int64_t probe_size {5LL * 1024 * 1024}`
  "probesize"
- `base::TimeDelta analyze_duration {base::Seconds(5)}`
  "analyzeduration"
- `bool find_stream_info {true}`
  "find_stream_info"
- `std::string forced_format`
  "iformat"
- `base::TimeDelta timeout`
  "timeout"/"rw_timeout"
- `bool delay_meta_init {false}`
  "ijkmeta-delay-init"

### `struct` avbase::NetConfig

*player/public/player_config.h:175*

- `bool reconnect {true}`
- `int reconnect_max_retries {3}`
- `base::TimeDelta reconnect_delay {base::Milliseconds(100)}`
- `bool dns_cache_clear {false}`
  "dns_cache_clear"
- `std::string user_agent`
- `std::map<std::string, std::string> headers`
- `base::TimeDelta live_max_latency`
  Zero means "do not chase the live edge".

### `struct` avbase::PlayerConfig

*player/public/player_config.h:210*

- `DemuxConfig demux`
- `BufferConfig buffer`
- `SeekConfig seek`
- `VideoConfig video`
- `AudioConfig audio`
- `SubtitleConfig subtitle`
- `NetConfig net`
- `DataSourceConfig data_source`
- `RenderConfig render`
- `SyncMasterType sync_master {SyncMasterType::kAudio}`
  "sync"
- `int loop_count {1}`
  "loop" (-1 = infinite)
- `bool fast {false}`
  "fast"
- `bool start_on_prepared {true}`
  "start-on-prepared"
- `bool async_init_decoder {false}`
  "async-init-decoder"
- `bool no_time_adjust {false}`
  "no-time-adjust"
- `bool get_frame_mode {false}`
  "get-frame-mode"
- `int rdft_speed {0}`
  "rdftspeed"
- `base::TimeDelta shutdown_timeout {base::Milliseconds(500)}`
  Upper bound on ~Player() and StopSync(). On expiry the offending sequence is detached and logged rather than blocking the caller forever (Δ15).
- `base::TimeDelta stats_interval {base::Seconds(1)}`
- `std::map<std::string, std::string> extra_format_options`
  Escape hatches: passed through verbatim to FFmpeg, matching ijkplayer's IJKMP_OPT_CATEGORY_FORMAT / CODEC / SWS.
- `std::map<std::string, std::string> extra_codec_options`
- `std::map<std::string, std::string> extra_scaler_options`

### `struct` avbase::RenderConfig

*player/public/player_config.h:197*

- `bool disable_video_output {false}`
  "nodisp"
- `bool render_wait_start {false}`
  "render-wait-start"
- `LinuxVideoBackend linux_backend {LinuxVideoBackend::kAuto}`
- `AudioBackend linux_audio {AudioBackend::kAuto}`
- `bool prefer_dmabuf_zero_copy {true}`
- `bool use_present_extension_for_vsync {true}`
- `bool gl_context_sharing {false}`
- `bool prefer_xshm {false}`

### `struct` avbase::SeekConfig

*player/public/player_config.h:80*

- `bool accurate {true}`
  "enable-accurate-seek": whether SeekMode::kAccurate may land on the frame you asked for. Defaults to TRUE here, unlike ijkplayer's 0, because the opt-in already happened per call – kAccurate is a first-class argument, where ijkplayer only had this switch. So the field reads as a veto: leave it alone and kAccurate behaves exactly as before, switch it off and every kAccurate request degrades to the keyframe landing (with a warning) – which is what an ijk application passing enable-accurate-seek=0 expects. The default **playback** behaviour is still keyframe seek: that comes from SeekMode's default, not from this field.
- `base::TimeDelta accurate_timeout {
      base::Seconds(5)}`
  "accurate-seek-timeout"
- `base::TimeDelta seek_at_start`
  "seek-at-start"
- `bool flush_on_seek {true}`

### `struct` avbase::SubtitleConfig

*player/public/player_config.h:149*

- `bool enabled {false}`
  "subtitle"
- `int selected_stream {-1}`
  "sst"
- `base::TimeDelta live_cue_max_age {base::Seconds(10)}`
  How far behind the media clock a cue may be and still be shown, on a LIVE source. Zero (the default) disables the drop, which is what recorded content wants: a seek lands on a subtitle, and that cue is exactly what the viewer asked for.

  On a live edge it is the other way round. The demuxer keeps delivering from the point the viewer joined, so a viewer who joins late, or whose pipeline stalls, receives cues for dialogue that has already been spoken and shown. Displaying those is worse than displaying none – the text is right, the timing is nonsense – so a cue further behind than this window is dropped instead.

  The default is deliberately generous (10 s): a window that is too narrow eats subtitles during a transient stall, which is a worse failure than a late line, so this should be set to the longest stall the product is willing to paper over, not to a "typical" one.

### `struct` avbase::VideoConfig

*player/public/player_config.h:99*

- `bool disabled {false}`
  "vn"
- `DecoderPreference decoder_preference {DecoderPreference::kAuto}`
- `HwCodecMask hw_codecs {static_cast<HwCodecMask>(HwCodecFlag::kAll)}`
  Which codecs the hardware path may handle; each bit is an HwCodecFlag. kAll (not 0) so that the Phase 3 default is "hardware first, fall back to
  software", matching decoder_preference{kAuto}; opt OUT per codec with `hw_codecs & ~HwCodecFlag::kAvc`, or force software via kSoftware. Replaces ijkplayer's "mediacodec-*" booleans.
- `std::string hw_decoder_name`
  "mediacodec-default-name"
- `bool hw_sync_mode {false}`
  "mediacodec-sync"
- `bool hw_async {false}`
  "videotoolbox-async"
- `bool hw_wait_async {true}`
  "videotoolbox-wait-async"
- `int hw_max_frame_width {0}`
  "videotoolbox-max-frame-width"
- `bool auto_rotate {false}`
  "mediacodec-auto-rotate"
- `bool handle_resolution_change {true}`
  Defaults to true (behaviour difference Δ3): leaving resolution changes unhandled corrupts the picture rather than degrading gracefully.
- `int max_fps {31}`
  "max-fps"
- `int max_frame_drop {0}`
  "framedrop"
- `OverlayFormat overlay_format {OverlayFormat::kRgb32}`
  "overlay-format"
- `int frame_queue_size {3}`
  "video-pictq-size"
- `int skip_loop_filter {0}`
  "skip-loop-filter"
- `bool calc_frame_rate {true}`
  !"skip-calc-frame-rate"
- `std::string filter_graph`
  "vf0"
- `int selected_stream {-1}`
  "vst"
- `std::string forced_mime_type`
  "video-mime-type"
- `HdrToneMapping hdr_tone_mapping {HdrToneMapping::kSimple}`

## player/public/player_event.h

### `class` avbase::PlayerObserver

*player/public/player_event.h:252*

Observer interface, for projects that prefer virtual dispatch over a std::function. Only the events you override are delivered.

Every callback runs on the event dispatch sequence and must not block; the default implementations ignore the event. Overriding OnEvent() alone receives everything (including the events without a dedicated hook).

- `PlayerObserver(const PlayerObserver &)=delete`
- `PlayerObserver & operator=(const PlayerObserver &)=delete`
- `~PlayerObserver()=default`
- `void OnPrepared(const PreparedPayload &)`
  Metadata is ready; the payload mirrors avbase::MediaInfo.
- `void OnCompleted(const CompletedPayload &)`
  Playback reached the end (one call per pass with SetLoopCount).
- `void OnError(const ErrorPayload &)`
  A fatal error; the payload carries the ErrorCode and a message.
- `void OnStateChanged(const StateChangedPayload &)`
  The machine changed state; the payload names the transition.
- `void OnVideoSizeChanged(const VideoSizeChangedPayload &)`
  The video size (or rotation) changed; re-layout the surface.
- `void OnBufferingProgress(const BufferingProgressPayload &)`
  Buffering progress; the payload carries buffered-ahead and percent.
- `void OnSeekCompleted(const SeekCompletedPayload &)`
  A seek finished; the payload carries requested-vs-actual.
- `void OnTimedText(const TimedTextPayload &)`
  A subtitle cue is due; the payload carries the cue text.
- `void OnStats(const StatsPayload &)`
  Periodic stats snapshot (see PlaybackStats).
- `void OnEvent(const PlayerEvent &)`
  Catch-all for anything not overridden above.

- `enum class PlayerState`
  - `kIdle` `= 0`
  - `kInitialized`
  - `kPreparing`
  - `kPrepared`
  - `kStarted`
  - `kPaused`
  - `kBuffering`
  - `kStopping`
  - `kStopped`
  - `kCompleted`
  - `kError`
  - `kEnd`
- `enum class EventType`
  - `kPrepared`
  - `kCompleted`
  - `kError`
  - `kFlushed`
  - `kStateChanged`
  - `kVideoSizeChanged`
  - `kRotationChanged`
  - `kRenderingStarted`
  - `kDecodingStarted`
  - `kDecoderOpened`
  - `kDecoderFallback`
  - `kStageReached`
  - `kWaiting`
  - `kBufferingStarted`
  - `kBufferingEnded`
  - `kBufferingProgress`
  - `kBufferedBytesUpdate`
  - `kBufferedTimeUpdate`
  - `kSeekCompleted`
  - `kAccurateSeekCompleted`
  - `kSeekRenderingStarted`
  - `kTimedText`
  - `kTrackChanged`
  - `kSnapshotCompleted`
  - `kStats`
  - `kHdrUnsupported`
- `using EventPayload = std::variant<PreparedPayload, CompletedPayload, FlushedPayload, ErrorPayload, StateChangedPayload, VideoSizeChangedPayload, RotationChangedPayload, RenderingStartedPayload, DecodingStartedPayload, DecoderOpenedPayload, DecoderFallbackPayload, StageReachedPayload, WaitingPayload, BufferingProgressPayload, BufferedUpdatePayload, SeekCompletedPayload, AccurateSeekCompletedPayload, SeekRenderingStartedPayload, TimedTextPayload, TrackChangedPayload, SnapshotCompletedPayload, StatsPayload, HdrUnsupportedPayload>;`
- `const char * GetPlayerStateName(PlayerState state)`
  Stable spelling of |state| for logs and diagnostics; never null. Side effects: none.
- `const char * GetEventTypeName(EventType type)`
  Stable spelling of |type| for logs and diagnostics; never null. Side effects: none.
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsPrepared, PreparedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsError, ErrorPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsStateChanged, StateChangedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsVideoSizeChanged, VideoSizeChangedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsRotationChanged, RotationChangedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsRenderingStarted, RenderingStartedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsDecodingStarted, DecodingStartedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsStageReached, StageReachedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsBufferingProgress, BufferingProgressPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsBufferedUpdate, BufferedUpdatePayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsSeekCompleted, SeekCompletedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsAccurateSeekCompleted, AccurateSeekCompletedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsTimedText, TimedTextPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsTrackChanged, TrackChangedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsSnapshotCompleted, SnapshotCompletedPayload)`
- `AVBASE_DECLARE_EVENT_ACCESSOR(AsStats, StatsPayload)`
- `bool ToLegacyEvent(const PlayerEvent &event, LegacyEvent *out)`
  Maps a modern PlayerEvent to the legacy Android-style event the struct above describes, so an existing Java message handler keeps working. Thread safety: any thread; pure mapping, no shared state. Errors: returns false (and leaves |out| defaulted) when |event| has no legacy equivalent. Side effects: none; |out| borrows the event's strings and stays valid only while |event| does.
- `#define AVBASE_DECLARE_EVENT_ACCESSOR AVBASE_PLAYER_EXPORT const Type* Name(const PlayerEvent& event)`
  Typed accessors. Each returns nullptr when |event| carries a different payload, so a switch on event.type plus one accessor is all a caller needs and std::visit never appears in user code (design rule E7).
### `struct` avbase::AccurateSeekCompletedPayload

*player/public/player_event.h:151*

- `base::TimeDelta position`

### `struct` avbase::BufferedUpdatePayload

*player/public/player_event.h:138*

- `base::TimeDelta cached_time`
- `base::TimeDelta high_water_mark_time`
- `size_t cached_bytes {0}`
- `size_t high_water_mark_bytes {0}`

### `struct` avbase::BufferingProgressPayload

*player/public/player_event.h:134*

- `base::TimeDelta buffered_ahead`
- `int percent {0}`

### `struct` avbase::CompletedPayload

*player/public/player_event.h:178*


### `struct` avbase::DecoderFallbackPayload

*player/public/player_event.h:113*

- `std::string from_decoder`
- `std::string to_decoder`
- `std::string reason`

### `struct` avbase::DecoderOpenedPayload

*player/public/player_event.h:108*

- `media::DemuxerStreamType type {media::DemuxerStreamType::kUnknown}`
- `std::string decoder_name`
- `bool outputs_opaque_surface {false}`

### `struct` avbase::DecodingStartedPayload

*player/public/player_event.h:102*

- `media::DemuxerStreamType type {media::DemuxerStreamType::kUnknown}`
- `media::VideoDecoderType video_type {media::VideoDecoderType::kUnknown}`
- `media::AudioDecoderType audio_type {media::AudioDecoderType::kUnknown}`
- `std::string codec_name`

### `struct` avbase::ErrorPayload

*player/public/player_event.h:81*

- `MediaError error`

### `struct` avbase::FlushedPayload

*player/public/player_event.h:177*


### `struct` avbase::HdrUnsupportedPayload

*player/public/player_event.h:179*

- `std::string transfer`

### `struct` avbase::LegacyEvent

*player/public/player_event.h:231*

Downgrade path for hosts that still speak ijkplayer's (what, arg1, arg2, obj) quadruple — used by the optional C ABI layer and the Android JNI binding so that an existing Java message handler keeps working unchanged. Side effects: none (pure mapping; |obj| in the result borrows the event's strings and stays valid while |event| does).

- `int what {0}`
- `int64_t arg1 {0}`
- `int64_t arg2 {0}`
- `std::string obj`

### `struct` avbase::PlayerEvent

*player/public/player_event.h:193*

- `EventType type {EventType::kError}`
- `EventPayload payload`
- `base::TimeTicks wall_time`
  When the event was produced.
- `base::TimeDelta media_time`
  Media position at production time.
- `int64_t sequence_number {0}`
  Monotonic; detects dropped events.

### `struct` avbase::PreparedPayload

*player/public/player_event.h:77*

- `MediaInfo media_info`
- `StageTimings stages`

### `struct` avbase::RenderingStartedPayload

*player/public/player_event.h:99*

- `media::DemuxerStreamType type {media::DemuxerStreamType::kUnknown}`

### `struct` avbase::RotationChangedPayload

*player/public/player_event.h:95*

- `int degrees {0}`
- `bool mirrored {false}`

### `struct` avbase::SeekCompletedPayload

*player/public/player_event.h:144*

- `int64_t request_id {0}`
  Matches the SeekTo() call; see Δ.
- `base::TimeDelta requested`
- `base::TimeDelta actual`
- `base::TimeDelta load_duration`
- `MediaError result`

### `struct` avbase::SeekRenderingStartedPayload

*player/public/player_event.h:154*

- `media::DemuxerStreamType type {media::DemuxerStreamType::kUnknown}`

### `struct` avbase::SnapshotCompletedPayload

*player/public/player_event.h:169*

- `base::TimeDelta at`
- `std::string file_path`
- `MediaError result`

### `struct` avbase::StageReachedPayload

*player/public/player_event.h:118*

- `enum class Stage`
  - `kOpenInput`
  - `kFindStreamInfo`
  - `kComponentOpen`
- `enum avbase::StageReachedPayload::Stage kOpenInput`
- `base::TimeDelta elapsed_since_prepare`

### `struct` avbase::StateChangedPayload

*player/public/player_event.h:84*

- `PlayerState from {PlayerState::kIdle}`
- `PlayerState to {PlayerState::kIdle}`

### `struct` avbase::StatsPayload

*player/public/player_event.h:174*

- `PlaybackStats stats`

### `struct` avbase::TimedTextPayload

*player/public/player_event.h:157*

- `std::string text`
- `base::TimeDelta pts`
- `base::TimeDelta duration`
- `int x {0}`
- `int y {0}`
- `int w {0}`
- `int h {0}`
- `std::vector<uint8_t> raw_ass`
  Raw ASS packet for hosts that do layout.

### `struct` avbase::TrackChangedPayload

*player/public/player_event.h:164*

- `media::DemuxerStreamType type {media::DemuxerStreamType::kUnknown}`
- `int old_index {-1}`
- `int new_index {-1}`

### `struct` avbase::VideoSizeChangedPayload

*player/public/player_event.h:90*

Merges ijkplayer's FFP_MSG_VIDEO_SIZE_CHANGED (400) and SAR_CHANGED (401), which could arrive out of order (behaviour difference Δ8).

- `media::Size coded_size`
- `media::Size natural_size`
- `media::Rational sar {1, 1}`

### `struct` avbase::WaitingPayload

*player/public/player_event.h:126*

- `enum class Reason`
  - `kBuffering`
  - `kDecoderStalled`
  - `kNetworkStalled`
  - `kKeyRequired`
- `enum avbase::WaitingPayload::Reason kBuffering`

## player/public/player_export.h

- `#define AVBASE_PLAYER_EXPORT`
## player/public/scene.h

### `class` avbase::Scene

*player/public/scene.h:288*

The player's composition surface: the one place several pictures become one.

OWNED BY THE PLAYER and returned by Player::scene().

A scene is a handle, not a promise. Whether it can currently DO anything depends on whether the running pipeline's sink can composite – and the default headless sink cannot, so a zero-configuration Player has a scene that does nothing. That is answered in words by Describe() rather than discovered by calling something and watching it fail.

THREAD SAFETY: every method may be called from any thread at any time. None blocks on the render thread and none needs GL to be current, which is what lets a UI thread move a layer while a frame is being drawn.

LIFETIME: the scene outlives pipelines. The renderer is rebuilt every Prepare and takes its sink – and therefore its engine – with it, so a host that had to re-register everything each cycle would be writing lifecycle code it has no business writing. Sources and the update callback are re-plumbed into each new pipeline automatically.

- `Scene()`
- `Scene(const Scene &)=delete`
- `Scene & operator=(const Scene &)=delete`
- `~Scene()`
- `std::shared_ptr<SceneSource> AddSource(std::string name, int z)`
  Names are not required to be unique: a name is how a host finds its own source again, not an identity, and FindSource() answers with the first.
- `bool RemoveSource(const std::shared_ptr< SceneSource > &source)`
  Drops a source from the scene; the compositor stops drawing it. Thread safety: any thread. Returns false when the handle was already gone (a double remove is not an error). Errors: none beyond that. Side effects: the source's layer is released and its outstanding frames are drained and freed.
- `std::shared_ptr<SceneSource> FindSource(std::string_view name) const`
  Looks up a registered source by name; names are not unique, so this returns the first match. Thread safety: any thread; reads the registry snapshot. Errors: returns nullptr when no source has that name. Side effects: none.
- `size_t source_count() const`
  Number of currently registered sources. Thread safety: any thread. Errors: none. Side effects: none.
- `void SetUpdateCallback(SceneUpdateCallback callback)`
  Installs the per-frame placement hook. Callable at any time; it takes effect from the next frame and replaces whatever was there, because one frame runs one hook.
- `bool SetBackgroundColor(float r, float g, float b, float a)`
  The colour an uncovered part of the composition is cleared to, in [0,1] RGBA. Opaque black by default. On the scene rather than on a layer: "what is behind everything" has one answer for the whole composition.
- `std::string Describe() const`
  One line saying whether this scene can do anything right now, and why not when it cannot. Worth attaching to a bug report: "picture-in-picture is
  not showing" has several quite different answers and this names which.

### `class` avbase::SceneFrame

*player/public/scene.h:137*

Once-per-displayed-frame access to the composition.

Given to a SceneUpdateCallback, which runs on the render thread after every source has synced and before the scene is drawn – so whatever the hook adds or moves is part of the frame that comes out.

VALID ONLY FOR THE DURATION OF THE CALLBACK, and that is a guarantee rather than a warning: SceneLayer keeps a pointer back here, and this object is marked dead when the callback returns. A handle kept past the callback fails every call instead of reaching into a frame that has gone.

- `SceneFrame()=default`
- `SceneFrame(const SceneFrame &)=delete`
- `SceneFrame & operator=(const SceneFrame &)=delete`
- `SceneLayer main_layer()`
  The layer the player's own stream went into this frame.
- `SceneLayer AddLayer(int z)`
  Adds an empty layer at |z|, or returns an invalid handle when the scene refused one. The layer stays until RemoveLayer().
- `bool RemoveLayer(SceneLayer layer)`
  Removes a layer; false when the handle was already invalid. Errors: none. Side effects: the layer stops compositing immediately.
- `size_t layer_count() const`
  Number of live layers. Errors: none. Side effects: none.
- `SceneLayer layer_of(const SceneSource &source)`
  The layer a registered source is drawing into, or an invalid handle when that source has not been synced yet – which is the first frame after AddSource(). Placement has to wait: until the engine has made the layer there is nothing to place.
- `SceneLayer layer_with_id(uint64_t id)`
  The layer with this id, or an invalid handle when it is gone – removed, or belonging to a scene that has since been rebuilt after a Reset.

  How a hook remembers a layer it made on an earlier frame. Handles cannot be kept, because the frame they were made from is dead by then; ids can, because they are never reused. Asking the scene is what turns "an id I
  wrote down" back into something usable.

### `class` avbase::SceneLayer

*player/public/scene.h:61*

A layer in a scene: where the composition draws one picture.

A HANDLE, not an owner. Copies are interchangeable and none of them keeps the layer alive – removing it from the scene is what does that, and after that every copy answers "this names nothing" instead of quietly addressing a different layer, which is why ids are handed out and never reused.

VALIDITY: only inside the callback it came from. That is enforced rather than merely documented – see SceneFrame.

- `SceneLayer()=default`
- `bool valid() const`
  Whether this handle refers to a live layer (false for a default-built or removed one). Errors: none. Side effects: none.
- `explicit operator bool() const`
  True exactly when valid(); lets a handle be used in a boolean context (if/while) without an explicit valid() call. Thread safety: any thread; reads only the immutable handle state. Errors: none. Side effects: none.
- `uint64_t id() const`
  Stable id of the layer, unique within the scene. Errors: none. Side effects: none.
- `bool SetRect(float x, float y, float w, float h)`
  Where the picture lands, in NORMALISED composition coordinates: (0,0) is the top-left of the whole composition, (1,1) the bottom-right, and w/h are fractions of it. The default is everything.
- `bool SetZ(int z)`
  Draw order: lower z is drawn first, so higher z ends up on top. Ties keep insertion order.
- `bool SetOpacity(float opacity)`
  [0,1]. Below 1 the layer blends with what is already on the target.
- `bool SetVisible(bool visible)`
  A hidden layer is skipped entirely and keeps its picture, so hiding and showing again costs nothing.
- `bool SetRotation(std::optional< int > degrees)`
  Clockwise about the layer's own centre, in degrees. nullopt – the default – follows the frame, which is where a container's display matrix ends up; a value pins the angle and stops it following. Snapped to a quarter turn, because that is the only set any stage here can carry.
- `bool SetSourceRect(float x, float y, float w, float h)`
  The part of this layer's OWN picture that is drawn, normalised over the frame as the decoder stored it. Read here rather than in the turned frame, so one rect means the same pixels whichever way the clip was shot.
- `bool SetContentMode(SceneContentMode mode)`
  Chooses how the source is fitted into the layer rect (fit / fill / stretch). Rejects an unknown mode instead of clamping. Errors: none – a bad mode returns false. Side effects: applies from the next composited frame.
- `int rotation() const`
  The angle this layer is drawn at, after the frame's own rotation and any override have been combined.
- `bool Upload(const media::VideoFrame &frame)`
  Replaces the picture this layer is showing. This is how you feed a layer nothing is publishing into – a logo, or a delayed copy of the main frame. A layer WITH a source is fed by its own producer, not here.

### `class` avbase::SceneSource

*player/public/scene.h:209*

A second (third, ...) input into the composition: where a producer that is not this player pushes frames.

The handle survives everything the scene does. Add it once, publish into it whenever, and it is re-plumbed automatically when the pipeline is rebuilt – which happens on every Prepare, because the sink belongs to the renderer and the renderer belongs to the pipeline. Hiding that churn is most of why this class exists.

- `const std::string & name() const`
  The name this source was registered under (not unique; see FindSource). Errors: none. Side effects: none.
- `int z() const`
  Stacking order the source was registered with. Errors: none. Side effects: none.
- `bool Publish(base::scoped_refptr< media::VideoFrame > frame)`
  Hands over the newest frame. Any thread, any number of them.

  False when nothing is currently taking frames – before the first Prepare, or after a Stop – in which case nothing was counted and nothing will be shown. True does NOT mean displayed: a frame still in the slot when a newer one arrives is superseded, which for a picture is the right thing to drop. frames_superseded() is how a producer finds out.

  In docs/14 stage 3 this is the ONLY way a second picture reaches the scene: the player builds the secondary leg's demuxer/decoder itself and routes its sink through here, so a host no longer creates a second Player and calls MakePublisherFactory() – that path was retired.
- `uint64_t frames_published() const`
  Frames accepted by Publish() so far. Errors: none. Side effects: none.
- `uint64_t frames_consumed() const`
  Frames the scene actually consumed for compositing. Errors: none. Side effects: none.
- `uint64_t frames_superseded() const`
  Frames dropped because a newer one arrived first. Errors: none. Side effects: none.

- `enum class SceneContentMode`
  How a layer's picture is placed inside its rect.
  - `kStretch` `= 0` — Fills the rect exactly, ignoring the picture's aspect. What every layer did before this enum existed, and still the default, so nothing moves.
  - `kFit` — Fits the WHOLE picture inside the rect, centred, leaving the scene's background on whichever axis is longer: the letterbox a player puts around a clip whose shape is not the window's.
- `using SceneUpdateCallback = base::RepeatingCallback<void( SceneFrame &frame, const base::scoped_refptr<media::VideoFrame> &main_frame)>;`
  Called on the render thread once per displayed frame.

  What belongs here is PLACEMENT – rect, opacity, z, and feeding any layer the player is not feeding. Registration does not: Scene::AddSource() is callable at any time, and doing it per frame would add the same source thousands of times.
