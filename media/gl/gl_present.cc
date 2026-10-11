// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_present.h"

#include <string>
#include <utility>

#include "base/logging.h"
#include "render/gl/gl_shaders.h"

// The video-domain half of the GL path. Entry-point loading, shader
// compilation, textures and the quad come from avbase_gl; what is left here is
// the part that is genuinely about video: which plane goes in which texture,
// what the colour matrix is, and how the overlay composites.

namespace avbase::media {
namespace {

// The YUV->RGB conversion. Sampler units are fixed: 0 = Y, 1 = U (or the
// interleaved UV of NV12), 2 = V.
constexpr char kYuvFragmentShaderBody[] = R"(
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexY;
uniform sampler2D uTexU;
uniform sampler2D uTexV;
uniform int uNv12;
uniform mat3 uMatrix;
uniform vec3 uOffset;
void main() {
    float y = texture(uTexY, vTexCoord).r;
    vec2 uv = (uNv12 == 1)
        ? texture(uTexU, vTexCoord).rg
        : vec2(texture(uTexU, vTexCoord).r, texture(uTexV, vTexCoord).r);
    vec3 rgb = uMatrix * (vec3(y, uv) - uOffset);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

// Rows are (Y, U, V) -> (R, G, B); stored column-major for
// glUniformMatrix3fv (column 0 = the three Y coefficients, and so on).
struct YuvMatrix {
  GLfloat m[9];
  GLfloat offset_y;
};

constexpr YuvMatrix kMatrix601Limited = {{1.164383f, 1.164383f, 1.164383f, 0.0f,
                                          -0.391762f, 2.017232f, 1.596027f,
                                          -0.812968f, 0.0f},
                                         16.0f / 255.0f};
constexpr YuvMatrix kMatrix601Full = {
    {1.0f, 1.0f, 1.0f, 0.0f, -0.344136f, 1.772f, 1.402f, -0.714136f, 0.0f},
    0.0f};
constexpr YuvMatrix kMatrix709Limited = {{1.164383f, 1.164383f, 1.164383f, 0.0f,
                                          -0.213249f, 2.112402f, 1.792741f,
                                          -0.532909f, 0.0f},
                                         16.0f / 255.0f};
constexpr YuvMatrix kMatrix709Full = {
    {1.0f, 1.0f, 1.0f, 0.0f, -0.187324f, 1.8556f, 1.5748f, -0.468124f, 0.0f},
    0.0f};
// BT.2020 non-constant-luminance, limited range. The chroma coefficients are
// the FULL-range ones times 255/224 (= 1.138393), the same relationship the two
// BT.709 rows above have: 0.164553*1.138393 = 0.187326 and 0.571353*1.138393 =
// 0.650424. Copying the full-range row here instead (as this table briefly did)
// leaves the limited-range Y scaling and offset in place while the chroma is
// wrong, which is an off-by-a-matrix colour error rather than a visible crash:
// limited-range BT.2020 content renders with the wrong saturation.
constexpr YuvMatrix kMatrix2020Limited = {{1.164383f, 1.164383f, 1.164383f,
                                           0.0f, -0.187326f, 2.141772f,
                                           1.678673f, -0.650424f, 0.0f},
                                          16.0f / 255.0f};
constexpr YuvMatrix kMatrix2020Full = {
    {1.0f, 1.0f, 1.0f, 0.0f, -0.164553f, 1.8814f, 1.4746f, -0.571353f, 0.0f},
    0.0f};

YuvMatrix SelectMatrix(const VideoColorSpace& cs) {
  const bool full = cs.range == ColorRange::kFull;
  switch (cs.matrix) {
  case ColorMatrix::kBT709:
    return full ? kMatrix709Full : kMatrix709Limited;
  case ColorMatrix::kBT2020Ncl:
  case ColorMatrix::kBT2020Cl:
    return full ? kMatrix2020Full : kMatrix2020Limited;
  case ColorMatrix::kSMPTE170M:
    return full ? kMatrix601Full : kMatrix601Limited;
  default:
    // Unmarked: sws and libyuv both assume limited-range BT.601; matching
    // them keeps the GL path indistinguishable from the CPU fallback.
    return full ? kMatrix601Full : kMatrix601Limited;
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
  for (auto& plane : planes_) {
    plane = gl::GlTexture::Create(*api_);
    if (!plane) {
      LOG(ERROR) << "gl present: texture creation failed";
      return false;
    }
  }
  return BuildPrograms();
}

bool GlPresenter::BuildPrograms() {
  const std::string prologue = gl::ShaderPrologue(*api_);

  std::string error;
  program_ = gl::GlProgram::Create(
      *api_, (prologue + gl::kQuadVertexShaderBody).c_str(),
      (prologue + kYuvFragmentShaderBody).c_str(), &error);
  if (!program_) {
    LOG(ERROR) << "gl present: video program: " << error;
    return false;
  }
  program_->Use();
  program_->SetInt("uTexY", 0);
  program_->SetInt("uTexU", 1);
  program_->SetInt("uTexV", 2);

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
  const gl::TextOverlay overlay = overlay_slot_->Snapshot();
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

bool GlPresenter::EnsureTextures(const VideoFrame& frame) {
  const int format = static_cast<int>(frame.format());
  if (planes_[0] && texture_width_ == frame.coded_size().width &&
      texture_height_ == frame.coded_size().height &&
      texture_format_ == format) {
    return true;
  }
  texture_width_ = frame.coded_size().width;
  texture_height_ = frame.coded_size().height;
  texture_format_ = format;

  // Y linesizes are not always multiples of four (unusual but legal crops).
  api_->context().PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  const int w = texture_width_;
  const int h = texture_height_;
  const bool nv12 = format == static_cast<int>(VideoFormat::kNV12);
  // Plane 0: Y (R8). Plane 1: U for planar formats, interleaved UV (RG8) for
  // NV12. Plane 2: V for planar formats; unused for NV12 but allocated
  // anyway, so the texture set never has holes.
  planes_[0]->SetImage2D(GL_R8, w, h, GL_RED, GL_UNSIGNED_BYTE, nullptr);
  planes_[1]->SetImage2D(nv12 ? GL_RG8 : GL_R8, w / 2, h / 2,
                         nv12 ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, nullptr);
  planes_[2]->SetImage2D(GL_R8, w / 2, h / 2, GL_RED, GL_UNSIGNED_BYTE,
                         nullptr);
  for (auto& plane : planes_) {
    plane->SetLinearClamp();
  }
  program_->Use();
  program_->SetInt("uNv12", nv12 ? 1 : 0);
  return true;
}

void GlPresenter::UploadPlanes(const VideoFrame& frame) {
  const int w = texture_width_;
  const int h = texture_height_;
  // YV12 stores V before U; the shader's unit 1/2 always mean (U, V) -- the
  // same swap the SDL_Renderer path does for SDL_PIXELFORMAT_YV12.
  const bool yv12 = frame.format() == VideoFormat::kYV12;
  const bool nv12 = frame.format() == VideoFormat::kNV12;
  struct PlaneUpload {
    gl::GlTexture* texture;
    GLenum format;
    int width;
    int height;
    const uint8_t* data;
  };
  const PlaneUpload uploads[] = {
      {planes_[0].get(), GL_RED, w, h,
       frame.visible_data(VideoFrame::kYPlane).data()},
      // The cast is load-bearing: glad's enumerants arrive as plain int
      // literals from #define, so a conditional between two of them is an int,
      // and a braced initialiser will not narrow a non-constant int to GLenum.
      {planes_[1].get(), static_cast<GLenum>(nv12 ? GL_RG : GL_RED), w / 2,
       h / 2,
       frame.visible_data(yv12 ? VideoFrame::kVPlane : VideoFrame::kUPlane)
           .data()},
      {planes_[2].get(), GL_RED, w / 2, h / 2,
       frame.visible_data(yv12 ? VideoFrame::kUPlane : VideoFrame::kVPlane)
           .data()},
  };
  for (const PlaneUpload& up : uploads) {
    if (!up.data) {
      continue;
    }
    up.texture->SetSubImage2D(0, 0, up.width, up.height, up.format,
                              GL_UNSIGNED_BYTE, up.data);
  }
}

void GlPresenter::UpdateColorUniforms(const VideoFrame& frame) {
  const YuvMatrix m = SelectMatrix(frame.color_space());
  program_->SetMat3("uMatrix", m.m);
  program_->SetVec3("uOffset", m.offset_y, 0.5f, 0.5f);
}

bool GlPresenter::Present(const VideoFrame& frame) {
  if (!gl_current_) {
    return false;
  }
  if (frame.format() != VideoFormat::kI420 &&
      frame.format() != VideoFormat::kYV12 &&
      frame.format() != VideoFormat::kNV12) {
    return false;
  }
  if (!EnsureTextures(frame)) {
    return false;
  }
  UploadPlanes(frame);

  int drawable_w = 0;
  int drawable_h = 0;
  gl_ctx_->GetDrawableSize(&drawable_w, &drawable_h);
  api_->context().Viewport(0, 0, drawable_w, drawable_h);
  api_->context().ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  api_->context().Clear(GL_COLOR_BUFFER_BIT);

  program_->Use();
  UpdateColorUniforms(frame);
  planes_[0]->Bind(0);
  planes_[1]->Bind(1);
  planes_[2]->Bind(2);
  quad_->Draw();
  DrawOverlay();
  gl_ctx_->SwapBuffers();
  return true;
}

void GlPresenter::Shutdown() {
  if (!api_) {
    return;
  }
  // After the render thread stops, the context is current nowhere; the host
  // still owns it, so re-activate to delete cleanly and release again.
  gl_ctx_->MakeCurrent();
  overlay_texture_.reset();
  for (auto& plane : planes_) {
    plane.reset();
  }
  quad_.reset();
  overlay_program_.reset();
  program_.reset();
  api_.reset();
  gl_ctx_->ReleaseCurrent();
  gl_current_ = false;
}

}  // namespace avbase::media
