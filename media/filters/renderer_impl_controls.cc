// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's post-initialise control surface: the Set* family, the
// thread-safe GetMediaTime(), the accurate-seek window, the wired audio and
// video track switches (with the multi-source foreign-stream guards,
// docs/14 stage 1) and the renderer type. renderer_impl.cc keeps
// construction, initialization, the clock/statistics pushes and the
// end-of-stream contract; the split is a line-count one (C1), and the seam is
// "called after startup" versus "called to start". The text leg -- the
// live-edge policy and the cue pump -- moved to renderer_impl_text.cc, the
// seam this file's allowlist entry used to name as the pending fix.

#include "media/filters/renderer_impl.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "media/base/media_constants.h"

namespace avbase::media {

void RendererImpl::SetPaused(bool paused) {
  if (paused_ == paused) {
    return;
  }
  paused_ = paused;
  if (av_sync_) {
    // Zeroing the clock speed is what freezes time: the audio path stops
    // consuming (its own pause gate), so nothing re-anchors the clocks, and
    // with speed 0 the seqlock extrapolation holds the last anchor exactly
    // instead of sliding forward through the pause.
    av_sync_->SetPlaybackRate(paused ? 0.0 : playback_rate_);
  }
  // Every leg pauses (docs/14 stage 1): a paused pipeline with one leg still
  // compositing would present frames against a frozen clock.
  for (auto& v : video_renderers_) {
    if (v) {
      deps_.video_task_runner->PostTask(
          FROM_HERE, base::BindOnce(&VideoRendererImpl::SetPaused,
                                    base::Unretained(v.get()), paused));
    }
  }
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AudioRendererImpl::SetPaused,
                                  base::Unretained(audio_.get()), paused));
  }
}

void RendererImpl::SetOutputTarget(base::scoped_refptr<NativeDisplay> display) {
  // Every leg gets the display: the secondary sinks were created with a null
  // one (CreateSubRenderers), so this is where they receive theirs. Copied
  // per leg, not moved -- moving inside the loop would hand leg 2..N a null
  // display.
  for (auto& v : video_renderers_) {
    if (v) {
      deps_.video_task_runner->PostTask(
          FROM_HERE, base::BindOnce(&VideoRendererImpl::SetOutputTarget,
                                    base::Unretained(v.get()), display));
    }
  }
}

void RendererImpl::BeginAccurateSeek(base::TimeDelta target,
                                     base::OnceClosure reached_cb) {
  // S1 in. The window is opened on S3 and observed by the presented-frame
  // hook; opening before the keyframe seek is the point -- the window
  // survives Flush(), so the new generation is already framed when decoding
  // restarts.
  accurate_reached_cb_ = std::move(reached_cb);
  accurate_target_micros_.store(target.InMicroseconds());
  // Source 0 only (docs/14 stage 1): the presented-frame hook that observes
  // the window is wired on source 0's leg alone, so opening it anywhere else
  // would arm a check nobody performs.
  if (video_renderer()) {
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::BeginAccurateSeek,
                                  base::Unretained(video_renderer()), target));
  } else if (accurate_reached_cb_) {
    // No video stream: nothing will ever be presented, so the wait cannot
    // succeed -- report immediately rather than ride the caller's timeout.
    std::move(accurate_reached_cb_).Run();
  }
}

void RendererImpl::EndAccurateSeek() {
  accurate_target_micros_.store(0);
  // The callback, if still pending, belongs to a completion path the caller
  // has decided against (timeout, supersede); dropping it here would leave
  // the caller's wait hanging, so it runs -- the caller re-checks its own
  // state on its sequence, where the closed window makes "reached" moot.
  if (accurate_reached_cb_) {
    deps_.media_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&RendererImpl::OnAccurateSeekTargetReached,
                                  weak_factory_.GetWeakPtr()));
  }
  if (video_renderer()) {
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::EndAccurateSeek,
                                  base::Unretained(video_renderer())));
  }
}

void RendererImpl::OnVideoFramePresented(base::TimeDelta timestamp,
                                         int32_t serial) {
  // S6. The video clock gets every presented frame, exactly as before; the
  // accurate-seek check only observes the atomic target and defers every
  // decision to S1.
  if (av_sync_) {
    av_sync_->OnVideoFramePresented(timestamp, serial);
  }
  const int64_t target = accurate_target_micros_.load();
  if (target > 0 && timestamp >= base::TimeDelta::FromMicroseconds(target)) {
    deps_.media_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&RendererImpl::OnAccurateSeekTargetReached,
                                  weak_factory_.GetWeakPtr()));
  }
  // The recovery edge must not depend on the 10 ms sampler either: a frame
  // just left for the display, which is exactly when starvation may have
  // ended. 30 hops/s is nothing; a missed kHaveEnough leaves the facade's
  // HWM stuck mid-cycle.
  //
  // S6 posting into S1 is the same shape as the ended/init hops in
  // renderer_impl.cc, and gets the same weak binding: a frame handed to the
  // display just before teardown must not be able to name a destroyed
  // renderer.
  deps_.media_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RendererImpl::CheckBufferingTransitions,
                                weak_factory_.GetWeakPtr()));
}

void RendererImpl::OnAccurateSeekTargetReached() {
  // S1. Re-checked here because EndAccurateSeek() also routes through this
  // hop when a callback was still pending at close time.
  if (accurate_target_micros_.load() == 0 || !accurate_reached_cb_) {
    return;
  }
  accurate_target_micros_.store(0);
  std::move(accurate_reached_cb_).Run();
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
        FROM_HERE,
        base::BindOnce(&AudioRendererImpl::SetLatencyHint,
                       base::Unretained(audio_.get()), latency_hint));
  }
  // Gap 6: the video side has no latency input, so a live stream can still fall
  // arbitrarily far behind on video while audio chases the hint.
}

void RendererImpl::SetPreservesPitch(bool preserves_pitch) {
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&AudioRendererImpl::SetPreservesPitch,
                       base::Unretained(audio_.get()), preserves_pitch));
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
        FROM_HERE,
        base::BindOnce(&AudioRendererImpl::SetPlaybackRate,
                       base::Unretained(audio_.get()), playback_rate));
  }
  for (auto& v : video_renderers_) {
    if (v) {
      deps_.video_task_runner->PostTask(
          FROM_HERE, base::BindOnce(&VideoRendererImpl::SetPlaybackRate,
                                    base::Unretained(v.get()), playback_rate));
    }
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

RendererType RendererImpl::GetRendererType() {
  return RendererType::kRendererImpl;
}

}  // namespace avbase::media
