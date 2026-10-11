// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_OVERLAY_H_
#define AVBASE_RENDER_GL_OVERLAY_H_

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "render/gl/gl_export.h"

namespace avbase::gl {

// A host-published overlay bitmap (subtitle cue, watermark, ...): tightly
// packed straight-alpha RGBA8, placed in fractions of the drawable size.
//
// The bitmap travels as raw RGBA rather than as text because this module has
// no business knowing what a subtitle is: the host rasterises whatever it
// wants (including text) and publishes the pixels.
struct AVBASE_GL_EXPORT TextOverlay {
  bool valid{false};
  int version{0};  // Bumped on every publish; the sink re-uploads on change.
  int width{0};
  int height{0};
  std::vector<uint8_t> rgba;
  float x{0}, y{0}, w{0}, h{0};  // Fractions of the drawable size.
};

// The host owns the slot and mutates it through Publish()/Clear() from its
// own threads; the renderer snapshots it once per presented frame on the
// render thread. The mutex is inside because the publish rate (per cue) and
// the snapshot rate (per frame) are on different threads by design.
struct AVBASE_GL_EXPORT TextOverlaySlot {
  void Publish(TextOverlay overlay) {
    std::lock_guard<std::mutex> lock(mu_);
    overlay.version = ++version_;
    current_ = std::move(overlay);
    current_.valid = true;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    // Drops the bitmap rather than only flagging it invalid: a consumer that
    // skips the copy while the version is unchanged must not keep paying for a
    // copy of a picture it is no longer allowed to draw.
    current_ = TextOverlay{};
    // Version bumped too. Clearing is a change a reader has to see, and a
    // consumer that early-outs on an unchanged version would otherwise keep
    // drawing an overlay that was just taken off screen.
    current_.version = ++version_;
  }
  // Copies the current overlay (or an invalid one when nothing is published).
  TextOverlay Snapshot() {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }
  // The same, into a buffer the caller keeps. Prefer this on a per-frame
  // path: assigning into an existing TextOverlay reuses its rgba capacity, so
  // the steady state copies without allocating.
  void SnapshotTo(TextOverlay& out) {
    std::lock_guard<std::mutex> lock(mu_);
    out = current_;
  }
  // The version alone, without the bitmap. A renderer asks this first and
  // only pays for a copy when it changed: Snapshot() at 1920x1080 moves ~8 MB
  // and holds the writer's lock while doing it, which is fine once per cue and
  // wasteful once per frame -- a subtitle stays up for hundreds of frames.
  int published_version() {
    std::lock_guard<std::mutex> lock(mu_);
    return version_;
  }

 private:
  std::mutex mu_;
  int version_{0};
  TextOverlay current_;
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_OVERLAY_H_
