// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_OVERLAY_H_
#define AVBASE_RENDER_GL_OVERLAY_H_

#include <cstdint>
#include <mutex>
#include <vector>

// A host-published overlay bitmap (subtitle cue, watermark, ...): tightly
// packed straight-alpha RGBA8, placed in fractions of the drawable size.
//
// Platform-neutral: lives in the shared GL layer because both the GL presenter
// and every backend's surface carry it. Moved out of platform/sdl2/surface.h
// so the GL path no longer depends on the SDL2-only header.
struct TextOverlay {
  bool valid{false};
  int version{0};  // Bumped on every publish; the sink re-uploads on change.
  int width{0};
  int height{0};
  std::vector<uint8_t> rgba;
  float x{0}, y{0}, w{0}, h{0};  // Fractions of the drawable size.
};

// The host owns the slot and mutates it through Publish()/Clear() from its
// own threads; the sink snapshots it once per presented frame on the render
// thread. The mutex is inside because the publish rate (per cue) and the
// snapshot rate (per frame) are on different threads by design.
struct TextOverlaySlot {
  void Publish(TextOverlay overlay) {
    std::lock_guard<std::mutex> lock(mu_);
    overlay.version = ++version_;
    current_ = std::move(overlay);
    current_.valid = true;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    current_.valid = false;
  }
  // Copies the current overlay (or an invalid one when nothing is published).
  TextOverlay Snapshot() {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }

 private:
  std::mutex mu_;
  int version_{0};
  TextOverlay current_;
};

#endif  // AVBASE_RENDER_GL_OVERLAY_H_
