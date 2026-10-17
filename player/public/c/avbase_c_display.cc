// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// C ABI: native display surface -- docs/08 M15.
//
// The window-handle wrappers. Every one of them is a thin forward into
// media::NativeDisplay's factories; see avbase_c.cc's header comment for why
// there is deliberately no C-side logic here.

#include <utility>

#include "player/public/c/avbase_c_internal.h"

// Native display
// ---------------------------------------------------------------------------

extern "C" AvbaseNativeDisplay* avbase_display_from_x11(void* x_display,
                                                        uintptr_t window) {
  if (x_display == nullptr || window == 0) {
    return nullptr;
  }
  avbase::media::X11WindowHandle handle;
  handle.display = x_display;
  handle.window = window;
  // The window must outlive the display handle: avbase does not own an X
  // Display it did not open itself.
  handle.avbase_owns_display = false;
  return WrapDisplay(avbase::media::NativeDisplay::FromX11Window(handle));
}

extern "C" AvbaseNativeDisplay* avbase_display_from_wayland(void* wl_display,
                                                            void* wl_surface,
                                                            int32_t width,
                                                            int32_t height) {
  if (wl_display == nullptr || wl_surface == nullptr) {
    return nullptr;
  }
  avbase::media::WaylandSurfaceHandle handle;
  handle.display = wl_display;
  handle.surface = wl_surface;
  handle.avbase_owns_display = false;
  // Wayland cannot report a surface size, so the embedder must supply it.
  handle.width = width;
  handle.height = height;
  return WrapDisplay(avbase::media::NativeDisplay::FromWaylandSurface(handle));
}

extern "C" AvbaseNativeDisplay* avbase_display_from_sdl2(void* sdl_window) {
  if (sdl_window == nullptr) {
    return nullptr;
  }
  return WrapDisplay(avbase::media::NativeDisplay::FromSdl2Window(sdl_window));
}

extern "C" AvbaseNativeDisplay* avbase_display_from_glfw(void* glfw_window) {
  if (glfw_window == nullptr) {
    return nullptr;
  }
  return WrapDisplay(avbase::media::NativeDisplay::FromGlfwWindow(glfw_window));
}

extern "C" AvbaseNativeDisplay* avbase_display_from_egl(void* egl_surface) {
  if (egl_surface == nullptr) {
    return nullptr;
  }
  return WrapDisplay(avbase::media::NativeDisplay::FromEglSurface(egl_surface));
}

extern "C" AvbaseNativeDisplay* avbase_display_from_hwnd(void* hwnd) {
  if (hwnd == nullptr) {
    return nullptr;
  }
  // Wrap() rather than a FromHwnd(): NativeDisplay has no typed HWND factory,
  // and adding one for the C ABI's convenience would put a Windows concept
  // into the C++ type for no gain -- the kind tag already carries it.
  return WrapDisplay(avbase::media::NativeDisplay::Wrap(
      hwnd, avbase::media::NativeDisplayKind::kHwnd, nullptr));
}

extern "C" AvbaseNativeDisplay*
avbase_display_from_android_native_window(void* window) {
  if (window == nullptr) {
    return nullptr;
  }
  return WrapDisplay(avbase::media::NativeDisplay::Wrap(
      window, avbase::media::NativeDisplayKind::kAndroidNativeWindow, nullptr));
}

extern "C" AvbaseNativeDisplay*
avbase_display_from_android_surface(void* surface) {
  if (surface == nullptr) {
    return nullptr;
  }
  return WrapDisplay(avbase::media::NativeDisplay::Wrap(
      surface, avbase::media::NativeDisplayKind::kAndroidSurface, nullptr));
}

extern "C" AvbaseNativeDisplay* avbase_display_from_cametal_layer(void* layer) {
  if (layer == nullptr) {
    return nullptr;
  }
  return WrapDisplay(avbase::media::NativeDisplay::Wrap(
      layer, avbase::media::NativeDisplayKind::kCametalLayer, nullptr));
}

extern "C" AvbaseNativeDisplay* avbase_display_from_caeagl_layer(void* layer) {
  if (layer == nullptr) {
    return nullptr;
  }
  return WrapDisplay(avbase::media::NativeDisplay::Wrap(
      layer, avbase::media::NativeDisplayKind::kCaEaglLayer, nullptr));
}

extern "C" AvbaseNativeDisplay*
avbase_display_wrap(void* raw, int kind,
                    void (*release)(void* raw, void* user_data),
                    void* user_data) {
  if (raw == nullptr) {
    return nullptr;
  }
  const auto display_kind = FromDisplayKind(kind);
  // An unknown kind would otherwise become a "valid" display that no backend
  // can drive. Refusing it here turns a confusing black screen into an
  // immediate null return.
  if (display_kind == avbase::media::NativeDisplayKind::kNone) {
    return nullptr;
  }
  avbase::base::OnceCallback<void(void*)> release_cb;
  if (release != nullptr) {
    // user_data is CAPTURED and raw stays the parameter, because the callback
    // is invoked as run(raw). Binding user_data positionally would build a
    // void(void*, void*) and no longer match OnceCallback<void(void*)> -- the
    // same signature the NativeDisplay type itself declares.
    release_cb = avbase::base::OnceCallback<void(void*)>(
        [release, user_data](void* wrapped) { release(wrapped, user_data); });
  }
  return WrapDisplay(avbase::media::NativeDisplay::Wrap(raw, display_kind,
                                                        std::move(release_cb)));
}

extern "C" AvbaseNativeDisplay* avbase_display_none(void) {
  std::unique_ptr<AvbaseNativeDisplay> handle(new (std::nothrow)
                                                  AvbaseNativeDisplay());
  if (handle == nullptr) {
    return nullptr;
  }
  handle->display = avbase::media::NativeDisplay::None();
  return handle.release();
}

extern "C" void avbase_display_release(AvbaseNativeDisplay* display) {
  delete display;
}

extern "C" int avbase_display_kind(const AvbaseNativeDisplay* display) {
  if (display == nullptr || display->display == nullptr) {
    return AVBASE_DISPLAY_KIND_NONE;
  }
  return static_cast<int>(display->display->kind());
}

extern "C" int avbase_display_is_valid(const AvbaseNativeDisplay* display) {
  if (display == nullptr || display->display == nullptr) {
    return 0;
  }
  // None() is a VALID target that renders nothing, so validity is the handle's
  // own answer and never "is the pointer null".
  return display->display->valid() ? 1 : 0;
}

extern "C" int32_t
avbase_display_debug_string(const AvbaseNativeDisplay* display, char* out,
                            int32_t capacity) {
  if (display == nullptr || display->display == nullptr) {
    return 0;
  }
  return CopyOut(display->display->AsDebugString(), out, capacity);
}

// ---------------------------------------------------------------------------
