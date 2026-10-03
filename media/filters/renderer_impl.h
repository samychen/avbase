// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/filters/renderer_impl.h` (BSD-3-Clause) in role:
// the composition root that turns a MediaResource into audio and video output
// and owns the clock.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// (milestone M7, docs/08 §2).
// Written without a compiler; never built or run. Excluded from every CMake
// target on purpose.
//
// WHAT IT REPLACES. In ijkplayer this role is played by the whole of
// ff_ffplay.c: `read_thread()` plus the ~200-field VideoState that every thread
// mutates. Here it is one class that owns two sub-renderers and one clock, and
// whose only cross-sequence traffic is a periodic clock push and the two sinks'
// callbacks. docs/01 defect #2 is the god-struct; this is the answer to it.
//
// ---------------------------------------------------------------------------
// GAPS -- close these before this file leaves DRAFT
// ---------------------------------------------------------------------------
//   1. Compile it. It depends on four other DRAFT files (media/base/renderer.h,
//      renderer_client.h, media_resource.h, pipeline_status.h) and on
//      VideoRendererImpl / AudioRendererImpl, which are DRAFT too. All seven
//      must leave DRAFT together; none of them can be validated alone.
// 2. AudioParameters are synthesised here from the stream's AudioDecoderConfig
// with a hardcoded frames_per_buffer, because media/audio/ (AudioManager,
// AudioOutputDevice) does not exist yet -- that is the component that
// negotiates a real device period and that owns device switching. When M7 adds
// it, this constructor argument moves there and
// RendererClient::OnAudioOutputDeviceChanged gains a caller. 3. TextRenderer is
// not composed in. Renderer::OnTracksChanged handles DemuxerStreamType::kText
// by logging and succeeding, which is a lie the SDK must not tell:
// Player::SelectTrack(kText) would report success and nothing would happen.
// Either wire text or return kNotImplemented until it is. 4. Buffering state is
// reported only at the transitions this class can see locally (start, end of
// stream, a dry audio ring). The three-tier high water mark and the hysteresis
// that stop it oscillating are M9's BufferController; until then
// OnBufferingStateChange is approximate and docs/08 M9's DoD cannot be met. 5.
// Statistics are assembled from the two sub-renderers and the compositor, but
// PipelineStatistics has 21 fields and this fills 11. The rest (text bytes,
// keyframe count, audio_bytes_decoded, last_seek_duration) need either counters
// that do not exist yet or a demuxer query. 6. SetLatencyHint() reaches the
// audio side (AudioRendererImpl) but not the video side: a live stream that
// must drop rather than fall behind needs the compositor to know the latency
// target too, and it has no such input. 7. Initialize() binds a
// std::unique_ptr<VideoRendererSink> into a base::BindOnce. bind.h documents
// itself as "the L1 tier of docs/08 §5.1 R2's fallback plan" and lists what it
// does NOT support; move-only bound arguments are not in either list, so
// whether this compiles is unknown and is one of the first things a real build
// will answer. If L1 cannot move a unique_ptr through BindOnce, the sink has to
// be handed over by a member field read inside the task instead of a bound
// argument. 8. No test. Same reason as the two sub-renderers: tests/support/
// does not exist, and this class needs a MockRendererClient on top of
// everything they need.

#ifndef AVBASE_MEDIA_FILTERS_RENDERER_IMPL_H_
#define AVBASE_MEDIA_FILTERS_RENDERER_IMPL_H_

#include <stdint.h>

#include <atomic>

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/time/tick_clock.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/media_resource.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer.h"
#include "media/base/renderer_client.h"
#include "media/filters/audio_renderer_impl.h"
#include "media/filters/decoder_selector.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/legacy/video_frame_compositor.h"
#include "media/filters/video_renderer_impl.h"
#include "media/base/text_decoder.h"
#include "media/media_export.h"

namespace avbase::media {

// Owns the two sub-renderers and the clock; implements media::Renderer.
//
// THREADING. Lives on the media sequence (S1, docs/04 §1). It creates and
// drives VideoRendererImpl (S3) and AudioRendererImpl (S4) by posting to their
// own runners, and never touches their internals from here. The one thing that
// crosses sequences by value is the master clock, pushed periodically -- which
// is deliberate: a clock that any thread may read at any time is what ffplay
// has, and it is why Δ14 exists.
class AVBASE_MEDIA_EXPORT RendererImpl final : public Renderer {
 public:
  // Everything the composition root needs, gathered so that adding a component
  // (TextRenderer at M7, VAAPI at M14) does not change this constructor's
  // signature and therefore does not churn every call site.
  struct Deps {
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner;   // S1
    base::scoped_refptr<base::SequencedTaskRunner> video_task_runner;   // S3
    base::scoped_refptr<base::SequencedTaskRunner> audio_task_runner;   // S4
    const base::TickClock* tick_clock = nullptr;
    std::vector<base::scoped_refptr<VideoDecoderFactory>> video_factories;
    // config.video.decoder_preference + hw_codecs, applied by
    // VideoRendererImpl once the stream config is known. Unset leaves the
    // injected order authoritative.
    DecoderPreference video_decoder_preference{DecoderPreference::kAuto};
    HwCodecMask video_hw_codecs{static_cast<HwCodecMask>(HwCodecFlag::kAll)};
    bool video_decoder_preference_set = false;
    std::vector<base::scoped_refptr<AudioDecoderFactory>> audio_factories;
    // Text leg (Phase 4.2). Null = text selection is not available in this
    // build (no-ffmpeg): SelectTrack(kText) then reports kNotImplemented.
    base::scoped_refptr<TextDecoderFactory> text_decoder_factory;
    std::unique_ptr<VideoRendererSink> video_sink;
    base::scoped_refptr<AudioRendererSink> audio_sink;
    VideoFrameCompositor::Thresholds compositor_thresholds;
    // The controller is constructed by the owner (PipelineImpl) and shared
    // with it, so PipelineImpl::GetMediaTime() can answer through the seqlock
    // even while the renderer is being torn down. |sync_thresholds| and
    // |sync_master| below document how it was built; RendererImpl no longer
    // constructs one itself.
    std::shared_ptr<AvSyncController> av_sync;
    AvSyncController::Thresholds sync_thresholds;
    AvSyncController::MasterType sync_master{
        AvSyncController::MasterType::kAudio};
    // Device period used until media/audio/ exists (gap 2). 1024 frames at
    // 48 kHz is ~21 ms, which is what ALSA and PulseAudio both default to.
    int audio_frames_per_buffer = 1024;
    // config.video.disabled / config.audio.disabled ("vn"/"an").
    bool video_disabled = false;
    bool audio_disabled = false;
  };

  explicit RendererImpl(Deps deps);
  RendererImpl(const RendererImpl&) = delete;
  RendererImpl& operator=(const RendererImpl&) = delete;
  ~RendererImpl() override;

  // ---- media::Renderer ----------------------------------------------------
  // All of these run on the media sequence unless the comment says otherwise.
  void Initialize(MediaResource* media_resource, RendererClient* client,
                  base::scoped_refptr<base::SequencedTaskRunner>
                      media_task_runner,
                  PipelineStatusCallback init_cb) override;
  void SetCdm(CdmContext* cdm_context,
              base::OnceCallback<void(bool)> cdm_attached_cb) override;
  void SetLatencyHint(std::optional<base::TimeDelta> latency_hint) override;
  void SetPreservesPitch(bool preserves_pitch) override;
  void SetRenderMutedAudio(bool render_muted_audio) override;
  // Live-edge text policy. See Renderer::SetSourceLiveness for why the
  // pipeline states this rather than the renderer discovering it.
  void SetSourceLiveness(bool is_live, base::TimeDelta max_cue_age) override;
  void Flush(base::OnceClosure flush_cb) override;
  void StartPlayingFrom(base::TimeDelta time) override;
  void SetPlaybackRate(double playback_rate) override;
  void SetVolume(float volume) override;
  // Added with the M8 pipeline wiring (see Renderer::SetPaused): pauses both
  // sub-renderers and freezes the clocks. Runs on the media sequence.
  void SetPaused(bool paused) override;
  void SetOutputTarget(
      base::scoped_refptr<NativeDisplay> display) override;
  void BeginAccurateSeek(base::TimeDelta target,
                         base::OnceClosure reached_cb) override;
  void EndAccurateSeek() override;
  // Thread-safe: reads the clock through AvSyncController's seqlock (Δ14), so
  // Player::GetMediaTime() can answer from the caller's thread without a
  // PostTask round trip and without a mutex.
  base::TimeDelta GetMediaTime() override;
  void OnTracksChanged(DemuxerStreamType track_type,
                       DemuxerStream* enabled_track,
                       base::OnceClosure change_completed_cb) override;
  RendererType GetRendererType() override;

  // For PipelineImpl's statistics aggregation.
  PipelineStatistics GetStatistics() const;
  bool ended() const { return ended_; }

  // Cadence of the S1 clock/statistics push loops; shared by the start path
  // and the audio-switch resumption.
  static constexpr base::TimeDelta kClockPushInterval = base::Milliseconds(10);
  static constexpr base::TimeDelta kStatsInterval = base::Seconds(1);

 private:
  // Handed to the sub-renderers; they run it on S3 and S4 respectively, so it
  // only hops -- the flags it flips are S1 state. Same split as the ended
  // callbacks below, and for the same reason: a TSan run of
  // tests/unit/media_filters/renderer_impl_unittest.cc reported the direct
  // version as a data race between S3, S4 and S1.
  void PostVideoInitialized(PipelineStatus status);
  void PostAudioInitialized(PipelineStatus status);
  // Runtime audio-track switch (Phase 4). Runs on S1; |settings| carries what
  // the outgoing renderer had, captured on S4 during teardown.
  struct SwitchedAudioSettings {
    float volume;
    bool muted;
    double playback_rate;
    bool preserves_pitch;
  };
  void SwitchAudioRenderer(DemuxerStream* new_stream,
                           base::OnceClosure change_completed_cb);
  void OnSwitchedAudioInitialized(bool was_rendering,
                                  base::TimeDelta resume_at,
                                  SwitchedAudioSettings settings,
                                  base::OnceClosure change_completed_cb,
                                  PipelineStatus status);
  // Text leg pump (Phase 4.2). All S1; Read replies are posted there by the
  // demuxer stream.
  void PumpText();
  void OnTextRead(int generation, DemuxerStream::Status status,
                  DemuxerStream::DecoderBufferVector buffers);
  // True when |cue| is too far behind the media clock to be worth showing, per
  // the live-edge policy SetSourceLiveness() installed. Always false when the
  // policy is off, which is every non-live source.
  bool IsCueStale(const TimedTextCue& cue) const;
  void FinishAudioSwitch(bool was_rendering, base::TimeDelta resume_at,
                         SwitchedAudioSettings settings,
                         base::OnceClosure change_completed_cb,
                         PipelineStatus status);
  void OnVideoInitialized(PipelineStatus status);
  void OnAudioInitialized(PipelineStatus status);
  void MaybeReportInitialized();
  // Builds the sub-renderers the resource has streams for, hands each its sink
  // and its init callback, and wires the ended/frame-presented callbacks.
  // Called once from Initialize() on S1; the Posts inside go to S3/S4.
  void CreateSubRenderers(DemuxerStream* video_stream,
                          DemuxerStream* audio_stream);
  // Periodic: read the master clock on S1, push it to S3.
  void PushMasterClock();
  void PushStatistics();
  void ReportError(MediaError error);
  // Runs (on the media sequence, via a hop) the pipeline's init callback with
  // |status|. Every arrival path funnels here: the sub-renderers complete on
  // S3/S4, and the pipeline's sequence checker is what caught the first draft
  // running the callback inline from whichever thread finished last.
  void CompleteInitialization(PipelineStatus status);
  void OnEnded();
  // Posted by the sub-renderers' ended callbacks (which fire on S3/S4); the
  // flags they set and the decision itself stay on S1. "Ended" means every
  // existing sub-renderer has reported end of stream *and* the video side has
  // nothing left to present. Fires OnEnded() once per playback.
  void PostVideoEnded();
  // S6: every frame the sink takes, feeding the video clock and the
  // accurate-seek target check.
  void OnVideoFramePresented(base::TimeDelta timestamp, int32_t serial);
  // S1: the presented frame satisfies the open accurate-seek window.
  void OnAccurateSeekTargetReached();
  void PostAudioEnded();
  void OnVideoStreamEnded();
  void OnAudioStreamEnded();
  void CheckForEnded();
  // S1, from the 10 ms clock push: publishes kHaveNothing when every stream
  // the renderer owns has run dry, and kHaveEnough when data flows again.
  // This is the real trigger behind the facade's BufferController cycles
  // -- before it existed, OnBufferingStart had no caller and the three-tier
  // HWM could never advance (M9).
  void CheckBufferingTransitions();
  static AudioParameters MakeAudioParameters(const AudioDecoderConfig& config,
                                             int frames_per_buffer);

  Deps deps_;
  base::raw_ptr<RendererClient> client_;
  // Shared with PipelineImpl (see Deps::av_sync); GetMediaTime() reads it
  // through the seqlock from any thread.
  std::shared_ptr<AvSyncController> av_sync_;
  std::unique_ptr<VideoRendererImpl> video_;
  std::unique_ptr<AudioRendererImpl> audio_;

  bool initialized_{false};
  bool video_initialized_{false};
  bool audio_initialized_{false};
  // The audio stream the current AudioRendererImpl was built with; identity
  // for the "same track, nothing to do" no-op in OnTracksChanged. Lifetime is
  // the MediaResource's (same contract as the sub-renderer's stream pointer).
  DemuxerStream* audio_stream_{nullptr};
  // Text leg (Phase 4.2): the base renders nothing -- it decodes subtitle
  // packets on S1 and hands cues to the client. |text_generation_| invalidates
  // in-flight Read replies across switches and flushes (the stream aborts
  // them, but only the generation knows which reply belongs to which leg).
  DemuxerStream* text_stream_{nullptr};
  std::unique_ptr<TextDecoder> text_decoder_;
  bool text_read_outstanding_{false};
  bool text_ended_{false};
  int text_generation_{0};
  // Live-edge text policy (Renderer::SetSourceLiveness). S1-only: the setter is
  // called on the media sequence before playback starts, and IsCueStale() runs
  // on S1 from OnTextRead.
  bool source_is_live_{false};
  base::TimeDelta max_cue_age_{base::TimeDelta()};
  // Counted so the policy is observable rather than merely correct: a live
  // stream that drops cues silently is indistinguishable from one that never
  // decoded them. Asserted directly by the live-source suite.
  int64_t stale_cues_dropped_ = 0;
  // Switched-out audio renderers. They are stopped and drained on S4 but NOT
  // deleted there: control tasks bound with Unretained may still be queued
  // behind the teardown task, and deleting would turn them into use-after-free
  // (a real one: the first draft's "delete on quiescence" aborted inside a
  // mutex of the freed object). Every AudioRendererImpl entry point
  // early-returns on stopping_, so a retired object is inert; the graveyard
  // is destroyed on S4 from ~RendererImpl, after the sequences' own teardown
  // ordering drains those in-flight tasks.
  std::vector<std::unique_ptr<AudioRendererImpl>> retired_audio_;
  bool has_video_{false};
  bool has_audio_{false};
  bool ended_{false};
  bool paused_{false};
  // Set by StartPlayingFrom: the buffering-starvation check below only
  // applies while rendering, so the dry pipeline before Play() does not
  // report kHaveNothing on top of kHaveMetadata.
  bool rendering_{false};
  // S1-only: the last buffering edge this renderer published. Only the
  // transitions cross the client boundary -- a repeated state would make
  // every 10 ms clock tick a kHaveNothing event.
  bool starved_reported_{false};
  // S1-only: the sub-renderers reported end of stream. The final decision
  // waits for the buffers to drain (CheckForEnded).
  bool video_ended_{false};
  bool audio_ended_{false};
  bool clock_push_scheduled_{false};
  bool stats_scheduled_{false};
  base::TimeDelta start_time_;
  // Kept so GetStatistics() can turn audio_->buffered_frames() into a duration
  // without asking the sub-renderer for its sample rate.
  AudioParameters audio_params_;
  // ---- accurate seek (renderer_impl_controls.cc) --------------------------
  // The open window's target in microseconds, 0 when closed. Atomic because
  // the S6 presented-frame hook reads it; every decision happens on S1
  // (BeginAccurateSeek/EndAccurateSeek/OnAccurateSeekTargetReached), so the
  // hook only ever observes.
  std::atomic<int64_t> accurate_target_micros_{0};
  // S1-only: the facade's "target reached" callback, held between Begin and
  // the completion hop.
  base::OnceClosure accurate_reached_cb_;
  // SetRenderMutedAudio()'s argument, stored because it cannot be honoured yet:
  // "stop rendering while muted" needs a device to stop, and media/audio/ does
  // not exist. Recording it here means the flag is not silently dropped, and
  // the gap is one line to close later.
  bool render_muted_audio_{true};
  double playback_rate_{1.0};
  float volume_{1.0f};
  // The pipeline's init callback, held from Initialize() until both
  // sub-renderers report. The first draft passed it through the video path's
  // callback; when video finished before audio, the closure died in the
  // returning stack frame and the pipeline waited forever.
  PipelineStatusCallback pending_init_cb_;
  // Must be last: the two repeating chains below (PushMasterClock /
  // PushStatistics) are bound through it, so a task that outlives the
  // renderer -- e.g. the 10 ms clock push still queued while the owner tears
  // the pipeline down -- is skipped instead of running on freed memory.
  // Found by the 30-minute TSan soak (SEGV in ~TaskEnvironment's final
  // RunUntilIdle, iteration 68).
  base::WeakPtrFactory<RendererImpl> weak_factory_{this};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_RENDERER_IMPL_H_
