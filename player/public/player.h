// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PUBLIC_PLAYER_H_
#define AVBASE_PLAYER_PUBLIC_PLAYER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/video_frame.h"
#include "player/public/deps.h"
#include "player/public/error.h"
#include "player/public/global.h"
#include "player/public/media_info.h"
#include "player/public/native_display.h"
#include "player/public/option_registry.h"
#include "player/public/playback_stats.h"
#include "player/public/player_config.h"
#include "player/public/player_event.h"
#include "player/public/player_export.h"

namespace avbase {
namespace media {
class DataSource;
}  // namespace media

class AVBASE_PLAYER_EXPORT Player;
class Scene;
class SceneSource;

// Re-exported from media/base so that SDK callers can spell it
// avbase::DataSourceDescriptor without reaching into the media layer. The type
// lives there because media::Demuxer takes it and media/ must not depend on
// player/ (docs/02 §2.1).
using media::DataSourceDescriptor;

// The SDK entry point: one Player owns a complete pipeline (demuxer, decoders,
// A/V sync, output sinks).
//
// Thread safety: every method may be called from any thread and none block,
// with the single exception of PrepareSync() and StopSync(). Destruction is
// bounded by PlayerConfig::shutdown_timeout, so ~Player() cannot hang the
// caller (behaviour difference Δ15).
//
// WATCH OUT: do NOT give a Player static storage duration with automatic
// destruction. ~Player() joins the event thread (and, via the pipeline, the
// media/video/audio threads). Running that join from C++ static-destruction
// phase -- a `static Player` torn down at program exit -- crashes empirically
// (bench/bench_event_hub.cc measured SIGABRT/SIGILL on exit for the analogous
// static EventHub). If a Player must outlive everything, leak it (e.g. `new
// Player(...)` and never delete) rather than letting its destructor run at
// atexit.
//
// Minimal use:
//
//   avbase::Player player;
//   player.SetEventHandler([&player](const avbase::PlayerEvent& e) {
//     if (e.type == avbase::EventType::kPrepared) player.Start();
//   });
//   AVBASE_RETURN_IF_ERROR(player.SetDataSource("video.mp4"));
//   AVBASE_RETURN_IF_ERROR(player.PrepareAsync());
//
// The event handler runs on the event dispatch sequence, never on a pipeline
// sequence, and it is safe to call any Player method from inside it — including
// Stop() and Reset(). That fixes a real deadlock in ijkplayer, whose
// message_loop dispatches while holding the player mutex.
class AVBASE_PLAYER_EXPORT Player {
 public:
  // Uses auto-detected platform backends. Inject custom ones via the
  // Deps overload or PlayerBuilder.
  Player();
  explicit Player(const PlayerConfig& config);
  Player(const PlayerConfig& config, std::unique_ptr<Deps> deps);

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  ~Player();

  // ---- Lifecycle -----------------------------------------------------------
  // Points the player at |uri|; the source is only opened by Prepare. An
  // empty URI is rejected without touching state.
  // Errors: kInvalidArgument for an empty URI.
  // Side effects: moves the machine to kInitialized.
  Status SetDataSource(std::string_view uri);
  // Descriptor form: same contract, for callers that already resolved the
  // source (memory buffer, custom downloader, secondary source).
  // Errors: kInvalidArgument for a malformed descriptor.
  // Side effects: moves the machine to kInitialized.
  Status SetDataSource(const DataSourceDescriptor& descriptor);
  // Starts demux/decoder setup and returns immediately; kPrepared arrives
  // through the event handler (or the return of PrepareSync).
  // Errors: kInvalidState from any state but kIdle/kInitialized.
  // Side effects: emits kPrepared, or kError on failure.
  Status PrepareAsync();
  // Blocking form of PrepareAsync with |timeout|. The autostart runs inside
  // it (see Start's WATCH OUT), so a successful return may already be
  // kStarted.
  // Errors: kTimeout when |timeout| elapses first; kInvalidState from any
  // state but kIdle/kInitialized.
  // Side effects: emits kPrepared, or kError on failure.
  Status PrepareSync(base::TimeDelta timeout = base::Seconds(30));
  // Legal only from kPrepared, kPaused and kCompleted; from any other state
  // it logs "Player::Start in state <s> is not legal" and returns without
  // changing anything (that rejection is deliberate, see
  // player/state_machine.h).
  //
  // WATCH OUT: with the default start_on_prepared=true, the autostart runs
  // inside PrepareSync() before it returns, so the Start() you write right
  // after PrepareSync() is the ILLEGAL one, not the missing one. Guard it
  // (examples/pull_frames.cc) or set start_on_prepared=false if you mean to
  // start playback yourself. ijkplayer cannot hit this at all: its start
  // only clears pause_req and is not a state transition.
  //
  // Errors: none -- an illegal state is logged and ignored (the rejection
  // is deliberate, see player/state_machine.h).
  // Side effects: emits kStateChanged(kStarted); from kCompleted replays
  // from the beginning.
  void Start();
  // Pauses a running player; legal only from kStarted.
  // Errors: none -- an illegal state is logged and ignored.
  // Side effects: emits kStateChanged(kPaused).
  void Pause();
  // Stops playback and tears the pipeline down, keeping the configured
  // backends. Idempotent: stopping an already-stopped player is a no-op.
  // Errors: none.
  // Side effects: emits kStateChanged(kStopped).
  void Stop();
  // Blocking Stop: returns once the pipeline has really stopped, bounded by
  // |timeout| so it cannot hang a shutdown path.
  // Errors: kTimeout if the pipeline does not stop within |timeout|.
  // Side effects: emits kStateChanged(kStopped).
  void StopSync(base::TimeDelta timeout);
  // Releases the current source and returns to kIdle, keeping config and
  // backends: the player is reusable afterwards.
  // Errors: none.
  // Side effects: emits kStateChanged(kIdle); drops every buffered packet.
  void Reset();
  // Blocks until playback reaches a terminal state. Convenience for CLIs and
  // tests; production hosts use SetEventHandler() with their own loop.
  // Errors: none (the terminal error, if any, arrives as an event).
  // Side effects: none beyond what playback itself emits.
  void RunUntilIdle();

  // ---- Playback control ----------------------------------------------------
  using SeekCB = base::OnceCallback<void(Status)>;
  // Runs |cb| on the event sequence when the seek finishes; never inline.
  // Returns the request id, which also appears in SeekCompletedPayload so a
  // caller doing rapid seeks can match results to requests.
  // SeekMode::kAccurate is additionally gated by config.seek.accurate
  // ("enable-accurate-seek"). With that switched off an accurate request does
  // not fail: it lands on the keyframe, and requested-vs-actual in the
  // completion is how you tell.
  // Errors: kInvalidState when not seekable in the current state.
  // Side effects: emits kSeekCompleted (or kAccurateSeekCompleted); the
  // callback runs on the event sequence, never inline.
  Result<int64_t> SeekTo(base::TimeDelta position, SeekMode mode, SeekCB cb);
  // Fire-and-forget seek to the nearest keyframe at or before |position|.
  // Errors: kInvalidState when not seekable in the current state.
  // Side effects: emits kSeekCompleted with the landed position.
  void SeekTo(base::TimeDelta position);
  // Advances exactly one frame while paused; no-op unless kPaused.
  // Errors: none -- an illegal state is a no-op, not a failure.
  // Side effects: emits kSeekRenderingStarted for the stepped frame.
  void StepOnce();
  // Sets the playback rate; the valid range is [0.25, 4.0] and out-of-range
  // values are clamped, not rejected.
  // Errors: none.
  // Side effects: takes effect on the next rendered frame; audio is
  // time-stretched to keep pitch (docs/04 §5).
  void SetPlaybackRate(double rate);
  // Sets the output volume in [0.0, 1.0]; out-of-range values are clamped.
  // Errors: none.
  // Side effects: applies to the audio sink immediately.
  void SetVolume(double volume);
  // Mutes or unmutes output without touching the volume level.
  // Errors: none.
  // Side effects: applies to the audio sink immediately.
  void SetMuted(bool muted);
  // Sets how many times playback repeats after the first pass; -1 means
  // infinite.
  // Errors: none.
  // Side effects: emits kCompleted once per finished pass.
  void SetLoopCount(int count);
  // Selects one track of |type| by its index in media_info(); disabled
  // tracks keep their place, so switching back is cheap.
  // Errors: kTrackNotFound for an index the media does not have.
  // Side effects: emits kTrackChanged; decoders are rebuilt lazily.
  Status SelectTrack(media::DemuxerStreamType type, int stream_index);
  // May be called while playing. Pass a null display to render nowhere while
  // keeping audio (useful for background playback).
  // Errors: none.
  // Side effects: re-targets the video sink on the next frame; emits
  // kRenderingStarted when output resumes.
  void SetVideoSurface(base::scoped_refptr<NativeDisplay> display);
  // Writes the frame at |at| to |file_path| (PNG). The write happens on a
  // worker sequence; completion arrives as an event.
  // Errors: kInvalidState when the position is not decodable yet.
  // Side effects: emits kSnapshotCompleted (success or failure).
  Status TakeSnapshot(base::TimeDelta at, std::string file_path);
  // Forces the retry path of NetConfig::reconnect immediately. Replaces
  // ijkplayer's write-only FFP_PROP_INT64_IMMEDIATE_RECONNECT property.
  // Errors: none -- without a live source the request is dropped.
  // Side effects: re-runs the reconnect ladder (docs/04 §7).
  void ReconnectNow();

  // ---- Queries: all thread-safe, non-blocking, lock-free snapshots ---------
  // Current machine state; kIdle until the first Prepare.
  // Errors: none. Side effects: none.
  PlayerState state() const;
  // Media metadata once prepared (duration, streams, rotation...); nullopt
  // before that.
  // Errors: none. Side effects: none.
  std::optional<MediaInfo> media_info() const;
  // Current playback position, including any seek in flight.
  // Errors: none. Side effects: none.
  base::TimeDelta GetMediaTime() const;
  // How much media is buffered ahead of the playhead.
  // Errors: none. Side effects: none.
  base::TimeDelta GetBufferedTime() const;
  // Total duration, or zero for a live source.
  // Errors: none. Side effects: none.
  base::TimeDelta GetDuration() const;
  // Whether the current source is live (no seekable timeline).
  // Errors: none. Side effects: none.
  bool is_live() const;
  // Whether playback is actively running (kStarted).
  // Errors: none. Side effects: none.
  bool IsPlaying() const;
  // Display size after rotation/aspect corrections.
  // Errors: none. Side effects: none.
  media::Size video_natural_size() const;
  // Coded size before rotation/aspect corrections.
  // Errors: none. Side effects: none.
  media::Size video_coded_size() const;
  // Clockwise rotation the host must apply, in degrees.
  // Errors: none. Side effects: none.
  int video_rotation() const;
  // Effective rate, after clamping.
  // Errors: none. Side effects: none.
  double playback_rate() const;
  // Effective volume, after clamping.
  // Errors: none. Side effects: none.
  double volume() const;
  // Current mute state.
  // Errors: none. Side effects: none.
  bool muted() const;
  // Current counters (frames, drops, buffered ranges) as one snapshot.
  // Errors: none. Side effects: none.
  PlaybackStats GetPlaybackStats() const;
  // Full internal state as JSON, ~1 ms, safe to call at any time. The first
  // thing to attach to a bug report; see docs/10 §10.
  std::string DumpDiagnostics() const;

  // ---- Composition ---------------------------------------------------------
  // Multi-layer output: add picture-in-picture, a second stream, a logo, or
  // drive layer placement per frame. See player/public/scene.h.
  //
  // Never null, and never invalidated -- the scene outlives every pipeline, so
  // a source registered once survives Stop(), Reset() and another Prepare().
  //
  // Whether it can currently DO anything is a separate question the scene
  // answers itself, because the default headless sink cannot composite and
  // "the PiP is not showing" then has a completely different cause from "the
  // layer is in the wrong place". Scene::Describe() names which.
  //
  // Callable from any thread; none of the scene's methods block on the render
  // thread. This header deliberately forward-declares Scene rather than
  // including it -- a caller that never composes should not have to compile
  // the composition API.
  // Errors: none. Side effects: none (the scene is inert until layers are
  // added and a sink that can composite is attached).
  Scene* scene();

  // ---- Multi-source composition (docs/14 stage 3) --------------------------
  // Registers a SECONDARY data source on this player and a scene layer for it,
  // returning the layer's handle. At PrepareAsync the player builds a demuxer
  // for |descriptor| and routes its decoded video into the layer this returns,
  // which the host places via SceneFrame::layer_of() in the per-frame hook.
  //
  // Source 0 is this player's own SetDataSource(); each AddSource() adds the
  // next source (id 1, then 2, ...). All sources are composited by the SAME
  // pipeline and the SAME master clock -- there is no second Player, and the
  // "second Player + mailbox" detour of earlier builds is retired.
  //
  // Call before PrepareAsync, from kIdle or kInitialized; a later call is
  // rejected with kInvalidState and returns null. The returned handle stays
  // valid across Stop()/Reset()/re-Prepare() -- the scene keeps it -- but the
  // demuxer is only rebuilt if the source is re-registered after a Reset().
  // Side effects: allocates a scene layer (visible after the next composite)
  // and, at Prepare, a second demuxer leg sharing the master clock.
  std::shared_ptr<SceneSource> AddSource(const DataSourceDescriptor& descriptor,
                                         std::string name, int z);

  // ---- Events --------------------------------------------------------------
  using EventHandler = base::RepeatingCallback<void(const PlayerEvent&)>;
  // Installs the event handler. Replaces any previous one; the handler runs
  // on the event dispatch sequence and may call any Player method (the
  // class comment explains why that cannot deadlock).
  // Errors: none. Side effects: none until the first event.
  void SetEventHandler(EventHandler handler);

  // RAII subscription handle: unsubscribes on destruction.
  class AVBASE_PLAYER_EXPORT Subscription {
   public:
    Subscription();
    Subscription(Subscription&&) noexcept;
    Subscription& operator=(Subscription&&) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription();
    // Drops the subscription early; the destructor does the same.
    // Errors: none. Side effects: stops event delivery to the observer.
    void Reset();
    // Whether the subscription still receives events.
    // Errors: none. Side effects: none.
    bool active() const;

   private:
    friend class Player;
    class Impl;
    std::unique_ptr<Impl> impl_;
  };
  // Subscribes |observer| to every PlayerEvent; keep the returned handle
  // alive for as long as the observer should receive them.
  // Errors: kInvalidArgument for a null observer.
  // Side effects: starts event delivery to |observer|.
  [[nodiscard]] Subscription AddObserver(PlayerObserver* observer);

  // ---- Runtime configuration ----------------------------------------------
  // Only whitelisted fields may change while running; anything else returns
  // ErrorCode::kInvalidState naming the offending field.
  // Side effects: applies at the next pipeline decision point (buffer
  // watermarks at the next refill, render settings at the next frame).
  Status UpdateConfig(const PlayerConfig& config);
  // The config in force; the reference stays valid for the player's
  // lifetime and is never mutated in place.
  // Errors: none. Side effects: none.
  const PlayerConfig& config() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<Deps> deps_;
};

// Builder form, for injecting custom backends and for migrating string options.
// Not thread-safe: configure, Build(), then hand the Player to its owner.
class AVBASE_PLAYER_EXPORT PlayerBuilder {
 public:
  PlayerBuilder();
  PlayerBuilder(const PlayerBuilder&) = delete;
  PlayerBuilder& operator=(const PlayerBuilder&) = delete;
  ~PlayerBuilder();

  // Takes ownership of |config| (a copy is kept).
  // Errors: none (validation happens in Build).
  // Side effects: none until Build.
  PlayerBuilder& SetConfig(PlayerConfig config);
  // Migrates one ijkplayer-style string option; unknown keys are rejected
  // with a "did you mean" suggestion (Δ2).
  // Errors: kInvalidArgument for an unknown key or malformed value.
  // Side effects: records the option for Build.
  Status SetOption(OptionCategory category, std::string_view key,
                   const OptionValue& value);
  // Installs custom backends; the defaults are used for every factory the
  // Deps leaves null.
  // Errors: none.
  // Side effects: none until Build.
  PlayerBuilder& SetDeps(std::unique_ptr<Deps> deps);
  // Sets the initial log severity.
  // Errors: none.
  // Side effects: applies from the first log call after Build.
  PlayerBuilder& SetLogLevel(base::logging::LogSeverity severity);
  // Produces the Player. Exactly one Build per builder.
  // Errors: kInvalidState on a second Build; configuration errors as
  // kInvalidArgument (see PlayerConfig::Issues()).
  // Side effects: hands the configured backends to the new Player.
  Result<std::unique_ptr<Player>> Build();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_PLAYER_H_
