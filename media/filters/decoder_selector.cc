// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/decoder_selector.h"

#include <algorithm>
#include <utility>

#include "base/logging.h"

namespace avbase::media {

// static
bool DecoderSelector::CodecAllowedByMask(VideoCodec codec, HwCodecMask mask) {
  // Resolve the codec's bit FIRST. Testing `mask == kAll` up front would
  // report "allowed" for codecs that have no hardware bit at all (kUnknown,
  // kVp8, kTheora), which is how a decoder gets selected for a stream it
  // cannot handle.
  HwCodecMask bit = 0;
  switch (codec) {
    case VideoCodec::kH264:       bit = static_cast<HwCodecMask>(HwCodecFlag::kAvc);   break;
    case VideoCodec::kHevc:       bit = static_cast<HwCodecMask>(HwCodecFlag::kHevc);  break;
    case VideoCodec::kMpeg2Video: bit = static_cast<HwCodecMask>(HwCodecFlag::kMpeg2);  break;
    case VideoCodec::kMpeg4:      bit = static_cast<HwCodecMask>(HwCodecFlag::kMpeg4);  break;
    case VideoCodec::kVp9:        bit = static_cast<HwCodecMask>(HwCodecFlag::kVp9);    break;
    case VideoCodec::kAv1:        bit = static_cast<HwCodecMask>(HwCodecFlag::kAv1);    break;
    case VideoCodec::kUnknown:
    case VideoCodec::kVp8:
    case VideoCodec::kTheora:     return false;
  }
  if (bit == 0) {
    return false;
  }
  return (mask & bit) != 0;
}

namespace {

// A factory is usable for |config| when it does not exclude the codec and, for
// hardware paths, when the resolution is inside its advertised maximum.
bool IsCandidate(const VideoDecoderFactory& factory,
                 const VideoDecoderConfig& config, bool want_hardware,
                 std::string* reason) {
  const VideoDecoderCapability cap = factory.GetCapability();
  if (cap.hardware != want_hardware) {
    *reason = want_hardware ? "software decoder" : "hardware decoder";
    return false;
  }
  if (cap.max_width > 0 && config.coded_size.width > cap.max_width) {
    *reason = "width " + std::to_string(config.coded_size.width) +
              " exceeds the decoder maximum " + std::to_string(cap.max_width);
    return false;
  }
  if (cap.max_height > 0 && config.coded_size.height > cap.max_height) {
    *reason = "height " + std::to_string(config.coded_size.height) +
              " exceeds the decoder maximum " + std::to_string(cap.max_height);
    return false;
  }
  return true;
}

}  // namespace

// static
std::vector<base::scoped_refptr<VideoDecoderFactory>>
DecoderSelector::SelectVideoDecoder(
    const std::vector<base::scoped_refptr<VideoDecoderFactory>>& factories,
    const VideoDecoderConfig& config, DecoderPreference preference,
    HwCodecMask hw_codecs, std::vector<std::string>* reasons) {
  std::vector<base::scoped_refptr<VideoDecoderFactory>> hardware;
  std::vector<base::scoped_refptr<VideoDecoderFactory>> software;

  for (const auto& factory : factories) {
    if (!factory) {
      continue;
    }
    const bool want_hardware = factory->GetCapability().hardware;
    std::string reason;
    if (!IsCandidate(*factory, config, want_hardware, &reason)) {
      // A hardware factory can still be rejected by the codec mask even when its
      // capability struct looks fine; that check is preference-specific below.
      if (reasons && !reason.empty() && reason != "software decoder" &&
          reason != "hardware decoder") {
        reasons->push_back(std::string(factory->name()) + ": " + reason);
      }
      continue;
    }
    if (want_hardware && !CodecAllowedByMask(config.codec, hw_codecs)) {
      if (reasons) {
        reasons->push_back(std::string(factory->name()) +
                           ": codec not enabled in config.video.hw_codecs");
      }
      continue;
    }
    (want_hardware ? hardware : software).push_back(factory);
  }

  // Higher priority first within each group, so a platform can express
  // "prefer the vendor decoder over the generic one".
  auto by_priority = [](const base::scoped_refptr<VideoDecoderFactory>& a,
                        const base::scoped_refptr<VideoDecoderFactory>& b) {
    return a->GetCapability().priority > b->GetCapability().priority;
  };
  std::stable_sort(hardware.begin(), hardware.end(), by_priority);
  std::stable_sort(software.begin(), software.end(), by_priority);

  std::vector<base::scoped_refptr<VideoDecoderFactory>> out;
  switch (preference) {
    case DecoderPreference::kSoftware:
      out = std::move(software);
      break;
    case DecoderPreference::kHardwareOnly:
      out = std::move(hardware);
      break;
    case DecoderPreference::kAuto:
    case DecoderPreference::kHardwareFirst:
      out = std::move(hardware);
      out.insert(out.end(), software.begin(), software.end());
      break;
  }

  if (reasons && out.empty()) {
    reasons->push_back("no decoder factory can handle codec=" +
                       std::string(GetVideoCodecName(config.codec)) + " " +
                       std::to_string(config.coded_size.width) + "x" +
                       std::to_string(config.coded_size.height));
  }
  return out;
}

}  // namespace avbase::media
