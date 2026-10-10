// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PLAYER_IMPL_H_
#define AVBASE_PLAYER_PLAYER_IMPL_H_

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
#include "media/filters/retry_data_source.h"
#include "media/renderers/default_renderer_factory.h"
#include "player/buffer_controller.h"
#include "player/event_hub.h"
#include "player/public/player.h"
#include "player/public/player_config.h"
#include "player/seek_controller.h"
#include "player/state_machine.h"

namespace avbase {

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
  // Issues a snapshot request; the outcome arrives as kSnapshotCompleted
  // (or kError) on the event handler -- synchronous Status only says whether
  // the request was accepted.
  Status TakeSnapshot(base::TimeDelta at, const std::string& file_path);
  Result<int64_t> SeekTo(base::TimeDelta position, SeekMode mode,
                         Player::SeekCB cb);
  // Runtime track switch (Phase 4). Accepted from any thread; the switch
  // itself is asynchronous and reports through the kTrackChanged event (or a
  // kError event when the handover failed). Only kAudio is wired; kText and
  // kVideo return kNotImplemented with the reason.
  Status SelectTrack(media::DemuxerStreamType type, int stream_index);
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
  void OnTimedText(const media::TimedTextCue& cue) override;
  void OnVideoConfigChange(const media::VideoDecoderConfig& config) override;

  // State transitions; publishes kStateChanged. Returns the previous state.
  PlayerState ChangeState(PlayerState to);
  // The ready signal arrives as the pipeline's first
  // OnBufferingStateChange(kHaveMetadata); snapshot media info and publish
  // kPrepared (then auto-start if configured).
  void OnPipelineReady();
  void OnMediaSeekDone(int64_t request_id, base::TimeDelta requested);

  // ---- accurate seek (M9, player/seek_controller.cc + events TU) ----------
  // All of these run on the media sequence. BeginAccurateWait supersedes any
  // running wait; CheckAccurateSeekExpiry is the posted deadline; the reached
  // hop comes from the renderer's presented-frame hook.
  void BeginAccurateWaitOnMedia(int64_t request_id, base::TimeDelta target);
  void EndAccurateWaitOnMedia();  // Keyframe seek supersedes the wait.
  void OnAccurateSeekTargetReached();
  void CheckAccurateSeekExpiry();
  // Publishes kAccurateSeekCompleted + kSeekCompleted and runs the user's
  // callback. |result| is ok on reach, kTimeout on expiry (Δ10: playback
  // continues either way).
  void CompleteAccurateSeek(bool reached);

  PlayerConfig config_;
  std::unique_ptr<Deps> deps_;

  // S1 / S3 / S4 of docs/04 §1. Owned here so that ~PlayerImpl destroys them
  // after the pipeline is gone; DefaultRendererFactory borrows their runners.
  base::Thread media_thread_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  EventHub event_hub_;
  // The audio track SelectTrack last switched to (-1 = the container default),
  // so kTrackChanged can report old/new without re-probing.
  std::atomic<int> audio_track_index_{-1};
  // The retry decorator on the network path, when config.net.reconnect and
  // an http(s) source produced one (Phase 2: the weaknet GAP). Kept so
  // ReconnectNow()/diagnostics can name the live object.
  base::scoped_refptr<media::RetryDataSource> retry_source_;

  // ---- the pipeline pair, and why it is a locked shared_ptr ---------------
  // Reset() is documented as callable from any thread (player.h:42) and it
  // REPLACES both of these, while other threads read them: the runtime
  // controls (SetPlaybackRate, ApplyGain, SetVideoSurface) and the queries
  // (GetMediaTime/GetBufferedTime/GetDuration) run on the caller's thread, and
  // the Pipeline::Client callbacks run on the media sequence. With a bare
  // unique_ptr those reads raced the reset and could dereference an object
  // mid-destruction -- a real UAF, not a theoretical one, because the contract
  // allows exactly that interleaving.
  //
  // Every use now goes through a local snapshot (GetPipeline /
  // GetRendererFactory). The snapshot holds a reference, so the object stays
  // alive for the whole call even if Reset() swaps the member meanwhile. The
  // lock is taken only to copy the pointer, never across a call, so the
  // "non-blocking queries" promise at player.h:107 still holds.
  mutable base::Lock pipeline_lock_;
  // Declared before pipeline_ on purpose: members are destroyed in reverse, so
  // this keeps the factory alive until after the pipeline.
  // PipelineImpl::Start() stores a raw RendererFactory* (the pipeline does not
  // ref-count the factory), so the factory must outlive it.
  std::shared_ptr<media::DefaultRendererFactory>
      renderer_factory_ GUARDED_BY(pipeline_lock_);
  std::shared_ptr<media::PipelineImpl> pipeline_ GUARDED_BY(pipeline_lock_);
  base::scoped_refptr<media::MediaLog> media_log_;

  // Snapshots of the live pipeline and its factory; null when none is
  // installed (before PrepareAsync, or after Reset). Safe from any thread.
  std::shared_ptr<media::PipelineImpl> GetPipeline() const {
    base::AutoLock scoped(pipeline_lock_);
    return pipeline_;
  }
  std::shared_ptr<media::DefaultRendererFactory> GetRendererFactory() const {
    base::AutoLock scoped(pipeline_lock_);
    return renderer_factory_;
  }

  // Installs are locked writes; the declaration order above is what keeps the
  // factory alive past the pipeline on teardown, so nothing but _Reset_ may
  // touch the pair directly.
  void SetPipeline(std::shared_ptr<media::PipelineImpl> pipeline) {
    base::AutoLock scoped(pipeline_lock_);
    pipeline_ = std::move(pipeline);
  }
  void
  SetRendererFactory(std::shared_ptr<media::DefaultRendererFactory> factory) {
    base::AutoLock scoped(pipeline_lock_);
    renderer_factory_ = std::move(factory);
  }

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
  // Which pending requests asked for an accurate landing, and to where.
  // Written on the caller's thread in SeekTo, consumed on the media sequence
  // in OnMediaSeekDone.
  std::map<int64_t, base::TimeDelta>
      accurate_seek_targets_ GUARDED_BY(seek_lock_);

  // Three-tier HWM policy (M9). All of its inputs -- buffering transitions,
  // statistics ticks, seek completions -- arrive on the media sequence via
  // the Pipeline::Client callbacks, so the controller itself needs no lock;
  // the sequence checker is what makes that claim checkable.
  SEQUENCE_CHECKER(buffer_controller_sequence_);
  player::BufferController buffer_controller_;

  // The accurate-seek policy state (M9). Same discipline as the buffer
  // controller: every mutation and decision happens on the media sequence.
  player::SeekController accurate_seek_;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PLAYER_IMPL_H_
