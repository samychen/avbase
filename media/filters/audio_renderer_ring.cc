// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The ring half of AudioRendererImpl: the pre-stretch that moves the
// algorithm's
// frames into the ready ring, and the drain that the device pulls out of it.
//
// A separate translation unit because the class hit the 500-line limit of
// invariant C1 for the second time (the first was the EOS fix that had to
// publish the algorithm's tail). The split is along a real seam rather than at
// a line number: everything here runs on S4 with the algorithm, while Render()
// -- which stayed behind -- runs on the device's thread S7, and the only state
// they share is the ring under |handoff_lock_|.

#include "media/filters/audio_renderer_impl.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "media/base/audio_bus.h"

namespace ijkpp::media {

void AudioRendererImpl::PreStretch() {
  const int chunk_frames = params_.frames_per_buffer();
  if (!stretch_bus_ || chunk_frames <= 0) {
    return;
  }
  const double rate = playback_rate_.load();
  while (true) {
    int free_index = -1;
    {
      base::AutoLock scoped(handoff_lock_);
      if (ring_count_ >= kReadyChunks) {
        return;
      }
      free_index = (ring_head_ + ring_count_) % kReadyChunks;
    }
    // Writing into an unpublished slot needs no lock: S7 only ever reads slots
    // inside [ring_head_, ring_head_ + ring_count_) and only consumes, so the
    // free set can grow but never shrink underneath this write. The fields are
    // published by the ring_count_ increment below, and that increment happens
    // under the lock, which is also the barrier that makes them visible to S7.
    ReadyChunk& slot = ring_[free_index];
    const int got =
        algorithm_.FillBuffer(slot.bus.get(), 0, chunk_frames, rate);
    if (got <= 0) {
      return;                     // dry; the next decoded buffer wakes us up
    }
    slot.frames = got;
    slot.media_time = next_chunk_media_time_;
    next_chunk_media_time_ += OutputFramesToMediaTime(got, rate,
                                                     params_.sample_rate());
    {
      base::AutoLock scoped(handoff_lock_);
      ++ring_count_;
    }
  }
}

// ---------------------------------------------------------------------------
// S7: the device callback
// ---------------------------------------------------------------------------

int AudioRendererImpl::DrainRing(AudioBus* dest, int64_t* first_media_micros) {
  DCHECK(dest);
  DCHECK(first_media_micros);
  const int requested = dest->frames();
  const int channels = dest->channels();
  const double rate = playback_rate_.load();
  int written = 0;
  bool have_time = false;

  base::AutoLock scoped(handoff_lock_);
  while (written < requested && ring_count_ > 0) {
    ReadyChunk& front = ring_[ring_head_];
    const int available = front.frames - front_offset_;
    const int n = std::min(available, requested - written);
    for (int c = 0; c < channels; ++c) {
      const float* src = front.bus->channel(c) + front_offset_;
      float* out = dest->channel(c) + written;
      for (int i = 0; i < n; ++i) {
        out[i] = src[i];
      }
    }
    if (!have_time) {
      *first_media_micros =
          (front.media_time +
           OutputFramesToMediaTime(front_offset_, rate,
                                   params_.sample_rate()))
              .InMicroseconds();
      have_time = true;
    }
    front_offset_ += n;
    written += n;
    if (front_offset_ >= front.frames) {
      front_offset_ = 0;
      ring_head_ = (ring_head_ + 1) % kReadyChunks;
      --ring_count_;
      // Consumer wake-up (the counterpart of ffplay's frame_queue_signal,
      // expressed as a posted task rather than a condvar): the pump may have
      // stopped on ring back-pressure, and only the consumer can observe that
      // the pressure has cleared. PumpDecoder re-checks every condition on S4,
      // so a spurious kick is a cheap no-op.
      task_runner_->PostTask(FROM_HERE,
                             base::BindOnce(&AudioRendererImpl::PumpDecoder,
                                            base::Unretained(this)));
    }
  }
  return written;
}

}  // namespace ijkpp::media
