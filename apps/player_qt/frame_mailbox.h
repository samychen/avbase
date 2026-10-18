// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The one-slot hand-off between avbase's render thread and Qt's GUI thread.
//
// avbase's sink PULLS frames on its own thread (VideoRendererSink is a pull
// model -- see media/base/video_renderer_sink.h), while Qt may only paint on
// the GUI thread, so the frame crosses as a reference swap under a mutex.
// One slot, not a queue: a display that cannot keep up with the decoder should
// show the NEWEST frame, not replay a backlog. The frame itself is already
// reference-counted, so "storing" it costs one atomic increment and never a
// pixel copy.

#ifndef AVBASE_APPS_PLAYER_QT_FRAME_MAILBOX_H_
#define AVBASE_APPS_PLAYER_QT_FRAME_MAILBOX_H_

#include <stdint.h>

#include <mutex>
#include <utility>

#include "base/memory/scoped_refptr.h"
#include "media/base/video_frame.h"

namespace avbase::apps {

class FrameMailbox {
 public:
  FrameMailbox() = default;
  FrameMailbox(const FrameMailbox&) = delete;
  FrameMailbox& operator=(const FrameMailbox&) = delete;

  // Any thread (in practice the sink's render sequence).
  void Store(base::scoped_refptr<media::VideoFrame> frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_ = std::move(frame);
    ++serial_;
  }

  // Any thread (in practice the GUI timer). |serial| is bumped on every store,
  // which lets the caller skip the YUV->RGB conversion when nothing arrived
  // since the previous paint -- the difference between one conversion per
  // painted frame and one per refresh tick.
  base::scoped_refptr<media::VideoFrame> Load(int64_t* serial) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (serial != nullptr) {
      *serial = serial_;
    }
    return frame_;
  }

  // Drops the current frame and bumps the serial, so a pending repaint sees
  // "nothing" instead of repainting a picture that belongs to the old source.
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_.reset();
    ++serial_;
  }

 private:
  mutable std::mutex mutex_;
  base::scoped_refptr<media::VideoFrame> frame_;
  int64_t serial_{0};
};

}  // namespace avbase::apps

#endif  // AVBASE_APPS_PLAYER_QT_FRAME_MAILBOX_H_
