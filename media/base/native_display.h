// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_MEDIA_BASE_NATIVE_DISPLAY_H_
#define IJKPP_MEDIA_BASE_NATIVE_DISPLAY_H_

#include <stdint.h>

#include <functional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "media/media_export.h"

namespace ijkpp::media {

// A type-erased window-system handle.
//
// The media layer must never include X11/Wayland/SDL headers, so the handle is
// carried as an opaque pointer plus a kind tag, with a release callback for
// handles ijkpp was asked to own. See docs/09 §3 for the embedding rules that
// govern who owns what.
enum class NativeDisplayKind {
  kNone = 0,
  kX11Window,
  kWaylandSurface,
  kSdl2Window,
  kDrmMaster,
  kGbmDevice,
  kAndroidNativeWindow,
  kAndroidSurface,
  kCametalLayer,
  kCaEaglLayer,
  kHwnd,
};

IJKPP_MEDIA_EXPORT const char* GetNativeDisplayKindName(NativeDisplayKind kind);

struct IJKPP_MEDIA_EXPORT X11WindowHandle {
  void* display{nullptr};      // X11 `Display*`, caller-owned unless noted.
  // X11's `Window` is an XID, i.e. `unsigned long` -- pointer-width on every
  // platform X11 runs on. Declaring it uintptr_t rather than uint64_t keeps the
  // void* round-trip in FromX11Window() cast-free (and -Wuseless-cast quiet on
  // LP64, where the two are the same type).
  uintptr_t window{0};
  bool ijkpp_owns_display{false};
};

struct IJKPP_MEDIA_EXPORT WaylandSurfaceHandle {
  void* display{nullptr};      // `wl_display*`.
  void* surface{nullptr};      // `wl_surface*`.
  bool ijkpp_owns_display{false};
  // Wayland cannot report a surface size, so the embedder must supply one.
  int32_t width{0};
  int32_t height{0};
};

class IJKPP_MEDIA_EXPORT NativeDisplay
    : public base::RefCountedThreadSafe<NativeDisplay> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  static base::scoped_refptr<NativeDisplay> FromX11Window(X11WindowHandle handle);
  static base::scoped_refptr<NativeDisplay> FromWaylandSurface(
      WaylandSurfaceHandle handle);
  static base::scoped_refptr<NativeDisplay> FromSdl2Window(void* sdl_window);
  static base::scoped_refptr<NativeDisplay> FromDrmMaster(int drm_fd,
                                                          void* gbm_device);
  // Escape hatch for embedders with their own window abstraction. |release| is
  // run on the sink's render sequence when the last reference goes away.
  static base::scoped_refptr<NativeDisplay> Wrap(
      void* raw, NativeDisplayKind kind, base::OnceCallback<void(void*)> release);
  // A valid "render nowhere" target; the sink enters discard mode.
  static base::scoped_refptr<NativeDisplay> None();

  NativeDisplayKind kind() const { return kind_; }
  bool valid() const { return kind_ != NativeDisplayKind::kNone; }

  // Returns the payload when |kind| matches, otherwise nullptr.
  const X11WindowHandle* x11() const;
  const WaylandSurfaceHandle* wayland() const;
  void* raw() const { return raw_; }
  int drm_fd() const { return drm_fd_; }

  std::string AsDebugString() const;

 private:
  friend class base::RefCountedThreadSafe<NativeDisplay>;
  ~NativeDisplay();
  NativeDisplay() = default;
  explicit NativeDisplay(NativeDisplayKind kind) : kind_(kind) {}

  NativeDisplayKind kind_{NativeDisplayKind::kNone};
  void* raw_{nullptr};
  int drm_fd_{-1};
  X11WindowHandle x11_;
  WaylandSurfaceHandle wayland_;
  base::OnceCallback<void(void*)> release_;
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_NATIVE_DISPLAY_H_
