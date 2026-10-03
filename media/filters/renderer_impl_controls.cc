// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's post-initialise control surface: the Set* family, the
// thread-safe GetMediaTime(), and the two interface methods that are still
// stubs (track switching and the renderer type). renderer_impl.cc keeps
// construction, initialization, the clock/statistics pushes and the
// end-of-stream contract; the split is a line-count one (C1), and the seam is
// "called after startup" versus "called to start".

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
  if (video_) {
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::SetPaused,
                                  base::Unretained(video_.get()), paused));
  }
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AudioRendererImpl::SetPaused,
                                  base::Unretained(audio_.get()), paused));
  }
}

void RendererImpl::SetOutputTarget(
    base::scoped_refptr<NativeDisplay> display) {
  if (video_) {
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::SetOutputTarget,
                                  base::Unretained(video_.get()),
                                  std::move(display)));
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
  if (video_) {
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::BeginAccurateSeek,
                                  base::Unretained(video_.get()), target));
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
  if (video_) {
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&VideoRendererImpl::EndAccurateSeek,
                       base::Unretained(video_.get())));
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
      FROM_HERE,
      base::BindOnce(&RendererImpl::CheckBufferingTransitions,
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
    // Phase 4.2 text leg: the base renders nothing, it decodes cue packets on
    // S1 and hands TimedTextCues to the client (kTimedText events). Disabling
    // (|enabled_track| == nullptr) tears the leg down.
    if (!deps_.text_decoder_factory) {
      ReportError(MediaError(
          ErrorCode::kNotImplemented, "text track selection is not available",
          "this build has no text decoder factory (the FFmpeg layer provides "
          "one; a no-ffmpeg build has none)",
          "build with AVBASE_ENABLE_FFMPEG for subtitle support"));
      std::move(change_completed_cb).Run();
      return;
    }
    ++text_generation_;
    text_read_outstanding_ = false;
    text_ended_ = false;
    text_stream_ = enabled_track;
    text_decoder_.reset();
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
      text_decoder_ =
          deps_.text_decoder_factory->CreateTextDecoder(config);
      if (!text_decoder_) {
        ReportError(MediaError(
            ErrorCode::kNotImplemented,
            "cannot decode that subtitle codec (\"" + config.codec_name +
                "\")",
            "the text decoder factory declined the config; see the log for "
            "the FFmpeg-side reason",
            "re-encode the subtitle track as srt/ass/mov_text, or use an "
            "FFmpeg build with that decoder enabled"));
        std::move(change_completed_cb).Run();
        return;
      }
      PumpText();
    } else {
      text_decoder_.reset();
    }
    std::move(change_completed_cb).Run();
    return;
  }
  if (track_type == DemuxerStreamType::kVideo) {
    // Video switching means re-initialising the video renderer AND retargeting
    // the sink's expectations (size/format change mid-playback); audio first,
    // video lands with the track-switch follow-up.
    ReportError(MediaError(
        ErrorCode::kNotImplemented, "video track selection is not supported",
        "switching the video stream needs a video renderer re-initialisation "
        "with live sink retarget, which is not wired yet",
        "restart playback with PlayerConfig's video.selected_stream set"));
    std::move(change_completed_cb).Run();
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
    std::move(change_completed_cb).Run();   // Same track: nothing to do.
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
      FROM_HERE,
      base::BindOnce(&AudioRendererImpl::StopAndDrainForTeardown,
                     base::Unretained(old), base::DoNothing()));
  audio_stream_ = new_stream;
  audio_params_ = MakeAudioParameters(new_stream->audio_decoder_config(),
                                      deps_.audio_frames_per_buffer);
  audio_ = std::make_unique<AudioRendererImpl>(
      deps_.audio_task_runner, deps_.audio_factories, av_sync_.get());
  audio_->set_ended_cb(base::BindRepeating(&RendererImpl::PostAudioEnded,
                                           base::Unretained(this)));
  audio_initialized_ = false;
  deps_.audio_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&AudioRendererImpl::Initialize,
                     base::Unretained(audio_.get()), new_stream, audio_params_,
                     deps_.audio_sink,
                     base::BindOnce(&RendererImpl::OnSwitchedAudioInitialized,
                                    base::Unretained(this), was_rendering,
                                    resume_at, settings,
                                    std::move(change_completed_cb))));
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

void RendererImpl::PumpText() {
  if (!text_stream_ || !text_decoder_ || text_read_outstanding_ ||
      text_ended_) {
    return;
  }
  text_read_outstanding_ = true;
  const int generation = text_generation_;
  text_stream_->Read(1, base::BindOnce(&RendererImpl::OnTextRead,
                                       weak_factory_.GetWeakPtr(), generation));
}

void RendererImpl::OnTextRead(int generation, DemuxerStream::Status status,
                              DemuxerStream::DecoderBufferVector buffers) {
  text_read_outstanding_ = false;
  if (generation != text_generation_) {
    return;   // Reply from a leg that was switched away or flushed.
  }
  if (status == DemuxerStream::Status::kAborted) {
    return;   // A flush invalidates the leg; the next PumpText re-arms.
  }
  if (status != DemuxerStream::Status::kOk) {
    LOG(ERROR) << "avbase.text: read failed ("
             << DemuxerStream::GetStatusName(status) << ")";
    return;
  }
  for (auto& buffer : buffers) {
    if (buffer->IsEndOfStream()) {
      text_ended_ = true;
      return;
    }
    if (text_stream_ && buffer->serial() < text_stream_->serial()) {
      continue;   // Pre-seek generation; docs/04 §4 R1.
    }
    std::vector<TimedTextCue> cues;
    if (const Status decode = text_decoder_->Decode(*buffer, &cues);
        !decode) {
      LOG(ERROR) << "avbase.text: decode failed: " << decode.error().ToString();
      continue;
    }
    if (client_) {
      for (TimedTextCue& cue : cues) {
        client_->OnTimedText(cue);
      }
    }
  }
  PumpText();
}

RendererType RendererImpl::GetRendererType() {
  return RendererType::kRendererImpl;
}

}  // namespace avbase::media
