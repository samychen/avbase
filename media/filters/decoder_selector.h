// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/filters/decoder_selector.h` (BSD-3-Clause).
//
// Replaces ijkplayer's hard-coded boolean soup: `ffp->mediacodec_all_videos ||
// ffp->mediacodec_avc || ffp->mediacodec_hevc || ffp->mediacodec_mpeg2 ||
// ffp->mediacodec_mpeg4` scattered across ffp_check_video_decoder() and
// ff_ffplay.c. Here it is one pure function over a capability list, so the
// ordering rule is testable without a device.

#ifndef AVBASE_MEDIA_FILTERS_DECODER_SELECTOR_H_
#define AVBASE_MEDIA_FILTERS_DECODER_SELECTOR_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/decoder_config.h"
#include "media/base/video_decoder_factory.h"
#include "media/media_export.h"

namespace avbase::media {

// Which decoder kind to prefer. Defined here rather than in player/public so
// that media/ never depends on player/ (invariant C22); PlayerConfig's field of
// the same name is an alias for this type.
enum class DecoderPreference {
  kAuto = 0,       // Hardware first, fall back to software.
  kHardwareFirst,  // Same as kAuto but logs a warning on fallback.
  kSoftware,       // Never use hardware.
  kHardwareOnly,   // Fail instead of falling back.
};

// Bitmask of codecs a hardware path is allowed to handle. Replaces ijkplayer's
// mediacodec-avc / -hevc / -mpeg2 / -mpeg4 / -all-videos booleans.
enum class HwCodecFlag : uint32_t {
  kNone = 0,
  kAvc = 1u << 0,
  kHevc = 1u << 1,
  kMpeg2 = 1u << 2,
  kMpeg4 = 1u << 3,
  kVp9 = 1u << 4,
  kAv1 = 1u << 5,
  kAll = 0xFFFFFFFFu,
};
using HwCodecMask = uint32_t;

// Pure ranking function. Given the registered factories, a stream config and
// the caller's preference, returns the factories to try in order. The caller
// attempts Initialize() on each and falls back to the next on failure, emitting
// a kDecoderFallback event (behaviour difference Δ12: ijkplayer surfaced a hard
// error in several of these cases).
//
// Deterministic and side-effect free so the ordering can be exhaustively unit
// tested; see decoder_selector_unittest.cc.
class AVBASE_MEDIA_EXPORT DecoderSelector {
 public:
  DecoderSelector(const DecoderSelector&) = delete;
  DecoderSelector& operator=(const DecoderSelector&) = delete;

  // |reasons|, when non-null, receives one line per rejected candidate naming
  // the factory and why it was skipped. This is what makes "why did my hardware
  // decoder not get used" answerable from a log instead of requiring a
  // debugger.
  static std::vector<base::scoped_refptr<VideoDecoderFactory>>
  SelectVideoDecoder(
      const std::vector<base::scoped_refptr<VideoDecoderFactory>>& factories,
      const VideoDecoderConfig& config, DecoderPreference preference,
      HwCodecMask hw_codecs, std::vector<std::string>* reasons);

  // True when |codec| is covered by |mask|. kAll covers everything.
  static bool CodecAllowedByMask(VideoCodec codec, HwCodecMask mask);

 private:
  DecoderSelector() = delete;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_DECODER_SELECTOR_H_
