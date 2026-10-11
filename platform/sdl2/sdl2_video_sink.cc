// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/sdl2_video_sink.h"

#include <memory>
#include <utility>

#include "platform/hwimport/hw_importer_factory.h"
#include "platform/sdl2/sdl2_output_target.h"

namespace avbase::media {

Sdl2VideoSink::Sdl2VideoSink(base::scoped_refptr<NativeDisplay> display)
    : GlVideoSink(std::move(display), NativeDisplayKind::kSdl2Window,
                  "Sdl2VideoSink", std::make_unique<Sdl2OutputTarget>()) {
  // The host's interop backend, installed here so this sink needs no second
  // wiring step beyond the one GlVideoSink exposes: on macOS this is the same
  // CGL/IOSurface importer the GLFW and EGL sinks get, because all three end
  // up on a CGL context.
  SetHwTextureImporter(platform::hwimport::CreateHwTextureImporter());
}

std::unique_ptr<VideoRendererSink>
Sdl2VideoSinkFactory::Create(base::scoped_refptr<NativeDisplay> display) {
  return std::make_unique<Sdl2VideoSink>(std::move(display));
}

}  // namespace avbase::media
