// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/gl_present.h"

#include <SDL.h>

#include "base/logging.h"
#include "platform/sdl2/gl_loader.h"

// GL 3.3 core, resolved through SDL_GL_GetProcAddress. No GLEW: the entry
// points live in this file's loader, the GL types are aliased locally, and
// the constants are plain integers -- so no system GL header (whose version
// coverage differs per platform) ever enters the build.

namespace avbase::media {
namespace {

// Ported from MediaComponent framework/render/gl/shader_source_common.h
// (kQuadVertexShader), trimmed to what a display pass needs.
constexpr char kVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

constexpr char kFragmentShader[] = R"(#version 330 core
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

// The overlay pass: same quad geometry, positioned by uRect (NDC), straight
// alpha blended over whatever the video pass left in the framebuffer.
constexpr char kOverlayVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
uniform vec4 uRect;
void main() {
    vec2 pos = aPos * uRect.zw + uRect.xy;
    gl_Position = vec4(pos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

constexpr char kOverlayFragmentShader[] = R"(#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    fragColor = texture(uTex, vTexCoord);
}
)";

// Rows are (Y, U, V) -> (R, G, B); stored column-major for
// glUniformMatrix3fv (column 0 = the three Y coefficients, and so on).
struct YuvMatrix {
  GLfloat m[9];
  GLfloat offset_y;
};

constexpr YuvMatrix kMatrix601Limited = {
    {1.164383f, 1.164383f, 1.164383f, 0.0f, -0.391762f, 2.017232f, 1.596027f,
     -0.812968f, 0.0f},
    16.0f / 255.0f};
constexpr YuvMatrix kMatrix601Full = {
    {1.0f, 1.0f, 1.0f, 0.0f, -0.344136f, 1.772f, 1.402f, -0.714136f, 0.0f},
    0.0f};
constexpr YuvMatrix kMatrix709Limited = {
    {1.164383f, 1.164383f, 1.164383f, 0.0f, -0.213249f, 2.112402f, 1.792741f,
     -0.532909f, 0.0f},
    16.0f / 255.0f};
constexpr YuvMatrix kMatrix709Full = {
    {1.0f, 1.0f, 1.0f, 0.0f, -0.187326f, 1.8556f, 1.5748f, -0.468124f, 0.0f},
    0.0f};
constexpr YuvMatrix kMatrix2020Limited = {
    {1.164383f, 1.164383f, 1.164383f, 0.0f, -0.187326f, 2.141772f, 1.678673f,
     -0.650424f, 0.0f},
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

// The resolved entry points, as a distinct type so the sink header stays
// free of anything GL-shaped.
struct GlPresenter::Loader : GlLoader {};

GlPresenter::GlPresenter(void* window, void* context)
    : window_(window), context_(context) {}

GlPresenter::~GlPresenter() {
  Shutdown();
}

bool GlPresenter::Init() {
  if (gl_current_) {
    return true;
  }
  if (SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window_),
                         static_cast<SDL_GLContext>(context_)) != 0) {
    LOG(ERROR) << "gl present: MakeCurrent failed: " << SDL_GetError();
    return false;
  }
  gl_current_ = true;
  loader_ = std::make_unique<Loader>();
  std::string missing;
  if (!ResolveGl(loader_.get(), &missing)) {
    LOG(ERROR) << "gl present: missing GL entry point " << missing;
    return false;
  }
  if (!BuildProgram()) {
    return false;
  }
  loader_->GenVertexArrays(1, &vao_);
  loader_->GenBuffers(1, &vbo_);
  loader_->BindVertexArray(vao_);
  loader_->BindBuffer(kGLArrayBuffer, vbo_);
  // Fullscreen triangle strip; texcoords match the default 2D mapping.
  static const GLfloat quad[] = {
      -1.0f, -1.0f, 0.0f, 1.0f,  //
      1.0f, -1.0f, 1.0f, 1.0f,   //
      -1.0f, 1.0f, 0.0f, 0.0f,   //
      1.0f, 1.0f, 1.0f, 0.0f};
  loader_->BufferData(kGLArrayBuffer, sizeof(quad), quad, kGLStaticDraw);
  loader_->EnableVertexAttribArray(0);
  loader_->VertexAttribPointer(0, 2, kGLFloat, false, 4 * sizeof(GLfloat),
                               nullptr);
  loader_->EnableVertexAttribArray(1);
  loader_->VertexAttribPointer(1, 2, kGLFloat, false, 4 * sizeof(GLfloat),
                               reinterpret_cast<const void*>(
                                   2 * sizeof(GLfloat)));
  loader_->GenTextures(3, textures_);
  loader_->UseProgram(program_);
  loader_->Uniform1i(loader_->GetUniformLocation(program_, "uTexY"), 0);
  loader_->Uniform1i(loader_->GetUniformLocation(program_, "uTexU"), 1);
  loader_->Uniform1i(loader_->GetUniformLocation(program_, "uTexV"), 2);
  if (!BuildOverlayProgram()) {
    return false;
  }
  return true;
}

bool GlPresenter::CompileProgram(const char* vertex_src,
                                 const char* fragment_src,
                                 unsigned int* out) {
  const GLuint vertex = loader_->CreateShader(kGLVertexShader);
  const GLuint fragment = loader_->CreateShader(kGLFragmentShader);
  loader_->ShaderSource(vertex, 1, &vertex_src, nullptr);
  loader_->CompileShader(vertex);
  loader_->ShaderSource(fragment, 1, &fragment_src, nullptr);
  loader_->CompileShader(fragment);
  auto check_shader = [&](GLuint shader, const char* what) {
    GLint status = 0;
    loader_->GetShaderiv(shader, kGLCompileStatus, &status);
    if (!status) {
      char log[512] = {0};
      loader_->GetShaderInfoLog(shader, sizeof(log), nullptr, log);
      LOG(ERROR) << "gl present: " << what << " shader: " << log;
    }
    return status != 0;
  };
  if (!check_shader(vertex, "vertex") || !check_shader(fragment, "fragment")) {
    return false;
  }
  const GLuint program = loader_->CreateProgram();
  loader_->AttachShader(program, vertex);
  loader_->AttachShader(program, fragment);
  loader_->LinkProgram(program);
  GLint status = 0;
  loader_->GetProgramiv(program, kGLLinkStatus, &status);
  if (!status) {
    char log[512] = {0};
    loader_->GetProgramInfoLog(program, sizeof(log), nullptr, log);
    LOG(ERROR) << "gl present: link: " << log;
    return false;
  }
  loader_->DeleteShader(vertex);
  loader_->DeleteShader(fragment);
  *out = program;
  return true;
}

bool GlPresenter::BuildProgram() {
  return CompileProgram(kVertexShader, kFragmentShader, &program_);
}

bool GlPresenter::BuildOverlayProgram() {
  if (!CompileProgram(kOverlayVertexShader, kOverlayFragmentShader,
                      &overlay_program_)) {
    return false;
  }
  loader_->UseProgram(overlay_program_);
  loader_->Uniform1i(loader_->GetUniformLocation(overlay_program_, "uTex"), 0);
  return true;
}

// (Re)creates the overlay RGBA texture and records which slot version it
// holds; Present re-uploads whenever the slot publishes a new version.
bool GlPresenter::EnsureOverlayTexture(const TextOverlay& overlay) {
  if (overlay_texture_ && overlay_version_ == overlay.version &&
      overlay_width_ == overlay.width && overlay_height_ == overlay.height) {
    return true;
  }
  if (!overlay_texture_) {
    loader_->GenTextures(1, &overlay_texture_);
  }
  loader_->BindTexture(kGLTexture2D, overlay_texture_);
  loader_->TexImage2D(kGLTexture2D, 0, kGLRGBA8, overlay.width, overlay.height,
                      0, kGLRGBA, kGLUnsignedByte, overlay.rgba.data());
  loader_->TexParameteri(kGLTexture2D, kGLTextureMinFilter, kGLLinear);
  loader_->TexParameteri(kGLTexture2D, kGLTextureMagFilter, kGLLinear);
  loader_->TexParameteri(kGLTexture2D, kGLTextureWrapS, kGLClampToEdge);
  loader_->TexParameteri(kGLTexture2D, kGLTextureWrapT, kGLClampToEdge);
  overlay_version_ = overlay.version;
  overlay_width_ = overlay.width;
  overlay_height_ = overlay.height;
  return true;
}

void GlPresenter::DrawOverlay() {
  if (!overlay_slot_) {
    return;
  }
  const TextOverlay overlay = overlay_slot_->Snapshot();
  if (!overlay.valid || overlay.rgba.size() !=
                            static_cast<size_t>(overlay.width) *
                                static_cast<size_t>(overlay.height) * 4u) {
    return;
  }
  if (!EnsureOverlayTexture(overlay)) {
    return;
  }
  loader_->Enable(kGLBlend);
  loader_->BlendFunc(kGLSrcAlpha, kGLOneMinusSrcAlpha);
  loader_->UseProgram(overlay_program_);
  loader_->Uniform4f(loader_->GetUniformLocation(overlay_program_, "uRect"),
                     overlay.x, overlay.y, overlay.w, overlay.h);
  loader_->ActiveTexture(kGLActiveTexture0);
  loader_->BindTexture(kGLTexture2D, overlay_texture_);
  loader_->BindVertexArray(vao_);
  loader_->DrawArrays(kGLTriangleStrip, 0, 4);
  loader_->Disable(kGLBlend);
}

bool GlPresenter::EnsureTextures(const VideoFrame& frame) {
  const int format = static_cast<int>(frame.format());
  if (textures_[0] && texture_width_ == frame.coded_size().width &&
      texture_height_ == frame.coded_size().height &&
      texture_format_ == format) {
    return true;
  }
  if (textures_[0]) {
    loader_->DeleteTextures(3, textures_);
    textures_[0] = textures_[1] = textures_[2] = 0;
  }
  texture_width_ = frame.coded_size().width;
  texture_height_ = frame.coded_size().height;
  texture_format_ = format;
  loader_->GenTextures(3, textures_);
  // Y linesizes are not always multiples of four (unusual but legal crops).
  loader_->PixelStorei(kGLUnpackAlignment, 1);
  const GLsizei w = texture_width_;
  const GLsizei h = texture_height_;
  const bool nv12 = format == static_cast<int>(VideoFormat::kNV12);
  // Plane 0: Y (R8). Plane 1: U for planar formats, interleaved UV (RG8) for
  // NV12. Plane 2: V for planar formats; unused for NV12 but allocated
  // anyway, so the texture set never has holes.
  loader_->BindTexture(kGLTexture2D, textures_[0]);
  loader_->TexImage2D(kGLTexture2D, 0, kGLR8, w, h, 0, kGLRed,
                      kGLUnsignedByte, nullptr);
  loader_->BindTexture(kGLTexture2D, textures_[1]);
  loader_->TexImage2D(kGLTexture2D, 0, nv12 ? kGLRG8 : kGLR8, w / 2, h / 2, 0,
                      nv12 ? kGLRG : kGLRed, kGLUnsignedByte, nullptr);
  loader_->BindTexture(kGLTexture2D, textures_[2]);
  loader_->TexImage2D(kGLTexture2D, 0, kGLR8, w / 2, h / 2, 0, kGLRed,
                      kGLUnsignedByte, nullptr);
  for (const unsigned int t : textures_) {
    loader_->BindTexture(kGLTexture2D, t);
    loader_->TexParameteri(kGLTexture2D, kGLTextureMinFilter, kGLLinear);
    loader_->TexParameteri(kGLTexture2D, kGLTextureMagFilter, kGLLinear);
    loader_->TexParameteri(kGLTexture2D, kGLTextureWrapS, kGLClampToEdge);
    loader_->TexParameteri(kGLTexture2D, kGLTextureWrapT, kGLClampToEdge);
  }
  loader_->UseProgram(program_);
  loader_->Uniform1i(loader_->GetUniformLocation(program_, "uNv12"),
                     nv12 ? 1 : 0);
  return true;
}

void GlPresenter::UploadPlanes(const VideoFrame& frame) {
  const GLsizei w = texture_width_;
  const GLsizei h = texture_height_;
  // YV12 stores V before U; the shader's unit 1/2 always mean (U, V) -- the
  // same swap the SDL_Renderer path does for SDL_PIXELFORMAT_YV12.
  const bool yv12 = frame.format() == VideoFormat::kYV12;
  struct PlaneUpload {
    GLuint texture;
    GLenum format;
    GLsizei width;
    GLsizei height;
    const uint8_t* data;
    int stride;
  };
  const PlaneUpload uploads[] = {
      {textures_[0], kGLRed, w, h,
       frame.visible_data(VideoFrame::kYPlane).data(),
       frame.stride(VideoFrame::kYPlane)},
      {textures_[1], frame.format() == VideoFormat::kNV12 ? kGLRG : kGLRed,
       w / 2, h / 2,
       frame.visible_data(yv12 ? VideoFrame::kVPlane : VideoFrame::kUPlane)
           .data(),
       frame.stride(yv12 ? VideoFrame::kVPlane : VideoFrame::kUPlane)},
      {textures_[2], kGLRed, w / 2, h / 2,
       frame.visible_data(yv12 ? VideoFrame::kUPlane : VideoFrame::kVPlane)
           .data(),
       frame.stride(yv12 ? VideoFrame::kUPlane : VideoFrame::kVPlane)},
  };
  for (const PlaneUpload& up : uploads) {
    if (!up.data) {
      continue;
    }
    loader_->BindTexture(kGLTexture2D, up.texture);
    loader_->TexSubImage2D(kGLTexture2D, 0, 0, 0, up.width, up.height,
                           up.format, kGLUnsignedByte, up.data);
  }
}

void GlPresenter::UpdateColorUniforms(const VideoFrame& frame) {
  const YuvMatrix m = SelectMatrix(frame.color_space());
  loader_->UniformMatrix3fv(loader_->GetUniformLocation(program_, "uMatrix"),
                            1, false, m.m);
  loader_->Uniform3f(loader_->GetUniformLocation(program_, "uOffset"),
                     m.offset_y, 0.5f, 0.5f);
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
  SDL_GL_GetDrawableSize(static_cast<SDL_Window*>(window_), &drawable_w,
                         &drawable_h);
  loader_->Viewport(0, 0, drawable_w, drawable_h);
  loader_->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  loader_->Clear(kGLColorBufferBit);
  loader_->UseProgram(program_);
  UpdateColorUniforms(frame);
  loader_->ActiveTexture(kGLActiveTexture0);
  loader_->BindTexture(kGLTexture2D, textures_[0]);
  loader_->ActiveTexture(kGLActiveTexture1);
  loader_->BindTexture(kGLTexture2D, textures_[1]);
  loader_->ActiveTexture(kGLActiveTexture2);
  loader_->BindTexture(kGLTexture2D, textures_[2]);
  loader_->BindVertexArray(vao_);
  loader_->DrawArrays(kGLTriangleStrip, 0, 4);
  DrawOverlay();
  SDL_GL_SwapWindow(static_cast<SDL_Window*>(window_));
  return true;
}

void GlPresenter::Shutdown() {
  if (!loader_) {
    return;
  }
  // After the render thread stops, the context is current nowhere; the host
  // still owns it, so re-activate to delete cleanly and release again.
  SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window_),
                     static_cast<SDL_GLContext>(context_));
  loader_->DeleteTextures(3, textures_);
  textures_[0] = textures_[1] = textures_[2] = 0;
  if (overlay_texture_) {
    loader_->DeleteTextures(1, &overlay_texture_);
    overlay_texture_ = 0;
  }
  loader_->DeleteBuffers(1, &vbo_);
  vbo_ = 0;
  loader_->DeleteVertexArrays(1, &vao_);
  vao_ = 0;
  loader_->DeleteProgram(program_);
  program_ = 0;
  loader_.reset();
  SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window_), nullptr);
  gl_current_ = false;
}

}  // namespace avbase::media
