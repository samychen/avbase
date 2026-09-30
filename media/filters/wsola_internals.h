// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/filters/wsola_internals.h` (BSD-3-Clause, Copyright
// 2012 The Chromium Authors), which is the same split for the same reason: the
// DSP primitives are separable from the buffering and index bookkeeping, and
// keeping them apart is what makes each half reviewable.
//
// ★PROVENANCE GAP — read this before trusting Similarity().
// Chromium's versions live in `media/filters/wsola_internals.cc`, which
// returned HTTP 503 when this file was written (2026-09-29). So: *
// TimeToFrames, FillPeriodicHanningWindow and OptimalIndex were read from
// Chromium's `audio_renderer_algorithm.cc`, which was reachable, and are
// faithful to it. * Similarity() is textbook normalised cross-correlation, NOT
// a verified port of `internal::SimilarityFloat`. It is the one function here
// that could differ from Chromium in a way that changes how 2x playback sounds.
// Diffing the two is a prerequisite for this file leaving DRAFT; it is also the
// reason this file exists separately, so that the one uncertain piece is a
// single named function rather than something buried in 700 lines.
//
// STATUS: DRAFT — NOT YET IN THE BUILD. Never compiled; see the gap list in
// audio_renderer_algorithm.h.
//
// The marker above must stay on its own line and must not be reflowed into a
// paragraph. check_invariants.py matches the literal substring "STATUS: DRAFT",
// so a comment re-wrap that splits it across two lines silently un-marks the
// file -- which is how this one lost its DRAFT status once already, and how it
// then became subject to the style and size rules it is meant to be exempt from
// until it compiles.

#ifndef IJKPP_MEDIA_FILTERS_WSOLA_INTERNALS_H_
#define IJKPP_MEDIA_FILTERS_WSOLA_INTERNALS_H_

#include <vector>

#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/media_export.h"

namespace ijkpp::media {

// The `internal` namespace matches Chromium's, so a reader moving between the
// two codebases finds the same names in the same place. Nothing here is part of
// ijkpp's API surface and none of it is exported.
namespace internal {

// Pi, spelled out rather than taken from <cmath>'s M_PI. M_PI is a POSIX
// extension, not standard C++: it is visible in this build only because CMake's
// CMAKE_CXX_EXTENSIONS defaults to ON (so the standard is -std=gnu++20), and it
// disappears the moment that is set to OFF or a stricter toolchain is used.
// Chromium defines base::kPiDouble for exactly this reason.
inline constexpr double kPi = 3.14159265358979323846;

// Rounds a duration to whole frames the way Chromium's
// AudioTimestampHelper::TimeToFrames does. Rounding rather than truncating
// keeps ola_window_size_ from coming out one frame short at rates such as 44100
// Hz, which would shift every subsequent window and make the overlap-add
// asymmetric.
IJKPP_MEDIA_EXPORT int TimeToFrames(base::TimeDelta time,
                                    int samples_per_second);

// Periodic Hanning window: w[n] = 0.5 * (1 - cos(2*pi*n / N)), n in [0, N).
// "Periodic" means the final sample is not forced to zero, which is what makes
// two consecutive windows sum to a constant during overlap-add -- the property
// that keeps output amplitude flat instead of pulsing at the hop rate.
// Chromium: internal::GetPeriodicHanningWindow.
IJKPP_MEDIA_EXPORT void FillPeriodicHanningWindow(std::vector<float>* out);

// Normalised cross-correlation between |target| and |search| at |offset|,
// summed over channels, in [-1, 1]. A value of 1 means the two windows are
// proportional, which is the "same place in the same waveform" condition WSOLA
// needs in order to splice one window onto another without an audible seam. See
// the provenance gap at the top of this file.
IJKPP_MEDIA_EXPORT float Similarity(const AudioBus* search, int offset,
                                    const AudioBus* target);

// Index of the candidate block inside |search| that is most similar to
// |target|, skipping the half-open range [exclude_begin, exclude_end). Returns
// 0 when every candidate is excluded.
IJKPP_MEDIA_EXPORT int OptimalIndex(const AudioBus* search,
                                    const AudioBus* target, int exclude_begin,
                                    int exclude_end);

}  // namespace internal
}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_WSOLA_INTERNALS_H_
