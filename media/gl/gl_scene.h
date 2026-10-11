// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_SCENE_H_
#define AVBASE_MEDIA_GL_GL_SCENE_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_video_layer.h"
#include "media/media_export.h"
#include "render/gl/gl_api.h"
#include "render/gl/gl_program.h"
#include "render/gl/gl_quad.h"

namespace avbase::media {

// A stack of video layers drawn in z order into whatever target is bound.
//
// This is the multi-source composition surface that the single-video presenter
// is the one-layer case of: same program, same layer object, same upload path.
// Building it that way is deliberate -- a scene that had its own copy of the
// drawing would be a second thing to keep in agreement, and the agreement
// would be discovered broken by whichever case nobody exercised.
//
// WHAT IT DOES NOT OWN: the render target and the GL context. Compose() draws
// into whatever is already bound, on the thread that made the context current,
// because the surface belongs to the caller. That split is what lets the same
// scene be composed into an off-screen target that is never displayed (an
// export) and into the canvas that is displayed -- without the scene knowing
// which it is doing.
//
// THREADING: not thread-safe. Every method must run on the thread holding the
// GL context -- in avbase, the sink's render thread.
class AVBASE_MEDIA_EXPORT GlScene {
 public:
  // A layer handle. Handles are never reused, so a handle kept across a
  // RemoveLayer() addresses nothing rather than quietly becoming a different
  // layer -- the failure mode that makes index-as-handle dangerous once layers
  // can be added and removed at runtime.
  using LayerId = uint64_t;
  static constexpr LayerId kNoLayer = 0;

  static std::unique_ptr<GlScene> Create(const gl::GlApi& gl,
                                         std::string* error);
  GlScene(const GlScene&) = delete;
  GlScene& operator=(const GlScene&) = delete;
  ~GlScene();

  // Adds an empty video layer at |z| and returns its handle. Lower z is drawn
  // first, so a higher z ends up on top. Ties are drawn in the order the
  // layers were added. Returns kNoLayer if the layer cannot be created.
  LayerId AddVideoLayer(int z);

  bool RemoveLayer(LayerId id);

  // Reorders an existing layer. Cheap (a field write); the sort happens in
  // Compose, so a caller dragging a layer around does not pay per change.
  bool SetLayerZ(LayerId id, int z);

  // A hidden layer is skipped entirely; it keeps its frame and its place, so
  // hiding and showing again does not re-upload anything.
  bool SetLayerVisible(LayerId id, bool visible);

  // Clamped to [0, 1]. At 1 the layer is opaque and the blend is a no-op --
  // GLScene blends every layer rather than branching, because a branch here
  // would be a second code path for the common case.
  bool SetLayerOpacity(LayerId id, float opacity);

  // Destination rectangle in NORMALISED target coordinates: (0,0) is the
  // top-left of the composition, (1,1) the bottom-right, and w/h are fractions
  // of the target. The default is the full target. Negative or non-finite
  // values are rejected rather than clamped, because a caller that computed a
  // NaN rect has a bug upstream that clamping would hide.
  bool SetLayerRect(LayerId id, float x, float y, float w, float h);

  // Uploads |frame| into a layer, creating or resizing its textures as needed.
  // False for an unknown handle, a format the shader cannot read, or a layer
  // that was already removed.
  //
  // |importer| is passed straight through to GlVideoLayer::Upload, which is
  // where the copy-or-import decision is made from the frame itself; it may be
  // null, and a GPU-resident frame is then rejected rather than drawn.
  bool Upload(LayerId id, const VideoFrame& frame,
              GlHwTextureImporter* importer = nullptr);

  // Which route the last successful Upload() into |id| took, or kNone for an
  // unknown handle or a layer nothing has been uploaded into. Per layer,
  // because only the caller knows which layers fed one displayed frame and
  // therefore how to merge their routes into one answer.
  UploadPath last_upload_path(LayerId id) const;

  // Clears the bound target and draws every visible, uploaded layer in z
  // order. Clearing belongs to the scene rather than the caller: "what an
  // uncovered part of the composition looks like" is a scene-level answer, and
  // leaving it to the caller means a caller that forgets gets whatever the
  // previous frame left behind.
  void Compose();

  // Number of layers, including hidden and not-yet-uploaded ones.
  size_t layer_count() const { return layers_.size(); }

 private:
  struct Layer {
    LayerId id{kNoLayer};
    int z{0};
    bool visible{true};
    float opacity{1.0f};
    // (x, y, w, h) normalised; the full target by default.
    std::array<float, 4> rect{0.0f, 0.0f, 1.0f, 1.0f};
    std::unique_ptr<GlVideoLayer> video;
  };

  GlScene(const gl::GlApi& gl, std::unique_ptr<gl::GlProgram> program,
          std::unique_ptr<gl::GlProgram> rect_program);
  Layer* Find(LayerId id);

  const gl::GlApi* gl_;
  // Two programs, chosen per layer by GlVideoLayer::texture_target(): an owned
  // frame is drawn from GL_TEXTURE_2D textures and an imported one from
  // GL_TEXTURE_RECTANGLE ones, and no single GLSL sampler type reads both.
  // A scene therefore can -- and normally does -- mix them.
  std::unique_ptr<gl::GlProgram> program_;
  std::unique_ptr<gl::GlProgram> rect_program_;
  std::unique_ptr<gl::GlQuad> quad_;
  std::vector<Layer> layers_;
  // Monotonic, never reset: see LayerId.
  LayerId next_id_{1};

  // Scratch for the draw order, kept as a member so Compose does not allocate
  // once per frame on the render thread.
  std::vector<size_t> draw_order_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_SCENE_H_
