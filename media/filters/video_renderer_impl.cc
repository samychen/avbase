// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// Never compiled. The header carries the threading deviation, the gap list and
// the argument for both.

#include "media/filters/video_renderer_impl.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/media_constants.h"
#include "media/base/renderer_client.h"

namespace avbase::media {

VideoRendererImpl::VideoRendererImpl(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner,
    std::vector<base::scoped_refptr<VideoDecoderFactory>> factories,
    const base::TickClock* tick_clock,
    const VideoFrameCompositor::Thresholds& thresholds)
    : task_runner_(std::move(task_runner)),
      factories_(std::move(factories)),
      decoder_stream_(task_runner_),
      compositor_(thresholds, tick_clock),
      max_pending_frames_(static_cast<size_t>(kVideoFrameQueueSizeMax)) {
  CHECK(task_runner_);
}

VideoRendererImpl::~VideoRendererImpl() {
  // Stop the sink before the compositor is destroyed: after Stop() returns the
  // sink guarantees no further Render() calls, and Render() is the only thing
  // that touches the compositor from another sequence.
  if (sink_) {
    sink_->Stop();
  }
}

void VideoRendererImpl::set_ended_cb(base::RepeatingClosure cb) {
  ended_cb_ = std::move(cb);
}

void VideoRendererImpl::set_frame_presented_cb(FramePresentedCB cb) {
  frame_presented_cb_ = std::move(cb);
}

void VideoRendererImpl::set_decoder_preference(DecoderPreference preference,
                                               HwCodecMask hw_codecs) {
  decoder_preference_ = preference;
  hw_codecs_ = hw_codecs;
  preference_set_ = true;
}

void VideoRendererImpl::Initialize(DemuxerStream* stream,
                                   std::unique_ptr<VideoRendererSink> sink,
                                   InitializeCB cb) {
  DCHECK(stream);
  if (!stream || !sink) {
    std::move(cb).Run(PipelineStatus::kVideoRendererInitializationError);
    return;
  }
  sink_ = std::move(sink);
  // Rank the factories NOW, with the stream's own config in hand. This is what
  // makes config.video.decoder_preference observable: before this,
  // kHardwareOnly meant "try the hardware factories in whatever order they
  // were injected, then silently continue into the software one" -- i.e. the
  // opposite of what it says, and a bug a user cannot see or work around.
  //
  // A host that injected its own list and set no preference keeps that list
  // untouched (preference_set_ false): it has already made the decision, and
  // re-ranking would overrule a choice made with more context than we have.
  std::vector<base::scoped_refptr<VideoDecoderFactory>> ranked;
  if (preference_set_) {
    const VideoDecoderConfig config =
        VideoDecoderStreamTraits::ConfigFromStream(stream);
    std::vector<std::string> reasons;
    ranked = DecoderSelector::SelectVideoDecoder(
        factories_, config, decoder_preference_, hw_codecs_, &reasons);
    for (const std::string& reason : reasons) {
      LOG(INFO) << "avbase.vdec: " << reason;
    }
    if (ranked.empty()) {
      LOG(ERROR) << "avbase.vdec: no decoder candidate for codec="
                 << GetVideoCodecName(config.codec) << " under the configured "
                 << "preference (hardware="
                 << (decoder_preference_ == DecoderPreference::kSoftware
                         ? "excluded"
                         : "allowed")
                 << ", hw_codecs mask=0x" << std::hex << hw_codecs_ << std::dec
                 << ")";
      // Reported through the same callback as any other initialization
      // failure, so the pipeline reports one status rather than a decoder
      // that silently never produces a frame.
      std::move(cb).Run(PipelineStatus::kVideoRendererInitializationError);
      return;
    }
    LOG(INFO) << "avbase.vdec: decoder preference resolved, " << ranked.size()
              << " candidate(s), first=" << ranked.front()->name();
  } else {
    ranked = factories_;
  }
  decoder_stream_.set_event_cb(base::BindRepeating(
      &VideoRendererImpl::OnDecoderStreamEvent, base::Unretained(this)));
  decoder_stream_.Initialize(
      stream, VideoDecoderStreamTraits::ConfigFromStream(stream),
      std::move(ranked),
      base::BindOnce(&VideoRendererImpl::OnDecoderInitialized,
                     base::Unretained(this), std::move(cb)));
}

void VideoRendererImpl::OnDecoderInitialized(InitializeCB cb,
                                             DecoderStatus status) {
  if (!status.is_ok()) {
    LOG(ERROR) << "avbase.vdec: video decoder failed to initialize: "
               << decoder_stream_.GetDisplayName() << " ("
               << status.AsDebugString() << ")";
    std::move(cb).Run(PipelineStatus::kVideoRendererInitializationError);
    return;
  }
  initialized_ = true;
  LOG(INFO) << "avbase.vdec: video decoder ready ("
            << decoder_stream_.GetDisplayName() << ")";
  // The sink is initialised here but not started: starting it before the first
  // frame exists would make it call Render() into an empty compositor, which is
  // harmless but produces a burst of "repeat previous frame" stats that hide a
  // real stall.
  sink_->Initialize(this);
  std::move(cb).Run(PipelineStatus::kOk);
}

void VideoRendererImpl::StartPlayingFrom(base::TimeDelta time) {
  DCHECK(initialized_);
  if (!initialized_ || stopping_) {
    return;
  }
  // Adopt the generation the demuxer is on *now*. The flush that came with the
  // seek ran while the demuxer was still moving (DoSeek starts the two in
  // parallel), so the decoder stream was left filtering on the old serial and
  // dropped every packet of the new one: after a seek, video never resumed.
  // The headless end-to-end test cannot see it -- its null sink does not count
  // frames -- and tests/integration/pipeline_synthetic_unittest.cc reproduces
  // it
  // (after SeekTo(5 s), not one frame of generation 1 reached the display).
  const int32_t stream_serial = decoder_stream_.demuxer_stream()->serial();
  if (serial_ != stream_serial) {
    serial_ = stream_serial;
    decoder_stream_.AdoptSerial(serial_);
  }
  decoder_stream_.HoldReads(false);
  compositor_.Flush();
  filter_.reset();
  ended_ = false;
  paused_ = false;
  compositor_.SetPaused(false);
  if (!started_) {
    sink_->Start();
    started_ = true;
  }
  sink_->Play();
  (void)time;  // the demuxer has already seeked; the first frame carries it
  PumpDecoder();
}

void VideoRendererImpl::Flush(int32_t serial, base::OnceClosure closure) {
  // Pause before flushing: the sink's contract says Flush() is only valid while
  // not playing, and a Render() that lands mid-flush would present a frame from
  // the generation being discarded.
  if (started_ && sink_) {
    sink_->Pause();
    sink_->Flush();
  }
  compositor_.Flush();
  filter_.reset();
  ended_ = false;
  serial_ = serial;
  // Hold reads until StartPlayingFrom adopts the new generation: the pump is
  // free-running, and a read issued here would see new-generation buffers
  // against this stale |serial| and burn the whole generation as "stale"
  // (docs/PROGRESS.md, #52).
  decoder_stream_.HoldReads(true);
  decoder_stream_.Flush(serial, std::move(closure));
}

void VideoRendererImpl::Stop() {
  stopping_ = true;
  if (sink_) {
    sink_->Stop();
  }
  started_ = false;
}

void VideoRendererImpl::StopAndDrainForTeardown(
    base::OnceClosure on_quiescent) {
  Stop();
  // Same contract as the audio side, and the reason the two are written the
  // same way: Flush() completes every pending read inline with
  // kDecodingAborted, so the reply hop back into this object has already run by
  // the time Flush's callback fires. After that no task naming this object can
  // be created, so the callback may delete it.
  //
  // The demuxer stream is only null before Initialize(), and a renderer that
  // was never initialized has nothing to drain -- so run the closure directly
  // rather than dereferencing it.
  if (!decoder_stream_.demuxer_stream()) {
    if (on_quiescent) {
      std::move(on_quiescent).Run();
    }
    return;
  }
  const int32_t serial = decoder_stream_.demuxer_stream()->serial();
  // A NULL closure is tolerated, not a programming error here: this is a
  // teardown entry point, and a caller that only wants the renderer stopped
  // has no reason to invent a callback. Running a null OnceClosure is a CHECK
  // failure, and the first version of this method had exactly that hole.
  decoder_stream_.Flush(serial, base::BindOnce(
                                    [](base::OnceClosure quiescent) {
                                      if (quiescent) {
                                        std::move(quiescent).Run();
                                      }
                                    },
                                    std::move(on_quiescent)));
}

std::unique_ptr<VideoRendererSink> VideoRendererImpl::TakeSinkForHandover() {
  return std::move(sink_);
}

// ---- clock and pacing inputs, all posted onto S3 by RendererImpl -----------

void VideoRendererImpl::SetMasterClock(base::TimeDelta media_time,
                                       int32_t serial, bool valid) {
  compositor_.SetMasterClock(media_time, serial, valid);
}

void VideoRendererImpl::SetMasterIsVideo(bool master_is_video) {
  compositor_.SetMasterIsVideo(master_is_video);
}

void VideoRendererImpl::SetPlaybackRate(double rate) {
  compositor_.SetPlaybackRate(rate);
}

void VideoRendererImpl::SetPaused(bool paused) {
  paused_ = paused;
  compositor_.SetPaused(paused);
}

void VideoRendererImpl::SetStepMode(bool step) {
  compositor_.SetStepMode(step);
}

void VideoRendererImpl::SetMaxFrameDrop(int max_frame_drop) {
  compositor_.SetMaxFrameDrop(max_frame_drop);
}

void VideoRendererImpl::SetMaxFps(int max_fps) {
  compositor_.SetMaxFps(max_fps);
}

void VideoRendererImpl::SetBufferingBlocked(bool blocked) {
  compositor_.SetBufferingBlocked(blocked);
}

void VideoRendererImpl::BeginAccurateSeek(base::TimeDelta target) {
  compositor_.BeginAccurateSeek(target);
}

void VideoRendererImpl::EndAccurateSeek() {
  compositor_.EndAccurateSeek();
}

void VideoRendererImpl::SetOutputTarget(
    base::scoped_refptr<NativeDisplay> display) {
  if (sink_) {
    sink_->SetOutputTarget(std::move(display));
  }
}

VideoFrameCompositor::Stats VideoRendererImpl::compositor_stats() const {
  return compositor_.GetStats();
}

void VideoRendererImpl::TakeSnapshot(base::TimeDelta at,
                                     Renderer::SnapshotFrameCallback callback) {
  (void)at;  // Recorded by the caller; see Renderer::TakeSnapshot.
  auto frame = compositor_.current_frame();
  if (!frame) {
    std::move(callback).Run(
        MediaError(ErrorCode::kInvalidState, "no frame has been presented yet",
                   "the compositor holds nothing (paused before the first "
                   "present, or a video-less source)",
                   "request the snapshot after the first frame is on screen"),
        nullptr);
    return;
  }
  std::move(callback).Run(MediaError(), std::move(frame));
}

size_t VideoRendererImpl::frames_pending() const {
  return compositor_.frames_pending();
}

// ---- S3: decode pump -------------------------------------------------------

void VideoRendererImpl::PumpDecoder() {
  if (stopping_ || read_outstanding_ || ended_ || paused_) {
    return;
  }
  // Back-pressure without a blocking wait. ffplay blocks its video thread on
  // frame_queue_signal() when pictq is full; here the pump simply does not ask
  // for another frame until the compositor has room, so nothing blocks and
  // nothing is dropped to make room (docs/08 M7's "三级 watermark" lives here).
  if (compositor_.frames_pending() >= max_pending_frames_) {
    return;
  }
  read_outstanding_ = true;
  decoder_stream_.Read(base::BindOnce(
      &VideoRendererImpl::OnDecoderOutput, base::Unretained(this),
      base::BindOnce(&VideoRendererImpl::PumpDecoder, base::Unretained(this))));
}

void VideoRendererImpl::OnDecoderOutput(base::OnceClosure pump_again,
                                        DecoderStatus status,
                                        base::scoped_refptr<VideoFrame> frame) {
  read_outstanding_ = false;
  if (stopping_) {
    return;
  }
  // Video has no EOS marker object: DecoderStream reports end of stream through
  // the status, and VideoDecoderStreamTraits::IsEndOfStreamOutput() is a no-op
  // precisely because a VideoFrame cannot carry that flag (unlike AudioBuffer).
  if (!status.is_ok()) {
    if (status.code() == DecoderStatus::Codes::kDecodingAborted) {
      return;  // a flush is in progress; not an error
    }
    LOG(ERROR) << "avbase.vdec: decode failed (" << status.AsDebugString()
               << ")";
    ended_ = true;
    compositor_.SetEndOfStream();
    ReportEndedOnce();
    return;
  }
  if (frame) {
    // The "vf" stage: created on the first frame (real coded geometry), and
    // rebuilt whenever the geometry moves (Δ3 resolution change). A filter
    // that fails to start is a config bug -- fall back to unfiltered and
    // say so once.
    if (!filter_graph_.empty()) {
      if (filter_ && filter_->CodedSize() != frame->coded_size()) {
        filter_.reset();
      }
      if (!filter_) {
        if (!filter_factory_) {
          LOG(ERROR) << "avbase.vfilter: filter_graph set but no stage "
                        "factory was injected (no-ffmpeg build?); playing "
                        "unfiltered";
          filter_graph_.clear();
        } else {
          filter_ = filter_factory_();
          if (!filter_->Initialize(filter_graph_, frame->format(),
                                   frame->coded_size())) {
            LOG(ERROR) << "avbase.vfilter: graph \"" << filter_graph_
                       << "\" failed to start; playing unfiltered";
            filter_.reset();
            filter_graph_.clear();
          }
        }
      }
    }
    if (filter_) {
      base::scoped_refptr<VideoFrame> filtered;
      if (!filter_->Process(std::move(frame), &filtered)) {
        return;
      }
      frame = std::move(filtered);
    }
    if (!frame) {
      // The graph buffered the frame (EAGAIN); nothing to present now.
      task_runner_->PostTask(FROM_HERE, std::move(pump_again));
      return;
    }
    // Publish immediately: decode and pacing are one causal step (see the
    // threading note in the header for why this is not posted to S1).
    compositor_.PutCurrentFrame(std::move(frame));
    // Posted, not inline: same reasoning as the audio pump -- a synchronous
    // delivery burst recursed once per buffered frame and could exhaust the
    // video thread's stack.
    task_runner_->PostTask(FROM_HERE, std::move(pump_again));
    return;
  }
  // kOk with no frame is how DecoderStream reports a *drained* stream
  // (MaybeDeliver's "EOS is reported as kOk with a null output"); the first
  // draft treated it as "nothing to do" and pumped forever, so a file that
  // played to its end never reached RendererClient::OnEnded().
  ended_ = true;
  compositor_.SetEndOfStream();
  ReportEndedOnce();
}

void VideoRendererImpl::ReportEndedOnce() {
  if (ended_cb_) {
    ended_cb_.Run();
  }
}

void VideoRendererImpl::OnDecoderStreamEvent(DecoderStreamEvent event) {
  if (event == DecoderStreamEvent::kNoDecoderAvailable) {
    LOG(ERROR) << "avbase.vdec: every video decoder candidate failed";
  } else {
    // Δ12: a hardware-to-software fallback is a recovery, not an error, but it
    // must be visible -- it is the difference between "4K is smooth" and
    // "4K is smooth until the driver resets".
    LOG(WARNING) << "avbase.vdec: " << GetDecoderStreamEventName(event)
                 << " -> " << decoder_stream_.GetDisplayName();
  }
}

// ---- S6: the sink's render callback ---------------------------------------

base::scoped_refptr<VideoFrame>
VideoRendererImpl::Render(base::TimeTicks deadline_min,
                          base::TimeTicks deadline_max) {
  // The whole body is one locked call into the compositor. Nothing here may
  // block, allocate or call back into Player: this runs on the sink's render
  // sequence, which for the SDL2 backend is the window's event loop (docs/04
  // §1, S6) -- stalling it freezes input handling as well as video.
  base::scoped_refptr<VideoFrame> frame =
      compositor_.Render(deadline_min, deadline_max);
  // Feed the video clock from here, on S6: this is the one sequence that knows
  // a frame actually left for the display, which mirrors ffplay setting
  // vidclk inside video_refresh(). A null frame means "repeat the previous
  // one", and repeating must not move the clock.
  if (frame && frame_presented_cb_) {
    frame_presented_cb_.Run(frame->timestamp(), frame->serial());
  }
  // Consumer wake-up for the decode pump (counterpart of ffplay's
  // frame_queue_signal): the pump stops at compositor back-pressure, and only
  // a present frees watermark room. Posted to S3; PumpDecoder re-checks every
  // condition there, so a spurious kick is a cheap no-op.
  if (frame) {
    task_runner_->PostTask(FROM_HERE,
                           base::BindOnce(&VideoRendererImpl::PumpDecoder,
                                          base::Unretained(this)));
  }
  return frame;
}

void VideoRendererImpl::OnFrameSubmitFailure() {
  // Counted rather than retried: a present failure is almost always a surface
  // that went away (resize, minimise, compositor restart), and the next
  // Render() will be asked for the right frame anyway. Retrying here would
  // present a frame whose deadline has already passed. base/logging.h has no
  // LOG_EVERY_N (the first draft used it), so the rate-limiting is a plain
  // counter. It must be rate-limited somehow: this runs on the render sequence
  // at display rate, so an unthrottled LOG(WARNING) is ~60 lines/second for as
  // long as the surface is broken.
  const uint64_t n = submit_failures_.fetch_add(1) + 1;
  if (n == 1 || n % 100 == 0) {
    LOG(WARNING) << "avbase.vout: frame submit failed (" << n << " times)";
  }
}

}  // namespace avbase::media
