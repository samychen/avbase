// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The device-facing surface of AudioRendererImpl: the sink's Render() callback
// and the arithmetic it leans on (OutputFramesToMediaTime, ScaleAndZeroTail)
// plus the device-error latch. This is a genuine seam -- everything here runs
// on the audio thread under the sink's contract, and it is the only code that
// does, so pulling it into its own translation unit keeps the S4 callback
// isolated from the S1/S4 decode-pump logic in audio_renderer_impl.cc.

#include "media/filters/audio_renderer_impl.h"

#include "base/check.h"
#include "base/logging.h"

namespace avbase::media {
namespace {

// Applies gain to the first |written| frames and zeroes the rest. Split out of
// Render() for two reasons: it keeps that function inside the 80-line budget of
// invariant C2, and it makes the "short return means silence" rule a named
// thing rather than a loop buried at the end of a callback. Zeroing matters --
// the caller's AudioBus may still hold samples from the previous period, and
// playing those would click at exactly the device's period rate.
void ScaleAndZeroTail(AudioBus* dest, int written, float gain) {
  const int total = dest->frames();
  for (int c = 0; c < dest->channels(); ++c) {
    float* out = dest->channel(c);
    if (gain != 1.0f) {
      for (int i = 0; i < written; ++i) {
        out[i] *= gain;
      }
    }
    for (int i = written; i < total; ++i) {
      out[i] = 0.0f;
    }
  }
}

}  // namespace

// static
base::TimeDelta AudioRendererImpl::OutputFramesToMediaTime(int frames,
                                                           double rate,
                                                           int sample_rate) {
  if (sample_rate <= 0) {
    return base::TimeDelta();
  }
  return base::SecondsD(static_cast<double>(frames) * rate / sample_rate);
}

int AudioRendererImpl::Render(base::TimeDelta delay,
                              base::TimeTicks delay_timestamp,
                              const AudioGlitchInfo& glitch_info,
                              AudioBus* dest) {
  if (!dest || render_error_.load()) {
    return 0;
  }
  if (paused_.load()) {
    // Gated by SetPaused(): consume nothing, report nothing. Returning 0 is
    // the documented "play silence" answer, and counting it as an underrun
    // would fill audio_glitches with one entry per device period for as long
    // as the user is paused.
    return 0;
  }
  starved_.store(false);  // a served period clears the verdict
  DCHECK(sink_->CurrentThreadIsRenderingThread());

  const double rate = playback_rate_.load();
  int64_t first_media_micros = 0;
  const int written = DrainRing(dest, &first_media_micros);
  // Everything past the lock is arithmetic on |written| samples plus one
  // seqlock write, which is what keeps this inside the 100 us budget (Δ14
  // exists so the clock write needs no mutex).

  ScaleAndZeroTail(dest, written, muted_.load() ? 0.0f : volume_.load());

  if (written == 0) {
    // Reported as a short count, not papered over: the sink turns this into an
    // AudioGlitchInfo entry, and audio_glitches is how an underrun becomes
    // measurable instead of "sometimes it crackles".
    underruns_.fetch_add(1);
    starved_.store(true);
    return 0;
  }
  if (written < dest->frames()) {
    starved_.store(true);  // partially served: the queue is on fumes
  }
  // |written| > 0 here, so DrainRing() necessarily filled |first_media_micros|.
  if (av_sync_) {
    av_sync_->OnAudioFramesConsumed(
        written, base::TimeDelta::FromMicroseconds(first_media_micros),
        serial_);
    last_media_time_micros_.store(
        first_media_micros +
        OutputFramesToMediaTime(written, rate, params_.sample_rate())
            .InMicroseconds());
  }
  frames_rendered_.fetch_add(static_cast<uint64_t>(written));
  (void)delay;
  (void)delay_timestamp;
  (void)glitch_info;
  return written;
}

void AudioRendererImpl::OnRenderError() {
  // Latching rather than retrying: a device that reports an error keeps
  // reporting it, and a callback that logs on every period would flood the log
  // at ~50 lines/second. RendererImpl decides whether to reopen the device.
  if (!render_error_.exchange(true)) {
    LOG(ERROR) << "avbase.aout: audio device reported a render error";
  }
}

}  // namespace avbase::media
