// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's runtime track-switch subsystem, carved out of
// renderer_impl_controls.cc as the C1 line-count split its allowlist entry
// named as the pending fix. These are method definitions of the same
// RendererImpl class, moved to their own translation unit -- not a new type.
// OnTracksChanged (the switch entry point, with the per-source foreign-stream
// guards), the audio/video Switch* hand-over pair, the sink hand-back, the
// switched-initialised callbacks and FinishAudioSwitch all live here.

#include "media/filters/renderer_impl.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "media/base/media_constants.h"

namespace avbase::media {

void RendererImpl::OnTracksChanged(DemuxerStreamType track_type,
                                   DemuxerStream* enabled_track,
                                   base::OnceClosure change_completed_cb,
                                   int source_id) {
  if (track_type == DemuxerStreamType::kText) {
    // Per-source text leg (docs/14 stage 4): decode cues on S1, deliver them
    // tagged with |source_id|; a null |enabled_track| tears the leg down.
    TextLeg* leg = text_leg(source_id);
    if (!leg) {
      ReportError(MediaError(
          ErrorCode::kInvalidArgument, "text track names an unknown source",
          "source_id " + std::to_string(source_id) + " but " +
              std::to_string(text_legs_.size()) + " source(s) exist",
          "select a track whose media_info index maps to a registered source"));
      std::move(change_completed_cb).Run();
      return;
    }
    if (!deps_.text_decoder_factory) {
      ReportError(MediaError(
          ErrorCode::kNotImplemented, "text track selection is not available",
          "this build has no text decoder factory (the FFmpeg layer provides "
          "one; a no-ffmpeg build has none)",
          "build with AVBASE_ENABLE_FFMPEG for subtitle support"));
      std::move(change_completed_cb).Run();
      return;
    }
    ++leg->generation;
    leg->read_outstanding = leg->ended = false;
    leg->stream = enabled_track;
    leg->decoder.reset();
    if (enabled_track) {
      const TextDecoderConfig& config = enabled_track->text_decoder_config();
      if (!config.IsValidConfig()) {
        ReportError(MediaError(
            ErrorCode::kInvalidArgument,
            "that stream carries no subtitle codec description",
            "text_decoder_config() has an empty codec_name",
            "pick an index from media_info().streams whose kind is kText"));
        std::move(change_completed_cb).Run();
        return;
      }
      leg->decoder = deps_.text_decoder_factory->CreateTextDecoder(config);
      if (!leg->decoder) {
        ReportError(MediaError(
            ErrorCode::kNotImplemented,
            "cannot decode that subtitle codec (\"" + config.codec_name + "\")",
            "the text decoder factory declined the config; see the log for "
            "the FFmpeg-side reason",
            "re-encode the subtitle track as srt/ass/mov_text, or use an "
            "FFmpeg build with that decoder enabled"));
        std::move(change_completed_cb).Run();
        return;
      }
      PumpText(source_id);
    }
    std::move(change_completed_cb).Run();
    return;
  }
  if (track_type == DemuxerStreamType::kVideo) {
    // ---- video: the wired path (docs/12 4.1) ---------------------------
    if (!initialized_ || ended_) {
      ReportError(MediaError(
          ErrorCode::kInvalidState, "cannot switch the video track now",
          "the switch hands over the video renderer, which needs an "
          "initialised, still-playing pipeline",
          "switch while prepared, playing or paused (Reset() first after an "
          "error or completion)"));
      std::move(change_completed_cb).Run();
      return;
    }
    if (!video_renderer() || !enabled_track) {
      ReportError(MediaError(
          ErrorCode::kNotImplemented,
          "enabling or disabling the video track at runtime is not supported",
          "runtime switching covers swapping between existing video streams; "
          "the enable/disable path owns the surface lifecycle",
          "restart playback with the video track enabled (or "
          "config.video.disabled false)"));
      std::move(change_completed_cb).Run();
      return;
    }
    if (enabled_track->type() != DemuxerStreamType::kVideo) {
      ReportError(MediaError(
          ErrorCode::kInvalidArgument, "that stream is not a video stream",
          "SelectTrack(kVideo) was given a stream of a different type",
          "pick an index from media_info().streams whose kind is kVideo"));
      std::move(change_completed_cb).Run();
      return;
    }
    if (enabled_track == video_stream_) {
      std::move(change_completed_cb).Run();  // Same track: nothing to do.
      return;
    }
    // FOREIGN-STREAM GUARD (docs/14 stage 1). The handover machinery below
    // switches source 0's alternates: it re-anchors against video_stream_,
    // hands over source 0's sink and clock hook. A stream belonging to
    // another source is none of those things, and reaching it would silently
    // build a replacement renderer for the wrong leg. Stage 4 made such a
    // stream RESOLVABLE by index, so the pipeline refuses it before routing
    // (pipeline_track_select.cc); this guard is the renderer's own boundary,
    // kept honest by a direct unit test, not a speculation.
    if (!IsPrimaryStream(DemuxerStreamType::kVideo, enabled_track)) {
      ReportError(MediaError(
          ErrorCode::kNotImplemented,
          "switching to another source's video track is not supported",
          "the video track switch hands over source 0's renderer, sink and "
          "clock hook; that stream belongs to a different source",
          "select one of the primary source's video tracks (its alternates "
          "are what runtime switching covers)"));
      std::move(change_completed_cb).Run();
      return;
    }
    SwitchVideoRenderer(enabled_track, std::move(change_completed_cb));
    return;
  }

  // ---- audio: the wired path ------------------------------------------------
  if (!initialized_ || ended_) {
    ReportError(MediaError(
        ErrorCode::kInvalidState, "cannot switch the audio track now",
        "the switch hands over the audio renderer, which needs an "
        "initialised, still-playing pipeline",
        "switch while prepared, playing or paused (Reset() first after an "
        "error or completion)"));
    std::move(change_completed_cb).Run();
    return;
  }
  if (!audio_ || !enabled_track) {
    ReportError(MediaError(
        ErrorCode::kNotImplemented,
        "enabling or disabling the audio track at runtime is not supported",
        "runtime switching only covers swapping between existing audio "
        "streams; the enable/disable path needs the sink lifecycle, which "
        "media/audio/ (M7 follow-up) owns",
        "restart playback with the audio track enabled (or "
        "config.audio.disabled false)"));
    std::move(change_completed_cb).Run();
    return;
  }
  if (enabled_track->type() != DemuxerStreamType::kAudio) {
    ReportError(MediaError(
        ErrorCode::kInvalidArgument, "that stream is not an audio stream",
        "SelectTrack(kAudio) was given a stream of a different type",
        "pick an index from media_info().streams whose kind is kAudio"));
    std::move(change_completed_cb).Run();
    return;
  }
  if (enabled_track == audio_stream_) {
    std::move(change_completed_cb).Run();  // Same track: nothing to do.
    return;
  }
  // FOREIGN-STREAM GUARD (docs/14 §4.2): the audio leg is source 0 only, so a
  // stream from another source has no renderer to switch INTO. Same shape and
  // status as the video guard above: the pipeline pre-checks it.
  if (!IsPrimaryStream(DemuxerStreamType::kAudio, enabled_track)) {
    ReportError(MediaError(
        ErrorCode::kNotImplemented,
        "switching to another source's audio track is not supported",
        "the audio leg belongs to source 0; that stream is a different "
        "source's, and secondary audio is seen but not selected",
        "select one of the primary source's audio tracks"));
    std::move(change_completed_cb).Run();
    return;
  }
  SwitchAudioRenderer(enabled_track, std::move(change_completed_cb));
}

void RendererImpl::SwitchAudioRenderer(DemuxerStream* new_stream,
                                       base::OnceClosure change_completed_cb) {
  const bool was_rendering = rendering_;
  base::TimeDelta resume_at = GetMediaTime();
  if (resume_at == kNoTimestamp || resume_at < base::TimeDelta()) {
    resume_at = start_time_;
  }
  // Capture the user-facing settings on the sequence that mutates them (the
  // atomics would also make an S1 read data-race-free, but the values must be
  // taken BEFORE the outgoing renderer dies, and the teardown below runs on
  // S4 -- capture there).
  // The settings are atomics on AudioRendererImpl, so reading them here (S1)
  // is data-race-free; they must be captured before the object is handed to
  // its teardown task.
  const SwitchedAudioSettings settings{audio_->volume(), audio_->muted(),
                                       audio_->playback_rate(),
                                       audio_->preserves_pitch()};
  // NO blocking here: S4's pending decoder replies are delivered through the
  // media runner, so waiting on S4 from S1 deadlocks (the first draft did
  // exactly that). Instead: release the old renderer from the member, queue
  // its quiescent teardown on S4 (StopAndDrainForTeardown deletes it there
  // once no task naming it can be created), and build the replacement
  // immediately. Playback gaps for the duration of the handover are the
  // accepted cost of a mid-flight switch; the clock falls back to the
  // external source for the gap and re-anchors at StartPlayingFrom below.
  retired_audio_.emplace_back(audio_.release());
  AudioRendererImpl* old = retired_audio_.back().get();
  deps_.audio_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&AudioRendererImpl::StopAndDrainForTeardown,
                                base::Unretained(old), base::DoNothing()));
  audio_stream_ = new_stream;
  audio_params_ = MakeAudioParameters(new_stream->audio_decoder_config(),
                                      deps_.audio_frames_per_buffer);
  audio_ = std::make_unique<AudioRendererImpl>(
      deps_.audio_task_runner, deps_.audio_factories, av_sync_.get());
  WireAudioCallbacks();
  audio_initialized_ = false;
  deps_.audio_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          &AudioRendererImpl::Initialize, base::Unretained(audio_.get()),
          new_stream, audio_params_, deps_.audio_sink,
          base::BindOnce(&RendererImpl::OnSwitchedAudioInitialized,
                         base::Unretained(this), was_rendering, resume_at,
                         settings, std::move(change_completed_cb))));
}

void RendererImpl::SwitchVideoRenderer(DemuxerStream* new_stream,
                                       base::OnceClosure change_completed_cb) {
  const bool was_rendering = rendering_;
  base::TimeDelta resume_at = GetMediaTime();
  if (resume_at == kNoTimestamp || resume_at < base::TimeDelta()) {
    resume_at = start_time_;
  }

  // THE SINK HAND-OVER, and it is the step the audio path does not have.
  //
  // Order is the whole safety argument. StopAndDrainForTeardown() runs FIRST:
  // it stops the sink (so no further Render() can arrive) and completes the
  // in-flight decode read inline (so no task names the old renderer). Only
  // once that has finished is it safe to take the sink back -- taking it
  // first would hand the replacement a sink whose RenderCallback still points
  // into the renderer being destroyed.
  VideoRendererImpl* old = video_renderer();
  deps_.video_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](VideoRendererImpl* retiring, RendererImpl* self) {
            retiring->StopAndDrainForTeardown(base::BindOnce(
                [](RendererImpl* owner, VideoRendererImpl* dead) {
                  // On S3, so taking the sink here is safe.
                  std::unique_ptr<VideoRendererSink> sink =
                      dead->TakeSinkForHandover();
                  // Back to S1, which is where the replacement is built.
                  owner->deps_.media_task_runner->PostTask(
                      FROM_HERE,
                      base::BindOnce(
                          [](RendererImpl* owner_s1,
                             std::unique_ptr<VideoRendererSink> returned) {
                            owner_s1->video_sink_in_handover_ =
                                std::move(returned);
                            owner_s1->FinishVideoHandoverSinkReady();
                          },
                          base::Unretained(owner), std::move(sink)));
                },
                base::Unretained(self), base::Unretained(retiring)));
          },
          base::Unretained(old), base::Unretained(this)));

  // Park the retiring renderer immediately: it is inert once its teardown task
  // starts, and parking it here frees the member without waiting for S3.
  // Source 0's slot: the handover machinery is source 0's (docs/14 stage 1),
  // and the foreign-stream guard above is what keeps it that way.
  retired_video_.emplace_back(video_renderers_[0].release());
  video_renderers_[0] = std::make_unique<VideoRendererImpl>(
      deps_.video_task_runner, deps_.video_factories, deps_.tick_clock,
      deps_.compositor_thresholds);
  // New object: everything inside it is re-installed (ApplyVideoConfig).
  ApplyVideoConfig(video_renderer());
  WireVideoCallbacks(0);
  video_initialized_[0] = false;
  pending_video_switch_.stream = new_stream;
  pending_video_switch_.was_rendering = was_rendering;
  pending_video_switch_.resume_at = resume_at;
  pending_video_switch_.change_completed_cb = std::move(change_completed_cb);
}

void RendererImpl::FinishVideoHandoverSinkReady() {
  DemuxerStream* stream = pending_video_switch_.stream;
  if (!stream) {
    return;  // Nothing pending: the caller's callback already ran.
  }
  if (!video_sink_in_handover_) {
    // The retiring renderer had no sink to give back, so there is nothing for
    // the replacement to render into. Reported rather than left as a silent
    // video-less pipeline.
    ReportError(MediaError(
        ErrorCode::kInvalidState, "the video track switch could not start",
        "the outgoing renderer did not return its sink, so the replacement "
        "has nothing to render into",
        "this needs a video sink that survives Stop() and can be "
        "re-Initialized; check the sink implementation"));
    if (pending_video_switch_.change_completed_cb) {
      std::move(pending_video_switch_.change_completed_cb).Run();
    }
    pending_video_switch_ = {};
    return;
  }
  std::unique_ptr<VideoRendererSink> sink = std::move(video_sink_in_handover_);
  const bool was_rendering = pending_video_switch_.was_rendering;
  const base::TimeDelta resume_at = pending_video_switch_.resume_at;
  base::OnceClosure done = std::move(pending_video_switch_.change_completed_cb);
  pending_video_switch_ = {};
  video_stream_ = stream;

  deps_.video_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](VideoRendererImpl* v, DemuxerStream* s,
             std::unique_ptr<VideoRendererSink> sink, RendererImpl* self,
             bool was_rendering, base::TimeDelta resume_at,
             base::OnceClosure done) {
            v->Initialize(
                s, std::move(sink),
                base::BindOnce(&RendererImpl::OnSwitchedVideoInitialized,
                               base::Unretained(self), was_rendering, resume_at,
                               std::move(done)));
          },
          base::Unretained(video_renderer()), stream, std::move(sink),
          base::Unretained(this), was_rendering, resume_at, std::move(done)));
}

void RendererImpl::OnSwitchedVideoInitialized(
    bool was_rendering, base::TimeDelta resume_at,
    base::OnceClosure change_completed_cb, PipelineStatus status) {
  // Runs on S3 (VideoRendererImpl::Initialize fires it inline); the decision
  // belongs to S1.
  deps_.media_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](RendererImpl* self, bool was_rendering, base::TimeDelta resume_at,
             base::OnceClosure cb, PipelineStatus s) {
            if (s != PipelineStatus::kOk) {
              // The outgoing renderer is already gone. Report and continue
              // audio-only rather than tearing playback down over a track
              // switch -- the same call the audio handover makes.
              self->video_initialized_[0] = false;
              self->ReportError(PipelineStatusToMediaError(s));
              if (cb) {
                std::move(cb).Run();
              }
              return;
            }
            self->video_initialized_[0] = true;
            if (was_rendering) {
              // Re-anchor: the replacement's compositor is empty by
              // construction, so playback resumes at |resume_at| rather than
              // wherever the old one happened to be. Without this the first
              // frame after a switch is judged against a stale clock and
              // dropped as late.
              //
              // POSTED, not called here. VideoRendererImpl runs on S3 and its
              // sequence checker says so; the audio handover avoids this
              // because AudioRendererImpl::StartPlayingFrom is reached
              // through its own Initialize. Calling it from S1 tripped
              // CalledOnValidSequence() the first time this ran.
              VideoRendererImpl* replacement = self->video_renderer();
              self->deps_.video_task_runner->PostTask(
                  FROM_HERE, base::BindOnce(
                                 [](VideoRendererImpl* v, base::TimeDelta at) {
                                   v->StartPlayingFrom(at);
                                 },
                                 base::Unretained(replacement), resume_at));
            }
            if (self->client_) {
              self->client_->OnVideoConfigChange(
                  self->video_renderer()->video_decoder_config());
            }
            if (cb) {
              std::move(cb).Run();
            }
          },
          base::Unretained(this), was_rendering, resume_at,
          std::move(change_completed_cb), status));
}

void RendererImpl::OnSwitchedAudioInitialized(
    bool was_rendering, base::TimeDelta resume_at,
    SwitchedAudioSettings settings, base::OnceClosure change_completed_cb,
    PipelineStatus status) {
  // The InitializeCB fires on S4 (AudioRendererImpl::Initialize runs it
  // inline); the decision and the completion belong to S1.
  deps_.media_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](RendererImpl* self, bool was_rendering, base::TimeDelta resume_at,
             SwitchedAudioSettings settings, base::OnceClosure cb,
             PipelineStatus s) {
            self->FinishAudioSwitch(was_rendering, resume_at, settings,
                                    std::move(cb), s);
          },
          base::Unretained(this), was_rendering, resume_at, settings,
          std::move(change_completed_cb), status));
}

void RendererImpl::FinishAudioSwitch(bool was_rendering,
                                     base::TimeDelta resume_at,
                                     SwitchedAudioSettings settings,
                                     base::OnceClosure change_completed_cb,
                                     PipelineStatus status) {
  if (status != PipelineStatus::kOk) {
    // The switch failed with the old renderer already gone: continue as
    // video-only and let the clock fall back, rather than tearing playback
    // down over a track switch. The error carries the actionable detail.
    audio_initialized_ = false;
    ReportError(PipelineStatusToMediaError(status));
    std::move(change_completed_cb).Run();
    return;
  }
  audio_initialized_ = true;
  // Re-apply what the user had set on the outgoing renderer (these post to
  // S4 themselves).
  audio_->SetVolume(settings.volume);
  audio_->SetMuted(settings.muted);
  audio_->SetPlaybackRate(settings.playback_rate);
  audio_->SetPreservesPitch(settings.preserves_pitch);
  if (was_rendering) {
    rendering_ = true;
    ended_ = false;
    audio_ended_ = false;
    deps_.audio_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AudioRendererImpl::StartPlayingFrom,
                                  base::Unretained(audio_.get()), resume_at));
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
  }
  std::move(change_completed_cb).Run();
}

// True when |stream| is one of source 0's streams of |type|, asked of the
// resource live (the no-axis GetStreams answers source 0). Lives here rather
// than with the text leg because its only callers are the two
// foreign-stream guards in OnTracksChanged above.
bool RendererImpl::IsPrimaryStream(DemuxerStreamType type,
                                   DemuxerStream* stream) const {
  if (!media_resource_ || !stream) {
    return false;
  }
  // The no-axis overload answers source 0 -- which is exactly the question.
  // Asked LIVE, not from an Initialize-time snapshot: a resource's stream
  // list can grow afterwards (renderer_option_wiring_unittest.cc adds a
  // second video stream between Initialize and the switch, and a snapshot
  // guard rejected it -- TheCapSurvivesAVideoTrackSwitch sat out its 5 s
  // ceiling until this became a live query).
  for (DemuxerStream* s : media_resource_->GetStreams(type)) {
    if (s == stream) {
      return true;
    }
  }
  return false;
}

}  // namespace avbase::media
