// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The M8 pipeline wiring. What "ready" means here is deliberately minimal:
// the demuxer probed and the renderer's decoders initialized. Buffering
// high-water marks (M9), accurate-seek framing (M9) and track switching are
// still ahead; the corresponding interface methods fail loudly rather than
// pretending.

#include "media/filters/pipeline_impl.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/default_tick_clock.h"
#include "media/base/native_display.h"

namespace avbase::media {

PipelineImpl::PipelineImpl() = default;

PipelineImpl::~PipelineImpl() {
  // The owner (Player::Impl) must have run Stop() and let the media sequence
  // tear the demuxer and renderer down first (docs/03 §10.1). Destroying them
  // here would race the demux thread and the sink callbacks.
  DCHECK(state_.load() == State::kNew || state_.load() == State::kStopped ||
         state_.load() == State::kError);
}

void PipelineImpl::SetSource(DataSourceDescriptor source,
                             DemuxerOptions options) {
  source_ = std::move(source);
  options_ = std::move(options);
}

void PipelineImpl::SetTickClock(const base::TickClock* clock) {
  tick_clock_ = clock ? clock : base::DefaultTickClock::GetInstance();
}

void PipelineImpl::SetClock(std::shared_ptr<AvSyncController> av_sync) {
  av_sync_ = std::move(av_sync);
}

void PipelineImpl::Start(
    std::unique_ptr<Demuxer> demuxer,
    RendererFactory* renderer_factory,
    RendererType renderer_type,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    Client* client) {
  DCHECK(demuxer);
  DCHECK(renderer_factory);
  DCHECK(client);
  demuxer_ = std::move(demuxer);
  renderer_factory_ = renderer_factory;
  client_ = client;
  media_runner_ = std::move(media_task_runner);
  // Posted, not run inline: the caller may be on any thread, and every piece
  // of media state below is media-sequence-exclusive.
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoStart, base::Unretained(this),
                     renderer_type));
}

void PipelineImpl::DoStart(RendererType renderer_type) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kNew) {
    return;
  }
  state_ = State::kStarting;

  // The demuxer doubles as the renderer's MediaResource (Demuxer derives
  // from it), and the renderer needs the probed stream configs, so the
  // demuxer initializes first and the renderer only starts on success --
  // firing both concurrently handed the renderer a demuxer with no streams.
  renderer_ = renderer_factory_->CreateRenderer(renderer_type, media_runner_);
  if (!renderer_) {
    // No factory could serve this type; report through the client rather
    // than leaving the pipeline half-started.
    state_ = State::kError;
    client_->OnError(PipelineStatusToMediaError(
        PipelineStatus::kFailedToCreatePipeline));
    return;
  }
  demuxer_->Initialize(source_, options_, this, media_runner_,
                       base::BindOnce(&PipelineImpl::OnDemuxerInitialized,
                                      base::Unretained(this)));
}

void PipelineImpl::OnDemuxerInitialized(Status status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  demuxer_ready_ = status.has_value();
  if (!status) {
    state_ = State::kError;
    // The demuxer's Status carries the actionable MediaError already.
    client_->OnError(status.error());
    return;
  }
  // Demuxer ready: the renderer needs the probed stream configs, so only now
  // can it initialize against the demuxer's MediaResource view.
  renderer_->Initialize(demuxer_.get(), this, media_runner_,
                        base::BindOnce(&PipelineImpl::OnRendererInitialized,
                                       base::Unretained(this)));
  {
    base::AutoLock scoped(snapshot_lock_);
    media_info_ = demuxer_->media_info();
  }
  const MediaInfo info = demuxer_->media_info();
  if (info.duration != kNoTimestamp) {
    duration_micros_.store(info.duration.InMicroseconds());
    client_->OnDurationChange(info.duration);
  }
  seekable_.store(info.seekable && !info.is_live);
  MaybeReady();
}

void PipelineImpl::OnRendererInitialized(PipelineStatus status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  renderer_ready_ = status == PipelineStatus::kOk;
  if (!renderer_ready_) {
    state_ = State::kError;
    client_->OnError(PipelineStatusToMediaError(status));
    return;
  }
  MaybeReady();
}

void PipelineImpl::MaybeReady() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kStarting || !demuxer_ready_ || !renderer_ready_) {
    return;
  }
  state_ = State::kReady;
  // Pipeline::Start()'s contract reports readiness through the client's first
  // buffering/duration callbacks; kHaveMetadata is "probed, not yet playing".
  client_->OnBufferingStateChange(BufferingState::kHaveMetadata,
                                  base::TimeDelta());
}

void PipelineImpl::Play() {
  if (media_runner_) {
    media_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&PipelineImpl::DoPlay, base::Unretained(this)));
  }
}

void PipelineImpl::DoPlay() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kReady || !renderer_) {
    return;
  }
  if (!playing_) {
    playing_ = true;
    // No demuxer seek here: the demux loop has been reading from the start
    // position since Initialize() (backpressure-queued), so a seek to 0
    // would only flush and re-read what is already buffered. The renderer's
    // pumps start from the first buffered serial.
    renderer_->StartPlayingFrom(base::TimeDelta());
    return;
  }
  // Resuming a pause: the demuxer is already running; release the sinks.
  renderer_->SetPaused(false);
}

void PipelineImpl::OnDemuxerStarted(Status status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!status) {
    LOG(WARNING) << "avbase.pipeline: demuxer start reported "
                 << status.error().summary();
  }
  if (state_ == State::kReady && renderer_) {
    renderer_->StartPlayingFrom(base::TimeDelta());
  }
}

void PipelineImpl::Pause() {
  if (media_runner_) {
    media_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&PipelineImpl::DoPause, base::Unretained(this)));
  }
}

void PipelineImpl::DoPause() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kReady && renderer_) {
    renderer_->SetPaused(true);
  }
}

void PipelineImpl::SetOutputTarget(base::scoped_refptr<NativeDisplay> display) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoSetOutputTarget, base::Unretained(this),
                     std::move(display)));
}

void PipelineImpl::DoSetOutputTarget(
    base::scoped_refptr<NativeDisplay> display) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (renderer_) {
    renderer_->SetOutputTarget(std::move(display));
  }
}

void PipelineImpl::BeginAccurateSeek(base::TimeDelta target,
                                     base::OnceClosure reached_cb) {
  if (!media_runner_) {
    std::move(reached_cb).Run();
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoBeginAccurateSeek, base::Unretained(this),
                     target, std::move(reached_cb)));
}

void PipelineImpl::DoBeginAccurateSeek(base::TimeDelta target,
                                       base::OnceClosure reached_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (renderer_) {
    renderer_->BeginAccurateSeek(target, std::move(reached_cb));
  }
}

void PipelineImpl::EndAccurateSeek() {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoEndAccurateSeek, base::Unretained(this)));
}

void PipelineImpl::DoEndAccurateSeek() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (renderer_) {
    renderer_->EndAccurateSeek();
  }
}

void PipelineImpl::Seek(base::TimeDelta time, base::OnceClosure seeked_cb) {
  if (!media_runner_) {
    std::move(seeked_cb).Run();
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoSeek, base::Unretained(this), time,
                     std::move(seeked_cb)));
}

void PipelineImpl::SelectAudioTrack(int stream_index,
                                    PipelineStatusCallback cb) {
  if (!media_runner_) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoSelectAudioTrack,
                     base::Unretained(this), stream_index, std::move(cb)));
}

void PipelineImpl::DoSelectAudioTrack(int stream_index,
                                      PipelineStatusCallback cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kReady || !renderer_ || !demuxer_) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  DemuxerStream* target = nullptr;
  const std::vector<DemuxerStream*> audio =
      demuxer_->GetStreams(DemuxerStreamType::kAudio);
  for (DemuxerStream* stream : audio) {
    if (stream->stream_index() == stream_index) {
      target = stream;
      break;
    }
  }
  if (!target) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  // Re-point routing BEFORE the handover: the moment the old renderer stops
  // draining its stream, that stream's queue would wedge the demux loop on
  // its own watermark and starve the very track we are switching to.
  demuxer_->SetActiveStream(DemuxerStreamType::kAudio, stream_index);
  // The renderer reports a failed handover through Client::OnError and still
  // runs the completion closure; the closure itself only means "the attempt
  // finished and the pipeline is consistent".
  renderer_->OnTracksChanged(DemuxerStreamType::kAudio, target,
                             base::BindOnce(
                                 [](PipelineStatusCallback cb) {
                                   std::move(cb).Run(PipelineStatus::kOk);
                                 },
                                 std::move(cb)));
}

void PipelineImpl::DoSeek(base::TimeDelta time, base::OnceClosure seeked_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kReady || !renderer_) {
    std::move(seeked_cb).Run();
    return;
  }
  if (seek_in_flight_) {
    // Collapse: the in-flight seek keeps going and this one starts the moment
    // it finishes. The superseded completion still runs, so a caller matching
    // completions to requests is never left waiting (the facade owns the
    // request ids and its own supersede policy).
    pending_seek_superseded_ = true;
    std::move(seeked_cb).Run();
    return;
  }
  seek_in_flight_ = true;
  seek_time_ = time;
  seek_cb_ = std::move(seeked_cb);
  seek_demuxer_done_ = false;
  seek_renderer_flushed_ = false;
  // Flush the renderer and seek the demuxer in parallel (docs/04 §4.1); only
  // when both have finished is it safe to restart rendering, or a post-seek
  // frame could be scheduled against a pre-seek clock anchor.
  renderer_->Flush(base::BindOnce(&PipelineImpl::OnRendererFlushed,
                                  base::Unretained(this)));
  demuxer_->StartPlayingFrom(
      time,
      base::BindOnce(&PipelineImpl::OnSeekDemuxerDone,
                     base::Unretained(this)));
}

void PipelineImpl::OnRendererFlushed() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  seek_renderer_flushed_ = true;
  FinishSeekIfBothDone();
}

void PipelineImpl::OnSeekDemuxerDone(Status status, base::TimeDelta actual) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!status) {
    LOG(WARNING) << "avbase.pipeline: seek reported "
                 << status.error().summary() << " at "
                 << actual.InSecondsF() << "s";
  }
  seek_demuxer_done_ = true;
  FinishSeekIfBothDone();
}

void PipelineImpl::FinishSeekIfBothDone() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!seek_in_flight_ || !seek_demuxer_done_ || !seek_renderer_flushed_) {
    return;
  }
  seek_in_flight_ = false;
  pending_seek_superseded_ = false;
  if (state_ == State::kReady && playing_ && renderer_) {
    renderer_->StartPlayingFrom(seek_time_);
  }
  if (seek_cb_) {
    std::move(seek_cb_).Run();
  }
}

void PipelineImpl::Stop() {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoStop, base::Unretained(this)));
}

void PipelineImpl::DoStop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kStopped || state_ == State::kStopping) {
    return;
  }
  state_ = State::kStopping;
  playing_ = false;
  // Order (docs/03 §10.1): renderer first -- destroying it stops both sinks,
  // which guarantees no further Render() calls -- then the demuxer, whose
  // Stop() interrupts the demux thread's blocking av_read_frame and joins it.
  renderer_.reset();
  if (demuxer_) {
    demuxer_->Stop();
    demuxer_.reset();
  }
  state_ = State::kStopped;
}

bool PipelineImpl::IsRunning() const {
  const State s = state_.load();
  return s == State::kStarting || s == State::kReady;
}

// The Set* family posts a task that reads |renderer_| on the media sequence
// rather than capturing renderer_.get() on the caller's thread: S1 may be
// tearing the renderer down concurrently with a call from any other thread.

void PipelineImpl::SetVolume(float volume) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce([](PipelineImpl* self, float v) {
        if (self->renderer_) {
          self->renderer_->SetVolume(v);
        }
      }, base::Unretained(this), volume));
}

void PipelineImpl::SetPlaybackRate(double rate) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce([](PipelineImpl* self, double r) {
        if (self->renderer_) {
          self->renderer_->SetPlaybackRate(r);
        }
      }, base::Unretained(this), rate));
}

void PipelineImpl::SetLatencyHint(base::TimeDelta hint) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce([](PipelineImpl* self, base::TimeDelta h) {
        if (self->renderer_) {
          self->renderer_->SetLatencyHint(h);
        }
      }, base::Unretained(this), hint));
}

void PipelineImpl::SetPreservesPitch(bool preserves_pitch) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce([](PipelineImpl* self, bool p) {
        if (self->renderer_) {
          self->renderer_->SetPreservesPitch(p);
        }
      }, base::Unretained(this), preserves_pitch));
}

base::TimeDelta PipelineImpl::GetMediaTime() {
  if (!av_sync_) {
    return kNoTimestamp;
  }
  return av_sync_->GetMasterClock();
}

base::TimeDelta PipelineImpl::GetBufferedTime() const {
  return base::TimeDelta::FromMicroseconds(buffered_micros_.load());
}

base::TimeDelta PipelineImpl::GetDuration() const {
  return base::TimeDelta::FromMicroseconds(duration_micros_.load());
}

Pipeline::Statistics PipelineImpl::GetStatistics() const {
  Pipeline::Statistics out;
  base::AutoLock scoped(snapshot_lock_);
  out.buffered_time = base::TimeDelta::FromMicroseconds(
      buffered_micros_.load());
  out.duration = base::TimeDelta::FromMicroseconds(duration_micros_.load());
  out.total_bytes_read = last_stats_.total_bytes_read;
  out.video_frames_presented = last_stats_.video_frames_presented;
  out.video_frames_dropped = last_stats_.video_frames_dropped;
  out.audio_glitches = last_stats_.audio_glitches;
  out.avg_av_diff_ms = last_stats_.avg_av_diff_ms;
  out.seek_count = last_stats_.seek_count;
  return out;
}

bool PipelineImpl::CanSeekForward() const {
  return seekable_.load();
}

bool PipelineImpl::CanSeekBackward() const {
  return seekable_.load();
}

}  // namespace avbase::media
