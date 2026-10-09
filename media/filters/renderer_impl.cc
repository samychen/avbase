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
// Never compiled. The header carries the gap list; the two most consequential
// are gap 3 (text tracks are accepted and silently ignored, which is a lie the
// SDK must not tell) and gap 2 (AudioParameters are synthesised here because
// media/audio/ does not exist).

#include "media/filters/renderer_impl.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/synchronization/waitable_event.h"
#include "media/base/media_constants.h"

namespace avbase::media {
namespace {

// How often the master clock is pushed from S1 to S3. 10 ms is a third of a
// 30 Hz frame interval, so a frame's lateness is never judged against a clock
// more than a third of a frame stale -- tighter than that buys nothing the
// compositor can use, and looser starts to show up as judder.

// Statistics period. Matches PlayerConfig::stats_interval's default so that the
// SDK's kStats event and the pipeline's OnStatisticsUpdate agree.

}  // namespace

RendererImpl::RendererImpl(Deps deps) : deps_(std::move(deps)) {
  CHECK(deps_.media_task_runner);
  CHECK(deps_.video_task_runner);
  CHECK(deps_.audio_task_runner);
  CHECK(deps_.tick_clock);
  CHECK(deps_.av_sync);
}

// Audio-specific teardown: stop + drain inline, then destroy, in ONE task on
// the owning sequence. See the comment at the call site in ~RendererImpl.
static void QuiesceAndDestroyOn(
    const base::scoped_refptr<base::SequencedTaskRunner>& runner,
    std::unique_ptr<AudioRendererImpl>* member) {
  base::WaitableEvent done;
  runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::unique_ptr<AudioRendererImpl>* m, base::WaitableEvent* e) {
            if (*m) {
              (*m)->StopAndDrainForTeardown(base::DoNothing());
              m->reset();
            }
            e->Signal();
          },
          member, &done));
  done.Wait();
}

template <typename T>
static void
DestroyOn(const base::scoped_refptr<base::SequencedTaskRunner>& runner,
          std::unique_ptr<T>* member) {
  base::WaitableEvent done;
  runner->PostTask(FROM_HERE,
                   base::BindOnce(
                       [](std::unique_ptr<T>* m, base::WaitableEvent* e) {
                         m->reset();
                         e->Signal();
                       },
                       member, &done));
  done.Wait();
}

RendererImpl::~RendererImpl() {
  // Fixed destruction order (docs/03 §10.1): consumers before producers, and
  // av_sync_ last because both sub-renderers hold a pointer to it.
  //
  // The sub-renderers are destroyed ON THEIR OWN SEQUENCES: their internals
  // (DecoderStream, compositor) carry sequence checkers attached to S3/S4,
  // and a first draft reset them here on S1 -- the checkers caught it
  // immediately. The unbounded Wait() is bounded from the outside by the
  // caller's shutdown contract (Player::StopSync detaches after
  // config.shutdown_timeout, Δ15), and the sinks' own Stop() guarantees no
  // further Render() callbacks while this runs.
  if (video_) {
    DestroyOn(deps_.video_task_runner.get(), &video_);
  }
  // The audio renderer must QUIESCE on S4 before its memory goes away: a
  // pump task whose read reply was still in flight would otherwise run after
  // the destroy and lock a freed mutex (ASan/abort caught exactly that in the
  // text-leg tests). StopAndDrainForTeardown completes the outstanding read
  // INLINE (DecoderStream::Flush -> Deliver runs the callback on the caller),
  // and after stopping_ is set no self-post can name this object again -- so
  // the same task that drains can safely destroy. DestroyOn alone has the
  // documented hole: tasks queued behind its reset run on freed memory.
  QuiesceAndDestroyOn(deps_.audio_task_runner.get(), &audio_);
  for (auto& retired : retired_audio_) {
    QuiesceAndDestroyOn(deps_.audio_task_runner.get(), &retired);
  }
  // The video graveyard uses DestroyOn rather than QuiesceAndDestroyOn: a
  // handed-back sink means the retiring renderer has nothing left to quiesce
  // (its sink is null), so the in-flight-decode drain that the audio path
  // needs has already happened inside the handover itself.
  DestroyOn(deps_.video_task_runner.get(), &video_);
  for (auto& retired : retired_video_) {
    DestroyOn(deps_.video_task_runner.get(), &retired);
  }
  av_sync_.reset();
}

// static
AudioParameters
RendererImpl::MakeAudioParameters(const AudioDecoderConfig& config,
                                  int frames_per_buffer) {
  // Gap 2: a real AudioManager would negotiate the device period and could
  // return something the hardware prefers; until media/audio/ exists this is a
  // fixed guess, and AudioRendererSink::IsOptimizedForHardwareParameters() is
  // how a backend says "you guessed wrong".
  //
  // config.channels is passed explicitly rather than left to be derived from
  // config.channel_layout: the layout collapses every count without a named
  // form (3, 4, 5, 7) to kDiscrete, which reads back as 0 channels and used to
  // abort the audio renderer.
  return AudioParameters(config.channel_layout, config.sample_format,
                         config.sample_rate, frames_per_buffer,
                         config.channels);
}

void RendererImpl::Initialize(
    MediaResource* media_resource, RendererClient* client,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    PipelineStatusCallback init_cb) {
  DCHECK(media_resource);
  DCHECK(client);
  client_ = client;
  if (media_task_runner) {
    deps_.media_task_runner = std::move(media_task_runner);
  }
  av_sync_ = deps_.av_sync;

  DemuxerStream* video_stream =
      deps_.video_disabled
          ? nullptr
          : media_resource->GetStream(DemuxerStreamType::kVideo);
  DemuxerStream* audio_stream =
      deps_.audio_disabled
          ? nullptr
          : media_resource->GetStream(DemuxerStreamType::kAudio);
  has_video_ = video_stream != nullptr;
  has_audio_ = audio_stream != nullptr;
  if (!has_video_ && !has_audio_) {
    // Not "no media": a subtitle-only file is a legitimate input, but this
    // renderer has no TextRenderer (gap 3), so it genuinely cannot play it.
    //
    // Reported through the same hop as the success path rather than inline:
    // renderer.h says |init_cb| "runs on the media sequence and is never run
    // inline, so a caller may safely destroy state in it", and this path broke
    // that -- the caller was re-entered while Initialize() was still on the
    // stack. RendererImplTest.InitCallbackIsNeverRunInline is the regression
    // test; it failed against the inline version.
    pending_init_cb_ = std::move(init_cb);
    CompleteInitialization(PipelineStatus::kMissingDemuxerStreams);
    return;
  }
  // An audio-only or video-only stream must fall back to the external clock;
  // AvSyncController::ResolveMasterType() does that, but only if it is told
  // which streams exist.
  av_sync_->SetStreamAvailability(has_audio_, has_video_);

  // Held before the sub-renderers are built. Their completion callbacks come
  // back to S1, so nothing can run this callback before Initialize() returns,
  // but taking ownership first keeps the "exactly one owner" rule visible.
  pending_init_cb_ = std::move(init_cb);
  CreateSubRenderers(video_stream, audio_stream);
  if (!video_ && !audio_) {
    MaybeReportInitialized();
  }
}

void RendererImpl::CreateSubRenderers(DemuxerStream* video_stream,
                                      DemuxerStream* audio_stream) {
  if (has_video_) {
    // Recorded next to the renderer, for the same reason audio_stream_ is
    // (media/base/media_resource.h: identity for "no switch needed" and for a
    // handover to re-anchor against).
    video_stream_ = video_stream;
    video_ = std::make_unique<VideoRendererImpl>(
        deps_.video_task_runner, deps_.video_factories, deps_.tick_clock,
        deps_.compositor_thresholds);
    if (!deps_.video_filter_graph.empty()) {
      video_->set_filter_graph(deps_.video_filter_graph,
                               deps_.video_filter_factory);
    }
    // Forwarded, not applied here: the ranking needs the stream's config, which
    // only exists once Initialize() runs on S3 (see
    // VideoRendererImpl::set_decoder_preference).
    if (deps_.video_decoder_preference_set) {
      video_->set_decoder_preference(deps_.video_decoder_preference,
                                     deps_.video_hw_codecs);
    }
  }
  if (has_audio_) {
    audio_stream_ = audio_stream;
    audio_params_ = MakeAudioParameters(audio_stream->audio_decoder_config(),
                                        deps_.audio_frames_per_buffer);
    audio_ = std::make_unique<AudioRendererImpl>(
        deps_.audio_task_runner, deps_.audio_factories, av_sync_.get());
    if (!deps_.audio_filter_graph.empty()) {
      audio_->set_filter_graph(deps_.audio_filter_graph,
                               deps_.audio_filter_factory);
    }
  }

  // Initialise the sub-renderers on their own sequences. Both callbacks come
  // back to S1, where MaybeReportInitialized() decides when the pipeline may be
  // told that startup finished.
  if (video_) {
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&VideoRendererImpl::Initialize,
                       base::Unretained(video_.get()), video_stream,
                       std::move(deps_.video_sink),
                       base::BindOnce(&RendererImpl::PostVideoInitialized,
                                      base::Unretained(this))));
  }
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&AudioRendererImpl::Initialize,
                       base::Unretained(audio_.get()), audio_stream,
                       audio_params_, deps_.audio_sink,
                       base::BindOnce(&RendererImpl::PostAudioInitialized,
                                      base::Unretained(this))));
  }
  // The ended callbacks fire on the sub-renderers' own sequences (S3/S4); the
  // decision "everything has drained" belongs to S1, so each callback only
  // posts a hop. CheckForEnded() then reads the two thread-safe buffered
  // getters (the compositor's lock and the audio handoff lock) instead of the
  // sub-renderers' plain bools, which would be a data race from S1.
  if (video_) {
    video_->set_ended_cb(base::BindRepeating(&RendererImpl::PostVideoEnded,
                                             base::Unretained(this)));
    video_->set_frame_presented_cb(base::BindRepeating(
        &RendererImpl::OnVideoFramePresented, base::Unretained(this)));
  }
  if (audio_) {
    audio_->set_ended_cb(base::BindRepeating(&RendererImpl::PostAudioEnded,
                                             base::Unretained(this)));
  }
}

// The two ended hops, and the two init hops below, are the class-level
// closure of the "task outlives its target" race for this class. They used to
// be Unretained, which made the renderer's destruction depend on every
// in-flight hop having already run -- a property no caller can establish, and
// which the suites could only approximate by waiting for the whole S1 queue to
// go quiet (tests/unit/media_filters/renderer_impl_unittest.cc, where a test
// that never reached EOS sat in that wait for its full 15 s ceiling). Binding
// the WeakPtr moves the guarantee into the binding layer: a hop that runs after
// ~RendererImpl is a no-op instead of a use-after-free, so teardown no longer
// has to be timed.
void RendererImpl::PostVideoEnded() {
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RendererImpl::OnVideoStreamEnded,
                                weak_factory_.GetWeakPtr()));
}

void RendererImpl::PostAudioEnded() {
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RendererImpl::OnAudioStreamEnded,
                                weak_factory_.GetWeakPtr()));
}

void RendererImpl::OnVideoStreamEnded() {
  video_ended_ = true;
  CheckForEnded();
}

void RendererImpl::OnAudioStreamEnded() {
  audio_ended_ = true;
  CheckForEnded();
}

void RendererImpl::CheckBufferingTransitions() {
  // Only PushMasterClock() calls this, on S1; this TU carries no sequence
  // checker (the class's S1 discipline is by construction and by the callers
  // that do have checkers).
  // Each sub-renderer publishes its own verdict (event-driven on the audio
  // side: the device callback knows exactly when it could not serve a
  // period; WSOLA's residual OLA window makes queue-depth tests
  // unsatisfiable mid-stream). Video uses the compositor's live pending
  // count. RendererImpl only merges -- it is not in a position to guess.
  const bool starved = rendering_ && !ended_ &&
                       (!video_ || video_->frames_pending() == 0) &&
                       (!audio_ || audio_->starved());
  if (starved == starved_reported_) {
    return;
  }
  starved_reported_ = starved;
  LOG(INFO) << "[hwm] edge " << (starved ? "DRY" : "RECOVER")
            << " video_pending="
            << (video_ ? static_cast<int>(video_->frames_pending()) : -1)
            << " audio_buffered=" << (audio_ ? audio_->buffered_frames() : -1);
  if (client_) {
    client_->OnBufferingStateChange(starved ? BufferingState::kHaveNothing
                                            : BufferingState::kHaveEnough,
                                    base::TimeDelta());
  }
}

void RendererImpl::CheckForEnded() {
  if (!initialized_ || ended_) {
    return;
  }
  const bool video_done =
      !video_ || (video_ended_ && video_->frames_pending() == 0);
  const bool audio_done =
      !audio_ || (audio_ended_ && audio_->buffered_frames() == 0);
  if (video_done && audio_done) {
    OnEnded();
  }
}

void RendererImpl::PostVideoInitialized(PipelineStatus status) {
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RendererImpl::OnVideoInitialized,
                                weak_factory_.GetWeakPtr(), status));
}

void RendererImpl::PostAudioInitialized(PipelineStatus status) {
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RendererImpl::OnAudioInitialized,
                                weak_factory_.GetWeakPtr(), status));
}

void RendererImpl::OnVideoInitialized(PipelineStatus status) {
  video_initialized_ = true;
  if (status != PipelineStatus::kOk) {
    CompleteInitialization(status);
    return;
  }
  MaybeReportInitialized();
}

void RendererImpl::OnAudioInitialized(PipelineStatus status) {
  audio_initialized_ = true;
  if (status != PipelineStatus::kOk) {
    ReportError(PipelineStatusToMediaError(status));
    return;
  }
  MaybeReportInitialized();
}

void RendererImpl::MaybeReportInitialized() {
  // Whichever sub-renderer finishes last reports, and only once. A pipeline
  // that is told "ready" twice starts playback twice; one that is never told
  // hangs forever, so the guard is on both sides.
  const bool video_done = !has_video_ || video_initialized_;
  const bool audio_done = !has_audio_ || audio_initialized_;
  if (!video_done || !audio_done || initialized_) {
    return;
  }
  initialized_ = true;
  LOG(INFO) << "avbase.pipeline: renderer ready (video=" << has_video_
            << " audio=" << has_audio_ << ")";
  if (pending_init_cb_) {
    CompleteInitialization(PipelineStatus::kOk);
  }
}

void RendererImpl::StartPlayingFrom(base::TimeDelta time) {
  DCHECK(initialized_);
  if (!initialized_) {
    return;
  }
  // The buffering-starvation check only applies while rendering. NOT
  // resetting starved_reported_ here: after a seek's Flush (which publishes
  // the deterministic DRY edge), clearing the flag would swallow the
  // RECOVER edge whenever data refills before the first 10 ms tick -- and a
  // dry moment right after Play is a legitimate kHaveNothing anyway
  // (buffering before the first frame, ffplay semantics).
  rendering_ = true;
  start_time_ = time;
  ended_ = false;
  video_ended_ = false;
  audio_ended_ = false;
  // Anchor the external clock at the start point: it has no writer of its own
  // (ffplay leaves extclk to extrapolate from the last set_clock), and when
  // the master falls back to external -- video-only files, muted audio, a
  // device that never delivers -- an unanchored clock would report
  // kNoTimestamp forever and the compositor would never present.
  if (av_sync_) {
    av_sync_->SetExternalClock(time, av_sync_->master_serial());
  }
  if (video_) {
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::StartPlayingFrom,
                                  base::Unretained(video_.get()), time));
  }
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AudioRendererImpl::StartPlayingFrom,
                                  base::Unretained(audio_.get()), time));
  }
  if (!clock_push_scheduled_) {
    clock_push_scheduled_ = true;
    deps_.media_task_runner->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&RendererImpl::PushMasterClock,
                       weak_factory_.GetWeakPtr()),
        kClockPushInterval);
  }
  if (!stats_scheduled_) {
    stats_scheduled_ = true;
    deps_.media_task_runner->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&RendererImpl::PushStatistics,
                       weak_factory_.GetWeakPtr()),
        kStatsInterval);
  }
  PumpText();
}

// The master-clock push loop, statistics sampler and snapshot grab live in
// renderer_impl_reporting.cc (C1 split): they read av_sync_ and the
// sub-renderers and report outward, and are not part of the
// initialize/flush/ended lifecycle carried here.

void RendererImpl::Flush(base::OnceClosure flush_cb) {
  if (!initialized_) {
    std::move(flush_cb).Run();
    return;
  }
  // Flush order matters: stop the consumer (video pacing) before the producer
  // (audio clock), or a frame can be scheduled against a clock that has already
  // been invalidated. Both are posted, and the callback runs after both have
  // completed, which is what makes Player::SeekTo()'s "flush done" meaningful.
  av_sync_->Flush();
  ended_ = false;
  video_ended_ = false;
  audio_ended_ = false;
  // The text leg's pending Read is aborted by the demuxer flush; the
  // generation bump makes the reply a no-op, and the pump re-arms at
  // StartPlayingFrom.
  ++text_generation_;
  text_read_outstanding_ = false;
  text_ended_ = false;
  // A flush empties every queue by definition, so the starvation edge is
  // deterministic here -- emitting it directly rather than letting the 10 ms
  // sampler race the demuxer's refill (a sub-10ms dry window is invisible to
  // a sample, and the facade's HWM must see every seek's fresh cycle).
  if (rendering_ && !starved_reported_) {
    starved_reported_ = true;
    if (client_) {
      client_->OnBufferingStateChange(BufferingState::kHaveNothing,
                                      base::TimeDelta());
    }
  }
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&AudioRendererImpl::Flush,
                       base::Unretained(audio_.get()), base::DoNothing()));
  }
  if (video_) {
    const int32_t serial = av_sync_->master_serial();
    // The video flush completes on S3 (where the decoder reset runs); the
    // pipeline's completion contract is S1, so hop it back. The sequence
    // checker caught the first draft running it inline.
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](VideoRendererImpl* v, int32_t s, base::OnceClosure done,
               base::scoped_refptr<base::SequencedTaskRunner> media) {
              v->Flush(
                  s, base::BindOnce(
                         [](base::OnceClosure d,
                            base::scoped_refptr<base::SequencedTaskRunner> m) {
                           m->PostTask(FROM_HERE, std::move(d));
                         },
                         std::move(done), std::move(media)));
            },
            base::Unretained(video_.get()), serial, std::move(flush_cb),
            deps_.media_task_runner));
  } else {
    std::move(flush_cb).Run();
  }
}

void RendererImpl::CompleteInitialization(PipelineStatus status) {
  // Hop to the media sequence: sub-renderers complete on S3/S4, and
  // PipelineImpl's sequence checker (correctly) rejects anything else.
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce([](PipelineStatusCallback cb,
                                   PipelineStatus s) { std::move(cb).Run(s); },
                                std::move(pending_init_cb_), status));
}

void RendererImpl::ReportError(MediaError error) {
  // May be called from S3/S4 (a sub-renderer's init failure); the pipeline
  // client runs on S1.
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(
                     [](RendererImpl* self, MediaError e) {
                       if (self->client_) {
                         self->client_->OnError(std::move(e));
                       }
                     },
                     base::Unretained(this), std::move(error)));
}

void RendererImpl::OnEnded() {
  if (ended_ || !client_) {
    return;
  }
  ended_ = true;
  client_->OnEnded();
}

}  // namespace avbase::media
