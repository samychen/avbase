// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/video_decoder_factory.h` (BSD-3-Clause).

#ifndef AVBASE_MEDIA_BASE_VIDEO_DECODER_FACTORY_H_
#define AVBASE_MEDIA_BASE_VIDEO_DECODER_FACTORY_H_

#include <memory>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/media_types.h"
#include "media/media_export.h"

namespace avbase::media {

class VideoDecoder;
struct VideoDecoderConfig;

// What a factory can actually decode. DecoderSelector ranks candidates using
// this, which replaces ijkplayer's pile of booleans (`mediacodec`,
// `mediacodec-avc`, `mediacodec-hevc`, `mediacodec-mpeg2`, ...).
struct AVBASE_MEDIA_EXPORT VideoDecoderCapability {
  bool hardware{false};
  bool outputs_opaque_surface{false};  // MediaCodec-into-Surface, VideoToolbox.
  bool handles_resolution_change{false};
  int max_width{0};  // 0 means unlimited.
  int max_height{0};
  int priority{0};  // Higher wins among otherwise equal candidates.
};

// Ref-counted because factories are shared between players and outlive any
// single pipeline: Deps holds them, and a running DecoderStream keeps its own
// reference. Matches Chromium's VideoDecoderFactory.
class AVBASE_MEDIA_EXPORT VideoDecoderFactory
    : public base::RefCountedThreadSafe<VideoDecoderFactory> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  VideoDecoderFactory(const VideoDecoderFactory&) = delete;
  VideoDecoderFactory& operator=(const VideoDecoderFactory&) = delete;

  virtual VideoDecoderCapability GetCapability() const = 0;
  virtual bool SupportsCodec(VideoDecoderType type_hint) const = 0;
  // Returns nullptr when this factory cannot handle |config|.
  virtual std::unique_ptr<VideoDecoder>
  CreateVideoDecoder(const VideoDecoderConfig& config) = 0;
  virtual const char* name() const = 0;

 protected:
  // Virtual: this class has virtual members, so per the rule documented in
  // base/memory/ref_counted.h it declares its own virtual destructor.
  friend class base::RefCountedThreadSafe<VideoDecoderFactory>;
  VideoDecoderFactory() = default;
  virtual ~VideoDecoderFactory() = default;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_VIDEO_DECODER_FACTORY_H_
