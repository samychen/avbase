// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/media_error.h` (BSD-3-Clause).
//
// Lives in media/base, not player/public, because media::Demuxer and
// media::VideoDecoder both report through it and media/ must not depend on
// player/ (docs/02 §2.1, invariant C22). player/public/error.h re-exports it.

#ifndef IJKPP_MEDIA_BASE_MEDIA_ERROR_H_
#define IJKPP_MEDIA_BASE_MEDIA_ERROR_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/types/expected.h"
#include "media/media_export.h"

namespace ijkpp::media {

enum class ErrorCode : uint16_t {
  kOk = 0,
  // Generic.
  kInvalidArgument, kInvalidState, kNotImplemented, kOutOfMemory, kAborted,
  kTimeout, kCancelled,
  // Data source.
  kSourceOpenFailed, kSourceNotFound, kSourcePermissionDenied,
  kSourceReadFailed, kSourceSeekFailed, kSourceUnsupported, kSourceEos,
  kNetworkUnreachable, kNetworkTimeout,
  // Decoding.
  kDecoderNotFound, kDecoderOpenFailed, kDecodeFailed, kDecoderUnsupportedCodec,
  kDecoderHwFallback,
  // Rendering.
  kSinkNotAttached, kSinkConfigureFailed, kSinkPresentFailed,
  // Player.
  kMediaUnseekable, kStreamNotFound, kTrackNotFound, kConfigInvalid, kEos,
};

IJKPP_MEDIA_EXPORT const char* GetErrorCodeName(ErrorCode code);

// A three-part error, because "playback failed" is not actionable.
//
// Every error ijkpp produces fills all of:
//   summary()    one line, <= 80 chars: what failed
//   detail()     the actual values involved (uri, codec, resolution, timeout)
//   suggestion() a concrete next step naming an API or a config field
// tests/unit/player/error_messages_unittest.cc asserts this for every
// ErrorCode, so the guarantee cannot silently rot. See docs/10 §4.
class IJKPP_MEDIA_EXPORT MediaError {
 public:
  MediaError() = default;
  MediaError(ErrorCode code, std::string summary, std::string detail,
             std::string suggestion, int native_code = 0,
             std::string context = {});

  static MediaError Ok();
  static MediaError Of(ErrorCode code, std::string summary,
                       std::string detail = {}, std::string suggestion = {});

  bool ok() const { return code_ == ErrorCode::kOk; }
  explicit operator bool() const { return !ok(); }

  ErrorCode code() const { return code_; }
  int native_code() const { return native_code_; }
  const std::string& context() const { return context_; }
  const std::string& summary() const { return summary_; }
  const std::string& detail() const { return detail_; }
  const std::string& suggestion() const { return suggestion_; }

  // Human-readable, multi-line: "<Code>: <summary>\n  context: ...\n
  //   detail: ...\n  hint: ...".
  std::string ToString() const;
  std::string ToJson() const;

  friend bool operator==(const MediaError& a, const MediaError& b) {
    return a.code_ == b.code_ && a.native_code_ == b.native_code_;
  }

 private:
  ErrorCode code_{ErrorCode::kOk};
  int native_code_{0};
  std::string context_;
  std::string summary_;
  std::string detail_;
  std::string suggestion_;
};

// The result type used across the whole API. base::expected, so no exceptions
// are involved (the project builds with -fno-exceptions).
template <typename T>
using Result = base::expected<T, MediaError>;
using Status = base::expected<void, MediaError>;

inline Status OkStatus() { return Status(); }

// std::expected<void, E> cannot be constructed implicitly from E, so error
// returns go through base::unexpected. This helper keeps call sites short:
//   return Err(ErrorCode::kTimeout, "prepare timed out", detail, hint);
inline base::unexpected<MediaError> Err(MediaError error) {
  return base::unexpected<MediaError>(std::move(error));
}
inline base::unexpected<MediaError> Err(ErrorCode code, std::string summary,
                                        std::string detail = {},
                                        std::string suggestion = {}) {
  return base::unexpected<MediaError>(
      MediaError(code, std::move(summary), std::move(detail),
                 std::move(suggestion)));
}


}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_MEDIA_ERROR_H_
