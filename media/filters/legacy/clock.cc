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

#include "media/filters/legacy/clock.h"

#include "base/check.h"

namespace ijkpp::media {

Clock::Clock(const base::TickClock* wall_clock) : wall_(wall_clock) {
  CHECK(wall_) << "Clock needs a TickClock; pass DefaultTickClock::GetInstance() "
                  "in production and SimpleTestTickClock in tests";
}

uint32_t Clock::ReadRetry() const {
  // Spin while a write is in flight (odd sequence). Writers hold the odd state
  // only for a handful of stores, so this never spins measurably.
  uint32_t seq = seq_.load(std::memory_order_acquire);
  while (IsOdd(seq)) {
    seq = seq_.load(std::memory_order_acquire);
  }
  return seq;
}

void Clock::Set(base::TimeDelta pts, int32_t serial, base::TimeTicks wall_time) {
  // Writer protocol: make the sequence odd, publish, make it even again.
  seq_.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);

  const int64_t pts_us = media::IsNoTimestamp(pts) ? 0 : pts.InMicroseconds();
  pts_micros_.store(pts_us, std::memory_order_relaxed);
  // drift is defined as pts minus the wall instant it was captured at, so that
  // Get() can reconstruct pts_at_now = drift + now.
  drift_micros_.store(pts_us - wall_time.since_origin_micros(),
                      std::memory_order_relaxed);
  updated_at_micros_.store(wall_time.since_origin_micros(),
                           std::memory_order_relaxed);
  serial_.store(serial, std::memory_order_relaxed);
  valid_.store(!media::IsNoTimestamp(pts), std::memory_order_relaxed);

  std::atomic_thread_fence(std::memory_order_release);
  seq_.fetch_add(1, std::memory_order_release);
}

void Clock::Set(base::TimeDelta pts, int32_t serial) {
  Set(pts, serial, wall_->NowTicks());
}

void Clock::SetTo(base::TimeDelta pts) {
  Set(pts, serial_.load(std::memory_order_relaxed), wall_->NowTicks());
}

void Clock::Invalidate() {
  Set(media::kNoTimestamp, serial_.load(std::memory_order_relaxed),
      wall_->NowTicks());
}

void Clock::SetSpeed(float speed) {
  // Re-anchor before changing the rate, otherwise the elapsed-time extrapolation
  // retroactively rescales the interval that was already played at the old rate.
  const Snapshot before = Read();
  speed_.store(speed, std::memory_order_release);
  if (before.valid) {
    const base::TimeDelta elapsed = wall_->NowTicks() - before.updated_at;
    Set(before.pts + elapsed * static_cast<int64_t>(1), before.serial,
        wall_->NowTicks());
    speed_.store(speed, std::memory_order_release);
  }
}

Clock::Snapshot Clock::Read() const {
  Snapshot snapshot;
  for (;;) {
    const uint32_t before = ReadRetry();
    snapshot.valid = valid_.load(std::memory_order_relaxed);
    snapshot.serial = serial_.load(std::memory_order_relaxed);
    snapshot.speed = speed_.load(std::memory_order_relaxed);
    const int64_t pts_us = pts_micros_.load(std::memory_order_relaxed);
    const int64_t drift_us = drift_micros_.load(std::memory_order_relaxed);
    const int64_t updated_us = updated_at_micros_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_.load(std::memory_order_acquire) == before) {
      snapshot.pts = base::Microseconds(pts_us);
      snapshot.drift = base::Microseconds(drift_us);
      snapshot.updated_at = base::TimeTicks::FromMicroseconds(updated_us);
      return snapshot;
    }
    // A write landed mid-read; retry. Bounded in practice because writers do not
    // loop, but cap it so a pathological writer cannot livelock a reader.
  }
}

base::TimeDelta Clock::Get() const {
  const Snapshot s = Read();
  if (!s.valid) {
    return media::kNoTimestamp;
  }
  const base::TimeDelta elapsed = wall_->NowTicks() - s.updated_at;
  const double rate = s.speed > 0.0f ? static_cast<double>(s.speed) : 1.0;
  // ffplay's get_clock() is
  //     pts_drift + time - (time - last_updated) * (1.0 - speed)
  // with pts_drift == pts - last_updated. Expanding it algebraically gives
  //     pts + (time - last_updated) * speed
  // which is what is computed here, so the two are provably identical at every
  // speed rather than only at 1.0.
  //
  // Bug #32: this used to read `s.drift + elapsed * rate`. Because drift
  // already has last_updated subtracted from it, that form removes the capture
  // instant twice and returns a master clock offset by -last_updated -- on a
  // real machine, by negative uptime. It went unnoticed because every unit test
  // drives a SimpleTestTickClock that starts at zero, where drift == pts and the
  // two forms agree. `ijkpp-inspect sync` caught it immediately: the master read
  // -33185 s on a host up for 9.2 hours, which would have marked every video
  // frame as infinitely late and dropped the entire stream.
  // The widening to double is explicit: -Wconversion (debug preset, -Werror)
  // rejects it otherwise, because an int64 above 2^53 microseconds -- about
  // 285,000 years -- would lose precision. Unreachable, but the cast says so.
  const double scaled =
      static_cast<double>(elapsed.InMicroseconds()) * rate;
  return base::Microseconds(s.pts.InMicroseconds() +
                            static_cast<int64_t>(scaled));
}

int32_t Clock::serial() const { return serial_.load(std::memory_order_acquire); }

float Clock::speed() const { return speed_.load(std::memory_order_acquire); }

bool Clock::valid() const { return valid_.load(std::memory_order_acquire); }

}  // namespace ijkpp::media
