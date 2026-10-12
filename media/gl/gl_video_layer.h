// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_VIDEO_LAYER_H_
#define AVBASE_MEDIA_GL_GL_VIDEO_LAYER_H_

#include <array>
#include <memory>

#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_hw_texture_importer.h"
#include "media/media_export.h"
#include "render/gl/gl_api.h"
#include "render/gl/gl_program.h"
#include "render/gl/gl_texture.h"

namespace avbase::media {

// The YUV->RGB fragment stage a GlVideoLayer is drawn with -- ONE body, for
// both texture targets, parameterised by three macros (SAMPLER_TYPE,
// LUMA_COORD, CHROMA_COORD) that the caller #define's in front of it; see the
// two define blocks below.
//
// A *body*, with no #version line: concatenate it after a vertex stage, with
// gl::ShaderPrologue(gl) in front of both, the way gl_shaders.h documents.
//
// WHY THE SAMPLER TYPE IS A MACRO AND NOT A SECOND BODY. GLSL has no sampler
// type that reads both kinds: a sampler2D on a rectangle texture is undefined
// and draws nothing. The two kinds are both needed because the one zero-copy
// route macOS offers -- an IOSurface imported with CGLTexImageIOSurface2D --
// accepts GL_TEXTURE_RECTANGLE and rejects GL_TEXTURE_2D outright (measured;
// see platform/hwimport). Carrying that as two copies of this text meant the
// NV12 branch, the matrix, the offset and the opacity were each written twice
// and had to be changed twice; the only real difference is the sampler type
// and, because a rectangle texture is addressed in texels, how a coordinate is
// formed.
//
// Prefer CreateVideoLayerProgram() below over assembling it by hand: the
// vertex stage and this one carry a uniform contract (uRect / uTexY..V /
// uMatrix / uOffset / uOpacity) that only matches as a pair.
AVBASE_MEDIA_EXPORT extern const char* const kYuvFragmentShaderBody;

// Ordinary textures: a coordinate is already normalised, so it is used as it
// arrives. uTexSize / uChromaSize are declared by the body but unused here, and
// the linker drops them -- which is why GlVideoLayer::Bind() only bothers
// setting them for the rectangle case.
AVBASE_MEDIA_EXPORT extern const char* const kYuvSampler2DDefines;

// Rectangle textures: addressed in TEXELS, so a coordinate has to be scaled by
// the plane's own size. There are two because the chroma plane is half the
// luma one and a rect sampler cannot derive that the way a normalised [0,1]
// coordinate implicitly does.
AVBASE_MEDIA_EXPORT extern const char* const kYuvSamplerRectDefines;

// Builds the program a GlVideoLayer is drawn with: the *rect* vertex stage,
// so a layer can be placed anywhere in the target rather than only filling it,
// plus kYuvFragmentShaderBody. Null on failure, with |error| filled in.
//
// One definition of "how a video layer is drawn", shared by the single-layer
// presenter and a multi-layer scene -- which is the point: the presenter's
// one layer and a scene's first layer are then the same code, not two things
// that have to be kept in agreement.
AVBASE_MEDIA_EXPORT std::unique_ptr<gl::GlProgram>
CreateVideoLayerProgram(const gl::GlApi& gl, std::string* error);

// The same, over kYuvSamplerRectDefines: the program for a layer whose
// texture_target() is GL_TEXTURE_RECTANGLE (an imported GPU frame). Same body,
// so it shares the vertex stage and every uniform name with the one above
// except the two plane sizes, and a caller's placement code does not change
// with the target.
AVBASE_MEDIA_EXPORT std::unique_ptr<gl::GlProgram>
CreateVideoLayerRectProgram(const gl::GlApi& gl, std::string* error);

// The three planar layouts a GlVideoLayer can upload. Stated once, so the
// format gate cannot mean one thing where the sink decides what it accepts and
// another where the upload happens.
AVBASE_MEDIA_EXPORT bool IsPlanarYuv(VideoFormat format);

// Turns a rect in NORMALISED target coordinates -- (0,0) top-left, (1,1)
// bottom-right, |w|/|h| as fractions of the target -- into the uRect the rect
// vertex stage takes: xy is the centre in NDC and zw the half-extent, so a
// full target is (0, 0, 1, 1).
//
// This is NOT the spelling gl::TextOverlay uses (there x/y are already NDC
// offsets and w/h are fractions of the half-extent), and the difference is
// worth a function rather than a comment at each call site: the two agree at
// the full-target case and diverge everywhere else.
AVBASE_MEDIA_EXPORT void NormalisedRectToUvRect(float x, float y, float w,
                                                float h, float* out_rect);

// (cos, sin) of a CLOCKWISE turn of |degrees| about a layer's own centre, for
// the uRotation uniform. Four values are possible because a container's display
// rotation is a quarter-turn count and nothing downstream can honour an
// arbitrary angle -- so the input is snapped rather than rejected and then
// silently mishandled.
//
// A table and not std::cos/std::sin: the four answers a caller can reach are
// exactly representable, and the library's are not. Computing cos(90 degrees)
// yields ~6e-17, which is a sub-pixel sampling offset -- invisible in a still
// and wrong in a rotation test that compares two orientations of one picture.
AVBASE_MEDIA_EXPORT void LayerRotationToCosSin(int degrees, float* out_cos_sin);

// Snaps |degrees| to the nearest quarter turn and normalises it into 0, 90,
// 180, 270. Exported because both the setter that records an angle and the
// shader-facing conversion need the same answer, and "what did that 89 become"
// is a question with exactly one right reply.
AVBASE_MEDIA_EXPORT int SnapLayerRotation(int degrees);

// One video source: three plane textures plus the colour conversion that turns
// them back into RGB.
//
// The split out of GlPresenter is what makes a multi-source scene possible.
// The presenter owns the composition surface and the window; a layer owns one
// video. A scene is a list of these, and the single-video path is the list of
// one -- deliberately the same code, so the case that is exercised is the case
// that ships.
//
// What lives here: which plane goes into which texture, the NV12 / YV12 / I420
// differences, the stride handling, and the colour matrix. What does not: the
// GL context (the caller makes it current), the program (shared, see above),
// the quad, and the render target.
//
// THREADING: not thread-safe. Every method runs on whichever thread has the GL
// context current -- the sink's render thread.
class AVBASE_MEDIA_EXPORT GlVideoLayer {
 public:
  static std::unique_ptr<GlVideoLayer> Create(const gl::GlApi& gl);
  GlVideoLayer(const GlVideoLayer&) = delete;
  GlVideoLayer& operator=(const GlVideoLayer&) = delete;
  ~GlVideoLayer();

  // Uploads |frame| into this layer's textures, (re)allocating them when the
  // coded size or the format moved. Returns false for a format the shader
  // cannot read, so the caller reports a submit failure instead of drawing
  // whatever the old textures happened to hold.
  //
  // THREE WAYS TO GET PIXELS, ONE ENTRY POINT. An owned CPU frame takes the
  // copy route: the plane bytes are uploaded with glTexSubImage2D. A
  // GPU-resident frame takes the import route, where |importer| points the
  // textures at the decoded surface and nothing is copied at all. A GPU frame
  // the import route will not or cannot take falls back to a readback and then
  // the copy route.
  //
  // Choosing between them is a property of the FRAME, not of the caller, so
  // the branch lives here rather than at every call site. So does the fallback,
  // which is the point of it: a frame is either displayed or it is not, and
  // "read it back or report a failure" is one decision that used to be made in
  // two sinks (once before the present, once again if the import was refused
  // during it) with a sticky flag in each. One answer means one place to be
  // wrong.
  //
  // Returns false only when there is genuinely nothing to draw: a format no
  // route can read, or a GPU frame with no readback path installed. The caller
  // reports that as a submit failure.
  //
  // |importer| is borrowed and may be null; a null importer means this host has
  // no interop backend, and a GPU frame then takes the readback route.
  bool Upload(const VideoFrame& frame, GlHwTextureImporter* importer = nullptr);

  // True once the import route has been refused at draw time by the backend
  // now installed -- it accepted the frame but would not point a texture at
  // its surface. Sticky, because a driver that declines this API declines it
  // for every frame after the first: asking again would pay a failed import
  // per frame to arrive at the same picture.
  //
  // Keyed to the BACKEND rather than being a bare flag, so a different one gets
  // a fresh chance without anybody having to remember to clear anything. The
  // sinks used to own this state as a bool and reset it in
  // SetHwTextureImporter(); a flag somebody has to reset is a flag somebody
  // forgets to reset.
  bool import_refused() const { return refused_importer_ != nullptr; }

  // Which route the last successful Upload() took. The presenter copies this
  // into PresentTiming, which is what makes "why is upload slow" answerable:
  // only the layer knows whether those microseconds were a copy or a re-point.
  UploadPath last_upload_path() const { return last_upload_path_; }

  // The GL target this layer's plane textures live on: GL_TEXTURE_2D after a
  // CPU upload, GL_TEXTURE_RECTANGLE after a hardware import. The caller uses
  // it for exactly one decision -- which of the two fragment programs above to
  // draw with -- because that is the only thing about an imported frame that
  // is visible from outside this class. Nothing else about a layer changes
  // with the target: the colour uniforms, the NV12 flag and the opacity are
  // identical, and Bind() sets them the same way either way.
  unsigned int texture_target() const;

  // Points |program|'s samplers at this layer's planes and sets this layer's
  // colour uniforms, NV12 flag and opacity. |program| must come from
  // CreateVideoLayerProgram() for a GL_TEXTURE_2D layer and from
  // CreateVideoLayerRectProgram() for a GL_TEXTURE_RECTANGLE one -- the two
  // differ in sampler type, which is exactly the distinction texture_target()
  // exists to let the caller make. Must already be current.
  //
  // uRect is deliberately NOT set here: placement is what differs between
  // layers, so the caller that decided where this layer goes is the one that
  // says so. A presenter drawing one full-target layer passes (0,0,1,1).
  void Bind(gl::GlProgram& program, float opacity = 1.0f) const;

  // True once a frame has been uploaded. Bind() on a layer that has none would
  // sample uninitialised textures, so callers check this rather than assuming.
  bool ready() const { return ready_; }

  // Coded size of the last uploaded frame; zero before the first upload and
  // after a failed one.
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  explicit GlVideoLayer(const gl::GlApi& gl);

  // The copy route: plane bytes from CPU memory into the plane textures.
  bool UploadCpuPlanes(const VideoFrame& frame);
  // The import route: the plane textures are pointed at the frame's own
  // graphics surface. The two differ in where the pixels come from and in
  // nothing else -- the texture set, the sampler parameters, the NV12 flag and
  // the colour matrix are established the same way, so the shader and Bind()
  // cannot tell which one ran.
  //
  // Precondition: importer.CanImport(frame) already answered yes. The question
  // is asked once, by Upload(), so that "the backend accepted it" is a fact
  // Upload() can tell apart from "the driver refused it" -- which is exactly
  // the distinction sticky import_refused_ rests on.
  bool ImportGpuPlanes(const VideoFrame& frame, GlHwTextureImporter& importer);
  // True when the plane textures do not yet describe |w| x |h| in |format|,
  // i.e. when they have to be (re)created and their sampler state (re)set.
  // Kept as one predicate because both routes ask exactly this question and a
  // second copy of the comparison is how the two drift apart.
  bool NeedsTextureSetup(int w, int h, VideoFormat format) const;
  // Makes sure the three plane textures exist on |target|, recreating them
  // when they were created on a different one. The target is part of a
  // texture's identity -- a name made for GL_TEXTURE_2D cannot be bound or
  // parameterised through GL_TEXTURE_RECTANGLE -- so a layer that switches
  // between an owned frame and an imported one has to switch texture sets with
  // it. Recreating rather than keeping both keeps a layer's GL memory
  // proportional to the route it is actually taking.
  bool EnsurePlaneTextures(unsigned int target);
  void ApplySamplers();
  // Resolves the frame's colour tags into uMatrix / uOffset. Both routes call
  // it, because a GPU frame is no less colour-tagged than a CPU one.
  void SetColourUniforms(const VideoFrame& frame);

  const gl::GlApi* gl_;
  std::unique_ptr<gl::GlTexture> planes_[3];

  bool ready_{false};
  int width_{0};
  int height_{0};
  int format_{0};  // VideoFormat as int; 0 = kUnknown, i.e. nothing uploaded.
  bool nv12_{false};
  UploadPath last_upload_path_{UploadPath::kNone};
  // See import_refused(). Per layer rather than per sink: the refusal is the
  // driver's answer about a surface, and each layer holds its own. Borrowed,
  // like every importer pointer in this tree.
  const GlHwTextureImporter* refused_importer_{nullptr};

  // The conversion for the last upload, resolved then rather than in Bind() so
  // the layer does not have to hold the frame alive. Column-major, because
  // that is what glUniformMatrix3fv wants.
  std::array<float, 9> matrix_{};
  std::array<float, 3> offset_{};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_VIDEO_LAYER_H_
