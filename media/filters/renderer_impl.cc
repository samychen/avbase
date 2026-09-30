// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// Never compiled. The header carries the
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// gap list; the two most consequential are gap 3 (text tracks are accepted and
// silently ignored, which is a lie the SDK must not tell) and gap 2
// (AudioParameters are synthesised here because media/audio/ does not exist).

#include "media/filters/renderer_impl.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/media_constants.h"

namespace ijkpp::media {
namespace {

// How often the master clock is pushed from S1 to S3. 10 ms is a third of a
// 30 Hz frame interval, so a frame's lateness is never judged against a clock
// more than a third of a frame stale -- tighter than that buys nothing the
// compositor can use, and looser starts to show up as judder.
constexpr base::TimeDelta kClockPushInterval = base::Milliseconds(10);

// Statistics period. Matches PlayerConfig::stats_interval's default so that the
// SDK's kStats event and the pipeline's OnStatisticsUpdate agree.
constexpr base::TimeDelta kStatsInterval = base::Seconds(1);

}  // namespace

RendererImpl::RendererImpl(Deps deps) : deps_(std::move(deps)) {
  CHECK(deps_.media_task_runner);
  CHECK(deps_.video_task_runner);
  CHECK(deps_.audio_task_runner);
  CHECK(deps_.tick_clock);
}

RendererImpl::~RendererImpl() {
  // Fixed destruction order (docs/03 §10.1): consumers before producers. The
  // sinks are inside the sub-renderers and their Stop() guarantees no further
  // Render() calls, so destroying video_ and audio_ here cannot race with a
  // callback that touches this object. av_sync_ goes last because both
  // sub-renderers hold a pointer to it.
  video_.reset();
  audio_.reset();
  av_sync_.reset();
}

// static
AudioParameters RendererImpl::MakeAudioParameters(
    const AudioDecoderConfig& config, int frames_per_buffer) {
  // Gap 2: a real AudioManager would negotiate the device period and could
  // return something the hardware prefers; until media/audio/ exists this is a
  // fixed guess, and AudioRendererSink::IsOptimizedForHardwareParameters() is
  // how a backend says "you guessed wrong".
  return AudioParameters(config.channel_layout, config.sample_format,
                         config.sample_rate, frames_per_buffer);
}

void RendererImpl::Initialize(MediaResource* media_resource,
                              RendererClient* client,
                              base::scoped_refptr<base::SequencedTaskRunner>
                                  media_task_runner,
                              PipelineStatusCallback init_cb) {
  DCHECK(media_resource);
  DCHECK(client);
  client_ = client;
  if (media_task_runner) {
    deps_.media_task_runner = std::move(media_task_runner);
  }
  av_sync_ = std::make_unique<AvSyncController>(
      deps_.sync_master, deps_.tick_clock, deps_.sync_thresholds);

  DemuxerStream* video_stream =
      media_resource->GetStream(DemuxerStreamType::kVideo);
  DemuxerStream* audio_stream =
      media_resource->GetStream(DemuxerStreamType::kAudio);
  has_video_ = video_stream != nullptr;
  has_audio_ = audio_stream != nullptr;
  if (!has_video_ && !has_audio_) {
    // Not "no media": a subtitle-only file is a legitimate input, but this
    // renderer has no TextRenderer (gap 3), so it genuinely cannot play it.
    std::move(init_cb).Run(PipelineStatus::kMissingDemuxerStreams);
    return;
  }
  // An audio-only or video-only stream must fall back to the external clock;
  // AvSyncController::ResolveMasterType() does that, but only if it is told
  // which streams exist.
  av_sync_->SetStreamAvailability(has_audio_, has_video_);

  if (has_video_) {
    video_ = std::make_unique<VideoRendererImpl>(
        deps_.video_task_runner, deps_.video_factories, deps_.tick_clock,
        deps_.compositor_thresholds);
  }
  if (has_audio_) {
    audio_params_ = MakeAudioParameters(audio_stream->audio_decoder_config(),
                                        deps_.audio_frames_per_buffer);
    audio_ = std::make_unique<AudioRendererImpl>(
        deps_.audio_task_runner, deps_.audio_factories, av_sync_.get());
  }

  // Initialise the sub-renderers on their own sequences. Both callbacks come
  // back to S1, where MaybeReportInitialized() decides when the pipeline may be
  // told that startup finished.
  if (video_) {
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](VideoRendererImpl* v, DemuxerStream* stream,
               std::unique_ptr<VideoRendererSink> sink,
               base::OnceCallback<void(PipelineStatus)> cb) {
              v->Initialize(stream, std::move(sink), std::move(cb));
            },
            base::Unretained(video_.get()), video_stream,
            std::move(deps_.video_sink),
            base::BindOnce(&RendererImpl::OnVideoInitialized,
                           base::Unretained(this), std::move(init_cb))));
  }
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](AudioRendererImpl* a, DemuxerStream* stream,
               const AudioParameters& p,
               base::scoped_refptr<AudioRendererSink> sink,
               base::OnceCallback<void(PipelineStatus)> cb) {
              a->Initialize(stream, p, std::move(sink), std::move(cb));
            },
            base::Unretained(audio_.get()), audio_stream, audio_params_,
            deps_.audio_sink,
            base::BindOnce(&RendererImpl::OnAudioInitialized,
                           base::Unretained(this))));
  }
  if (!video_ && !audio_) {
    MaybeReportInitialized();
  }
}

void RendererImpl::OnVideoInitialized(PipelineStatusCallback init_cb,
                                     PipelineStatus status) {
  video_initialized_ = true;
  if (status != PipelineStatus::kOk) {
    std::move(init_cb).Run(status);
    return;
  }
  MaybeReportInitializedWith(std::move(init_cb));
}

void RendererImpl::OnAudioInitialized(PipelineStatus status) {
  audio_initialized_ = true;
  if (status != PipelineStatus::kOk) {
    ReportError(PipelineStatusToMediaError(status));
    return;
  }
  MaybeReportInitializedWith(PipelineStatusCallback());
}

void RendererImpl::MaybeReportInitialized() {
  MaybeReportInitializedWith(PipelineStatusCallback());
}

void RendererImpl::MaybeReportInitializedWith(PipelineStatusCallback cb) {
  // Whichever sub-renderer finishes last reports, and only once. A pipeline
  // that is told "ready" twice starts playback twice; one that is never told
  // hangs forever, so the guard is on both sides.
  const bool video_done = !has_video_ || video_initialized_;
  const bool audio_done = !has_audio_ || audio_initialized_;
  if (!video_done || !audio_done || initialized_) {
    return;
  }
  initialized_ = true;
  LOG(INFO) << "ijkpp.pipeline: renderer ready (video=" << has_video_
            << " audio=" << has_audio_ << ")";
  if (cb) {
    std::move(cb).Run(PipelineStatus::kOk);
  }
}

void RendererImpl::StartPlayingFrom(base::TimeDelta time) {
  DCHECK(initialized_);
  if (!initialized_) {
    return;
  }
  start_time_ = time;
  ended_ = false;
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
                       base::Unretained(this)),
        kClockPushInterval);
  }
  if (!stats_scheduled_) {
    stats_scheduled_ = true;
    deps_.media_task_runner->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&RendererImpl::PushStatistics, base::Unretained(this)),
        kStatsInterval);
  }
}

void RendererImpl::PushMasterClock() {
  if (!initialized_ || ended_) {
    clock_push_scheduled_ = false;
    return;
  }
  const AvSyncController::Snapshot snapshot = av_sync_->GetSnapshot();
  if (video_ && snapshot.master_valid) {
    // Posted, not called: the compositor's write side must stay on S3 (see the
    // threading note in video_renderer_impl.h for why S3 and not S1).
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&VideoRendererImpl::SetMasterClock,
                       base::Unretained(video_.get()), snapshot.master,
                       av_sync_->master_serial(), true));
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&VideoRendererImpl::SetMasterIsVideo,
                       base::Unretained(video_.get()),
                       snapshot.resolved ==
                           AvSyncController::MasterType::kVideo));
  }
  deps_.media_task_runner->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RendererImpl::PushMasterClock, base::Unretained(this)),
      kClockPushInterval);
}

void RendererImpl::PushStatistics() {
  if (!initialized_ || ended_) {
    stats_scheduled_ = false;
    return;
  }
  if (client_) {
    client_->OnStatisticsUpdate(GetStatistics());
  }
  deps_.media_task_runner->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RendererImpl::PushStatistics, base::Unretained(this)),
      kStatsInterval);
}

PipelineStatistics RendererImpl::GetStatistics() const {
  PipelineStatistics stats;
  if (av_sync_) {
    const AvSyncController::Snapshot s = av_sync_->GetSnapshot();
    stats.avg_av_diff_ms = s.av_diff.InMillisecondsF();
    if (audio_ && audio_params_.sample_rate() > 0) {
      stats.buffered_time = base::SecondsD(
          static_cast<double>(audio_->buffered_frames()) /
          audio_params_.sample_rate());
    }
  }
  if (video_) {
    const VideoFrameCompositor::Stats c = video_->compositor_stats();
    stats.video_frames_presented = c.frames_presented;
    stats.video_frames_dropped = c.frames_dropped;
    stats.video_frames_repeated = c.frames_repeated;
    if (c.avg_av_diff_ms != 0.0) {
      stats.avg_av_diff_ms = c.avg_av_diff_ms;
    }
  }
  if (audio_) {
    stats.audio_underruns = audio_->underruns();
  }
  return stats;
}

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
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&AudioRendererImpl::Flush,
                       base::Unretained(audio_.get()), base::OnceClosure()));
  }
  if (video_) {
    const int32_t serial = av_sync_->master_serial();
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(
                       [](VideoRendererImpl* v, int32_t s,
                          base::OnceClosure done) {
                         v->Flush(s, std::move(done));
                       },
                       base::Unretained(video_.get()), serial,
                       std::move(flush_cb)));
  } else {
    std::move(flush_cb).Run();
  }
}

void RendererImpl::SetCdm(CdmContext* /*cdm_context*/,
                          base::OnceCallback<void(bool)> cdm_attached_cb) {
  // D8: DRM is not implemented. Run the callback with false rather than drop
  // it -- a caller waiting on this would otherwise hang, which is the failure
  // class Δ15 exists to prevent.
  if (cdm_attached_cb) {
    std::move(cdm_attached_cb).Run(false);
  }
}

void RendererImpl::SetLatencyHint(std::optional<base::TimeDelta> latency_hint) {
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AudioRendererImpl::SetLatencyHint,
                                  base::Unretained(audio_.get()),
                                  latency_hint));
  }
  // Gap 6: the video side has no latency input, so a live stream can still fall
  // arbitrarily far behind on video while audio chases the hint.
}

void RendererImpl::SetPreservesPitch(bool preserves_pitch) {
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AudioRendererImpl::SetPreservesPitch,
                                  base::Unretained(audio_.get()),
                                  preserves_pitch));
  }
}

void RendererImpl::SetRenderMutedAudio(bool render_muted_audio) {
  // When false, muting should stop the audio device instead of feeding it
  // silence, so the clock does not advance on samples nobody hears. There is no
  // device to stop until media/audio/ exists, so the flag is stored and NOT
  // acted on -- the first draft mapped it onto SetMuted(), which is simply
  // wrong: SetMuted() is the user's mute control and this is a power/sync
  // policy. Storing it means closing the gap later is one line, and means the
  // caller's request is not silently reinterpreted as something else.
  render_muted_audio_ = render_muted_audio;
}

void RendererImpl::SetPlaybackRate(double playback_rate) {
  playback_rate_ = playback_rate;
  if (av_sync_) {
    av_sync_->SetPlaybackRate(playback_rate);
  }
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AudioRendererImpl::SetPlaybackRate,
                                  base::Unretained(audio_.get()),
                                  playback_rate));
  }
  if (video_) {
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::SetPlaybackRate,
                                  base::Unretained(video_.get()),
                                  playback_rate));
  }
}

void RendererImpl::SetVolume(float volume) {
  volume_ = volume;
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AudioRendererImpl::SetVolume,
                                  base::Unretained(audio_.get()), volume));
  }
}

base::TimeDelta RendererImpl::GetMediaTime() {
  // Thread-safe by construction: this is the only Renderer method called from
  // outside S1, and it goes through the seqlock rather than touching any member
  // that S1 mutates. av_sync_ itself is created in Initialize() and never
  // reassigned, so reading the raw pointer from another thread is safe once
  // initialization has completed -- which Pipeline guarantees by ordering.
  return av_sync_ ? av_sync_->GetMasterClock() : kNoTimestamp;
}

void RendererImpl::OnTracksChanged(DemuxerStreamType track_type,
                                   DemuxerStream* enabled_track,
                                   base::OnceClosure change_completed_cb) {
  if (track_type == DemuxerStreamType::kText) {
    // Gap 3. Succeeding here would make Player::SelectTrack(kText) report
    // success while nothing happens, which is worse than failing: a UI would
    // show a subtitle toggle that does nothing. Fail loudly until TextRenderer
    // exists.
    LOG(WARNING) << "ijkpp.text: text tracks are not implemented yet";
    ReportError(MediaError(
        ErrorCode::kNotImplemented, "text track selection is not supported",
        "this build has no TextRenderer (milestone M7)",
        "select an audio or video track, or build against a release that "
        "includes text rendering"));
    std::move(change_completed_cb).Run();
    return;
  }
  (void)enabled_track;
  // Audio and video track switching means re-initialising the corresponding
  // sub-renderer with a different DemuxerStream. That is real work (stop the
  // sink, tear down the decoder, rebuild) and is not done here yet.
  ReportError(MediaError(ErrorCode::kNotImplemented,
                         "track switching is not wired up yet",
                         "RendererImpl::OnTracksChanged is a stub for "
                         "audio/video as well as text",
                         "restart playback with PlayerConfig's selected_stream "
                         "set instead (video.selected_stream / "
                         "audio.selected_stream)"));
  std::move(change_completed_cb).Run();
}

RendererType RendererImpl::GetRendererType() {
  return RendererType::kRendererImpl;
}

void RendererImpl::ReportError(MediaError error) {
  if (client_) {
    client_->OnError(std::move(error));
  }
}

void RendererImpl::OnEnded() {
  if (ended_ || !client_) {
    return;
  }
  ended_ = true;
  client_->OnEnded();
}

}  // namespace ijkpp::media
