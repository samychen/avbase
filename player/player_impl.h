// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_PLAYER_PLAYER_IMPL_H_
#define IJKPP_PLAYER_PLAYER_IMPL_H_

#include <atomic>
#include <map>
#include <memory>

#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/media_info.h"
#include "media/base/pipeline.h"
#include "media/filters/pipeline_impl.h"
#include "media/renderers/default_renderer_factory.h"
#include "player/event_hub.h"
#include "player/public/player.h"
#include "player/public/player_config.h"
#include "player/state_machine.h"

namespace ijkpp {

// Everything behind the Player facade: the S1/S3/S4 threads, the event hub,
// the renderer factory and the pipeline.
//
// THREADING. SetDataSource/Prepare/Start/Pause/Stop/SeekTo are called from
// any thread; they validate state, mutate it under state_lock_, and post the
// real work to the pipeline (media sequence). The callbacks from the pipeline
// arrive on the media sequence and are translated into PlayerEvents posted to
// the event hub, so user code never runs on a media sequence (docs/04 §1).
class PlayerImpl final : public media::Pipeline::Client {
 public:
  PlayerImpl(const PlayerConfig& config, std::unique_ptr<Deps> deps);
  PlayerImpl(const PlayerImpl&) = delete;
  PlayerImpl& operator=(const PlayerImpl&) = delete;
  ~PlayerImpl() override;

  // ---- Lifecycle (Player forwarding, thread-safe) --------------------------
  Status SetDataSource(std::string_view uri);
  Status SetDataSource(const media::DataSourceDescriptor& descriptor);
  Status PrepareAsync();
  Status PrepareSync(base::TimeDelta timeout);
  void Start();
  void Pause();
  void Stop();
  void StopSync(base::TimeDelta timeout);
  void Reset();
  Result<int64_t> SeekTo(base::TimeDelta position, SeekMode mode,
                         Player::SeekCB cb);
  void SetVideoSurface(base::scoped_refptr<NativeDisplay> display);

  // ---- Control -------------------------------------------------------------
  void SetPlaybackRate(double rate);
  void SetVolume(double volume);
  void SetMuted(bool muted);
  void ApplyGain();
  void SetLoopCount(int count);

  // ---- Queries (thread-safe snapshots) -------------------------------------
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
  double playback_rate() const { return playback_rate_.load(); }
  double volume() const { return volume_.load(); }
  bool muted() const { return muted_.load(); }
  PlaybackStats GetPlaybackStats() const;
  std::string DumpDiagnostics() const;

  // ---- Events --------------------------------------------------------------
  void SetEventHandler(Player::EventHandler handler) {
    event_hub_.SetEventHandler(std::move(handler));
  }
  int AddObserver(PlayerObserver* observer);
  void RemoveObserver(int id);

  const PlayerConfig& config() const { return config_; }

 private:
  // MediaLog is what the media layer reports stages and errors through; the
  // facade does not subscribe (MediaLog events are diagnostics, not SDK
  // events), but the demuxer wants one.
  std::unique_ptr<media::Demuxer> CreateDemuxer();

  // Pipeline::Client (media sequence):
  void OnError(MediaError error) override;
  void OnEnded() override;
  void OnDurationChange(base::TimeDelta duration) override;
  void OnBufferingStateChange(media::BufferingState state,
                              base::TimeDelta memory_usage) override;
  void OnWaiting(media::WaitingReason reason) override;
  void OnStatisticsUpdate(const media::PipelineStatistics& stats) override;
  void OnVideoConfigChange(const media::VideoDecoderConfig& config) override;

  // State transitions; publishes kStateChanged. Returns the previous state.
  PlayerState ChangeState(PlayerState to);
  // The ready signal arrives as the pipeline's first
  // OnBufferingStateChange(kHaveMetadata); snapshot media info and publish
  // kPrepared (then auto-start if configured).
  void OnPipelineReady();
  void OnMediaSeekDone(int64_t request_id, base::TimeDelta requested);

  PlayerConfig config_;
  std::unique_ptr<Deps> deps_;

  // S1 / S3 / S4 of docs/04 §1. Owned here so that ~PlayerImpl destroys them
  // after the pipeline is gone; DefaultRendererFactory borrows their runners.
  base::Thread media_thread_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  EventHub event_hub_;

  std::unique_ptr<media::DefaultRendererFactory> renderer_factory_;
  std::unique_ptr<media::PipelineImpl> pipeline_;
  base::scoped_refptr<media::MediaLog> media_log_;

  // ---- state ---------------------------------------------------------------
  mutable base::Lock state_lock_;
  PlayerStateMachine machine_ GUARDED_BY(state_lock_);
  // Signalled from the media sequence when prepare reaches a verdict (ready
  // or error), which is what PrepareSync waits on.
  base::WaitableEvent prepared_event_;
  bool prepared_handled_ GUARDED_BY(state_lock_) = false;
  media::DataSourceDescriptor source_ GUARDED_BY(state_lock_);
  bool source_set_ GUARDED_BY(state_lock_) = false;
  base::scoped_refptr<NativeDisplay> display_ GUARDED_BY(state_lock_);

  std::atomic<double> playback_rate_{1.0};
  std::atomic<double> volume_{1.0};
  std::atomic<bool> muted_{false};
  std::atomic<int> loop_count_{1};
  std::atomic<int64_t> next_request_id_{1};

  // Snapshots written on the media sequence (through the pipeline client
  // callbacks) and read from any thread.
  mutable base::Lock snapshot_lock_;
  media::MediaInfo media_info_ GUARDED_BY(snapshot_lock_);
  media::PipelineStatistics last_stats_ GUARDED_BY(snapshot_lock_);
  media::Size video_coded_size_ GUARDED_BY(snapshot_lock_);
  media::Size video_natural_size_ GUARDED_BY(snapshot_lock_);
  int video_rotation_ GUARDED_BY(snapshot_lock_) = 0;
  bool is_live_ GUARDED_BY(snapshot_lock_) = false;

  // Seek bookkeeping: the media layer reports a bare completion, the facade
  // matches it to the request id and runs the user's callback on the event
  // sequence (docs/05 table 5's request_id fix).
  mutable base::Lock seek_lock_;
  std::map<int64_t, Player::SeekCB> pending_seeks_ GUARDED_BY(seek_lock_);
};

}  // namespace ijkpp

#endif  // IJKPP_PLAYER_PLAYER_IMPL_H_
