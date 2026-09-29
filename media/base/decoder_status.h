// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/decoder_status.h` (BSD-3-Clause).

#ifndef IJKPP_MEDIA_BASE_DECODER_STATUS_H_
#define IJKPP_MEDIA_BASE_DECODER_STATUS_H_

#include <string>

#include "media/media_export.h"

namespace ijkpp::media {

class IJKPP_MEDIA_EXPORT DecoderStatus {
 public:
  enum class Codes {
    kOk = 0,
    kUnknownError,
    kNotInitialized,
    kUnsupportedCodec,
    kUnsupportedResolution,
    kUnsupportedConfig,
    kDecodeError,
    kDecodingAborted,
    kElidedEndOfStreamForConfigChange,
  };

  DecoderStatus() = default;
  DecoderStatus(Codes code, std::string description)
      : code_(code), description_(std::move(description)) {}

  bool is_ok() const { return code_ == Codes::kOk; }
  Codes code() const { return code_; }
  const std::string& description() const { return description_; }
  std::string AsDebugString() const;

 private:
  Codes code_{Codes::kOk};
  std::string description_;
};

IJKPP_MEDIA_EXPORT const char* GetDecoderStatusCodeName(DecoderStatus::Codes code);

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_DECODER_STATUS_H_
