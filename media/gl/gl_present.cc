// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_present.h"

#include <string>
#include <utility>

#include "base/logging.h"
#include "base/time/time.h"
#include "render/gl/gl_shaders.h"

// The video-domain half of the GL path. Entry-point loading, shader
// compilation, textures and the quad come from avbase_gl; what is left here is
// the part that is genuinely about video: which plane goes in which texture,
// what the colour matrix is, and how the overlay composites.

namespace avbase::media {
namespace {

// One frame can be fed by several layers, and with an importer installed they
// need not all take the same route. A single field then has to mean something
// more precise than "the last one", and the useful claim is the weakest one:
// a copy ANYWHERE means the frame was not zero-copy, so kCpuSync wins. The
// other direction would let a scene with one imported layer and one copied
// layer report kHwZeroCopy, which is exactly the claim this field exists to
// keep honest.
void MergeUploadPath(PresentTiming* timing, UploadPath path) {
  if (!timing || path == UploadPath::kNone) {
    return;
  }
  if (timing->upload_path == UploadPath::kNone ||
      path == UploadPath::kCpuSync) {
    timing->upload_path = path;
  }
}

}  // namespace

GlPresenter::GlPresenter(std::unique_ptr<gl::NativeGLContext> ctx)
    : gl_ctx_(std::move(ctx)) {}

GlPresenter::~GlPresenter() {
  Shutdown();
}

bool GlPresenter::Init() {
  if (gl_current_) {
    return true;
  }
  if (!gl_ctx_->MakeCurrent()) {
    LOG(ERROR) << "gl present: MakeCurrent failed: " << gl_ctx_->GetError();
    return false;
  }
  gl_current_ = true;

  api_ = std::make_unique<gl::GlApi>();
  std::string error;
  if (!api_->Load(gl_ctx_.get(), &error)) {
    LOG(ERROR) << "gl present: " << error;
    return false;
  }
  LOG(INFO) << "gl present: " << api_->renderer() << " / "
            << api_->version_string() << " (GLSL "
            << api_->shading_language_version() << ")";

  quad_ = gl::GlQuad::Create(*api_);
  if (!quad_) {
    LOG(ERROR) << "gl present: quad creation failed";
    return false;
  }
  layer_ = GlVideoLayer::Create(*api_);
  if (!layer_) {
    LOG(ERROR) << "gl present: video layer creation failed";
    return false;
  }
  canvas_ = gl::GlFramebuffer::Create(*api_);
  if (!canvas_) {
    LOG(ERROR) << "gl present: canvas allocation failed";
    return false;
  }
  std::string blit_error;
  blitter_ = gl::GlBlitter::Create(*api_, &blit_error);
  if (!blitter_) {
    LOG(ERROR) << "gl present: " << blit_error;
    return false;
  }
  // The two cross-fade sides and their mixer. Built here with everything else
  // that compiles a shader, so a failure to compile is reported at Init()
  // rather than in the middle of the first transition, when the picture is
  // already supposed to be fading.
  transition_from_ = gl::GlFramebuffer::Create(*api_);
  transition_to_ = gl::GlFramebuffer::Create(*api_);
  std::string transition_error;
  transition_ = gl::GlTransition::Create(*api_, &transition_error);
  if (!transition_from_ || !transition_to_ || !transition_) {
    LOG(ERROR) << "gl present: " << transition_error;
    return false;
  }
  return BuildPrograms();
}

bool GlPresenter::BuildPrograms() {
  const std::string prologue = gl::ShaderPrologue(*api_);

  std::string error;
  program_ = CreateVideoLayerProgram(*api_, &error);
  if (!program_) {
    LOG(ERROR) << "gl present: video program: " << error;
    return false;
  }
  // The rectangle-texture variant, for a frame the host imported rather than
  // copied. Compiled here so a driver that refuses sampler2DRect says so at
  // Init() -- the alternative is a picture that is simply black the first time
  // a hardware frame arrives.
  rect_program_ = CreateVideoLayerRectProgram(*api_, &error);
  if (!rect_program_) {
    LOG(ERROR) << "gl present: video rect program: " << error;
    return false;
  }
  // One source filling the whole canvas. The rect stage is what a scene uses
  // to place several layers; here it is the identity case, which is what keeps
  // the single-video path and a scene's first layer on the same shader -- and
  // why changing how a layer is drawn cannot fix one and break the other.
  // uRect is a uniform of both programs (they share the vertex stage), so both
  // have to be told, and both are told the same value. It is set ONCE, here:
  // nothing in this path ever places the video anywhere but the full canvas,
  // so the per-frame SetVec4 this used to carry was re-stating a constant.
  //
  // The sampler units are deliberately NOT set here. CreateVideoLayerProgram /
  // CreateVideoLayerRectProgram set them, and those are the same functions a
  // GlScene builds its own programs with -- so the binding contract between
  // "units 0/1/2" and GlVideoLayer::Bind is written once, in one file, instead
  // of once per program owner.
  for (gl::GlProgram* program : {program_.get(), rect_program_.get()}) {
    program->Use();
    program->SetVec4("uRect", 0.0f, 0.0f, 1.0f, 1.0f);
  }

  overlay_program_ = gl::GlProgram::Create(
      *api_, (prologue + gl::kQuadVertexShaderWithRectBody).c_str(),
      (prologue + gl::kTextureFragmentShaderBody).c_str(), &error);
  if (!overlay_program_) {
    LOG(ERROR) << "gl present: overlay program: " << error;
    return false;
  }
  overlay_program_->Use();
  overlay_program_->SetInt("uTex", 0);
  return true;
}

// (Re)creates the overlay RGBA texture and records which slot version it
// holds; Present re-uploads whenever the slot publishes a new version.
bool GlPresenter::EnsureOverlayTexture(const gl::TextOverlay& overlay) {
  if (overlay_texture_ && overlay_version_ == overlay.version &&
      overlay_width_ == overlay.width && overlay_height_ == overlay.height) {
    return true;
  }
  if (!overlay_texture_) {
    overlay_texture_ = gl::GlTexture::Create(*api_);
    if (!overlay_texture_) {
      return false;
    }
  }
  overlay_texture_->SetImage2D(GL_RGBA8, overlay.width, overlay.height, GL_RGBA,
                               GL_UNSIGNED_BYTE, overlay.rgba.data());
  overlay_texture_->SetLinearClamp();
  overlay_version_ = overlay.version;
  overlay_width_ = overlay.width;
  overlay_height_ = overlay.height;
  return true;
}

void GlPresenter::DrawOverlay() {
  if (!overlay_slot_) {
    return;
  }
  // WHY THE VERSION IS READ FIRST. SnapshotTo() copies the entire RGBA block
  // and holds the writer's lock while doing it; at 1920x1080 that is ~8 MB.
  // The common case is that nothing was published since the last frame -- a
  // subtitle cue sits on screen for seconds, i.e. hundreds of frames -- and
  // every one of those frames used to pay for a copy it did not need.
  const int version = overlay_slot_->published_version();
  if (version != snapshot_version_) {
    overlay_slot_->SnapshotTo(overlay_);
    snapshot_version_ = version;
  }
  const gl::TextOverlay& overlay = overlay_;
  if (!overlay.valid ||
      overlay.rgba.size() != static_cast<size_t>(overlay.width) *
                                 static_cast<size_t>(overlay.height) * 4u) {
    return;
  }
  if (!EnsureOverlayTexture(overlay)) {
    return;
  }
  api_->context().Enable(GL_BLEND);
  api_->context().BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  overlay_program_->Use();
  overlay_program_->SetVec4("uRect", overlay.x, overlay.y, overlay.w,
                            overlay.h);
  overlay_texture_->Bind(0);
  quad_->Draw();
  api_->context().Disable(GL_BLEND);
}

void GlPresenter::SetCanvasSize(int width, int height) {
  canvas_width_ = width;
  canvas_height_ = height;
}

bool GlPresenter::EnsureCanvas() {
  // A non-positive request means "follow the drawable", re-read every frame:
  // that is what keeps a window resize working exactly as it did before the
  // canvas existed. When the size is unchanged, Ensure() is a no-op.
  int width = canvas_width_;
  int height = canvas_height_;
  if (width <= 0 || height <= 0) {
    gl_ctx_->GetDrawableSize(&width, &height);
  }
  std::string error;
  if (!canvas_->Ensure(width, height, &error)) {
    LOG(ERROR) << "gl present: " << error;
    return false;
  }
  return true;
}

bool GlPresenter::Compose(const VideoFrame& frame, PresentTiming* timing) {
  if (!gl_current_) {
    return false;
  }
  if (!IsPlanarYuv(frame.format())) {
    return false;
  }
  const base::TimeTicks t_begin =
      timing ? base::TimeTicks::Now() : base::TimeTicks();
  if (!EnsureCanvas()) {
    return false;
  }

  if (!layer_->Upload(frame, hw_importer_)) {
    return false;
  }
  base::TimeTicks t_uploaded;
  if (timing) {
    t_uploaded = base::TimeTicks::Now();
    timing->upload_ms = (t_uploaded - t_begin).InMillisecondsF();
    timing->upload_path = layer_->last_upload_path();
  }

  // Bind() sets the viewport to the canvas size along with the framebuffer, so
  // the geometry below is expressed in the composition's own coordinates
  // rather than the window's.
  canvas_->Bind();
  api_->context().ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  api_->context().Clear(GL_COLOR_BUFFER_BIT);

  // Which of the two programs draws this frame follows from the route the
  // upload just took, not from a mode: a hardware import leaves the layer on
  // rectangle textures, and only the rect program can read those.
  gl::GlProgram* video = layer_->texture_target() == GL_TEXTURE_RECTANGLE
                             ? rect_program_.get()
                             : program_.get();
  video->Use();
  // No SetVec4("uRect") here: both programs were told the identity rect in
  // BuildPrograms() and this path never places the layer anywhere else, so a
  // per-frame write would be re-stating a constant 60 times a second.
  layer_->Bind(*video);
  quad_->Draw();
  DrawOverlay();
  if (timing) {
    timing->compose_ms =
        (base::TimeTicks::Now() - t_uploaded).InMillisecondsF();
  }
  return true;
}

GlScene* GlPresenter::EnsureScene() {
  if (scene_) {
    return scene_.get();
  }
  if (!api_) {
    return nullptr;
  }
  std::string error;
  scene_ = GlScene::Create(*api_, &error);
  if (!scene_) {
    LOG(ERROR) << "gl present: " << error;
  }
  return scene_.get();
}

bool GlPresenter::UploadLayer(GlScene& scene, GlScene::LayerId id,
                              const VideoFrame& frame, PresentTiming* timing) {
  const base::TimeTicks t_begin =
      timing ? base::TimeTicks::Now() : base::TimeTicks();
  if (!scene.Upload(id, frame, hw_importer_)) {
    return false;
  }
  if (timing) {
    timing->upload_ms += (base::TimeTicks::Now() - t_begin).InMillisecondsF();
    // The layer reports its own route; a layer that skipped the upload (already
    // removed, or a frame it refuses) leaves the path untouched rather than
    // claiming one.
    MergeUploadPath(timing, scene.last_upload_path(id));
  }
  return true;
}

bool GlPresenter::ComposeScene(GlScene& scene, PresentTiming* timing) {
  if (!gl_current_) {
    return false;
  }
  const base::TimeTicks t_begin =
      timing ? base::TimeTicks::Now() : base::TimeTicks();
  if (!EnsureCanvas()) {
    return false;
  }
  // Bind() sets the viewport too, which is what makes the scene's normalised
  // rects land on the composition rather than on the window.
  canvas_->Bind();
  // The scene clears and draws; it is told nothing about the canvas, so the
  // same call composes into an export target just as well.
  scene.Compose();
  DrawOverlay();
  if (timing) {
    timing->compose_ms = (base::TimeTicks::Now() - t_begin).InMillisecondsF();
  }
  return true;
}

bool GlPresenter::PresentScene(GlScene& scene, PresentTiming* timing) {
  if (!ComposeScene(scene, timing)) {
    return false;
  }
  return Swap(timing);
}

bool GlPresenter::ComposeTransition(GlScene& from, GlScene& to, float progress,
                                    PresentTiming* timing) {
  if (!gl_current_) {
    return false;
  }
  const base::TimeTicks t_begin =
      timing ? base::TimeTicks::Now() : base::TimeTicks();
  if (!EnsureCanvas()) {
    return false;
  }
  std::string error;
  if (!transition_from_->Ensure(canvas_->width(), canvas_->height(), &error) ||
      !transition_to_->Ensure(canvas_->width(), canvas_->height(), &error)) {
    LOG(ERROR) << "gl present: transition target: " << error;
    return false;
  }

  transition_from_->Bind();
  from.Compose();
  transition_to_->Bind();
  to.Compose();

  // Back to the canvas for the mix. Its viewport matches the two sides (see
  // the header on why they are composed at the output size), so the transition
  // is a combination rather than a resample.
  canvas_->Bind();
  transition_->Draw(transition_from_->color_texture(),
                    transition_to_->color_texture(), progress, canvas_->width(),
                    canvas_->height());
  DrawOverlay();
  if (timing) {
    timing->compose_ms = (base::TimeTicks::Now() - t_begin).InMillisecondsF();
  }
  return true;
}

bool GlPresenter::PresentTransition(GlScene& from, GlScene& to, float progress,
                                    PresentTiming* timing) {
  if (!ComposeTransition(from, to, progress, timing)) {
    return false;
  }
  return Swap(timing);
}

void GlPresenter::PresentComposed(PresentTiming* timing) {
  if (!canvas_ || !canvas_->valid()) {
    return;
  }
  const base::TimeTicks t_begin =
      timing ? base::TimeTicks::Now() : base::TimeTicks();

  int drawable_w = 0;
  int drawable_h = 0;
  gl_ctx_->GetDrawableSize(&drawable_w, &drawable_h);
  // Back to the window's framebuffer before the blit binds its own viewport;
  // the blitter would otherwise draw the canvas into the canvas.
  canvas_->Unbind();
  blitter_->Blit(canvas_->color_texture(), drawable_w, drawable_h);
  if (timing) {
    timing->blit_ms = (base::TimeTicks::Now() - t_begin).InMillisecondsF();
  }
}

bool GlPresenter::Swap(PresentTiming* timing) {
  // Each stage probes its own boundaries, so present_ms below measures exactly
  // the swap -- the one stage that can block on vsync. Composing the three
  // into a single number would make a vsync-throttled frame indistinguishable
  // from a slow one.
  PresentComposed(timing);
  if (!timing) {
    gl_ctx_->SwapBuffers();
    return true;
  }
  const base::TimeTicks t_drawn = base::TimeTicks::Now();
  gl_ctx_->SwapBuffers();
  timing->present_ms = (base::TimeTicks::Now() - t_drawn).InMillisecondsF();
  return true;
}

bool GlPresenter::Present(const VideoFrame& frame, PresentTiming* timing) {
  if (!Compose(frame, timing)) {
    return false;
  }
  return Swap(timing);
}

void GlPresenter::Shutdown() {
  if (!api_) {
    return;
  }
  // After the render thread stops, the context is current nowhere; the host
  // still owns it, so re-activate to delete cleanly and release again.
  gl_ctx_->MakeCurrent();
  transition_.reset();
  transition_to_.reset();
  transition_from_.reset();
  blitter_.reset();
  canvas_.reset();
  scene_.reset();
  overlay_texture_.reset();
  // The cached snapshot is a host-sized bitmap; a stopped presenter has no
  // further use for it, and holding it would pin memory the host already
  // replaced or cleared.
  overlay_ = gl::TextOverlay{};
  snapshot_version_ = -1;
  layer_.reset();
  quad_.reset();
  overlay_program_.reset();
  rect_program_.reset();
  program_.reset();
  api_.reset();
  gl_ctx_->ReleaseCurrent();
  gl_current_ = false;
}

}  // namespace avbase::media
