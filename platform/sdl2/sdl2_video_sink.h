// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_SDL2_VIDEO_SINK_H_
#define AVBASE_PLATFORM_SDL2_SDL2_VIDEO_SINK_H_

#include "media/base/video_renderer_sink.h"
#include "media/gl/gl_video_sink.h"
#include "media/media_export.h"

namespace avbase::media {

// The SDL2 video output endpoint: an Sdl2OutputTarget behind the shared
// GlVideoSink loop.
//
// WHY THIS CLASS IS TEN LINES AND NOT FIVE HUNDRED. It used to carry its own
// copy of the render loop -- Start/Stop/Pause/Play/Flush/SetOutputTarget/
// PresentOne, byte-identical apart from the one question "is this frame going
// through GL or through SDL_Renderer". Two copies of a loop is two copies of
// its teardown ordering and of its cadence, and both of those had already been
// pulled out of here once (NextFrameDelay(), PresentStats) precisely because
// they were written twice. What was left duplicated was the loop itself, so
// the question moved into the target and the loop stopped being this class's.
//
// It still exists as a name because hosts ask for it by name: the factory
// below, and the examples that name the sink type to reach SetSceneUpdate().
class AVBASE_MEDIA_EXPORT Sdl2VideoSink final : public GlVideoSink {
 public:
  explicit Sdl2VideoSink(base::scoped_refptr<NativeDisplay> display);
  Sdl2VideoSink(const Sdl2VideoSink&) = delete;
  Sdl2VideoSink& operator=(const Sdl2VideoSink&) = delete;
  ~Sdl2VideoSink() override = default;
};

class AVBASE_MEDIA_EXPORT Sdl2VideoSinkFactory final
    : public VideoRendererSinkFactory {
 public:
  std::unique_ptr<VideoRendererSink>
  Create(base::scoped_refptr<NativeDisplay> display) override;
  const char* name() const override { return "sdl2"; }
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_SDL2_SDL2_VIDEO_SINK_H_
