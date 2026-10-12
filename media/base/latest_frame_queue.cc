// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/latest_frame_queue.h"

#include <utility>

#include "base/check.h"

namespace avbase::media {

LatestFrameQueue::LatestFrameQueue() = default;
LatestFrameQueue::~LatestFrameQueue() = default;

base::scoped_refptr<VideoFrame>
LatestFrameQueue::Publish(base::scoped_refptr<VideoFrame> frame) {
  DCHECK(frame);
  base::AutoLock scoped(lock_);
  ++published_;
  base::scoped_refptr<VideoFrame> superseded = std::move(pending_);
  if (superseded) {
    ++superseded_;
  }
  pending_ = std::move(frame);
  return superseded;
}

base::scoped_refptr<VideoFrame> LatestFrameQueue::Consume() {
  base::AutoLock scoped(lock_);
  base::scoped_refptr<VideoFrame> frame = std::move(pending_);
  if (frame) {
    ++consumed_;
  }
  return frame;
}

base::scoped_refptr<VideoFrame> LatestFrameQueue::Clear() {
  base::AutoLock scoped(lock_);
  base::scoped_refptr<VideoFrame> dropped = std::move(pending_);
  if (dropped) {
    ++cleared_;
  }
  return dropped;
}

bool LatestFrameQueue::has_pending() const {
  base::AutoLock scoped(lock_);
  return pending_ != nullptr;
}

uint64_t LatestFrameQueue::published_frames() const {
  base::AutoLock scoped(lock_);
  return published_;
}

uint64_t LatestFrameQueue::superseded_frames() const {
  base::AutoLock scoped(lock_);
  return superseded_;
}

uint64_t LatestFrameQueue::consumed_frames() const {
  base::AutoLock scoped(lock_);
  return consumed_;
}

uint64_t LatestFrameQueue::cleared_frames() const {
  base::AutoLock scoped(lock_);
  return cleared_;
}

}  // namespace avbase::media
