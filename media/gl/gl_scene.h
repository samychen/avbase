// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_SCENE_H_
#define AVBASE_MEDIA_GL_GL_SCENE_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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

// How a layer's picture is placed inside its rect.
//
// Two answers rather than one, because "stretch" is a real choice and not a
// bug: a full-bleed background layer wants it. What it cannot be is the only
// answer -- a 4:3 clip in a 16:9 rect then comes out with everything 33%
// too wide, which is a wrong picture the caller never asked for and cannot
// see from any return value.
enum class LayerContentMode {
  // Fills the rect exactly, ignoring the picture's aspect. What every layer
  // did before aspect handling existed; still the default, so nothing moves.
  kStretch = 0,
  // Fits the WHOLE picture inside the rect, centred, leaving background on
  // whichever axis is longer: the letterbox a player puts around a clip whose
  // shape is not the window's.
  kFit,
};

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
//
// BAD NUMBERS: the setters REJECT a non-finite value (NaN, infinity) instead
// of clamping it, and leave the previous value standing.
//
// That is deliberately the opposite of what a per-frame draw does, and the two
// are not in conflict -- they are the same rule applied to two situations:
//
//   * A setter can say no. The caller gets false, the layer keeps the value it
//     had, and a caller that computed a NaN finds out. Reading NaN as 0
//     instead would make a layer vanish for a reason nothing records.
//   * A per-frame draw cannot say no. It returns void, it has no previous
//     value to fall back on, and skipping the draw would leave the target
//     holding whatever it held. Degrading to a defined value is the only
//     outcome that keeps the picture defined -- see GlTransition::Draw.
//
// So "reject" and "degrade" are one policy split by whether the call can
// report failure. What is not allowed is a third answer chosen per call site.
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

  // A scene that draws with programs the caller ALREADY compiled, instead of
  // compiling its own copies.
  //
  // GlPresenter builds exactly these two for its single-video path, so a scene
  // it created with Create() was paying for an identical compile twice per
  // presenter.
  //
  // Sharing is safe because a program's uniforms are program state rather than
  // per-owner state, and every draw path sets the ones it depends on before
  // drawing. That clause is the whole contract, and it is why this is a
  // separate factory instead of a default on Create(): a caller that shares
  // has to mean it. The programs must outlive the scene.
  static std::unique_ptr<GlScene> CreateWithPrograms(
      const gl::GlApi& gl, std::shared_ptr<gl::GlProgram> program,
      std::shared_ptr<gl::GlProgram> rect_program, std::string* error);
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

  // Turns a layer's picture about its own centre, clockwise, in degrees.
  //
  // nullopt -- the default, and what a player wants -- means "follow the
  // frame": the rotation is taken from the last frame uploaded into this layer,
  // which is where a container's display matrix ends up. Passing a value pins
  // the angle and stops it following, which is what a host that is rotating a
  // layer on purpose (a picture-in-picture it wants sideways, say) needs.
  //
  // The angle is SNAPPED to a quarter turn rather than rejected when it is not
  // a multiple of 90, because that is the only set any stage can carry: no
  // sampler or rect here can express a source region at any other angle, so
  // the alternative to snapping is a picture that is quietly not rotated.
  bool SetLayerRotation(LayerId id, std::optional<int> degrees);

  // The part of a layer's OWN picture that is drawn, in normalised SOURCE
  // coordinates over the frame as the decoder stored it: (0,0) is the first row
  // and column, (1,1) the last. The whole picture by default.
  //
  // Read in the STORED frame, so the four numbers always name a region the
  // caller can actually see in the file: the rotation decides only which way
  // that region ends up FACING, never which region it is. Reading them in the
  // turned frame instead would make one rect mean different pixels on a
  // phone-shot clip than on an upright one, and would sweep the box outside
  // the picture whenever the crop was not square.
  //
  // (The vertex stage computes this the other way round -- turn first, crop
  // second -- because it maps SCREEN back to SOURCE, so the step the viewer
  // sees last is the one it has to undo first. Same meaning, reversed text.)
  //
  // Rejected rather than clamped: a rect reaching outside the picture would
  // be sampled with CLAMP_TO_EDGE and smear its edge row and column over the
  // uncovered strip, which is a picture that looks like a bug in the sampler
  // rather than a bad argument.
  bool SetLayerSourceRect(LayerId id, float x, float y, float w, float h);

  // How the picture is fitted into |rect|; see LayerContentMode.
  bool SetLayerContentMode(LayerId id, LayerContentMode mode);

  // The colour the composition is cleared to before the layers are drawn, in
  // [0,1] RGBA -- what an uncovered part of the target looks like. Opaque
  // black by default, which is what the single-video path has always cleared
  // to, so an uncovered part of a scene reads as the composition's own
  // background rather than as a hole.
  //
  // On the scene and not on the layer, because "what is behind everything" is
  // one answer for the whole composition; a per-layer background would need a
  // rule for what happens where two of them overlap.
  bool SetBackgroundColor(float r, float g, float b, float a);

  // The angle a layer is drawn at, in degrees, after the frame's own rotation
  // and any override have been combined. Exposed because "which way is this
  // layer" is a rendering fact the caller cannot otherwise observe, and
  // because a test that wants to assert it should not have to read pixels to
  // find out.
  int LayerRotation(LayerId id) const;

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
  // |dst_width| x |dst_height| is the bound target's size in PIXELS. It has to
  // be passed in because a scene draws into whatever is bound and has no way
  // to ask how big that is -- and fitting a picture into a rect is a
  // relationship between the picture and the PIXEL grid, not the normalised
  // one: a rect covering half the width and half the height of a 1920x1080
  // target is a 16:9 box, not a square. Non-positive on either axis means
  // "no grid to fit to", and every layer then stretches -- which is the only
  // thing normalised coordinates can express on their own.
  void Compose(int dst_width, int dst_height);

  // Number of layers, including hidden and not-yet-uploaded ones.
  size_t layer_count() const { return layers_.size(); }

  // Getters for the three attributes above, so a caller (and a test) can read
  // back what it set without having to sample the composed pixels.
  const std::array<float, 4>& source_rect(LayerId id) const;
  LayerContentMode content_mode(LayerId id) const;
  const std::array<float, 4>& background_color() const { return background_; }

 private:
  struct Layer {
    LayerId id{kNoLayer};
    int z{0};
    bool visible{true};
    float opacity{1.0f};
    // (x, y, w, h) normalised; the full target by default.
    std::array<float, 4> rect{0.0f, 0.0f, 1.0f, 1.0f};
    // The part of the source picture |rect| shows; see SetLayerSourceRect.
    std::array<float, 4> source_rect{0.0f, 0.0f, 1.0f, 1.0f};
    // How the picture is fitted into |rect|; see LayerContentMode.
    LayerContentMode content_mode{LayerContentMode::kStretch};
    // nullopt follows the frame's own rotation; see SetLayerRotation.
    std::optional<int> rotation_override;
    // The rotation the last frame uploaded here carried. It has to be captured
    // during Upload() rather than at draw time: the caller releases the frame
    // as soon as Publish() returns, so by the time Compose() runs there is no
    // frame left to ask.
    int frame_rotation{0};
    std::unique_ptr<GlVideoLayer> video;
  };

  GlScene(const gl::GlApi& gl, std::shared_ptr<gl::GlProgram> program,
          std::shared_ptr<gl::GlProgram> rect_program);
  Layer* Find(LayerId id);
  const Layer* Find(LayerId id) const;
  // The rect a layer is actually drawn into: its own rect when the mode is
  // stretch, or that rect narrowed about its centre to the picture's aspect
  // when the mode is fit. See Compose() on why the pixel grid has to be here.
  std::array<float, 4> FittedRect(const Layer& layer, int dst_width,
                                  int dst_height) const;

  const gl::GlApi* gl_;
  // Two programs, chosen per layer by GlVideoLayer::texture_target(): an owned
  // frame is drawn from GL_TEXTURE_2D textures and an imported one from
  // GL_TEXTURE_RECTANGLE ones, and no single GLSL sampler type reads both.
  // A scene therefore can -- and normally does -- mix them.
  // Shared rather than owned when the scene came from CreateWithPrograms --
  // GlPresenter holds the other reference. shared_ptr and not a borrowed
  // pointer because there is no single owner to point at: the presenter and
  // the scene are built in different places, and either may be the one that
  // compiled them.
  std::shared_ptr<gl::GlProgram> program_;
  std::shared_ptr<gl::GlProgram> rect_program_;
  std::unique_ptr<gl::GlQuad> quad_;
  std::vector<Layer> layers_;
  // What an uncovered part of the composition looks like; see
  // SetBackgroundColor() on why this is one answer for the whole scene.
  std::array<float, 4> background_{0.0f, 0.0f, 0.0f, 1.0f};
  // Monotonic, never reset: see LayerId.
  LayerId next_id_{1};

  // Scratch for the draw order, kept as a member so Compose does not allocate
  // once per frame on the render thread.
  std::vector<size_t> draw_order_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_SCENE_H_
