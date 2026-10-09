// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_FFMPEG_VIDEO_ENCODER_FACTORY_H_
#define AVBASE_PLATFORM_FFMPEG_VIDEO_ENCODER_FACTORY_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/decoder_config.h"       // VideoCodec
#include "media/filters/decoder_selector.h"  // HwCodecMask, HwCodecFlag
#include "media/media_export.h"

namespace avbase::media {
class FFmpegVideoEncoder;
}  // namespace avbase::media

namespace avbase::platform::ffmpeg {

// E5: Encoder factory, symmetric with VideoDecoderFactory. Creates a
// configured FFmpegVideoEncoder for a given codec. Hardware encoder
// factories (VideoToolbox/VAAPI/NVENC) implement this interface; the
// default software factory creates libx264/mjpeg encoders.
//
// HwCodecMask controls which codecs the hardware path is allowed to handle,
// mirroring the decoder side's HwCodecFlag bitmask.
class AVBASE_MEDIA_EXPORT VideoEncoderFactory
    : public base::RefCountedThreadSafe<VideoEncoderFactory> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  VideoEncoderFactory(const VideoEncoderFactory&) = delete;
  VideoEncoderFactory& operator=(const VideoEncoderFactory&) = delete;

  struct EncoderCapability {
    bool hardware{false};
    int priority{0};   // Higher wins among otherwise equal candidates.
    int max_width{0};  // 0 = unlimited.
    int max_height{0};
  };

  virtual EncoderCapability GetCapability() const = 0;
  virtual bool SupportsCodec(media::VideoCodec codec,
                             media::HwCodecMask hw_mask) const = 0;
  // The concrete FFmpeg encoder name this factory would use for |codec|:
  // "libx264" for the software factory, "h264_videotoolbox" for VideoToolbox.
  // Returns "" when the factory cannot encode it.
  //
  // Without this accessor, selecting a factory is unusable: CreateEncoder()
  // takes a codec *name*, so a caller that has just been told "use the
  // VideoToolbox factory" still has no way to learn what to ask for. That is
  // the same shape as the decoder defect logged in round 23 — an API with no
  // production caller that could actually reach it.
  virtual std::string EncoderNameFor(media::VideoCodec codec) const = 0;
  virtual std::unique_ptr<media::FFmpegVideoEncoder>
  CreateEncoder(const std::string& codec_name, int width, int height,
                int bit_rate, int crf, int gop, const std::string& preset,
                int fps_num, int fps_den) = 0;
  virtual const char* name() const = 0;

 protected:
  friend class base::RefCountedThreadSafe<VideoEncoderFactory>;
  VideoEncoderFactory() = default;
  virtual ~VideoEncoderFactory() = default;
};

// Factory creation helpers.

// Software encoder: libx264, mjpeg, etc.
AVBASE_MEDIA_EXPORT
base::scoped_refptr<VideoEncoderFactory> CreateSoftwareVideoEncoderFactory();

#if defined(__APPLE__)
// macOS/iOS VideoToolbox hardware encoder.
AVBASE_MEDIA_EXPORT
base::scoped_refptr<VideoEncoderFactory> CreateVideoToolboxEncoderFactory();
#endif

#if defined(__linux__) && !defined(__APPLE__)
// Linux VAAPI hardware encoder.
AVBASE_MEDIA_EXPORT
base::scoped_refptr<VideoEncoderFactory> CreateVaapiEncoderFactory();
#endif

// NVIDIA NVENC hardware encoder (Windows/Linux). Kept unguarded: unlike the
// two above it is not tied to a platform SDK header, just an FFmpeg encoder
// name, so building it everywhere costs nothing and the factory simply never
// Initializes where the driver is absent.
AVBASE_MEDIA_EXPORT
base::scoped_refptr<VideoEncoderFactory> CreateNvencEncoderFactory();

// The platform's factory list, highest priority first, in the order the
// caller should try them. Keeping the #if sprawl here means media/ never has
// to know which platforms exist.
AVBASE_MEDIA_EXPORT
std::vector<base::scoped_refptr<VideoEncoderFactory>>
CreateVideoEncoderFactories();

// Select the best encoder factory for |codec| from a list, considering
// the hw mask. Returns factories in priority order (highest first).
AVBASE_MEDIA_EXPORT
std::vector<base::scoped_refptr<VideoEncoderFactory>> SelectVideoEncoder(
    const std::vector<base::scoped_refptr<VideoEncoderFactory>>& factories,
    media::VideoCodec codec, media::HwCodecMask hw_mask);

}  // namespace avbase::platform::ffmpeg

#endif  // AVBASE_PLATFORM_FFMPEG_VIDEO_ENCODER_FACTORY_H_
