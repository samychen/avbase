// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/audio_parameters.h"

#include <cstring>

namespace avbase::media {

const char* GetSampleFormatName(SampleFormat format) {
  switch (format) {
  case SampleFormat::kUnknown:
    return "unknown";
  case SampleFormat::kU8:
    return "u8";
  case SampleFormat::kS16:
    return "s16";
  case SampleFormat::kS32:
    return "s32";
  case SampleFormat::kF32:
    return "f32";
  case SampleFormat::kS16P:
    return "s16p";
  case SampleFormat::kS32P:
    return "s32p";
  case SampleFormat::kF32P:
    return "f32p";
  }
  return "invalid";
}

const char* GetChannelLayoutName(ChannelLayout layout) {
  switch (layout) {
  case ChannelLayout::kNone:
    return "none";
  case ChannelLayout::kMono:
    return "mono";
  case ChannelLayout::kStereo:
    return "stereo";
  case ChannelLayout::k2_1:
    return "2.1";
  case ChannelLayout::kSurround:
    return "surround";
  case ChannelLayout::k4_0:
    return "4.0";
  case ChannelLayout::kQuad:
    return "quad";
  case ChannelLayout::k5_0:
    return "5.0";
  case ChannelLayout::k5_1:
    return "5.1";
  case ChannelLayout::k7_1:
    return "7.1";
  case ChannelLayout::kDiscrete:
    return "discrete";
  }
  return "invalid";
}

int ChannelLayoutToChannelCount(ChannelLayout layout) {
  switch (layout) {
  case ChannelLayout::kNone:
    return 0;
  case ChannelLayout::kMono:
    return 1;
  case ChannelLayout::kStereo:
    return 2;
  case ChannelLayout::k2_1:
    return 3;
  case ChannelLayout::kSurround:
    return 3;
  case ChannelLayout::k4_0:
    return 4;
  case ChannelLayout::kQuad:
    return 4;
  case ChannelLayout::k5_0:
    return 5;
  case ChannelLayout::k5_1:
    return 6;
  case ChannelLayout::k7_1:
    return 8;
  case ChannelLayout::kDiscrete:
    return 0;
  }
  return 0;
}

int SampleFormatBytesPerChannel(SampleFormat format) {
  switch (format) {
  case SampleFormat::kUnknown:
    return 0;
  case SampleFormat::kU8:
    return 1;
  case SampleFormat::kS16:
  case SampleFormat::kS16P:
    return 2;
  case SampleFormat::kS32:
  case SampleFormat::kS32P:
  case SampleFormat::kF32:
  case SampleFormat::kF32P:
    return 4;
  }
  return 0;
}

float DecodeSample(const uint8_t* src, SampleFormat format) {
  if (!src) {
    return 0.0f;
  }
  switch (format) {
  case SampleFormat::kU8:
    return (static_cast<float>(src[0]) - 128.0f) / 128.0f;
  case SampleFormat::kS16:
  case SampleFormat::kS16P: {
    int16_t v;
    std::memcpy(&v, src, sizeof(v));
    return static_cast<float>(v) / 32768.0f;
  }
  case SampleFormat::kS32:
  case SampleFormat::kS32P: {
    int32_t v;
    std::memcpy(&v, src, sizeof(v));
    return static_cast<float>(v) / 2147483648.0f;
  }
  case SampleFormat::kF32:
  case SampleFormat::kF32P: {
    float v;
    std::memcpy(&v, src, sizeof(v));
    return v;
  }
  case SampleFormat::kUnknown:
    return 0.0f;
  }
  return 0.0f;
}

}  // namespace avbase::media
