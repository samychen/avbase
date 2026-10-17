// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Shared internals of the C ABI (docs/08 M15) -- NOT a public header.
//
// The C ABI is split across three .cc files (player / display / events) to stay
// inside the C1 500-line limit. What they share lives here: the opaque handle
// definitions, the enum translations, and the string copy-out helper.
//
// The handles are declared at GLOBAL scope, not in an anonymous namespace: the
// public header's typedefs already bind these names, and re-declaring them
// inside a namespace would make every unqualified use ambiguous.

#ifndef AVBASE_PLAYER_PUBLIC_C_AVBASE_C_INTERNAL_H_
#define AVBASE_PLAYER_PUBLIC_C_AVBASE_C_INTERNAL_H_

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/base/media_types.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "player/public/c/avbase_c.h"

#include "player/public/c/avbase_c.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/base/media_info.h"
#include "media/base/media_types.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "player/public/global.h"
#include "player/public/media_info.h"
#include "player/public/player.h"
#include "player/public/player_event.h"
#include "player/public/scene.h"

// Defined at GLOBAL scope, not in an anonymous namespace: the header's typedefs
// already bind these names in C, and re-declaring them inside a namespace makes
// every unqualified use ambiguous.
struct AvbasePlayer {
  std::unique_ptr<avbase::Player> player;
};

struct AvbaseNativeDisplay {
  avbase::base::scoped_refptr<avbase::media::NativeDisplay> display;
};

struct AvbaseSource {
  std::shared_ptr<avbase::SceneSource> source;
};

namespace {

// ---------------------------------------------------------------------------
// Enum translation
// ---------------------------------------------------------------------------

// The C codes are a frozen list declared in the same order as the C++ enum. A
// plain static_cast would compile and be silently wrong if someone reordered
// the header, so the mapping goes through a switch and an unnamed code becomes
// a loud generic failure instead of an arbitrary number.
AvbaseStatus ToC(avbase::ErrorCode code) {
  switch (code) {
    case avbase::ErrorCode::kOk:
      return AVBASE_OK;
    case avbase::ErrorCode::kInvalidArgument:
      return AVBASE_ERR_INVALID_ARGUMENT;
    case avbase::ErrorCode::kInvalidState:
      return AVBASE_ERR_INVALID_STATE;
    case avbase::ErrorCode::kNotImplemented:
      return AVBASE_ERR_NOT_IMPLEMENTED;
    case avbase::ErrorCode::kOutOfMemory:
      return AVBASE_ERR_OUT_OF_MEMORY;
    case avbase::ErrorCode::kAborted:
      return AVBASE_ERR_ABORTED;
    case avbase::ErrorCode::kTimeout:
      return AVBASE_ERR_TIMEOUT;
    case avbase::ErrorCode::kSourceOpenFailed:
      return AVBASE_ERR_SOURCE_OPEN_FAILED;
    case avbase::ErrorCode::kSourceNotFound:
      return AVBASE_ERR_SOURCE_NOT_FOUND;
    case avbase::ErrorCode::kSourceReadFailed:
      return AVBASE_ERR_SOURCE_READ_FAILED;
    case avbase::ErrorCode::kDecoderNotFound:
      return AVBASE_ERR_DECODER_NOT_FOUND;
    case avbase::ErrorCode::kDecoderOpenFailed:
      return AVBASE_ERR_DECODER_OPEN_FAILED;
    case avbase::ErrorCode::kDecodeFailed:
      return AVBASE_ERR_DECODE_FAILED;
    case avbase::ErrorCode::kDecoderUnsupportedCodec:
      return AVBASE_ERR_DECODER_UNSUPPORTED_CODEC;
    case avbase::ErrorCode::kSinkNotAttached:
      return AVBASE_ERR_SINK_NOT_ATTACHED;
    case avbase::ErrorCode::kStreamNotFound:
      return AVBASE_ERR_STREAM_NOT_FOUND;
    case avbase::ErrorCode::kTrackNotFound:
      return AVBASE_ERR_TRACK_NOT_FOUND;
    case avbase::ErrorCode::kConfigInvalid:
      return AVBASE_ERR_CONFIG_INVALID;
    case avbase::ErrorCode::kEos:
    case avbase::ErrorCode::kSourceEos:
      return AVBASE_ERR_EOS;
    default:
      // Codes the ABI does not name yet still have to produce something a C
      // host can branch on, and "failed" is the only honest answer.
      return AVBASE_ERR_FAILED;
  }
}

AvbaseStatus ToC(const avbase::Status& status) {
  // Status is base::expected<void, MediaError>. A success carries no error, so
  // the error() call is guarded here rather than left to convention.
  if (status.has_value()) {
    return AVBASE_OK;
  }
  return ToC(status.error().code());
}

int64_t ToMs(avbase::base::TimeDelta delta) {
  return delta.InMilliseconds();
}

avbase::media::DemuxerStreamType FromStreamType(int stream_type) {
  switch (stream_type) {
    case AVBASE_STREAM_AUDIO:
      return avbase::media::DemuxerStreamType::kAudio;
    case AVBASE_STREAM_VIDEO:
      return avbase::media::DemuxerStreamType::kVideo;
    case AVBASE_STREAM_TEXT:
      return avbase::media::DemuxerStreamType::kText;
    default:
      return avbase::media::DemuxerStreamType::kUnknown;
  }
}

avbase::media::NativeDisplayKind FromDisplayKind(int kind) {
  switch (kind) {
    case AVBASE_DISPLAY_KIND_X11_WINDOW:
      return avbase::media::NativeDisplayKind::kX11Window;
    case AVBASE_DISPLAY_KIND_WAYLAND_SURFACE:
      return avbase::media::NativeDisplayKind::kWaylandSurface;
    case AVBASE_DISPLAY_KIND_SDL2_WINDOW:
      return avbase::media::NativeDisplayKind::kSdl2Window;
    case AVBASE_DISPLAY_KIND_DRM_MASTER:
      return avbase::media::NativeDisplayKind::kDrmMaster;
    case AVBASE_DISPLAY_KIND_GBM_DEVICE:
      return avbase::media::NativeDisplayKind::kGbmDevice;
    case AVBASE_DISPLAY_KIND_ANDROID_NATIVE_WINDOW:
      return avbase::media::NativeDisplayKind::kAndroidNativeWindow;
    case AVBASE_DISPLAY_KIND_ANDROID_SURFACE:
      return avbase::media::NativeDisplayKind::kAndroidSurface;
    case AVBASE_DISPLAY_KIND_CAMETAL_LAYER:
      return avbase::media::NativeDisplayKind::kCametalLayer;
    case AVBASE_DISPLAY_KIND_CAEAGL_LAYER:
      return avbase::media::NativeDisplayKind::kCaEaglLayer;
    case AVBASE_DISPLAY_KIND_HWND:
      return avbase::media::NativeDisplayKind::kHwnd;
    case AVBASE_DISPLAY_KIND_GLFW_WINDOW:
      return avbase::media::NativeDisplayKind::kGlfwWindow;
    case AVBASE_DISPLAY_KIND_EGL_WINDOW:
      return avbase::media::NativeDisplayKind::kEglWindow;
    default:
      return avbase::media::NativeDisplayKind::kNone;
  }
}

// Copies |text| into the caller's buffer and returns the full length excluding
// the NUL, so a caller can size a buffer and retry. Writing past the
// capacity is
// the classic C ABI crash; truncating silently is the classic C ABI lie. This
// does both halves correctly and never allocates.
int32_t CopyOut(const std::string& text, char* out, int32_t capacity) {
  const int32_t needed = static_cast<int32_t>(text.size());
  if (out == nullptr || capacity <= 0) {
    return needed;
  }
  const int32_t writable = std::min(capacity - 1, needed);
  if (writable > 0) {
    std::memcpy(out, text.data(), static_cast<size_t>(writable));
  }
  out[writable] = '\0';
  return needed;
}

// Holds the C function pointer and its user_data as a pair. A shared_ptr
// because the Player may outlive the C handle during destruction, and a raw
// capture would dangle exactly when a late event can arrive.
struct EventBridge {
  AvbaseEventCallback callback{nullptr};
  void* user_data{nullptr};
};

// None() is a VALID non-rendering target, so validity is not "non-null" --
// but a null pointer here still means the factory refused (bad handle), and
// both are a failure to create.
AvbaseNativeDisplay* WrapDisplay(
    avbase::base::scoped_refptr<avbase::media::NativeDisplay> display) {
  if (display == nullptr || !display->valid()) {
    return nullptr;
  }
  std::unique_ptr<AvbaseNativeDisplay> handle(
      new (std::nothrow) AvbaseNativeDisplay());
  if (handle == nullptr) {
    return nullptr;
  }
  handle->display = std::move(display);
  return handle.release();
}

}  // namespace

#endif  // AVBASE_PLAYER_PUBLIC_C_AVBASE_C_INTERNAL_H_
