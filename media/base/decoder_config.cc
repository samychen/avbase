// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/decoder_config.h"

namespace avbase::media {

const char* GetVideoCodecName(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kUnknown:     return "unknown";
    case VideoCodec::kH264:        return "h264";
    case VideoCodec::kHevc:        return "hevc";
    case VideoCodec::kVp8:         return "vp8";
    case VideoCodec::kVp9:         return "vp9";
    case VideoCodec::kAv1:         return "av1";
    case VideoCodec::kMpeg4:       return "mpeg4";
    case VideoCodec::kMpeg2Video:  return "mpeg2video";
    case VideoCodec::kTheora:      return "theora";
  }
  return "invalid";
}

const char* GetAudioCodecName(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::kUnknown:  return "unknown";
    case AudioCodec::kAac:      return "aac";
    case AudioCodec::kMp3:      return "mp3";
    case AudioCodec::kOpus:     return "opus";
    case AudioCodec::kVorbis:   return "vorbis";
    case AudioCodec::kFlac:     return "flac";
    case AudioCodec::kPcmS16Le: return "pcm_s16le";
    case AudioCodec::kAc3:      return "ac3";
    case AudioCodec::kEac3:     return "eac3";
  }
  return "invalid";
}

VideoCodec VideoCodecFromName(std::string_view name) {
  if (name == "h264" || name == "libx264") return VideoCodec::kH264;
  if (name == "hevc" || name == "h265")    return VideoCodec::kHevc;
  if (name == "vp8")                        return VideoCodec::kVp8;
  if (name == "vp9")                        return VideoCodec::kVp9;
  if (name == "av1")                        return VideoCodec::kAv1;
  if (name == "mpeg4")                      return VideoCodec::kMpeg4;
  if (name == "mpeg2video")                 return VideoCodec::kMpeg2Video;
  if (name == "theora")                     return VideoCodec::kTheora;
  return VideoCodec::kUnknown;
}

AudioCodec AudioCodecFromName(std::string_view name) {
  if (name == "aac")        return AudioCodec::kAac;
  if (name == "mp3")        return AudioCodec::kMp3;
  if (name == "opus")       return AudioCodec::kOpus;
  if (name == "vorbis")     return AudioCodec::kVorbis;
  if (name == "flac")       return AudioCodec::kFlac;
  if (name == "pcm_s16le")  return AudioCodec::kPcmS16Le;
  if (name == "ac3")        return AudioCodec::kAc3;
  if (name == "eac3")       return AudioCodec::kEac3;
  return AudioCodec::kUnknown;
}

}  // namespace avbase::media
