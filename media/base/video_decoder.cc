// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_decoder.h"

namespace avbase::media {

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::NeedsBitstreamConversion() const { return false; }

// Default: a decoder that allocates a fresh frame per output can always accept
// more input. Decoders with a fixed frame pool (MediaCodec in surface mode)
// override this to return false when the pool is exhausted.
bool VideoDecoder::CanReadWithoutStalling() const { return true; }

int VideoDecoder::GetMaxDecodeRequests() const { return 1; }

}  // namespace avbase::media
