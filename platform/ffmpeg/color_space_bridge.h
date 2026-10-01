// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_FFMPEG_COLOR_SPACE_BRIDGE_H_
#define AVBASE_PLATFORM_FFMPEG_COLOR_SPACE_BRIDGE_H_

#include "media/base/decoder_config.h"
#include "media/base/video_color_space.h"

namespace avbase::platform::ffmpeg {

// Maps an AVFrame's colour metadata onto avbase's VideoColorSpace, falling
// back to the container-keyed table (GuessColorSpaceFallback) when the frame
// carries no usable tags. Never returns an IsSpecified()==false result for a
// config with a known codec: the point of the bridge is that every frame that
// leaves a decoder has a colour story.
//
// Defined here rather than in media/ because it reads AVFrame fields
// (invariant C4: libav types stay inside platform/ffmpeg).
media::VideoColorSpace ColorSpaceFromAvFrame(
    const AVFrame* frame, const media::VideoDecoderConfig& config);

}  // namespace avbase::platform::ffmpeg

#endif  // AVBASE_PLATFORM_FFMPEG_COLOR_SPACE_BRIDGE_H_
