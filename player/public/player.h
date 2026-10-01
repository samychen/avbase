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
  Status SetDataSource(std::string_view uri);
  Status SetDataSource(const DataSourceDescriptor& descriptor);
  Status PrepareAsync();
  Status PrepareSync(base::TimeDelta timeout = base::Seconds(30));
  void Start();
  void Pause();
  void Stop();                                   // Non-blocking.
  void StopSync(base::TimeDelta timeout);
  void Reset();                                  // Back to kIdle, reusable.
  // Blocks until playback reaches a terminal state. Convenience for CLIs and
  // tests; production hosts use SetEventHandler() with their own loop.
  void RunUntilIdle();

  // ---- Playback control ----------------------------------------------------
  using SeekCB = base::OnceCallback<void(Status)>;
  // Runs |cb| on the event sequence when the seek finishes; never inline.
  // Returns the request id, which also appears in SeekCompletedPayload so a
  // caller doing rapid seeks can match results to requests.
  Result<int64_t> SeekTo(base::TimeDelta position, SeekMode mode, SeekCB cb);
  void SeekTo(base::TimeDelta position);
  void StepOnce();                               // Single-frame advance while paused.
  void SetPlaybackRate(double rate);             // [0.25, 4.0]
  void SetVolume(double volume);                 // [0.0, 1.0]
  void SetMuted(bool muted);
  void SetLoopCount(int count);                  // -1 = infinite
  Status SelectTrack(media::DemuxerStreamType type, int stream_index);
  // May be called while playing. Pass a null display to render nowhere while
  // keeping audio (useful for background playback).
  void SetVideoSurface(base::scoped_refptr<NativeDisplay> display);
  Status TakeSnapshot(base::TimeDelta at, std::string file_path);
  // Forces the retry path of NetConfig::reconnect immediately. Replaces
  // ijkplayer's write-only FFP_PROP_INT64_IMMEDIATE_RECONNECT property.
  void ReconnectNow();

  // ---- Queries: all thread-safe, non-blocking, lock-free snapshots ---------
  PlayerState state() const;
  std::optional<MediaInfo> media_info() const;
  base::TimeDelta GetMediaTime() const;
  base::TimeDelta GetBufferedTime() const;
  base::TimeDelta GetDuration() const;
  bool is_live() const;
  bool IsPlaying() const;
  media::Size video_natural_size() const;
  media::Size video_coded_size() const;
  int video_rotation() const;
  double playback_rate() const;
  double volume() const;
  bool muted() const;
  PlaybackStats GetPlaybackStats() const;
  // Full internal state as JSON, ~1 ms, safe to call at any time. The first
  // thing to attach to a bug report; see docs/10 §10.
  std::string DumpDiagnostics() const;

  // ---- Events --------------------------------------------------------------
  using EventHandler = base::RepeatingCallback<void(const PlayerEvent&)>;
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
    void Reset();
    bool active() const;

   private:
    friend class Player;
    class Impl;
    std::unique_ptr<Impl> impl_;
  };
  [[nodiscard]] Subscription AddObserver(PlayerObserver* observer);

  // ---- Runtime configuration ----------------------------------------------
  // Only whitelisted fields may change while running; anything else returns
  // ErrorCode::kInvalidState naming the offending field.
  Status UpdateConfig(const PlayerConfig& config);
  const PlayerConfig& config() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<Deps> deps_;
};

// Builder form, for injecting custom backends and for migrating string options.
class AVBASE_PLAYER_EXPORT PlayerBuilder {
 public:
  PlayerBuilder();
  PlayerBuilder(const PlayerBuilder&) = delete;
  PlayerBuilder& operator=(const PlayerBuilder&) = delete;
  ~PlayerBuilder();

  PlayerBuilder& SetConfig(PlayerConfig config);
  // Unknown keys are rejected with a "did you mean" suggestion (Δ2).
  Status SetOption(OptionCategory category, std::string_view key,
                   const OptionValue& value);
  PlayerBuilder& SetDeps(std::unique_ptr<Deps> deps);
  PlayerBuilder& SetLogLevel(base::logging::LogSeverity severity);
  Result<std::unique_ptr<Player>> Build();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_PLAYER_H_
