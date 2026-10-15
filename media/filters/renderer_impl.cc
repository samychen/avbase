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

#include <algorithm>
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
  // A flush whose last leg had not answered yet still owes the pipeline its
  // completion. The single-leg code this replaces posted the caller's closure
  // straight to S1 -- no renderer state, so it survived teardown; the fan-out
  // barrier (docs/14 stage 1) holds it in a member instead, so teardown runs
  // it. First thing, so a completion that re-enters the renderer finds a
  // fully alive object.
  if (video_flush_done_cb_) {
    std::move(video_flush_done_cb_).Run();
  }
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
  //
  // One loop per video source (docs/14 stage 1): every leg is destroyed the
  // same way, on S3, before the graveyard is drained.
  for (auto& video : video_renderers_) {
    if (video) {
      DestroyOn(deps_.video_task_runner.get(), &video);
    }
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
  media_resource_ = media_resource;
  if (media_task_runner) {
    deps_.media_task_runner = std::move(media_task_runner);
  }
  av_sync_ = deps_.av_sync;

  // Discover every source's streams through the addressing axis (docs/14
  // stage 1). The no-axis GetStream(type) the old code called IS source 0, so
  // a single-source resource walks exactly the same path it always did.
  const int source_count = media_resource->source_count();
  std::vector<SourceStreams> sources;
  sources.reserve(source_count);
  bool any_media = false;
  for (int id = 0; id < source_count; ++id) {
    SourceStreams s;
    s.source_id = id;
    s.video = deps_.video_disabled
                  ? nullptr
                  : media_resource->GetStream(id, DemuxerStreamType::kVideo);
    s.audio = deps_.audio_disabled
                  ? nullptr
                  : media_resource->GetStream(id, DemuxerStreamType::kAudio);
    any_media = any_media || s.video != nullptr || s.audio != nullptr;
    sources.push_back(s);
    // Declare the source to the clock controller (docs/14 stage 2), so a
    // later SetSourceOffset() for it is accepted rather than ignored: every
    // source, not only video-bearing ones -- the offset is per demuxer.
    if (av_sync_) {
      av_sync_->RegisterSource(id);
    }
  }
  // Source 0 is the clock-relevant source: its audio is the only audio leg
  // (§4.2) and its video is the only one feeding the master video clock. An
  // empty aggregate (source_count() == 0) has no source 0 at all, which is
  // the same "nothing to play" case as a subtitle-only file below.
  has_video_ = !sources.empty() && sources[0].video != nullptr;
  has_audio_ = !sources.empty() && sources[0].audio != nullptr;
  if (!any_media) {
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
  CreateSubRenderers(sources);
  if (video_renderer_count() == 0 && !audio_) {
    MaybeReportInitialized();
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
void RendererImpl::PostVideoEnded(int source_index) {
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RendererImpl::OnVideoStreamEnded,
                                weak_factory_.GetWeakPtr(), source_index));
}

void RendererImpl::PostAudioEnded() {
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RendererImpl::OnAudioStreamEnded,
                                weak_factory_.GetWeakPtr()));
}

void RendererImpl::OnVideoStreamEnded(int source_index) {
  const size_t i = static_cast<size_t>(source_index);
  if (i >= video_ended_.size()) {
    return;  // Reply from a leg that no longer exists (post-teardown tick).
  }
  video_ended_[i] = true;
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
  //
  // The merge now spans every video leg (docs/14 stage 1): the pipeline is
  // starved when EVERY leg has nothing pending, so one lagging secondary
  // source cannot mask a dry primary.
  bool any_video = false;
  bool all_video_dry = true;
  for (const auto& v : video_renderers_) {
    if (!v) {
      continue;
    }
    any_video = true;
    if (v->frames_pending() != 0) {
      all_video_dry = false;
    }
  }
  const bool starved = rendering_ && !ended_ && (!any_video || all_video_dry) &&
                       (!audio_ || audio_->starved());
  if (starved == starved_reported_) {
    return;
  }
  starved_reported_ = starved;
  const int primary_pending =
      video_renderers_.empty() || !video_renderers_[0]
          ? -1
          : static_cast<int>(video_renderers_[0]->frames_pending());
  LOG(INFO) << "[hwm] edge " << (starved ? "DRY" : "RECOVER")
            << " video_pending=" << primary_pending
            << " video_legs=" << video_renderer_count()
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
  // Every video leg must both report EOS and drain (docs/14 stage 1). The old
  // single-leg rule was this rule with N == 1.
  bool video_done = true;
  for (size_t i = 0; i < video_renderers_.size(); ++i) {
    const auto& v = video_renderers_[i];
    if (!v) {
      continue;
    }
    if (!video_ended_[i] || v->frames_pending() != 0) {
      video_done = false;
      break;
    }
  }
  const bool audio_done =
      !audio_ || (audio_ended_ && audio_->buffered_frames() == 0);
  if (video_done && audio_done) {
    OnEnded();
  }
}

void RendererImpl::PostVideoInitialized(int source_index,
                                        PipelineStatus status) {
  deps_.media_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&RendererImpl::OnVideoInitialized,
                     weak_factory_.GetWeakPtr(), source_index, status));
}

void RendererImpl::PostAudioInitialized(PipelineStatus status) {
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RendererImpl::OnAudioInitialized,
                                weak_factory_.GetWeakPtr(), status));
}

void RendererImpl::OnVideoInitialized(int source_index, PipelineStatus status) {
  const size_t i = static_cast<size_t>(source_index);
  if (i < video_initialized_.size()) {
    video_initialized_[i] = true;
  }
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
  //
  // The video half waits for EVERY leg (docs/14 stage 1): "the renderer is
  // ready" means every decode chain it built has its decoder, and a pipeline
  // that starts while a secondary leg is still initializing would race that
  // leg's first StartPlayingFrom against its own Initialize.
  bool video_done = true;
  for (bool initialized : video_initialized_) {
    if (!initialized) {
      video_done = false;
      break;
    }
  }
  const bool audio_done = !has_audio_ || audio_initialized_;
  if (!video_done || !audio_done || initialized_) {
    return;
  }
  initialized_ = true;
  LOG(INFO) << "avbase.pipeline: renderer ready (video=" << has_video_
            << " audio=" << has_audio_
            << " video_legs=" << video_renderer_count() << ")";
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
  std::fill(video_ended_.begin(), video_ended_.end(), false);
  audio_ended_ = false;
  // Anchor the external clock at the start point: it has no writer of its own
  // (ffplay leaves extclk to extrapolate from the last set_clock), and when
  // the master falls back to external -- video-only files, muted audio, a
  // device that never delivers -- an unanchored clock would report
  // kNoTimestamp forever and the compositor would never present.
  if (av_sync_) {
    av_sync_->SetExternalClock(time, av_sync_->master_serial());
  }
  // Every leg starts (docs/14 stage 1); each renderer is posted on S3.
  for (auto& v : video_renderers_) {
    if (v) {
      deps_.video_task_runner->PostTask(
          FROM_HERE, base::BindOnce(&VideoRendererImpl::StartPlayingFrom,
                                    base::Unretained(v.get()), time));
    }
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

// The class's other seams, all C1 line-count splits: sub-renderer
// construction and callback wiring live in renderer_impl_sources.cc; the
// post-startup control surface (the Set* family, the accurate-seek window and
// both track switches) in renderer_impl_controls.cc; and the master-clock
// push loop, statistics sampler and snapshot grab in
// renderer_impl_reporting.cc. Those last ones read av_sync_ and the
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
  std::fill(video_ended_.begin(), video_ended_.end(), false);
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
  // Video fans out to every leg (docs/14 stage 1). Each leg's Flush completes
  // on S3 (where the decoder reset runs) and hops the completion back to S1;
  // the barrier counts them so the pipeline's callback still means "all legs
  // flushed", which is what makes Player::SeekTo()'s "flush done" meaningful.
  size_t pending = 0;
  for (const auto& v : video_renderers_) {
    if (v) {
      ++pending;
    }
  }
  if (pending == 0) {
    std::move(flush_cb).Run();
    return;
  }
  DCHECK_EQ(pending_video_flushes_, 0u);
  DCHECK(!video_flush_done_cb_);
  pending_video_flushes_ = pending;
  video_flush_done_cb_ = std::move(flush_cb);
  const int32_t serial = av_sync_->master_serial();
  // Captured, not bound: this bind.h unwraps a BOUND WeakPtr to a raw pointer
  // (base/functional/bind.h UnwrapArg), so a lambda parameter typed WeakPtr
  // would be handed a RendererImpl*. The capture keeps the token a WeakPtr,
  // and the inner BindOnce binds it as the method's receiver -- which is where
  // the liveness check lives.
  const base::WeakPtr<RendererImpl> weak = weak_factory_.GetWeakPtr();
  for (auto& v : video_renderers_) {
    if (!v) {
      continue;
    }
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [weak](VideoRendererImpl* v, int32_t s,
                   base::SequencedTaskRunner* media) {
              v->Flush(s, base::BindOnce(
                              [weak](base::SequencedTaskRunner* media) {
                                // Weak, like every other S3->S1 hop in this
                                // class: a flush completing after ~RendererImpl
                                // must be a no-op, not a walk through freed
                                // members.
                                media->PostTask(
                                    FROM_HERE,
                                    base::BindOnce(
                                        &RendererImpl::OnVideoFlushed, weak));
                              },
                              media));
            },
            base::Unretained(v.get()), serial, deps_.media_task_runner.get()));
  }
}

void RendererImpl::OnVideoFlushed() {
  DCHECK_GT(pending_video_flushes_, 0u);
  if (pending_video_flushes_ > 0) {
    --pending_video_flushes_;
  }
  if (pending_video_flushes_ != 0) {
    return;
  }
  if (video_flush_done_cb_) {
    std::move(video_flush_done_cb_).Run();
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
