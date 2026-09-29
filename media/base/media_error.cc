// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/media_error.h"

#include <cstdio>
#include <utility>

namespace ijkpp::media {

const char* GetErrorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk: return "Ok";
    case ErrorCode::kInvalidArgument: return "InvalidArgument";
    case ErrorCode::kInvalidState: return "InvalidState";
    case ErrorCode::kNotImplemented: return "NotImplemented";
    case ErrorCode::kOutOfMemory: return "OutOfMemory";
    case ErrorCode::kAborted: return "Aborted";
    case ErrorCode::kTimeout: return "Timeout";
    case ErrorCode::kCancelled: return "Cancelled";
    case ErrorCode::kSourceOpenFailed: return "SourceOpenFailed";
    case ErrorCode::kSourceNotFound: return "SourceNotFound";
    case ErrorCode::kSourcePermissionDenied: return "SourcePermissionDenied";
    case ErrorCode::kSourceReadFailed: return "SourceReadFailed";
    case ErrorCode::kSourceSeekFailed: return "SourceSeekFailed";
    case ErrorCode::kSourceUnsupported: return "SourceUnsupported";
    case ErrorCode::kSourceEos: return "SourceEos";
    case ErrorCode::kNetworkUnreachable: return "NetworkUnreachable";
    case ErrorCode::kNetworkTimeout: return "NetworkTimeout";
    case ErrorCode::kDecoderNotFound: return "DecoderNotFound";
    case ErrorCode::kDecoderOpenFailed: return "DecoderOpenFailed";
    case ErrorCode::kDecodeFailed: return "DecodeFailed";
    case ErrorCode::kDecoderUnsupportedCodec: return "DecoderUnsupportedCodec";
    case ErrorCode::kDecoderHwFallback: return "DecoderHwFallback";
    case ErrorCode::kSinkNotAttached: return "SinkNotAttached";
    case ErrorCode::kSinkConfigureFailed: return "SinkConfigureFailed";
    case ErrorCode::kSinkPresentFailed: return "SinkPresentFailed";
    case ErrorCode::kMediaUnseekable: return "MediaUnseekable";
    case ErrorCode::kStreamNotFound: return "StreamNotFound";
    case ErrorCode::kTrackNotFound: return "TrackNotFound";
    case ErrorCode::kConfigInvalid: return "ConfigInvalid";
    case ErrorCode::kEos: return "Eos";
  }
  return "Unknown";
}

MediaError::MediaError(ErrorCode code, std::string summary, std::string detail,
                       std::string suggestion, int native_code,
                       std::string context)
    : code_(code),
      native_code_(native_code),
      context_(std::move(context)),
      summary_(std::move(summary)),
      detail_(std::move(detail)),
      suggestion_(std::move(suggestion)) {}

// static
MediaError MediaError::Ok() { return MediaError(); }

// static
MediaError MediaError::Of(ErrorCode code, std::string summary,
                          std::string detail, std::string suggestion) {
  return MediaError(code, std::move(summary), std::move(detail),
                    std::move(suggestion));
}

std::string MediaError::ToString() const {
  if (ok()) {
    return "Ok";
  }
  std::string out = std::string(GetErrorCodeName(code_)) + ": " + summary_;
  if (!context_.empty()) {
    out += "\n  context: " + context_;
  }
  if (!detail_.empty()) {
    out += "\n  detail:  " + detail_;
  }
  if (!suggestion_.empty()) {
    out += "\n  hint:    " + suggestion_;
  }
  return out;
}

std::string MediaError::ToJson() const {
  // Minimal hand-rolled JSON; the project deliberately has no JSON dependency.
  auto escape = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
      switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += c;
          }
      }
    }
    return out;
  };
  std::string out = "{";
  out += "\"code\":\"" + std::string(GetErrorCodeName(code_)) + "\",";
  out += "\"native_code\":" + std::to_string(native_code_) + ",";
  out += "\"context\":\"" + escape(context_) + "\",";
  out += "\"summary\":\"" + escape(summary_) + "\",";
  out += "\"detail\":\"" + escape(detail_) + "\",";
  out += "\"hint\":\"" + escape(suggestion_) + "\"}";
  return out;
}

}  // namespace ijkpp::media
