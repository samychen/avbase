// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's post-initialise control surface: the Set* family, the
// thread-safe GetMediaTime(), the accurate-seek window and the renderer type.
// The wired audio/video track switches moved to renderer_impl_switch.cc (the
// C1 line-count split the old allowlist entry named as the pending fix);
// renderer_impl.cc keeps construction, initialization, the clock/statistics
// pushes and the end-of-stream contract. The split is a line-count one (C1),
// and the seam is "called after startup" versus "called to start". The text
// leg -- the live-edge policy and the cue pump -- moved to
// renderer_impl_text.cc.

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

RendererType RendererImpl::GetRendererType() {
  return RendererType::kRendererImpl;
}

}  // namespace avbase::media
