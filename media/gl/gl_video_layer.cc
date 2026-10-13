// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_video_layer.h"

#include <atomic>
#include <string>
#include <utility>

#include "base/logging.h"
#include "media/gl/gl_layer_geometry.h"
#include "media/gl/gl_yuv_colour.h"
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
//
// SAMPLER_TYPE / LUMA_COORD / CHROMA_COORD come from one of the define blocks
// in the header: this is the one text both programs compile.
const char* const kYuvFragmentShaderBody = R"(
in vec2 vTexCoord;
out vec4 fragColor;
uniform SAMPLER_TYPE uTexY;
uniform SAMPLER_TYPE uTexU;
uniform SAMPLER_TYPE uTexV;
uniform vec2 uTexSize;
uniform vec2 uChromaSize;
uniform int uNv12;
uniform mat3 uMatrix;
uniform vec3 uOffset;
uniform float uOpacity;
void main() {
    float y = texture(uTexY, LUMA_COORD).r;
    vec2 uv = (uNv12 == 1)
        ? texture(uTexU, CHROMA_COORD).rg
        : vec2(texture(uTexU, CHROMA_COORD).r, texture(uTexV, CHROMA_COORD).r);
    vec3 rgb = uMatrix * (vec3(y, uv) - uOffset);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), uOpacity);
}
)";

const char* const kYuvSampler2DDefines =
    "#define SAMPLER_TYPE sampler2D\n"
    "#define LUMA_COORD vTexCoord\n"
    "#define CHROMA_COORD vTexCoord\n";

const char* const kYuvSamplerRectDefines =
    "#define SAMPLER_TYPE sampler2DRect\n"
    "#define LUMA_COORD (vTexCoord * uTexSize)\n"
    "#define CHROMA_COORD (vTexCoord * uChromaSize)\n";

// The colour conversion the uniforms below carry lives in gl_yuv_colour.h.
// It is a table, not behaviour: the layer asks it for a matrix and an offset
// and does not otherwise participate in choosing them.

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
  // The picture's own shape is taken from the frame the caller handed over,
  // not from whatever the routes below end up uploading: the readback frame the
  // fallback produces is a fresh I420 conversion that does not carry the
  // container's display rotation, so asking IT would put the correction for a
  // sideways clip on the wrong axis.
  const float aspect = SourceAspectOf(frame);
  const bool ok = frame.IsMappable() ? UploadCpuPlanes(frame)
                                     : UploadGpuFrame(frame, importer);
  if (!ok) {
    // Nothing is being shown, so there is no shape to report either.
    source_aspect_ = 0.0f;
    return false;
  }
  source_aspect_ = aspect;
  return true;
}

bool GlVideoLayer::UploadGpuFrame(const VideoFrame& frame,
                                  GlHwTextureImporter* importer) {
  // A GPU frame. Try the import route -- unless this backend has already been
  // refused by it, in which case asking again is a failed import per frame for
  // the same answer. CanImport() is asked here and once, so that the two ways
  // of failing are distinguishable below: declining a FORMAT is not a driver
  // refusal and must not poison the layer for the frames after it.
  if (importer && refused_importer_ != importer && importer->CanImport(frame)) {
    if (ImportGpuPlanes(frame, *importer)) {
      return true;
    }
    // The backend said yes and the driver still would not point a texture at
    // the surface. That answer does not change between frames.
    refused_importer_ = importer;
    LOG(ERROR) << "hardware import refused at draw time by " << importer->name()
               << "; reading back this frame and every frame after it";
  }

  // Fallback: one readback beats a black screen. It is here rather than in the
  // sink because this is the last place that knows the frame is still alive
  // and unused -- the sink has already handed it over, and re-presenting the
  // whole scene from there would walk every layer again for one that failed.
  base::scoped_refptr<VideoFrame> mapped = frame.ToI420();
  if (!mapped) {
    // No readback path installed by the producer: nothing to draw, and the
    // caller is the one that can report it.
    return false;
  }
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true)) {
    LOG(WARNING) << "gl layer read a hardware frame back via ToI420(); expect "
                    "this only on a host that cannot import or whose driver "
                    "refused to";
  }
  return UploadCpuPlanes(*mapped);
}

// The colour uniforms are a function of the frame's tags, and BOTH routes need
// them: a GPU frame is no less colour-tagged than a CPU one, and a hardware
// decoder that reports "unspecified" gets the same 601 assumption as the
// software path.
void GlVideoLayer::SetColourUniforms(const VideoFrame& frame) {
  const YuvMatrix m = SelectYuvMatrix(frame.color_space());
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
  if (w <= 0 || h <= 0) {
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
    PlaneUploadLayout layout;
    layout.internal_format = (i == 1 && nv12) ? GL_RG8 : GL_R8;
    layout.format = (i == 1 && nv12) ? GL_RG : GL_RED;
    layout.type = GL_UNSIGNED_BYTE;
    layout.width = chroma ? w / 2 : w;
    layout.height = chroma ? h / 2 : h;
    if (!importer.ImportPlane(frame, i, layout)) {
      return false;
    }
  }
  SetColourUniforms(frame);
  // Deliberately NOT capturing source_aspect_ here. Upload() owns it, and it
  // has to: this route is also reached through the readback fallback, where
  // |frame| is a fresh I420 conversion that has lost the container's display
  // rotation -- so the aspect read off it would be the unturned one, and the
  // correction for a sideways clip would land on the wrong axis. Two writers,
  // one of them wrong, is a coin toss; Upload() is the only one that sees the
  // frame the caller actually asked to draw.
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
  // Same reason as the import route: the aspect is Upload()'s to record, not
  // this function's. It is also called with the readback frame, which carries
  // no rotation, and a second writer there would silently win half the time.
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
    // Divided as a float, not as an int: `width_ / 2` truncates on an odd
    // width and the shader then samples 0.5 texel off. Video widths are
    // almost always even so this changes nothing in practice -- it is the
    // odd frame that would have been silently wrong.
    program.SetVec2("uChromaSize", static_cast<float>(width_) / 2.0f,
                    static_cast<float>(height_) / 2.0f);
  }
  planes_[0]->Bind(0);
  planes_[1]->Bind(1);
  planes_[2]->Bind(2);
}

namespace {

// The two video programs differ in exactly one input -- which GLSL sampler
// type reads their textures -- and in nothing else: same body, same vertex
// stage, same units. Building them from one function over one text is what
// keeps that true, because the alternative is two texts that have to agree and
// do not fail when they stop.
std::unique_ptr<gl::GlProgram> CreateLayerProgram(const gl::GlApi& gl,
                                                  const char* sampler_defines,
                                                  std::string* error) {
  const std::string prologue = gl::ShaderPrologue(gl);
  // The defines first: each must precede every non-preprocessor line, and
  // each turns on a uniform the stage then expects to be set. CROP is part of
  // this contract rather than an option because a program that declared
  // uSrcRect without anybody setting it would draw nothing -- an unset vec4
  // is (0,0,0,0), which is not "the whole picture" but an empty box.
  auto program = gl::GlProgram::Create(
      gl,
      (prologue + gl::kVertexCropDefine + gl::kVertexRotationDefine +
       gl::kQuadVertexShaderWithRectBody)
          .c_str(),
      (prologue + sampler_defines + kYuvFragmentShaderBody).c_str(), error);
  if (!program) {
    return nullptr;
  }
  // The sampler units are part of the contract between this program and
  // GlVideoLayer::Bind, which binds Y/U/V to exactly these three -- and binds
  // once for BOTH variants, which is only safe because the units are set the
  // same way in both. Stated once here rather than once per program, so the
  // contract has one place to be read and one place to be changed.
  program->Use();
  program->SetInt("uTexY", 0);
  program->SetInt("uTexU", 1);
  program->SetInt("uTexV", 2);
  // Leave a fresh program UPRIGHT rather than merely unset: an unset vec2 reads
  // back as (0, 0), which is not the identity but a collapse onto the centre
  // -- a blank frame, with no error and no log line to explain it.
  program->SetVec2("uRotation", 1.0f, 0.0f);
  // Same reason, in the other transform: the whole picture is the identity
  // of a crop, and (0,0,0,0) is an empty box rather than "no crop".
  program->SetVec4("uSrcRect", 0.0f, 0.0f, 1.0f, 1.0f);
  return program;
}

}  // namespace

std::unique_ptr<gl::GlProgram> CreateVideoLayerProgram(const gl::GlApi& gl,
                                                       std::string* error) {
  return CreateLayerProgram(gl, kYuvSampler2DDefines, error);
}

std::unique_ptr<gl::GlProgram> CreateVideoLayerRectProgram(const gl::GlApi& gl,
                                                           std::string* error) {
  return CreateLayerProgram(gl, kYuvSamplerRectDefines, error);
}

}  // namespace avbase::media
