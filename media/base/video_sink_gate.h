// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_VIDEO_SINK_GATE_H_
#define AVBASE_MEDIA_BASE_VIDEO_SINK_GATE_H_

#include <atomic>

namespace avbase::media {

// The question every video sink's render loop asks itself once per tick:
// should I pull a frame from the compositor right now?
//
// Two things can say yes, and the second is why this is a named object rather
// than an `if` written three times:
//
//   * we are playing, so the display is being fed; or
//   * the compositor was just flushed, so the NEXT frame is a different
//     picture and has to be shown even though the clock is stopped.
//
// The second case is "seek while paused". Every sink gated on playing alone,
// and Flush() is what closes that gate -- so a seek made while paused left the
// pre-seek frame on screen while the pipeline decoded the new generation
// behind it. Nothing in the decoder, the compositor or the seek path was
// wrong: the sink had simply stopped asking.
//
// Header-only and inline: no out-of-line code, hence no export macro. The two
// flags are plain atomics because SetPlaying/NoteFlushed come from the control
// sequence and ShouldPull/NoteFrameReceived run on the render sequence.
class VideoSinkGate {
 public:
  void SetPlaying(bool playing) { playing_.store(playing); }

  // "The next frame is new content." One shot, and spent by the first frame
  // that comes back rather than by the first ask: the frame is usually still
  // being decoded when the ask goes out.
  void NoteFlushed() { refresh_.store(true); }

  bool ShouldPull() const { return playing_.load() || refresh_.load(); }

  // Called with the answer to a pull. A null answer ("repeat what you have")
  // spends nothing -- there is no new picture yet.
  void NoteFrameReceived() { refresh_.store(false); }

 private:
  std::atomic<bool> playing_{false};
  std::atomic<bool> refresh_{false};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_VIDEO_SINK_GATE_H_
