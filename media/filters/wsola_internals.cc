// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// See wsola_internals.h for the
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// provenance gap that applies to Similarity() specifically.

#include "media/filters/wsola_internals.h"

#include <algorithm>   // std::min in Similarity()
#include <cmath>

namespace ijkpp::media {
namespace internal {

int TimeToFrames(base::TimeDelta time, int samples_per_second) {
  if (samples_per_second <= 0) {
    return 0;
  }
  const int64_t micros = time.InMicroseconds();
  return static_cast<int>((micros * samples_per_second + 500000) / 1000000);
}

void FillPeriodicHanningWindow(std::vector<float>* out) {
  if (!out) {
    return;
  }
  const int n = static_cast<int>(out->size());
  if (n <= 0) {
    return;
  }
  const double step = 2.0 * kPi / static_cast<double>(n);
  for (int i = 0; i < n; ++i) {
    (*out)[i] = static_cast<float>(0.5 * (1.0 - std::cos(step * i)));
  }
}

float Similarity(const AudioBus* search, int offset, const AudioBus* target) {
  if (!search || !target) {
    return 0.0f;
  }
  const int frames = target->frames();
  const int channels = std::min(search->channels(), target->channels());
  double cross = 0.0;
  double target_energy = 0.0;
  double search_energy = 0.0;
  for (int c = 0; c < channels; ++c) {
    const float* t = target->channel(c);
    const float* s = search->channel(c) + offset;
    for (int n = 0; n < frames; ++n) {
      const double tv = t[n];
      const double sv = s[n];
      cross += tv * sv;
      target_energy += tv * tv;
      search_energy += sv * sv;
    }
  }
  const double denominator = std::sqrt(target_energy * search_energy);
  // Silence against silence has no defined similarity. Returning 0 rather than
  // NaN matters: a NaN would poison every comparison in OptimalIndex and make
  // the chosen block arbitrary, which is inaudible while the signal is silent
  // and catastrophic the moment a real signal follows it.
  if (!(denominator > 0.0)) {
    return 0.0f;
  }
  return static_cast<float>(cross / denominator);
}

int OptimalIndex(const AudioBus* search, const AudioBus* target,
                 int exclude_begin, int exclude_end) {
  if (!search || !target) {
    return 0;
  }
  const int last = search->frames() - target->frames();
  int best_index = 0;
  // Below the attainable minimum of -1, so any real candidate wins and the
  // "everything excluded" case still returns a defined index.
  float best_similarity = -2.0f;
  for (int n = 0; n <= last; ++n) {
    if (n >= exclude_begin && n < exclude_end) {
      continue;
    }
    const float similarity = Similarity(search, n, target);
    if (similarity > best_similarity) {
      best_similarity = similarity;
      best_index = n;
    }
  }
  return best_index;
}

}  // namespace internal
}  // namespace ijkpp::media
