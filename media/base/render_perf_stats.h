// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_RENDER_PERF_STATS_H_
#define AVBASE_MEDIA_BASE_RENDER_PERF_STATS_H_

#include <stdint.h>

#include <string>

#include "media/media_export.h"

namespace avbase::media {

// How a decoded frame reached the GPU. The vocabulary mirrors MediaComponent's
// UploadPath (framework/render/common/render_perf_stats.h) so a reader moving
// between the two codebases does not have to translate.
//
// Reporting the PATH and not only the timing is what makes "why is upload
// slow" answerable: the same 0.2 ms means something different on a synchronous
// CPU copy than it would on a zero-copy hardware import, and a number that
// cannot say which one it is invites the wrong conclusion.
enum class UploadPath {
  kNone = 0,    // No frame uploaded yet.
  kCpuSync,     // Owned CPU frame copied in with glTexSubImage2D on the
                // render thread, so the upload blocks that thread.
  kPbo,         // Reserved: asynchronous pixel-buffer upload (not built).
  kHwZeroCopy,  // A decoded GPU frame imported as a GL texture with no CPU
                // copy: the plane textures are pointed at the decoder's own
                // surface. Built for VideoToolbox / CVPixelBuffer; see
                // GlHwTextureImporter.
};

AVBASE_MEDIA_EXPORT const char* UploadPathName(UploadPath path);

// Whether a requested "vf" filter graph is reaching the pixels, and when it is
// not, why. A graph that quietly does nothing is indistinguishable from one
// that works until somebody reads this: the symptom is a picture carrying none
// of the requested processing, which looks exactly like a picture that never
// asked for any.
//
// It sits beside UploadPath because the two are one decision seen from both
// ends: a CPU filter graph needs mappable planes, so asking for the graph is
// also asking to give up the hardware import. Naming the refusal is what keeps
// that trade from being invisible -- and "refused", not "failed", because a
// configuration that cannot work is not a runtime fault to be retried.
enum class VideoFilterState {
  kOff = 0,               // No graph was requested.
  kRunning,               // Requested, and applied to every frame shown.
  kRefusedHardwareFrame,  // Requested, but the decoder produces unmappable
                          // frames (zero-copy); shown unfiltered.
  kRefusedNoFactory,      // Requested, but no stage factory was injected
                          // (no-ffmpeg build).
  kRefusedGraphFailed,    // Requested, but the graph would not start.
};

AVBASE_MEDIA_EXPORT const char* VideoFilterStateName(VideoFilterState state);

// Per-frame breakdown of one present, in milliseconds. The four fields are
// kept apart because they are separated by a real cost boundary -- each can
// regress on its own, and merging them turns "rendering got slower" into a
// number nobody can act on:
//
//   upload_ms   texture uploads (plane pixels -> GPU).
//   compose_ms  clear + the draw passes (YUV->RGB and any overlay), drawn into
//               the off-screen canvas.
//   blit_ms     the composite -> window pass: one full-viewport textured quad,
//               scaled to the drawable. Zero when nothing was composed.
//   present_ms  SwapBuffers -- the only stage that can block on vsync, so a
//               value that tracks the refresh period is expected, not a bug.
//
// blit_ms exists as its own field rather than folded into compose_ms because
// it is the price of the off-screen canvas and nothing else: a reader who sees
// compose_ms grow has to be able to tell "the draw got more expensive" from
// "the canvas round-trip was added". Folding it in would have hidden exactly
// the cost this layer introduced.
//
// Reported as a value, not a callback: the producer is on the render thread
// and the consumer is on the media sequence, so the hand-off is a snapshot.
struct AVBASE_MEDIA_EXPORT PresentTiming {
  double upload_ms{0.0};
  double compose_ms{0.0};
  double blit_ms{0.0};
  double present_ms{0.0};
  // Which route this frame took. Carried with the timing rather than inferred
  // by the sink, because only the presenter knows: it is the one that decided
  // between a CPU copy and a hardware import.
  //
  // Starts at kNone, not kCpuSync: "nothing uploaded yet" and "uploaded the
  // expensive way" are different claims, and a default of kCpuSync made the
  // second one unfalsifiable -- a path that never uploaded anything reported
  // the same value as one that copied every frame. With several layers fed
  // into one frame the two routes can both happen, and the merge rule is that
  // a copy wins: kHwZeroCopy therefore asserts that EVERY layer of this frame
  // was imported, which is the claim worth being able to trust.
  UploadPath upload_path{UploadPath::kNone};
};

// Exponential moving average of a per-frame duration, with the worst sample
// seen since the last Reset(). A max is kept next to the average because a
// spike that a 1-in-100 frame costs is invisible in the mean and is usually
// the thing that shows up as a stutter.
//
// Thread-compatible, not thread-safe: the writer is the render thread, the
// reader copies the whole RenderPerfStats under the sink's own lock.
class AVBASE_MEDIA_EXPORT DurationEwma {
 public:
  void Add(double ms);
  void Reset();

  double average_ms() const { return average_ms_; }
  double max_ms() const { return max_ms_; }
  uint64_t samples() const { return samples_; }

 private:
  double average_ms_{0.0};
  double max_ms_{0.0};
  uint64_t samples_{0};
};

// Render-pipeline telemetry: what the GL path costs per frame. Populated by
// the presenter (PresentTiming), folded by the sink, and carried up to
// PlaybackStats so a caller can see it without a profiler attached.
//
// Frame counts are deliberately NOT here: VideoSinkStats already owns
// frames_presented / frames_dropped, and a second copy would be the kind of
// duplicated state that drifts. This struct answers "what did each frame
// cost", that one answers "how many frames were there".
struct AVBASE_MEDIA_EXPORT RenderPerfStats {
  DurationEwma upload;
  DurationEwma compose;
  DurationEwma blit;
  DurationEwma present;
  UploadPath upload_path{UploadPath::kNone};
  // Whether the "vf" graph is running; see the enum above. Carried with the
  // timings rather than with the configuration that asked for it, because what
  // a reader needs is not "was a graph requested" but "is one applied" -- the
  // two differ exactly when something went wrong.
  VideoFilterState filter_state{VideoFilterState::kOff};

  std::string ToJson() const;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_RENDER_PERF_STATS_H_
