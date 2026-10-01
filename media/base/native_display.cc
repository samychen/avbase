// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/native_display.h"

namespace avbase::media {

const char* GetNativeDisplayKindName(NativeDisplayKind kind) {
  switch (kind) {
    case NativeDisplayKind::kNone:                return "none";
    case NativeDisplayKind::kX11Window:           return "x11-window";
    case NativeDisplayKind::kWaylandSurface:      return "wayland-surface";
    case NativeDisplayKind::kSdl2Window:          return "sdl2-window";
    case NativeDisplayKind::kDrmMaster:           return "drm-master";
    case NativeDisplayKind::kGbmDevice:           return "gbm-device";
    case NativeDisplayKind::kAndroidNativeWindow: return "android-native-window";
    case NativeDisplayKind::kAndroidSurface:      return "android-surface";
    case NativeDisplayKind::kCametalLayer:        return "cametal-layer";
    case NativeDisplayKind::kCaEaglLayer:         return "caeagl-layer";
    case NativeDisplayKind::kHwnd:                return "hwnd";
  }
  return "invalid";
}

NativeDisplay::~NativeDisplay() {
  if (release_ && raw_) {
    std::move(release_).Run(raw_);
  }
}

// static
base::scoped_refptr<NativeDisplay> NativeDisplay::FromX11Window(
    X11WindowHandle handle) {
  base::scoped_refptr<NativeDisplay> display(
      new NativeDisplay(NativeDisplayKind::kX11Window));
  display->x11_ = handle;
  display->raw_ = reinterpret_cast<void*>(handle.window);
  return display;
}

// static
base::scoped_refptr<NativeDisplay> NativeDisplay::FromWaylandSurface(
    WaylandSurfaceHandle handle) {
  base::scoped_refptr<NativeDisplay> display(
      new NativeDisplay(NativeDisplayKind::kWaylandSurface));
  display->wayland_ = handle;
  display->raw_ = handle.surface;
  return display;
}

// static
base::scoped_refptr<NativeDisplay> NativeDisplay::FromSdl2Window(void* window) {
  base::scoped_refptr<NativeDisplay> display(
      new NativeDisplay(NativeDisplayKind::kSdl2Window));
  display->raw_ = window;
  return display;
}

// static
base::scoped_refptr<NativeDisplay> NativeDisplay::FromDrmMaster(int drm_fd,
                                                               void* gbm) {
  base::scoped_refptr<NativeDisplay> display(
      new NativeDisplay(NativeDisplayKind::kDrmMaster));
  display->drm_fd_ = drm_fd;
  display->raw_ = gbm;
  return display;
}

// static
base::scoped_refptr<NativeDisplay> NativeDisplay::Wrap(
    void* raw, NativeDisplayKind kind, base::OnceCallback<void(void*)> release) {
  base::scoped_refptr<NativeDisplay> display(new NativeDisplay(kind));
  display->raw_ = raw;
  display->release_ = std::move(release);
  return display;
}

// static
base::scoped_refptr<NativeDisplay> NativeDisplay::None() {
  return base::scoped_refptr<NativeDisplay>(new NativeDisplay());
}

const X11WindowHandle* NativeDisplay::x11() const {
  return kind_ == NativeDisplayKind::kX11Window ? &x11_ : nullptr;
}

const WaylandSurfaceHandle* NativeDisplay::wayland() const {
  return kind_ == NativeDisplayKind::kWaylandSurface ? &wayland_ : nullptr;
}

std::string NativeDisplay::AsDebugString() const {
  std::string out = GetNativeDisplayKindName(kind_);
  if (kind_ == NativeDisplayKind::kX11Window) {
    out += " window=0x" + std::to_string(x11_.window);
  } else if (kind_ == NativeDisplayKind::kWaylandSurface) {
    out += " " + std::to_string(wayland_.width) + "x" +
           std::to_string(wayland_.height);
  } else if (raw_) {
    out += " raw=set";
  }
  return out;
}

}  // namespace avbase::media
