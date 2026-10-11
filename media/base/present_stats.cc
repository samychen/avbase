// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/present_stats.h"

namespace avbase::media {

void PresentStats::NotePresented() {
  frames_presented_.fetch_add(1, std::memory_order_relaxed);
}

void PresentStats::NotePresentTiming(const PresentTiming& timing) {
  const base::TimeTicks now = base::TimeTicks::Now();
  base::AutoLock scoped(lock_);
  render_.upload.Add(timing.upload_ms);
  render_.compose.Add(timing.compose_ms);
  render_.blit.Add(timing.blit_ms);
  render_.present.Add(timing.present_ms);
  render_.upload_path = timing.upload_path;
  // The spacing between presents is the only honest source for the display
  // refresh: a sink's GetDisplayInterval() returns the 60 Hz estimate it was
  // constructed with, and this is what turns that estimate into a measurement.
  if (!last_present_ticks_.is_null() && now > last_present_ticks_) {
    present_interval_ms_.Add((now - last_present_ticks_).InMillisecondsF());
  }
  last_present_ticks_ = now;
}

void PresentStats::NoteDropped() {
  frames_dropped_.fetch_add(1, std::memory_order_relaxed);
}

void PresentStats::NoteSubmitFailure() {
  submit_failures_.fetch_add(1, std::memory_order_relaxed);
}

VideoSinkStats PresentStats::Get() const {
  VideoSinkStats stats;
  stats.frames_presented = frames_presented_.load(std::memory_order_relaxed);
  stats.frames_dropped = frames_dropped_.load(std::memory_order_relaxed);
  stats.submit_failures = submit_failures_.load(std::memory_order_relaxed);
  base::AutoLock scoped(lock_);
  stats.render = render_;
  if (present_interval_ms_.samples() > 0 &&
      present_interval_ms_.average_ms() > 0.0) {
    stats.measured_refresh_hz = 1000.0 / present_interval_ms_.average_ms();
  }
  return stats;
}

}  // namespace avbase::media
