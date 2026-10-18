// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// avbase C ABI -- docs/08 M15, answers Q12.
//
// WHY THIS EXISTS
// The C++ API in player/public/ is the primary surface and stays that way: it
// carries std::string, Result<T> and move-only types that a C compiler cannot
// even parse. This header is a strictly NARROWER door to the same engine, for
// the hosts that cannot link C++ at all -- JNI (Android), Objective-C/Swift
// (iOS/macOS), and plain C hosts. Per docs/05 the goal is a zero-change
// migration for an existing ijkplayer's mEventHandler, so the event channel
// deliberately speaks the legacy (what, arg1, arg2, obj) quadruple rather than
// inventing a new event vocabulary.
//
// WHAT THIS IS NOT
// It is NOT a second implementation and NOT a subset with its own semantics.
// Every call below forwards to the same avbase::Player. Where the C++ API has
// an answer, this file either forwards it verbatim or deliberately does not
// expose it -- with the reason written next to it. A C ABI that silently
// diverges from its C++ original is worse than no C ABI, because the bug then
// only reproduces on one side of the boundary.
//
// COMPATIBILITY RULES (binding for any future edit here)
//   1. Opaque handles only. avbase_player* and friends are never dereferenced.
//   2. Layout is frozen. All structs are opaque; POD structs that cross the
//      boundary are append-only, and a new field is only legal at the end.
//   3. int32 codes, never C++ enums by value in a signature -- an enum's
//      underlying width is a compiler decision, not a contract.
//   4. Every string returned by this ABI is owned by the library, is NUL
//      terminated, and stays valid until the owning handle is destroyed. Copy
//      it if you need to keep it. No free() is exported: that would be a
//      cross-module allocator pairing, and mismatching them is a crash that
//      only reproduces in a release build.
//   5. No exceptions cross the boundary. Every entry point catches; a failure
//      becomes an AvbaseStatus with a code, never a C++ exception unwinding
//      through a C frame (which is undefined behaviour, not "an error").
//   6. Thread safety matches the C++ API: per-player methods are callable from
//      any thread; the event callback runs on the event sequence.
//   7. No entry point blocks the caller, with two documented exceptions --
//      avbase_player_stop_sync and avbase_player_prepare_sync -- which wait
//      for the pipeline and are bounded by their timeout argument. Every
//      other call returns immediately; work happens on engine sequences and
//      arrives through the event callback.

#ifndef AVBASE_PLAYER_PUBLIC_C_AVBASE_C_H_
#define AVBASE_PLAYER_PUBLIC_C_AVBASE_C_H_

#include <stdint.h>

#include "player/public/c/avbase_c_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

// Version of the C ABI itself, not of the engine. Bumped ONLY when an existing
// symbol changes meaning or disappears; adding symbols is a minor bump. A host
// compiled against an older header keeps working against a newer library, so
// this exists to let it decide whether to feature-detect instead.
// Side effects: none.
AVBASE_C_API const char* avbase_c_version(void);

// Mirrors avbase::GetVersion() ("0.1.0").
// Side effects: none.
AVBASE_C_API const char* avbase_engine_version(void);

// Mirrors avbase::GlobalInit(). Optional: every avbase_player_create() calls
// it. Call it yourself only to guarantee it happens before your first X11 or
// FFmpeg call.
// Side effects: idempotent global init (FFmpeg/X11 once per process).
AVBASE_C_API void avbase_global_init(void);

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

// Mirrors avbase::ErrorCode as a flat int. The C++ enum is a uint16_t list of
// 30 values; the numeric values are part of this ABI, so new codes may only be
// APPENDED here.
typedef int32_t AvbaseStatus;

// kOk -- also the only value for which the other out-params are meaningful.
#define AVBASE_OK 0
// Generic failure. Prefer the specific codes: a host that can tell "file not
// found" from "codec unsupported" can show the user something useful.
#define AVBASE_ERR_FAILED 1
#define AVBASE_ERR_INVALID_ARGUMENT 2
#define AVBASE_ERR_INVALID_STATE 3
#define AVBASE_ERR_NOT_IMPLEMENTED 4
#define AVBASE_ERR_OUT_OF_MEMORY 5
#define AVBASE_ERR_ABORTED 6
#define AVBASE_ERR_TIMEOUT 7
#define AVBASE_ERR_SOURCE_OPEN_FAILED 8
#define AVBASE_ERR_SOURCE_NOT_FOUND 9
#define AVBASE_ERR_SOURCE_READ_FAILED 10
#define AVBASE_ERR_DECODER_NOT_FOUND 11
#define AVBASE_ERR_DECODER_OPEN_FAILED 12
#define AVBASE_ERR_DECODE_FAILED 13
#define AVBASE_ERR_DECODER_UNSUPPORTED_CODEC 14
#define AVBASE_ERR_SINK_NOT_ATTACHED 15
#define AVBASE_ERR_STREAM_NOT_FOUND 16
#define AVBASE_ERR_TRACK_NOT_FOUND 17
#define AVBASE_ERR_CONFIG_INVALID 18
#define AVBASE_ERR_EOS 19

// Stable, never-null, never-freed name of a code, e.g. "kSourceNotFound".
// The name is the enum SPELLING on purpose: it is what a bug report will say,
// and it stays readable across a mapping to a legacy code in a Java handler.
// Side effects: none.
AVBASE_C_API const char* avbase_status_name(AvbaseStatus status);

// The engine's own message for |status|: a human sentence naming the likely
// cause, or "" when it has nothing to add. Never null.
// Side effects: none.
AVBASE_C_API const char* avbase_status_message(AvbaseStatus status);

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------

// Opaque. Create with avbase_player_create(); destroy with
// avbase_player_destroy(), which is bounded by PlayerConfig's shutdown
// timeout and therefore cannot hang the caller.
typedef struct AvbasePlayer AvbasePlayer;

// An opaque, host-owned window handle. Mirrors the kinds in
// media/base/native_display.h; see NativeDisplay::FromX11Window et al.
typedef struct AvbaseNativeDisplay AvbaseNativeDisplay;

// An opaque handle for a secondary media source registered with
// avbase_player_add_source(). Stable across Stop/Reset.
typedef struct AvbaseSource AvbaseSource;

// Creates a player with the default platform backends and an event handler
// already attached, so no call can race a subscription. Returns null only when
// the process is out of memory.
// Side effects: allocates a player (release with avbase_player_destroy).
AVBASE_C_API AvbasePlayer* avbase_player_create(void);

// Destroying a null player is a no-op (not an error) so that cleanup paths can
// call it unconditionally after a failed create.
// Side effects: releases every pipeline resource, bounded by the shutdown
// timeout.
AVBASE_C_API void avbase_player_destroy(AvbasePlayer* player);

// Bounded by PlayerConfig::shutdown_timeout; does not hang.
// Blocking Stop: returns once the pipeline has really stopped.
// Side effects: emits kStateChanged(kStopped).
AVBASE_C_API void avbase_player_stop_sync(AvbasePlayer* player,
                                          int64_t timeout_ms);

// SetDataSource(uri). |uri| is copied; the caller keeps ownership.
// Side effects: moves the machine to kInitialized.
AVBASE_C_API AvbaseStatus avbase_player_set_data_source(AvbasePlayer* player,
                                                        const char* uri);

// PrepareAsync(): setup starts and this returns immediately; kPrepared (what
// =200) arrives through the event callback.
// Side effects: emits kPrepared, or kError on failure.
AVBASE_C_API AvbaseStatus avbase_player_prepare(AvbasePlayer* player);
// Blocking PrepareAsync, bounded by |timeout_ms|; a successful return may
// already be kStarted when start_on_prepared is set.
// Side effects: emits kPrepared, or kError on failure.
AVBASE_C_API AvbaseStatus avbase_player_prepare_sync(AvbasePlayer* player,
                                                     int64_t timeout_ms);

// Starts playback (legal from kPrepared/kPaused/kCompleted); illegal states
// are logged and ignored, never fatal.
// Side effects: emits kStateChanged(kStarted).
AVBASE_C_API void avbase_player_start(AvbasePlayer* player);
// Pauses a running player (legal from kStarted only).
// Side effects: emits kStateChanged(kPaused).
AVBASE_C_API void avbase_player_pause(AvbasePlayer* player);
// Stops playback and tears the pipeline down; idempotent.
// Side effects: emits kStateChanged(kStopped).
AVBASE_C_API void avbase_player_stop(AvbasePlayer* player);

// Back to kIdle and reusable. Releases the current media.
// Side effects: emits kStateChanged(kIdle); drops every buffered packet.
AVBASE_C_API void avbase_player_reset(AvbasePlayer* player);

// Single-frame advance while paused.
// Side effects: emits kSeekRenderingStarted for the stepped frame.
AVBASE_C_API void avbase_player_step_once(AvbasePlayer* player);

// Fire-and-forget seek; completion arrives as an event. |position_ms| is
// relative to the media start. Deliberately no return value: the C++ API's
// Result<int64_t> exists to hand back a request id for correlation, and that
// correlation is already carried by the event payload's request_id -- a host
// that needs it should use the richer C++ API.
// Side effects: emits kSeekCompleted (what=600, arg1 = actual ms).
AVBASE_C_API void avbase_player_seek_to(AvbasePlayer* player,
                                        int64_t position_ms);

// |rate| is clamped to [0.25, 4.0] as the C++ API does.
// Side effects: takes effect on the next rendered frame.
AVBASE_C_API void avbase_player_set_playback_rate(AvbasePlayer* player,
                                                  double rate);

// |volume| is clamped to [0.0, 1.0].
// Side effects: applies to the audio sink immediately.
AVBASE_C_API void avbase_player_set_volume(AvbasePlayer* player, double volume);
// Mutes or unmutes without touching the volume level.
// Side effects: applies to the audio sink immediately.
AVBASE_C_API void avbase_player_set_muted(AvbasePlayer* player, int muted);

// |count| < 0 means infinite.
// Side effects: emits kCompleted (what=300) once per finished pass.
AVBASE_C_API void avbase_player_set_loop_count(AvbasePlayer* player, int count);

// |stream_type| is an AVBASE_STREAM_* value below. |stream_index| is the index
// reported by the prepared media, NOT an avbase_source index.
// Side effects: emits kTrackChanged; decoders are rebuilt lazily.
AVBASE_C_API AvbaseStatus avbase_player_select_track(AvbasePlayer* player,
                                                     int stream_type,
                                                     int stream_index);

// Attaches |display| as the video output, or detaches when null (audio keeps
// playing -- the documented background-playback case). The player holds a
// reference; destroying the display handle afterwards is safe.
// Side effects: re-targets the video sink on the next frame.
AVBASE_C_API void avbase_player_set_video_surface(AvbasePlayer* player,
                                                  AvbaseNativeDisplay* display);

// Writes the frame at |at_ms| to |file_path| (PNG); completion arrives as
// kSnapshotCompleted (what=1000, arg2 = Status).
// Side effects: writes one file; emits kSnapshotCompleted.
AVBASE_C_API AvbaseStatus avbase_player_take_snapshot(AvbasePlayer* player,
                                                      int64_t at_ms,
                                                      const char* file_path);

// Forces the network retry path now. Replaces ijkplayer's write-only
// FFP_PROP_INT64_IMMEDIATE_RECONNECT property.
// Side effects: re-runs the reconnect ladder.
AVBASE_C_API void avbase_player_reconnect_now(AvbasePlayer* player);

// ---- Queries: lock-free snapshots, callable from any thread ----------------

// Mirrors PlayerState. See AVBASE_STATE_* below.
// Side effects: none.
AVBASE_C_API int avbase_player_state(const AvbasePlayer* player);
// Current playback position, including any seek in flight.
// Side effects: none.
AVBASE_C_API int64_t avbase_player_media_time_ms(const AvbasePlayer* player);
// How much media is buffered ahead of the playhead.
// Side effects: none.
AVBASE_C_API int64_t avbase_player_buffered_time_ms(const AvbasePlayer* player);
// Total duration in ms, or zero for a live source.
// Side effects: none.
AVBASE_C_API int64_t avbase_player_duration_ms(const AvbasePlayer* player);
// Nonzero when the source is live (no seekable timeline).
// Side effects: none.
AVBASE_C_API int avbase_player_is_live(const AvbasePlayer* player);
// Nonzero while playback is actively running.
// Side effects: none.
AVBASE_C_API int avbase_player_is_playing(const AvbasePlayer* player);
// Display width after rotation/aspect corrections.
// Side effects: none.
AVBASE_C_API int32_t avbase_player_video_width(const AvbasePlayer* player);
// Display height after rotation/aspect corrections.
// Side effects: none.
AVBASE_C_API int32_t avbase_player_video_height(const AvbasePlayer* player);
// Clockwise rotation the host must apply, in degrees.
// Side effects: none.
AVBASE_C_API int32_t avbase_player_video_rotation(const AvbasePlayer* player);
// Effective rate, after clamping.
// Side effects: none.
AVBASE_C_API double avbase_player_playback_rate(const AvbasePlayer* player);
// Effective volume, after clamping.
// Side effects: none.
AVBASE_C_API double avbase_player_volume(const AvbasePlayer* player);
// Current mute state.
// Side effects: none.
AVBASE_C_API int avbase_player_muted(const AvbasePlayer* player);

// Copies at most |capacity| bytes (including the NUL) of the human-readable
// media title into |out| and returns the number of bytes the full string needs,
// excluding the NUL. A return value >= |capacity| means it was truncated, so
// the caller can size a buffer and retry -- guessing a size is how the legacy
// path used to truncate paths silently.
// Side effects: none.
AVBASE_C_API int32_t avbase_player_title(AvbasePlayer* player, char* out,
                                         int32_t capacity);

// Full internal state as JSON (~1 ms). The first thing to attach to a bug
// report. Same copy-into-your-buffer contract as the title.
// Side effects: none.
AVBASE_C_API int32_t avbase_player_dump_diagnostics(const AvbasePlayer* player,
                                                    char* out,
                                                    int32_t capacity);

// ---------------------------------------------------------------------------
// Media info
// ---------------------------------------------------------------------------

// Fills a caller-allocated buffer with the media's duration, dimensions and
// track counts. A snapshot, not a live view. |out| may be null only if all
// pointers passed in are null (a no-op way to probe support).
typedef struct AvbaseMediaInfo {
  int64_t duration_ms;
  int64_t bitrate;
  // Live streams report a zero duration, so this flag -- not the duration --
  // is how a host tells them apart.
  int is_live;
  int seekable;
  int32_t video_width;
  int32_t video_height;
  int32_t video_rotation;
  // Counts, so a host can size a track menu without parsing anything. Taken
  // from the FIRST stream of each kind for the size/rotation fields.
  int32_t video_track_count;
  int32_t audio_track_count;
  int32_t subtitle_track_count;
} AvbaseMediaInfo;

// Available only after avbase_player_prepare() has reported kPrepared;
// returns AVBASE_ERR_INVALID_STATE before that.
// Side effects: none.
AVBASE_C_API AvbaseStatus
avbase_player_get_media_info(const AvbasePlayer* player, AvbaseMediaInfo* out);

// ---------------------------------------------------------------------------
// Multi-source composition
// ---------------------------------------------------------------------------

// Registers a SECONDARY source and returns its layer handle, or null when the
// player is no longer in a state that accepts one. Call before prepare().
//
// The returned handle stays valid across stop/reset (the scene keeps it) but
// must be released with avbase_source_release(). Source 0 is the player's own
// avbase_player_set_data_source(); each call adds the next id.
// Side effects: allocates a scene layer visible after the next composite.
AVBASE_C_API AvbaseSource* avbase_player_add_source(AvbasePlayer* player,
                                                    const char* uri,
                                                    const char* name,
                                                    int32_t z_order);

// Releases a source handle. Null is a no-op. Does not unregister the source;
// it only drops this reference (matching the C++ shared_ptr semantics).
// Side effects: none (the source outlives the handle).
AVBASE_C_API void avbase_source_release(AvbaseSource* source);

// Re-points a registered source at a new URI; takes effect at the next
// prepare.
// Side effects: invalidates the source's demuxer until re-prepared.
AVBASE_C_API AvbaseStatus avbase_source_set_uri(AvbaseSource* source,
                                                const char* uri);

// ---------------------------------------------------------------------------
// Native display
// ---------------------------------------------------------------------------

// Wrappers over media::NativeDisplay. Each takes the raw handle of that window
// system as an opaque pointer -- deliberately NOT typed, so that including
// this header never drags X11/Wayland/SDL/Win32 headers into a C host.

// X11: |display| is a `Display*`, |window| is an XID. The pointers are copied,
// not retained: the window must outlive the display handle.
// Side effects: allocates a display handle (release with
// avbase_display_release).
AVBASE_C_API AvbaseNativeDisplay* avbase_display_from_x11(void* x_display,
                                                          uintptr_t window);

// Wayland: |surface| is a `wl_surface*`. Wayland cannot report a surface size,
// so |width|/|height| must be supplied by the embedder.
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay* avbase_display_from_wayland(void* wl_display,
                                                              void* wl_surface,
                                                              int32_t width,
                                                              int32_t height);

// SDL2: |sdl_window| is an `SDL_Window*`.
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay* avbase_display_from_sdl2(void* sdl_window);
// GLFW: |glfw_window| is a `GLFWwindow*`.
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay* avbase_display_from_glfw(void* glfw_window);
// EGL: |egl_surface| is an `EGLSurface` as an opaque pointer.
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay* avbase_display_from_egl(void* egl_surface);

// Windows HWND. Kept explicit rather than folded into the Wrap() escape hatch
// because it is the one kind every Windows host asks for.
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay* avbase_display_from_hwnd(void* hwnd);

// Android ANativeWindow / Surface.
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay*
avbase_display_from_android_native_window(void* window);
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay*
avbase_display_from_android_surface(void* surface);

// iOS: a CAMetalLayer or CAEAGLLayer.
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay*
avbase_display_from_cametal_layer(void* layer);
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay* avbase_display_from_caeagl_layer(void* layer);

// The generic door: |kind| is an AVBASE_DISPLAY_* value and |release| is an
// optional callback run on the render sequence when the last reference dies
// (may be null). This is how a host with its own window abstraction plugs in
// without asking this library for a new entry point per toolkit.
//
// |release| receives BOTH |raw| (the pointer that was wrapped) and |user_data|
// (whatever the host passed). Passing user_data separately is the whole point:
// a C host that needs cleanup state has nowhere else to keep it, and deriving
// it from |raw| would be a guess. It runs on the render sequence, so it must
// not block.
// Side effects: allocates a display handle; |release| runs when the last
// reference dies.
AVBASE_C_API AvbaseNativeDisplay*
avbase_display_wrap(void* raw, int kind,
                    void (*release)(void* raw, void* user_data),
                    void* user_data);

// A valid "render nowhere" target; the sink discards frames but audio keeps
// playing.
// Side effects: allocates a display handle.
AVBASE_C_API AvbaseNativeDisplay* avbase_display_none(void);

// Drops one reference; null is a no-op.
// Side effects: frees the handle when the last reference dies.
AVBASE_C_API void avbase_display_release(AvbaseNativeDisplay* display);
// The AVBASE_DISPLAY_KIND_* this handle was created with.
// Side effects: none.
AVBASE_C_API int avbase_display_kind(const AvbaseNativeDisplay* display);

// 0 when the display can render, non-zero when it is None() or failed to be
// created. Never use the pointer itself for this: a null pointer here means
// "could not create", and None() is a VALID non-rendering target -- the two
// are different states and conflating them is what makes "the PiP is not
// showing" undiagnosable (see the note on Player::scene()).
// Side effects: none.
AVBASE_C_API int avbase_display_is_valid(const AvbaseNativeDisplay* display);

// Copy-into-your-buffer contract, same as the title: returns the full length
// excluding the NUL, and a value >= |capacity| means truncated.
// Side effects: none.
AVBASE_C_API int32_t avbase_display_debug_string(
    const AvbaseNativeDisplay* display, char* out, int32_t capacity);

// Mirrors media::NativeDisplayKind. Frozen: append only.
#define AVBASE_DISPLAY_KIND_NONE 0
#define AVBASE_DISPLAY_KIND_X11_WINDOW 1
#define AVBASE_DISPLAY_KIND_WAYLAND_SURFACE 2
#define AVBASE_DISPLAY_KIND_SDL2_WINDOW 3
#define AVBASE_DISPLAY_KIND_DRM_MASTER 4
#define AVBASE_DISPLAY_KIND_GBM_DEVICE 5
#define AVBASE_DISPLAY_KIND_ANDROID_NATIVE_WINDOW 6
#define AVBASE_DISPLAY_KIND_ANDROID_SURFACE 7
#define AVBASE_DISPLAY_KIND_CAMETAL_LAYER 8
#define AVBASE_DISPLAY_KIND_CAEAGL_LAYER 9
#define AVBASE_DISPLAY_KIND_HWND 10
#define AVBASE_DISPLAY_KIND_GLFW_WINDOW 11
#define AVBASE_DISPLAY_KIND_EGL_WINDOW 12

// Mirrors media::DemuxerStreamType. Frozen: append only.
#define AVBASE_STREAM_UNKNOWN 0
#define AVBASE_STREAM_AUDIO 1
#define AVBASE_STREAM_VIDEO 2
#define AVBASE_STREAM_TEXT 3

// Mirrors PlayerState. Frozen: append only.
#define AVBASE_STATE_IDLE 0
#define AVBASE_STATE_INITIALIZED 1
#define AVBASE_STATE_PREPARING 2
#define AVBASE_STATE_PREPARED 3
#define AVBASE_STATE_STARTED 4
#define AVBASE_STATE_PAUSED 5
#define AVBASE_STATE_BUFFERING 6
#define AVBASE_STATE_STOPPING 7
#define AVBASE_STATE_STOPPED 8
#define AVBASE_STATE_COMPLETED 9
#define AVBASE_STATE_ERROR 10
#define AVBASE_STATE_END 11

// ---------------------------------------------------------------------------
// Events -- the legacy (what, arg1, arg2, obj) quadruple
// ---------------------------------------------------------------------------
//
// This is the whole reason the C ABI exists (docs/05): an ijkplayer's Java
// mEventHandler is already written against these codes, so forwarding them
// unchanged is what makes the migration a drop-in. The numbers below are the
// ones ToLegacyEvent() in player/player_event.cc emits; they are an ABI
// contract, so they are duplicated here rather than referenced.
//
//   0     kFlushed
//   100   kError          arg1 = ErrorCode, obj = message
//   200   kPrepared
//   300   kCompleted
//   400   kVideoSizeChanged arg1 = width, arg2 = height
//   402/403 kRenderingStarted (video / audio)
//   404   kRotationChanged arg1 = degrees
//   405/406 kDecodingStarted (audio / video)
//   407/408/409 kStageReached (open-input / find-stream-info / other)
//   500/501 kBufferingStarted / kBufferingEnded
//   502   kBufferingProgress arg1 = buffered ahead ms, arg2 = percent
//   600   kSeekCompleted  arg1 = actual ms, arg2 = Status
//   700   kStateChanged
//   800   kTimedText      obj = the cue text
//   900   kAccurateSeekCompleted arg1 = position ms
//   1000  kSnapshotCompleted arg1 = at ms, arg2 = Status, obj = file path
//   10001 kDecoderOpened
//
// Events with no ijkplayer equivalent (kDecoderFallback, kWaiting,
// kBufferedBytesUpdate, kBufferedTimeUpdate, kSeekRenderingStarted,
// kTrackChanged, kStats, kHdrUnsupported) are reported with what == 0, and
// their detail is available in full JSON via avbase_player_dump_diagnostics().
// Dropping them silently is the alternative; what == 0 makes "this host does
// not know that event" observable instead of invisible.

// |obj| is owned by the library and valid only for the duration of the call.
typedef void (*AvbaseEventCallback)(void* user_data, int what, int64_t arg1,
                                    int64_t arg2, const char* obj);

// Replaces any previous callback (pass null to remove it). The callback runs
// on the event sequence, never on a pipeline sequence, and it is safe to call
// any avbase_player_* function from inside it -- including stop and reset.
// That is the deadlock fix over ijkplayer's message_loop; do not reintroduce
// a "must not reenter" rule here.
// Side effects: starts event delivery to |cb|.
AVBASE_C_API void avbase_player_set_event_callback(AvbasePlayer* player,
                                                   AvbaseEventCallback cb,
                                                   void* user_data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AVBASE_PLAYER_PUBLIC_C_AVBASE_C_H_
