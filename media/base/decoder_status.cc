// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/decoder_status.h"

namespace avbase::media {

const char* GetDecoderStatusCodeName(DecoderStatus::Codes code) {
  using C = DecoderStatus::Codes;
  switch (code) {
  case C::kOk:
    return "ok";
  case C::kUnknownError:
    return "unknown-error";
  case C::kNotInitialized:
    return "not-initialized";
  case C::kUnsupportedCodec:
    return "unsupported-codec";
  case C::kUnsupportedResolution:
    return "unsupported-resolution";
  case C::kUnsupportedConfig:
    return "unsupported-config";
  case C::kDecodeError:
    return "decode-error";
  case C::kDecodingAborted:
    return "decoding-aborted";
  case C::kElidedEndOfStreamForConfigChange:
    return "elided-eos-for-config-change";
  }
  return "invalid";
}

std::string DecoderStatus::AsDebugString() const {
  std::string out = GetDecoderStatusCodeName(code_);
  if (!description_.empty()) {
    out += ": " + description_;
  }
  return out;
}

}  // namespace avbase::media
