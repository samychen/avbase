// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_MEDIA_FILTERS_PIPELINE_IMPL_H_
#define IJKPP_MEDIA_FILTERS_PIPELINE_IMPL_H_

#include <atomic>
#include <memory>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/time/tick_clock.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/demuxer.h"
#include "media/base/media_info.h"
#include "media/base/pipeline.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer.h"
#include "media/base/renderer_client.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/media_export.h"

namespace ijkpp::media {

// The only Pipeline implementation: owns the demuxer and the renderer, runs
// them on the media sequence, and forwards renderer events to Pipeline::Client.
//
// WHAT IT OWNS. The demuxer (and, through Demuxer::Stop(), the demux thread's
// shutdown), the renderer (whose sub-renderers own the sinks), and the shared
// AvSyncController. The fixed destruction order at docs/03 §10.1 lives here:
// renderer first (its sinks stop pulling), then the demuxer (its thread is
// interrupted and joined), then the clock -- which is why ~PipelineImpl() may
// only run on the media sequence, and why Stop() posts rather than blocking
// (Δ1).
//
// PARAMETERS NOT IN THE FROZEN SIGNATURE. Pipeline::Start() takes a demuxer
// but not a source: Chromium's Start() reached the source through
// MediaResource. ijkpp's demuxer needs the DataSourceDescriptor and options to
// open
// with, so the owner sets them via SetSource() before calling Start(). That is
// a concrete-side parameter channel, not an interface extension.
//
// THREADED GETTERS. GetMediaTime()/GetDuration()/GetBufferedTime() are
// thread-safe: the clock through AvSyncController's seqlock (Δ14), the other
// two through atomics written on the media sequence. GetMediaTime() in
// particular survives renderer teardown, because the controller is a
// shared_ptr held by both this class and RendererImpl.
class IJKPP_MEDIA_EXPORT PipelineImpl final : public Pipeline,
                                              public RendererClient,
                                              public Demuxer::Host {
 public:
  PipelineImpl();
  PipelineImpl(const PipelineImpl&) = delete;
  PipelineImpl& operator=(const PipelineImpl&) = delete;
  ~PipelineImpl() override;

  // Concrete-side parameters, set before Start(). |clock| must outlive the
  // pipeline (Player::Impl owns it via Deps).
  void SetSource(DataSourceDescriptor source, DemuxerOptions options);
  void SetTickClock(const base::TickClock* clock);
  // The shared clock controller; the facade constructs it per playback and
  // hands the same instance to the renderer factory, so the master clock has
  // exactly one owner chain and GetMediaTime() survives renderer teardown.
  void SetClock(std::shared_ptr<AvSyncController> av_sync);

  // Pipeline:
  void Start(std::unique_ptr<Demuxer> demuxer,
             RendererFactory* renderer_factory,
             RendererType renderer_type,
             base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
             Client* client) override;
  void Stop() override;
  void Play() override;
  void Pause() override;
  void SetOutputTarget(
      base::scoped_refptr<NativeDisplay> display) override;
  bool IsRunning() const override;
  void SetVolume(float volume) override;
  void SetPlaybackRate(double rate) override;
  void SetLatencyHint(base::TimeDelta hint) override;
  void SetPreservesPitch(bool preserves_pitch) override;
  base::TimeDelta GetMediaTime() override;
  base::TimeDelta GetBufferedTime() const override;
  base::TimeDelta GetDuration() const override;
  Statistics GetStatistics() const override;
  void Seek(base::TimeDelta time, base::OnceClosure seeked_cb) override;
  bool CanSeekForward() const override;
  bool CanSeekBackward() const override;
  int GetAudioStreamId() const override;
  int GetVideoStreamId() const override;
  int GetTextStreamId() const override;
  void AddVideoStream(int id) override;
  void RemoveVideoStream(int id) override;
  void AddAudioStream(int id) override;
  void RemoveAudioStream(int id) override;
  void AddTextStream(int id) override;
  void RemoveTextStream(int id) override;

  // The controller PipelineImpl keeps the clock in; exposed because the
  // renderer needs the same instance and the player reports master selection
  // through it.
  std::shared_ptr<AvSyncController> av_sync() const { return av_sync_; }

  // For the facade's diagnostics and stats. Thread-safe snapshot.
  MediaInfo media_info() const;

 private:
  // ---- media sequence only ------------------------------------------------
  // Demuxer callbacks carry the demuxer-level Status -- the expected<void,
  // MediaError> alias, not DemuxerStream::Status. The expected carries the
  // three-part error straight through to the client.
  void DoStart(RendererType renderer_type);
  void OnDemuxerInitialized(Status status);
  void OnRendererInitialized(PipelineStatus status);
  void MaybeReady();
  void DoPlay();
  void OnDemuxerStarted(Status status);
  void DoPause();
  void DoSeek(base::TimeDelta time, base::OnceClosure seeked_cb);
  void OnSeekDemuxerDone(Status status, base::TimeDelta actual);
  void OnRendererFlushed();
  void FinishSeekIfBothDone();
  void DoStop();
  void DoSetOutputTarget(base::scoped_refptr<NativeDisplay> display);

  // Demuxer::Host (media sequence):
  void SetDuration(base::TimeDelta duration) override;
  void OnBufferedTimeUpdate(base::TimeDelta buffered,
                            base::TimeDelta playback_time) override;
  void OnDemuxerError(MediaError error) override;

  // RendererClient (media sequence unless noted):
  void OnError(MediaError error) override;
  void OnEnded() override;
  void OnBufferingStateChange(BufferingState state,
                              base::TimeDelta memory_usage) override;
  void OnWaiting(WaitingReason reason) override;
  void OnDurationChange(base::TimeDelta duration) override;
  void OnStatisticsUpdate(const PipelineStatistics& stats) override;
  void OnVideoConfigChange(const VideoDecoderConfig& config) override;
  // RendererClient-only hooks (Pipeline::Client has neither): the device
  // change is logged, and there is no overlay sequence in this build.
  void OnAudioOutputDeviceChanged(const std::string& device_id, bool is_default,
                                  OutputDeviceStatus status) override;
  base::scoped_refptr<base::SequencedTaskRunner> GetOverlayTaskRunner()
      override;

  DataSourceDescriptor source_;
  DemuxerOptions options_;
  base::raw_ptr<const base::TickClock> tick_clock_ = nullptr;
  base::scoped_refptr<base::SequencedTaskRunner> media_runner_;
  base::raw_ptr<Client> client_ = nullptr;
  base::raw_ptr<RendererFactory> renderer_factory_ = nullptr;

  std::unique_ptr<Demuxer> demuxer_;
  std::unique_ptr<Renderer> renderer_;
  std::shared_ptr<AvSyncController> av_sync_;

  // ---- state ---------------------------------------------------------------
  enum class State { kNew, kStarting, kReady, kStopping, kStopped, kError };
  // Atomic because IsRunning() answers from any thread while the media
  // sequence mutates the value.
  std::atomic<State> state_{State::kNew};
  bool demuxer_ready_ = false;
  bool renderer_ready_ = false;
  bool playing_ = false;
  bool seek_in_flight_ = false;
  bool pending_seek_superseded_ = false;
  // Seek completion is two-phase (renderer flush + demuxer seek) and these
  // three carry the request between the phases; all S1-exclusive.
  base::TimeDelta seek_time_;
  base::OnceClosure seek_cb_;
  bool seek_demuxer_done_ = false;
  bool seek_renderer_flushed_ = false;


  // ---- thread-safe snapshots ----------------------------------------------
  // duration/buffered are atomics; the statistics snapshot and the media info
  // are copied under a lock because they are multi-field and written on the
  // media sequence while any thread may read them.
  std::atomic<int64_t> duration_micros_{0};
  std::atomic<int64_t> buffered_micros_{0};
  std::atomic<bool> seekable_{false};
  mutable base::Lock snapshot_lock_;
  PipelineStatistics last_stats_ GUARDED_BY(snapshot_lock_);
  MediaInfo media_info_ GUARDED_BY(snapshot_lock_);

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_PIPELINE_IMPL_H_
