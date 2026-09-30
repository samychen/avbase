// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Structure mirrors Chromium's `media/filters/video_renderer_impl.h`
// (BSD-3-Clause). The pacing algorithm is not here: it lives in
// media/filters/legacy/video_frame_compositor.* (a line-by-line port of
// ffplay's video_refresh() + compute_target_delay(), LGPL-2.1). This class is
// the plumbing around it -- decode pump, sink lifecycle, clock distribution.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// (milestone M7, docs/08 §2).
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// Written without a compiler available; never built or run. Excluded from every
// CMake target on purpose.
//
// ---------------------------------------------------------------------------
// ★THREADING: A DELIBERATE DEVIATION FROM docs/04 §1, RAISED NOT SMUGGLED
// ---------------------------------------------------------------------------
// docs/04's thread table puts the compositor's WRITE side on S1 (`ijkpp-media`,
// where RendererImpl lives) and only "VideoFrameCompositor 的部分读" on S3
// (`ijkpp-video`, where this class lives). Taken literally that means S3
// decodes a frame, posts it to S1, and S1 calls PutCurrentFrame(). This file
// does not do that. Both the decode pump and PutCurrentFrame() run on S3, for
// three reasons: 1. Decode and publish are one causal step. Splitting them
// inserts a queue and a task hop between "the decoder produced frame N" and
// "the compositor may show frame N", and that queue is a second place where
// frames can be dropped, reordered or stranded after a seek. ijkpp already has
// one such queue (VideoFrameQueue); adding an implicit second one to satisfy a
// table row is how ffplay ended up with pictq AND sampq AND a refresh thread.
// 2. The compositor is internally locked (it takes base::Lock) and its read
// side, Render(), is documented as running on the sink's sequence (S6)
// regardless. So moving the writes from S1 to S3 does not create a data race;
// it changes which sequence serialises them. 3. RendererImpl still owns the
// CLOCK. It posts SetMasterClock() to this class's sequence, so the thing
// docs/04 actually cares about -- that the master clock has a single
// authoritative writer and that A/V sync state is not decided in two places --
// is preserved. What is lost: docs/04's guarantee that every compositor
// mutation happens on one named sequence becomes "on S3", and the table needs
// updating. That is a documentation change with a design rationale attached, so
// it is recorded in docs/PROGRESS.md rather than made silently here. If the
// review prefers the literal S1 model, the change is mechanical: give this
// class a second task runner and post PutCurrentFrame() to it.
// ---------------------------------------------------------------------------
// GAPS -- close these before this file leaves DRAFT
// ---------------------------------------------------------------------------
// 1. Compile it. Nothing here has been built. 2. No tests. They need
// tests/support/ (a fake VideoRendererSink that can drive Render() with
// controlled deadlines, a mock video decoder, a synthetic demuxer); that
// directory does not exist. See the same note in audio_renderer_impl.h -- the
// fakes are a prerequisite for the rest of M7, not something to improvise per
// class. 3. Accurate seek is plumbed (BeginAccurateSeek/EndAccurateSeek are
// called) but the policy that decides when the target is reached lives in
// player/seek_controller (M9), so today the compositor's accurate-seek window
// never closes on its own. Do not ship that: a never-closing window drops
// frames forever. 4. Resolution change: DecoderStream surfaces kConfigChanged
// via the demuxer stream, but this class reports it upward as
// OnVideoConfigChange without re-initialising the sink, and a sink may need
// reconfiguration when the coded size changes. 5. SetOutputTarget() (runtime
// surface swap, Player::SetVideoSurface) is forwarded but not serialised
// against a running Render(). The sink's own contract says implementations must
// serialise it; this class should still DCHECK that it is not called mid-flush.
// 6. No frame-rate detection. SetFpsDuration() is never called, so the
// compositor derives duration from container deltas alone; ffplay's fps-probe
// fallback (config.video.calc_frame_rate) is unwired.

#ifndef IJKPP_MEDIA_FILTERS_VIDEO_RENDERER_IMPL_H_
#define IJKPP_MEDIA_FILTERS_VIDEO_RENDERER_IMPL_H_

#include <stdint.h>

#include <atomic>
#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/time/tick_clock.h"
#include "media/base/pipeline_status.h"
#include "media/base/video_decoder_factory.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/filters/decoder_stream.h"
#include "media/filters/legacy/video_frame_compositor.h"
#include "media/media_export.h"

namespace ijkpp::media {

class RendererClient;

// Decode pump + sink lifecycle + frame pacing, for one video stream.
//
// WHAT IT REPLACES. ffplay's `video_thread()` plus the pacing half of
// `video_refresh()`: two functions that shared state through VideoState with no
// synchronisation, where `video_refresh()` mixed "which frame should be shown"
// with "how do I sleep until then" and used three `goto retry` edges to loop
// (docs/01 defect #1). Here the decision is a pure static function inside
// VideoFrameCompositor with 55 unit tests, this class only moves data, and the
// sleep is the sink's job -- which is what makes vsync-accurate presentation
// possible at all (Δ18).
//
// THREADING. Everything except Render() and OnFrameSubmitFailure() runs on
// |task_runner| (S3). Those two run on the sink's render sequence (S6) and only
// touch the compositor, which is internally locked.
class IJKPP_MEDIA_EXPORT VideoRendererImpl final
    : public VideoRendererSink::RenderCallback {
 public:
  using InitializeCB = base::OnceCallback<void(PipelineStatus)>;

  VideoRendererImpl(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner,
      std::vector<base::scoped_refptr<VideoDecoderFactory>> factories,
      const base::TickClock* tick_clock,
      const VideoFrameCompositor::Thresholds& thresholds);
  VideoRendererImpl(const VideoRendererImpl&) = delete;
  VideoRendererImpl& operator=(const VideoRendererImpl&) = delete;
  ~VideoRendererImpl() override;

  // Runs on S3. The sink is adopted but not started until StartPlayingFrom().
  void Initialize(DemuxerStream* stream,
                  std::unique_ptr<VideoRendererSink> sink, InitializeCB cb);

  void StartPlayingFrom(base::TimeDelta time);
  void Flush(int32_t serial, base::OnceClosure closure);
  void Stop();

  // ---- Called by RendererImpl, posted onto S3 -----------------------------
  // The master clock has exactly one authoritative source (AvSyncController,
  // owned by RendererImpl); this is how it reaches the pacing logic.
  void SetMasterClock(base::TimeDelta media_time, int32_t serial, bool valid);
  void SetMasterIsVideo(bool master_is_video);
  void SetPlaybackRate(double rate);
  void SetPaused(bool paused);
  void SetStepMode(bool step);
  void SetMaxFrameDrop(int max_frame_drop);
  void SetMaxFps(int max_fps);
  void SetBufferingBlocked(bool blocked);
  void BeginAccurateSeek(base::TimeDelta target);
  void EndAccurateSeek();

  // Runtime surface swap (Player::SetVideoSurface). May be called from any
  // thread; the sink serialises it against its own Render().
  void SetOutputTarget(base::scoped_refptr<NativeDisplay> display);

  bool initialized() const { return initialized_; }
  bool ended() const { return ended_; }
  VideoFrameCompositor::Stats compositor_stats() const;
  size_t frames_pending() const;

  // ---- VideoRendererSink::RenderCallback, i.e. S6 -------------------------
  // Returns the frame to present, or nullptr to keep showing the previous one.
  // Must not block, allocate beyond what the compositor already owns, or call
  // back into Player.
  base::scoped_refptr<VideoFrame> Render(base::TimeTicks deadline_min,
                                         base::TimeTicks deadline_max) override;
  void OnFrameSubmitFailure() override;

 private:
  void PumpDecoder();
  void OnDecoderInitialized(InitializeCB cb, DecoderStatus status);
  void OnDecoderOutput(base::OnceClosure pump_again, DecoderStatus status,
                       base::scoped_refptr<VideoFrame> frame);
  void OnDecoderStreamEvent(DecoderStreamEvent event);

  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  std::vector<base::scoped_refptr<VideoDecoderFactory>> factories_;
  DecoderStream<VideoDecoderStreamTraits> decoder_stream_;
  VideoFrameCompositor compositor_;
  std::unique_ptr<VideoRendererSink> sink_;

  bool initialized_{false};
  bool started_{false};
  bool paused_{false};
  bool ended_{false};
  bool stopping_{false};
  bool read_outstanding_{false};
  int32_t serial_{0};
  // Written on the sink's render sequence (S6), read only for logging, so
  // relaxed is enough: it rate-limits a message and nothing depends on its
  // ordering. base/logging.h has no LOG_EVERY_N to do this for us.
  std::atomic<uint64_t> submit_failures_{0};
  // Frames decoded but not yet handed to the compositor cannot exist in this
  // design -- decode publishes immediately -- so back-pressure is expressed by
  // not calling Read() while the compositor's pending deque is deep. This is
  // the watermark that replaces ffplay's frame_queue_signal() blocking wait.
  size_t max_pending_frames_{0};
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_VIDEO_RENDERER_IMPL_H_
