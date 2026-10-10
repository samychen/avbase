// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/interrupt_callback.h"

namespace avbase::media::ffmpeg {

// static
int InterruptCallback::Poll(void* opaque) {
  auto* self = static_cast<InterruptCallback*>(opaque);
  return self && self->flag_.IsSet() ? 1 : 0;
}

void InterruptCallback::Install(AVFormatContext* ctx) {
  if (!ctx) {
    return;
  }
  ctx->interrupt_callback.callback = &InterruptCallback::Poll;
  ctx->interrupt_callback.opaque = this;
}

}  // namespace avbase::media::ffmpeg
