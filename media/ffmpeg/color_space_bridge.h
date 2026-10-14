// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_COLOR_SPACE_BRIDGE_H_
#define AVBASE_MEDIA_FFMPEG_COLOR_SPACE_BRIDGE_H_

#include "media/base/decoder_config.h"
#include "media/base/video_color_space.h"

// Only ever an opaque pointer here (the definition lives in av_includes.h, and
// C4 keeps libav types inside media/ffmpeg). Forward-declared rather than
// included so this header stays cheap: it used to rely on the includer -- and
// on avbase_ffmpeg's precompiled header -- having already seen it.
struct AVFrame;

namespace avbase::media::ffmpeg {

// Maps an AVFrame's colour metadata onto avbase's VideoColorSpace, falling
// back to the container-keyed table (GuessColorSpaceFallback) when the frame
// carries no usable tags. Never returns an IsSpecified()==false result for a
// config with a known codec: the point of the bridge is that every frame that
// leaves a decoder has a colour story.
//
// Defined here rather than in media/ because it reads AVFrame fields
// (invariant C4: libav types stay inside platform/ffmpeg).
media::VideoColorSpace
ColorSpaceFromAvFrame(const AVFrame* frame,
                      const media::VideoDecoderConfig& config);

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_COLOR_SPACE_BRIDGE_H_
