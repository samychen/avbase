// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_VIDEO_DECODER_DEFAULTS_H_
#define AVBASE_PLAYER_VIDEO_DECODER_DEFAULTS_H_

#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_decoder_factory.h"
#include "media/filters/decoder_selector.h"

namespace avbase {

// The platform's default hardware-decoder factory list, in priority order.
// Returns an empty vector when the build has no hardware factory enabled or
// the preference forbids hardware. The codec-level gate lives inside each
// factory (CodecAllowedByMask over |allowed_codecs|), so an excluded codec is
// a "not me" for the hardware path and the stream walks on to the software
// tail without any error. A missing device or unsupported profile fails at
// decoder Initialize() and DecoderStream falls back -- the same chain that
// already handles host-injected factories.
std::vector<base::scoped_refptr<media::VideoDecoderFactory>>
DefaultHardwareVideoDecoderFactories(
    const base::scoped_refptr<base::SequencedTaskRunner>& video_runner,
    media::HwCodecMask allowed_codecs);

}  // namespace avbase

#endif  // AVBASE_PLAYER_VIDEO_DECODER_DEFAULTS_H_
