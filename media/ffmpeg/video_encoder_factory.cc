// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/video_encoder_factory.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "media/ffmpeg/ffmpeg_video_encoder.h"

namespace avbase::media::ffmpeg {
namespace {

// Software encoder factory: creates libx264/mjpeg encoders.
class SoftwareVideoEncoderFactory final : public VideoEncoderFactory {
 public:
  SoftwareVideoEncoderFactory() = default;

  EncoderCapability GetCapability() const override {
    EncoderCapability cap;
    cap.hardware = false;
    cap.priority = 10;  // Lower than hardware.
    return cap;
  }

  bool SupportsCodec(media::VideoCodec codec,
                     media::HwCodecMask hw_mask) const override {
    (void)hw_mask;
    return codec == media::VideoCodec::kH264 ||
           codec == media::VideoCodec::kHevc ||
           codec == media::VideoCodec::kMjpeg ||
           codec == media::VideoCodec::kVp8 || codec == media::VideoCodec::kVp9;
  }

  std::unique_ptr<media::FFmpegVideoEncoder>
  CreateEncoder(const std::string& codec_name, int width, int height,
                int bit_rate, int crf, int gop, const std::string& preset,
                int fps_num, int fps_den) override {
    auto enc = std::make_unique<media::FFmpegVideoEncoder>();
    media::FFmpegVideoEncoder::Params params;
    params.codec_name = codec_name;
    params.width = width;
    params.height = height;
    params.bit_rate = bit_rate;
    params.crf = crf;
    params.gop = gop;
    params.preset = preset;
    params.fps_num = fps_num;
    params.fps_den = fps_den;
    if (!enc->Initialize(params)) {
      LOG(ERROR) << "software encoder factory: init failed for " << codec_name;
      return nullptr;
    }
    return enc;
  }

  std::string EncoderNameFor(media::VideoCodec codec) const override {
    switch (codec) {
    case media::VideoCodec::kH264:
      return "libx264";
    case media::VideoCodec::kHevc:
      return "libx265";
    case media::VideoCodec::kVp8:
      return "libvpx";
    case media::VideoCodec::kVp9:
      return "libvpx-vp9";
    case media::VideoCodec::kMjpeg:
      return "mjpeg";
    default:
      return {};  // Not encodable in software.
    }
  }

  const char* name() const override { return "sw-encoder"; }

 private:
  ~SoftwareVideoEncoderFactory() override = default;
};

// Hardware encoder factory: wraps FFmpeg's hardware encoder path
// (VideoToolbox on macOS/iOS, VAAPI on Linux). The factory delegates to
// FFmpegVideoEncoder with a hardware codec_name (e.g. "h264_videotoolbox",
// "h264_vaapi", "h264_nvenc").
class HardwareVideoEncoderFactory final : public VideoEncoderFactory {
 public:
  struct Spec {
    std::string display_name;
    std::string encoder_name_h264;
    std::string encoder_name_hevc;
    int priority = 100;  // Higher than software.
  };

  explicit HardwareVideoEncoderFactory(Spec spec) : spec_(std::move(spec)) {}

  EncoderCapability GetCapability() const override {
    EncoderCapability cap;
    cap.hardware = true;
    cap.priority = spec_.priority;
    return cap;
  }

  std::string EncoderNameFor(media::VideoCodec codec) const override {
    switch (codec) {
    case media::VideoCodec::kH264:
      return spec_.encoder_name_h264;
    case media::VideoCodec::kHevc:
      return spec_.encoder_name_hevc;
    default:
      return {};  // Hardware encoders here cover AVC/HEVC only.
    }
  }

  bool SupportsCodec(media::VideoCodec codec,
                     media::HwCodecMask hw_mask) const override {
    switch (codec) {
    case media::VideoCodec::kH264:
      return (hw_mask &
              static_cast<media::HwCodecMask>(media::HwCodecFlag::kAvc)) != 0;
    case media::VideoCodec::kHevc:
      return (hw_mask &
              static_cast<media::HwCodecMask>(media::HwCodecFlag::kHevc)) != 0;
    default:
      return false;
    }
  }

  std::unique_ptr<media::FFmpegVideoEncoder>
  CreateEncoder(const std::string& codec_name, int width, int height,
                int bit_rate, int crf, int gop, const std::string& preset,
                int fps_num, int fps_den) override {
    auto enc = std::make_unique<media::FFmpegVideoEncoder>();
    media::FFmpegVideoEncoder::Params params;
    params.codec_name = codec_name;
    params.width = width;
    params.height = height;
    params.bit_rate = bit_rate;
    params.crf = crf;  // Hardware encoders may ignore CRF.
    params.gop = gop;
    params.preset = preset;
    params.fps_num = fps_num;
    params.fps_den = fps_den;
    if (!enc->Initialize(params)) {
      LOG(ERROR) << "hardware encoder factory: init failed for " << codec_name
                 << " (" << spec_.display_name << ")";
      return nullptr;
    }
    return enc;
  }

  const char* name() const override { return spec_.display_name.c_str(); }

 private:
  ~HardwareVideoEncoderFactory() override = default;
  const Spec spec_;
};

}  // namespace

base::scoped_refptr<VideoEncoderFactory> CreateSoftwareVideoEncoderFactory() {
  return base::MakeRefCounted<SoftwareVideoEncoderFactory>();
}

#if defined(__APPLE__)
base::scoped_refptr<VideoEncoderFactory> CreateVideoToolboxEncoderFactory() {
  HardwareVideoEncoderFactory::Spec spec;
  spec.display_name = "VideoToolbox";
  spec.encoder_name_h264 = "h264_videotoolbox";
  spec.encoder_name_hevc = "hevc_videotoolbox";
  spec.priority = 100;
  return base::MakeRefCounted<HardwareVideoEncoderFactory>(std::move(spec));
}
#endif

#if defined(__linux__) && !defined(__APPLE__)
base::scoped_refptr<VideoEncoderFactory> CreateVaapiEncoderFactory() {
  HardwareVideoEncoderFactory::Spec spec;
  spec.display_name = "VAAPI";
  spec.encoder_name_h264 = "h264_vaapi";
  spec.encoder_name_hevc = "hevc_vaapi";
  spec.priority = 100;
  return base::MakeRefCounted<HardwareVideoEncoderFactory>(std::move(spec));
}
#endif

base::scoped_refptr<VideoEncoderFactory> CreateNvencEncoderFactory() {
  HardwareVideoEncoderFactory::Spec spec;
  spec.display_name = "NVENC";
  spec.encoder_name_h264 = "h264_nvenc";
  spec.encoder_name_hevc = "hevc_nvenc";
  spec.priority = 90;  // Below VideoToolbox/VAAPI: dedicated-ASIC paths win.
  return base::MakeRefCounted<HardwareVideoEncoderFactory>(std::move(spec));
}

std::vector<base::scoped_refptr<VideoEncoderFactory>>
CreateVideoEncoderFactories() {
  std::vector<base::scoped_refptr<VideoEncoderFactory>> factories;
  factories.push_back(CreateSoftwareVideoEncoderFactory());
#if defined(__APPLE__)
  factories.push_back(CreateVideoToolboxEncoderFactory());
#endif
#if defined(__linux__) && !defined(__APPLE__)
  factories.push_back(CreateVaapiEncoderFactory());
#endif
  factories.push_back(CreateNvencEncoderFactory());
  return factories;
}

std::vector<base::scoped_refptr<VideoEncoderFactory>> SelectVideoEncoder(
    const std::vector<base::scoped_refptr<VideoEncoderFactory>>& factories,
    media::VideoCodec codec, media::HwCodecMask hw_mask) {
  std::vector<base::scoped_refptr<VideoEncoderFactory>> result;
  for (const auto& f : factories) {
    if (f->SupportsCodec(codec, hw_mask)) {
      result.push_back(f);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const base::scoped_refptr<VideoEncoderFactory>& a,
               const base::scoped_refptr<VideoEncoderFactory>& b) {
              return a->GetCapability().priority > b->GetCapability().priority;
            });
  return result;
}

}  // namespace avbase::media::ffmpeg
