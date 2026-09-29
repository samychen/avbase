// Copyright (c) 2013-2026 Zhang Rui <bbcallen@gmail.com>
// Copyright (c) 2013-2026 Bilibili
// Copyright (c) 2003-2013 Fabrice Bellard (ffplay.c)
// Copyright 2026 The ijkpp Authors. All rights reserved.
//
// This file is part of ijkpp.
//
// ijkpp is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License as published by the Free
// Software Foundation; either version 2.1 of the License, or (at your option)
// any later version.
//
// ijkpp is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
// details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
//
// ---------------------------------------------------------------------------
// LICENSE: LGPL-2.1-or-later. Full text: media/filters/legacy/LICENSE.LGPL-2.1
//
// WHY THIS FILE IS LGPL AND NOT BSD-3 (decision D10, risk R8, docs/08 §4/§5):
// this is a port of ffplay's `struct Clock` (get_clock / set_clock /
// set_clock_at), i.e. a derivative work of ijkplayer (LGPL-2.1). The seqlock
// read path is original to ijkpp (behaviour difference Δ14) but it implements
// the same quantity, so the file stays under the ported licence. See
// media/filters/legacy/README.md for the boundary rules.
//
// Port of ffplay's `struct Clock` (get_clock / set_clock / set_clock_at) from
// ff_ffplay.c (LGPL-2.1), with the data race fixed.
//
// WHY A SEQLOCK. ffplay stores pts/drift as plain doubles and reads them from
// the video thread while the audio thread writes them. On a 64-bit target the
// load usually does not tear, so the bug is invisible for years — until it
// tears, and one frame is scheduled against a nonsense timestamp. TSan reports
// it unconditionally. A seqlock gives the same lock-free read cost (one atomic
// load, no mutex, no allocation) with a well-defined result: readers retry
// while a write is in flight. This is behaviour difference Δ14.

#ifndef IJKPP_MEDIA_FILTERS_LEGACY_CLOCK_H_
#define IJKPP_MEDIA_FILTERS_LEGACY_CLOCK_H_

#include <stdint.h>

#include <atomic>

#include "base/memory/raw_ptr.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "media/base/media_constants.h"
#include "media/media_export.h"

namespace ijkpp::media {

// A media clock: a media timestamp anchored to a wall-clock instant, plus a
// playback speed. Reading it extrapolates forward by the elapsed wall time, so
// callers get "where the media is *now*", not "where the last decoded frame
// was".
//
// Single writer, multiple readers. The writer must be confined to one
// sequence (the audio clock to the audio sequence, the video clock to the
// compositor sequence); readers may be anywhere.
class IJKPP_MEDIA_EXPORT Clock {
 public:
  struct Snapshot {
    base::TimeDelta pts{media::kNoTimestamp};
    base::TimeDelta drift;         // pts minus the wall time it was captured at
    int32_t serial{-1};
    float speed{1.0f};
    bool valid{false};
    base::TimeTicks updated_at;
  };

  explicit Clock(const base::TickClock* wall_clock);
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  ~Clock() = default;

  // ---- Writer (single sequence) ----
  // Anchors the clock at |pts| and records |wall_time| as the instant that
  // timestamp was true. |serial| tags the seek generation.
  void Set(base::TimeDelta pts, int32_t serial, base::TimeTicks wall_time);
  // Convenience: anchors at the wall clock's current instant.
  void Set(base::TimeDelta pts, int32_t serial);
  // Moves the clock without changing its serial (audio consumption does this).
  void SetTo(base::TimeDelta pts);
  void Invalidate();
  void SetSpeed(float speed);

  // ---- Readers (any thread, lock-free) ----
  // Extrapolated current media time, or kNoTimestamp when invalid.
  base::TimeDelta Get() const;
  int32_t serial() const;
  float speed() const;
  bool valid() const;
  // A consistent snapshot of every field. Retries internally while a write is
  // in flight, so the values always belong to the same update.
  Snapshot Read() const;

 private:
  // Fields are plain values guarded by |seq_|: the seqlock protocol, not the
  // types, provides the atomicity. Each is individually atomic so the compiler
  // cannot merge or reorder the loads across the sequence counter.
  uint32_t ReadRetry() const;
  static bool IsOdd(uint32_t seq) { return (seq & 1u) != 0; }

  mutable std::atomic<uint32_t> seq_{0};
  mutable std::atomic<int64_t> pts_micros_{0};
  mutable std::atomic<int64_t> drift_micros_{0};
  mutable std::atomic<int64_t> updated_at_micros_{0};
  mutable std::atomic<int32_t> serial_{-1};
  mutable std::atomic<float> speed_{1.0f};
  mutable std::atomic<bool> valid_{false};
  base::raw_ptr<const base::TickClock> wall_;
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_LEGACY_CLOCK_H_
