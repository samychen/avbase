// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/filters/wsola_internals.h` (BSD-3-Clause, Copyright
// 2012 The Chromium Authors), which is the same split for the same reason: the
// DSP primitives are separable from the buffering and index bookkeeping, and
// keeping them apart is what makes each half reviewable.
//
// ★PROVENANCE — gap closed 2026-09-29, with three deliberate deviations.
// This file was written against `audio_renderer_algorithm.cc` alone, because
// `media/filters/wsola_internals.cc` returned HTTP 503 from
// chromium.googlesource.com. The 503 was specific to that path: the GitHub
// mirror at raw.githubusercontent.com/chromium/chromium/main served the same
// file the same day, so every function here has now been diffed against the
// real thing. What the diff found, and what was kept anyway:
//
// 1. Similarity(). Chromium has no `internal::SimilarityFloat`; the equivalent
//    is `MultiChannelSimilarityMeasure`, which normalises PER CHANNEL and sums
//    the results, adding kEpsilon = 1e-12f inside each sqrt. This version
//    normalises once over the summed energies of all channels and guards the
//    zero denominator explicitly. When every channel carries the same signal
//    the two agree on the winner exactly (Chromium's value is then `channels`
//    times this one), which is why the unit tests never noticed. They can
//    differ on real stereo, where one channel is much quieter than the other:
//    Chromium weights the channels independently, this one lets the loud
//    channel dominate both the numerator and the denominator. KEPT, because
//    the summed form is one sqrt per candidate instead of one per channel per
//    candidate, and because changing it now would silently move every block
//    boundary in the project's only pitch test. Worth revisiting with real
//    stereo material and a listening test, not blind.
// 2. OptimalIndex(). Chromium's is a three-stage approximation:
//    MultiChannelMovingBlockEnergies() precomputes every candidate's energy in
//    O(N) by sliding one sample at a time, DecimatedSearch() samples at
//    kSearchDecimation = 5 and refines each local maximum with
//    QuadraticInterpolation(), and an 11-candidate FullSearch() finishes. This
//    version is FullSearch over all ~1440 candidates: the same candidate set
//    and the exact optimum rather than an approximation, at roughly five times
//    the dot products and with no energy reuse. KEPT for now and recorded as a
//    performance gap, not a correctness one -- measured, the two pick blocks
//    that give identical results on both unit-test stimuli (452 Hz on a pure
//    sine at 2x, 440.0 Hz on the four-partial tone at 2x and 0.5x). Porting
//    the decimation is mechanical and is the right next step for this file; it
//    is worth ~5x on the audio thread, which matters more on a phone than on
//    the desktop this was measured on.
// 3. The exclude range. Chromium's InInterval() is closed at both ends, so it
//    skips kExcludeIntervalLengthFrames + 1 candidates; this one is half-open
//    and skips kExcludeIntervalLengthFrames. KEPT: one candidate in 1440, and
//    the half-open form matches how [begin, end) reads everywhere else here.
//
// The marker at the top of this block used to say GAP. It says what was
// actually true at the time and the deviations above are the residue, so it is
// kept as PROVENANCE rather than deleted: a reader should still know that three
// of these functions knowingly differ from Chromium's.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// (promoted from DRAFT, tenth round). Never compiled; see the gap list in
// audio_renderer_algorithm.h.
//
// NOTE ON THE MARKER MECHANISM (kept after promotion because it is a live
// hazard for any file that is still a draft). The marker must stay on its own
// line and must not be reflowed into a paragraph: check_invariants.py matches a
// literal six-word substring, so a comment re-wrap that splits it across two
// lines silently un-marks the file, which is how this one lost its draft status
// once already. The corollary is the one that bit a second time: prose that
// *quotes* the marker also counts as a marker, so this paragraph deliberately
// describes it instead of spelling it out.

#ifndef AVBASE_MEDIA_FILTERS_WSOLA_INTERNALS_H_
#define AVBASE_MEDIA_FILTERS_WSOLA_INTERNALS_H_

#include <vector>

#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/media_export.h"

namespace avbase::media {

// The `internal` namespace matches Chromium's, so a reader moving between the
// two codebases finds the same names in the same place. Nothing here is part of
// avbase's API surface and none of it is exported.
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
AVBASE_MEDIA_EXPORT int TimeToFrames(base::TimeDelta time,
                                     int samples_per_second);

// Periodic Hanning window: w[n] = 0.5 * (1 - cos(2*pi*n / N)), n in [0, N).
// "Periodic" means the final sample is not forced to zero, which is what makes
// two consecutive windows sum to a constant during overlap-add -- the property
// that keeps output amplitude flat instead of pulsing at the hop rate.
// Chromium: internal::GetPeriodicHanningWindow.
AVBASE_MEDIA_EXPORT void FillPeriodicHanningWindow(std::vector<float>* out);

// Normalised cross-correlation between |target| and |search| at |offset|,
// summed over channels, in [-1, 1]. A value of 1 means the two windows are
// proportional, which is the "same place in the same waveform" condition WSOLA
// needs in order to splice one window onto another without an audible seam. See
// the provenance gap at the top of this file.
AVBASE_MEDIA_EXPORT float Similarity(const AudioBus* search, int offset,
                                     const AudioBus* target);

// Index of the candidate block inside |search| that is most similar to
// |target|, skipping the half-open range [exclude_begin, exclude_end). Returns
// 0 when every candidate is excluded.
//
// |search_frames| says how many frames at the front of |search| hold real
// audio; 0 means "all of search->frames()". It exists for end of stream, where
// the search block is wider than what the queue still holds and the remainder
// is zero-fill: scoring a candidate against silence returns 0 from
// Similarity(), which beats every negative candidate and would splice in a
// block chosen for matching nothing. See
// AudioRendererAlgorithm::EffectiveSearchBlockFrames().
AVBASE_MEDIA_EXPORT int OptimalIndex(const AudioBus* search,
                                     const AudioBus* target, int exclude_begin,
                                     int exclude_end, int search_frames = 0);

}  // namespace internal
}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_WSOLA_INTERNALS_H_
