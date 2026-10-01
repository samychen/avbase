// Copyright 2026 The ijkpp Authors. All rights reserved.
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
#include "base/logging.h"
#include "media/base/media_constants.h"

namespace ijkpp::media {

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
                                  base::Unretained(this)));
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
                                  base::Unretained(this)));
  }
  // The recovery edge must not depend on the 10 ms sampler either: a frame
  // just left for the display, which is exactly when starvation may have
  // ended. 30 hops/s is nothing; a missed kHaveEnough leaves the facade's
  // HWM stuck mid-cycle.
  deps_.media_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&RendererImpl::CheckBufferingTransitions,
                     base::Unretained(this)));
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

}  // namespace ijkpp::media
