// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_PLATFORM_FFMPEG_INTERRUPT_CALLBACK_H_
#define IJKPP_PLATFORM_FFMPEG_INTERRUPT_CALLBACK_H_

#include "base/synchronization/atomic_flag.h"
#include "platform/ffmpeg/av_includes.h"

namespace ijkpp::platform::ffmpeg {

// Makes a blocking av_read_frame() / avformat_open_input() abortable.
//
// ijkplayer needs `abort_request` signalled from five separate places and an
// interrupt callback wired by hand; missing one is what makes its release()
// hang. Here there is exactly ONE flag and exactly ONE place that sets it
// (see docs/04 §2.1), and FFmpeg polls it through the AVIOInterruptCB.
class InterruptCallback {
 public:
  InterruptCallback() = default;
  InterruptCallback(const InterruptCallback&) = delete;
  InterruptCallback& operator=(const InterruptCallback&) = delete;

  // Fills |ctx| so it can be assigned to AVFormatContext::interrupt_callback.
  // |this| must outlive the AVFormatContext.
  void Install(AVFormatContext* ctx);

  void RequestInterrupt() { flag_.Set(); }
  void Clear() { flag_.Reset(); }
  bool IsRequested() const { return flag_.IsSet(); }

 private:
  // Returns 1 to make FFmpeg fail the in-flight call with AVERROR_EXIT.
  static int Poll(void* opaque);

  base::AtomicFlag flag_;
};

}  // namespace ijkpp::platform::ffmpeg

#endif  // IJKPP_PLATFORM_FFMPEG_INTERRUPT_CALLBACK_H_
