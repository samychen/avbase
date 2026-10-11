// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_video_layer.h"

#include <string>
#include <utility>

#include "render/gl/gl_shaders.h"

// The video-domain half of the GL path, in one place. Entry-point loading,
// shader compilation, textures and the quad come from avbase_gl; what is left
// here is the part that is genuinely about video: which plane goes in which
// texture, what the colour matrix is, and how the stride is handled.

namespace avbase::media {

// The YUV->RGB conversion, with an opacity so a scene can fade a layer. The
// alpha is a uniform rather than a second shader because the alternative is
// two programs whose bodies differ by one character, which is a maintenance
// trap dressed as a performance win.
const char* const kYuvFragmentShaderBody = R"(
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexY;
uniform sampler2D uTexU;
uniform sampler2D uTexV;
uniform int uNv12;
uniform mat3 uMatrix;
uniform vec3 uOffset;
uniform float uOpacity;
void main() {
    float y = texture(uTexY, vTexCoord).r;
    vec2 uv = (uNv12 == 1)
        ? texture(uTexU, vTexCoord).rg
        : vec2(texture(uTexU, vTexCoord).r, texture(uTexV, vTexCoord).r);
    vec3 rgb = uMatrix * (vec3(y, uv) - uOffset);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), uOpacity);
}
)";

// The RECTANGLE-texture twin of the body above. A rectangle texture is
// addressed in texels, not in [0,1], so vTexCoord is scaled by the plane's own
// size; everything after that -- the NV12 branch, the matrix, the opacity -- is
// character for character the same, and has to be, because both bodies are
// drawn with the same vertex stage and the same uRect placement.
const char* const kYuvRectFragmentShaderBody = R"(
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2DRect uTexY;
uniform sampler2DRect uTexU;
uniform sampler2DRect uTexV;
uniform vec2 uTexSize;
uniform vec2 uChromaSize;
uniform int uNv12;
uniform mat3 uMatrix;
uniform vec3 uOffset;
uniform float uOpacity;
void main() {
    float y = texture(uTexY, vTexCoord * uTexSize).r;
    vec2 uv = (uNv12 == 1)
        ? texture(uTexU, vTexCoord * uChromaSize).rg
        : vec2(texture(uTexU, vTexCoord * uChromaSize).r,
               texture(uTexV, vTexCoord * uChromaSize).r);
    vec3 rgb = uMatrix * (vec3(y, uv) - uOffset);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), uOpacity);
}
)";

namespace {

// Rows are (Y, U, V) -> (R, G, B); stored column-major for
// glUniformMatrix3fv (column 0 = the three Y coefficients, and so on).
struct YuvMatrix {
  std::array<float, 9> m;
  float offset_y;
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

bool IsPlanarYuv(VideoFormat format) {
  return format == VideoFormat::kI420 || format == VideoFormat::kYV12 ||
         format == VideoFormat::kNV12;
}

GlVideoLayer::GlVideoLayer(const gl::GlApi& gl) : gl_(&gl) {}

GlVideoLayer::~GlVideoLayer() = default;

std::unique_ptr<GlVideoLayer> GlVideoLayer::Create(const gl::GlApi& gl) {
  // unique_ptr rather than make_unique because the constructor is private.
  auto layer = std::unique_ptr<GlVideoLayer>(new GlVideoLayer(gl));
  // The common case's textures, made here so that a driver that cannot hand
  // out texture names is reported by whoever asked for the layer rather than
  // by the first frame. The import route replaces them; see
  // EnsurePlaneTextures.
  if (!layer->EnsurePlaneTextures(GL_TEXTURE_2D)) {
    return nullptr;
  }
  return layer;
}

bool GlVideoLayer::EnsurePlaneTextures(unsigned int target) {
  if (planes_[0] && planes_[0]->target() == target) {
    return true;
  }
  // Drop the previous set first: a name belongs to the target it was made for,
  // so two sets can never coexist under these three members.
  for (auto& plane : planes_) {
    plane.reset();
  }
  // Nothing is uploaded into the new set yet, and leaving the old geometry
  // behind would make NeedsTextureSetup() claim a match against textures that
  // no longer exist.
  ready_ = false;
  width_ = 0;
  height_ = 0;
  format_ = 0;
  for (auto& plane : planes_) {
    plane = gl::GlTexture::Create(*gl_, target);
    if (!plane) {
      return false;
    }
  }
  ApplySamplers();
  return true;
}

unsigned int GlVideoLayer::texture_target() const {
  // GL_TEXTURE_2D before the first upload, which is what Create() made: a
  // caller asking early gets the answer for the common case rather than a
  // target no texture is on.
  return planes_[0] ? planes_[0]->target() : GL_TEXTURE_2D;
}

bool GlVideoLayer::NeedsTextureSetup(int w, int h, VideoFormat format) const {
  return width_ != w || height_ != h || format_ != static_cast<int>(format);
}

void GlVideoLayer::ApplySamplers() {
  for (auto& plane : planes_) {
    plane->SetLinearClamp();
  }
}

bool GlVideoLayer::Upload(const VideoFrame& frame,
                          GlHwTextureImporter* importer) {
  last_upload_path_ = UploadPath::kNone;
  // Which route a frame takes is a property of the frame, so the branch is
  // here and not at each call site. A GPU frame with no importer is a caller
  // error, not a crash: false is the answer, and the sink that produced it is
  // expected to have read the frame back first.
  if (!frame.IsMappable()) {
    return importer && ImportGpuPlanes(frame, *importer);
  }
  return UploadCpuPlanes(frame);
}

// The colour uniforms are a function of the frame's tags, and BOTH routes need
// them: a GPU frame is no less colour-tagged than a CPU one, and a hardware
// decoder that reports "unspecified" gets the same 601 assumption as the
// software path.
void GlVideoLayer::SetColourUniforms(const VideoFrame& frame) {
  const YuvMatrix m = SelectMatrix(frame.color_space());
  matrix_ = m.m;
  offset_ = {m.offset_y, 0.5f, 0.5f};
}

bool GlVideoLayer::ImportGpuPlanes(const VideoFrame& frame,
                                   GlHwTextureImporter& importer) {
  if (!IsPlanarYuv(frame.format())) {
    return false;
  }
  const int w = frame.coded_size().width;
  const int h = frame.coded_size().height;
  if (w <= 0 || h <= 0 || !importer.CanImport(frame)) {
    return false;
  }
  // From here on the textures no longer hold a complete picture: a half
  // imported frame drawn as if it were whole is a torn image, and `ready_` is
  // the flag Compose() reads to decide whether to draw at all.
  ready_ = false;
  // The backend names the target, this layer does not: which one the interop
  // API accepts is its business (CGL takes RECTANGLE and refuses 2D), and
  // spelling the same enumerant on both sides is how a GL constant silently
  // becomes two constants. See GlHwTextureImporter::target().
  //
  // Switching the target drops whatever owned frame this layer was holding,
  // which is the right outcome -- the plane textures are about to point at a
  // surface this layer does not own and cannot upload into.
  //
  // No storage is allocated here. ImportPlane() defines level 0 itself, and a
  // glTexImage2D first would be an allocation thrown away on the same call.
  if (!EnsurePlaneTextures(importer.target())) {
    return false;
  }
  width_ = w;
  height_ = h;
  format_ = static_cast<int>(frame.format());
  const bool nv12 = frame.format() == VideoFormat::kNV12;
  nv12_ = nv12;

  // The shader's slots, in its own order: luma, then the U texture (which for
  // NV12 is the interleaved chroma), then V. Which SURFACE plane that maps to
  // is the backend's business -- it is the only side that knows the decoder's
  // layout -- so a format the backend cannot place is one it declines.
  const int plane_count = nv12 ? 2 : 3;
  for (int i = 0; i < plane_count; ++i) {
    // Bind() is what the importer needs and all it needs: this layer's texture
    // name, on this layer's target, for the current unit. The unit numbers
    // match the samplers both video programs fixed, which is why Bind() takes
    // one.
    planes_[i]->Bind(static_cast<unsigned int>(i));
    const bool chroma = i > 0;
    const unsigned int internal_format = (i == 1 && nv12) ? GL_RG8 : GL_R8;
    const unsigned int format = (i == 1 && nv12) ? GL_RG : GL_RED;
    if (!importer.ImportPlane(frame, i, internal_format, format,
                              GL_UNSIGNED_BYTE, chroma ? w / 2 : w,
                              chroma ? h / 2 : h)) {
      return false;
    }
  }
  SetColourUniforms(frame);
  ready_ = true;
  last_upload_path_ = UploadPath::kHwZeroCopy;
  return true;
}

bool GlVideoLayer::UploadCpuPlanes(const VideoFrame& frame) {
  if (!IsPlanarYuv(frame.format())) {
    return false;
  }
  const int w = frame.coded_size().width;
  const int h = frame.coded_size().height;
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (!EnsurePlaneTextures(GL_TEXTURE_2D)) {
    return false;
  }

  const bool nv12 = frame.format() == VideoFormat::kNV12;
  if (NeedsTextureSetup(w, h, frame.format())) {
    width_ = w;
    height_ = h;
    format_ = static_cast<int>(frame.format());
    ready_ = false;

    // Y linesizes are not always multiples of four (unusual but legal crops).
    gl_->context().PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Plane 0: Y (R8). Plane 1: U for planar formats, interleaved UV (RG8) for
    // NV12. Plane 2: V for planar formats; unused for NV12 but allocated
    // anyway, so the texture set never has holes.
    planes_[0]->SetImage2D(GL_R8, w, h, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    planes_[1]->SetImage2D(nv12 ? GL_RG8 : GL_R8, w / 2, h / 2,
                           nv12 ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, nullptr);
    planes_[2]->SetImage2D(GL_R8, w / 2, h / 2, GL_RED, GL_UNSIGNED_BYTE,
                           nullptr);
    ApplySamplers();
  }
  nv12_ = nv12;

  // YV12 stores V before U; the shader's unit 1/2 always mean (U, V) -- the
  // same swap the SDL_Renderer path does for SDL_PIXELFORMAT_YV12.
  const bool yv12 = frame.format() == VideoFormat::kYV12;
  const VideoFrame::Plane u_plane =
      yv12 ? VideoFrame::kVPlane : VideoFrame::kUPlane;
  const VideoFrame::Plane v_plane =
      yv12 ? VideoFrame::kUPlane : VideoFrame::kVPlane;

  // A plane's rows are |stride| bytes apart, and the stride is NOT the visible
  // width: every frame this tree allocates has its rows aligned to 64 bytes
  // (see VideoFrame::CreateBlackFrame, which is what the software decoder uses
  // too). For 1920-wide video the chroma stride happens to land on 960 and the
  // two agree; for 320x240 the chroma stride is 192 where only 160 bytes are
  // image, and for 854x480 it is 448 against 427. A GL upload that ignores this
  // reads the plane as if it were packed and lands a SHEARED copy in the
  // texture -- the picture keeps its luma and gets visibly wrong chroma.
  //
  // GL_UNPACK_ROW_LENGTH is the fix, and its unit is texels of the upload
  // format rather than bytes: GL_RED samples one byte, GL_RG (NV12's
  // interleaved chroma) samples a two-byte pair. GlTexture has exposed this
  // setter since the module was factored out; nothing had ever called it,
  // which is why the bug survived until a test read the pixels back.
  struct PlaneUpload {
    gl::GlTexture* texture;
    GLenum format;
    int width;
    int height;
    int row_texels;
    const uint8_t* data;
  };
  const PlaneUpload uploads[] = {
      {planes_[0].get(), GL_RED, w, h, frame.stride(VideoFrame::kYPlane),
       frame.visible_data(VideoFrame::kYPlane).data()},
      // The cast is load-bearing: glad's enumerants arrive as plain int
      // literals from #define, so a conditional between two of them is an int,
      // and a braced initialiser will not narrow a non-constant int to GLenum.
      {planes_[1].get(), static_cast<GLenum>(nv12 ? GL_RG : GL_RED), w / 2,
       h / 2, nv12 ? frame.stride(u_plane) / 2 : frame.stride(u_plane),
       frame.visible_data(u_plane).data()},
      {planes_[2].get(), GL_RED, w / 2, h / 2, frame.stride(v_plane),
       frame.visible_data(v_plane).data()},
  };
  // Which planes this format actually has. NV12 carries its chroma interleaved
  // in plane 1 and has no plane 2 AT ALL -- so expecting one and failing on its
  // absence would reject NV12 outright, while skipping whatever happens to be
  // null would let a genuinely truncated I420 through and draw stale texture
  // contents. Naming the count removes both readings.
  const int plane_count = nv12 ? 2 : 3;
  for (int i = 0; i < plane_count; ++i) {
    const PlaneUpload& up = uploads[i];
    if (!up.data) {
      return false;
    }
    up.texture->SetUnpackRowLength(up.row_texels);
    up.texture->SetSubImage2D(0, 0, up.width, up.height, up.format,
                              GL_UNSIGNED_BYTE, up.data);
  }
  // Back to "rows are packed". Despite living on GlTexture, this is context
  // state, not texture state -- and it applies to glTexImage2D as well, so
  // leaving a plane's stride behind would make the next overlay bitmap upload
  // read its own rows at the wrong pitch, a corruption that would point at the
  // overlay rather than at this line.
  planes_[0]->SetUnpackRowLength(0);

  SetColourUniforms(frame);
  ready_ = true;
  last_upload_path_ = UploadPath::kCpuSync;
  return true;
}

void GlVideoLayer::Bind(gl::GlProgram& program, float opacity) const {
  program.SetInt("uNv12", nv12_ ? 1 : 0);
  program.SetMat3("uMatrix", matrix_.data());
  program.SetVec3("uOffset", offset_[0], offset_[1], offset_[2]);
  program.SetFloat("uOpacity", opacity);
  if (texture_target() == GL_TEXTURE_RECTANGLE) {
    // The rect body addresses its samplers in texels, so it has to be told how
    // big the planes are -- both of them, because a rect sampler has no way to
    // derive the chroma size from the luma one the way a normalised [0,1]
    // coordinate does. Harmless to set on the 2D program (no such uniform, so
    // the call is a silent no-op), which is why it is guarded by the target
    // rather than by the program.
    program.SetVec2("uTexSize", static_cast<float>(width_),
                    static_cast<float>(height_));
    program.SetVec2("uChromaSize", static_cast<float>(width_ / 2),
                    static_cast<float>(height_ / 2));
  }
  planes_[0]->Bind(0);
  planes_[1]->Bind(1);
  planes_[2]->Bind(2);
}

std::unique_ptr<gl::GlProgram> CreateVideoLayerProgram(const gl::GlApi& gl,
                                                       std::string* error) {
  const std::string prologue = gl::ShaderPrologue(gl);
  auto program = gl::GlProgram::Create(
      gl, (prologue + gl::kQuadVertexShaderWithRectBody).c_str(),
      (prologue + kYuvFragmentShaderBody).c_str(), error);
  if (!program) {
    return nullptr;
  }
  // The sampler units are part of the contract between this program and
  // GlVideoLayer::Bind, which binds Y/U/V to exactly these three.
  program->Use();
  program->SetInt("uTexY", 0);
  program->SetInt("uTexU", 1);
  program->SetInt("uTexV", 2);
  return program;
}

std::unique_ptr<gl::GlProgram> CreateVideoLayerRectProgram(const gl::GlApi& gl,
                                                           std::string* error) {
  const std::string prologue = gl::ShaderPrologue(gl);
  auto program = gl::GlProgram::Create(
      gl, (prologue + gl::kQuadVertexShaderWithRectBody).c_str(),
      (prologue + kYuvRectFragmentShaderBody).c_str(), error);
  if (!program) {
    return nullptr;
  }
  // The same unit contract as the 2D program above; the pair is the reason
  // GlVideoLayer::Bind can bind once for both.
  program->Use();
  program->SetInt("uTexY", 0);
  program->SetInt("uTexU", 1);
  program->SetInt("uTexV", 2);
  return program;
}

void NormalisedRectToUvRect(float x, float y, float w, float h,
                            float* out_rect) {
  // NDC x spans -1..1 as the normalised x spans 0..1, so a normalised width of
  // w covers 2w of NDC and the half-extent is w. NDC y is flipped: normalised
  // y = 0 is the TOP, which in NDC is +1.
  out_rect[0] = 2.0f * (x + w * 0.5f) - 1.0f;
  out_rect[1] = 1.0f - 2.0f * (y + h * 0.5f);
  out_rect[2] = w;
  out_rect[3] = h;
}

}  // namespace avbase::media
