// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_api.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace avbase::gl {
namespace {

const char* AsString(const unsigned char* p) {
  return p ? reinterpret_cast<const char*>(p) : "";
}

// Splits a space-separated extension list. Used only by the pre-3.0 fallback:
// from GL 3.0 on the list is indexed through glGetStringi, because in a core
// profile glGetString(GL_EXTENSIONS) is not even legal.
void SplitExtensionList(const char* list, std::vector<std::string>* out) {
  const char* p = list;
  while (p && *p) {
    while (*p == ' ') {
      ++p;
    }
    if (!*p) {
      break;
    }
    const char* start = p;
    while (*p && *p != ' ') {
      ++p;
    }
    out->emplace_back(start, static_cast<size_t>(p - start));
  }
}

// Parses the leading "3.3" out of a GL_VERSION string ("3.3.0 NVIDIA-10.2.7").
// Only a fallback: drivers that expose GL_MAJOR_VERSION are asked directly,
// since vendor strings are free-form.
void ParseVersionString(const char* s, int* major, int* minor) {
  int a = 0;
  int b = 0;
  if (s && std::sscanf(s, "%d.%d", &a, &b) == 2) {
    *major = a;
    *minor = b;
  }
}

// Reads one integer back from a GetIntegerv query. Two things make this worth
// a function rather than five copies of the same three lines: the queries below
// all arrived after GL 1.1, so an older driver answers with GL_INVALID_ENUM
// instead of a value (the caller supplies |fallback| for that case), and the
// error has to be consumed either way -- an error left queued would be read by
// whoever calls GetError() next, which is somebody else's code.
GLint QueryInt(const GladGLContext& gl, GLenum pname, GLint fallback) {
  GLint value = 0;
  gl.GetIntegerv(pname, &value);
  return gl.GetError() == GL_NO_ERROR ? value : fallback;
}

}  // namespace

GLADapiproc GlApi::ResolveEntryPoint(void* userptr, const char* name) {
  auto* ctx = static_cast<NativeGLContext*>(userptr);
  return reinterpret_cast<GLADapiproc>(ctx->GetProcAddress(name));
}

bool GlApi::Load(NativeGLContext* ctx, std::string* error) {
  if (!ctx) {
    if (error) {
      *error = "null GL context";
    }
    return false;
  }

  // glad fills its table by calling back once per symbol it knows about, so the
  // window system keeps arriving through a single interface rather than glad
  // growing a second path to the driver. A zero return means it could not even
  // find glGetString, which is what a context that is not current looks like
  // from here.
  context_ = GladGLContext{};
  if (gladLoadGLContextUserPtr(&context_, &GlApi::ResolveEntryPoint, ctx) ==
      0) {
    if (error) {
      *error =
          "no GL entry points resolved: the context is probably not current "
          "on this thread";
    }
    return false;
  }

  // Anything the driver raised while glad was probing belongs to us, not to the
  // caller's next DrawArrays. Clear it before the queries below, whose
  // fallbacks depend on reading a fresh error.
  DrainErrors();

  // Version. GL_MAJOR_VERSION/GL_MINOR_VERSION exist from 3.0; on anything
  // older the query raises GL_INVALID_ENUM and the string parser takes over.
  // -1 is a safe "the query failed" sentinel because neither number is
  // negative.
  const auto kVersion = version_string();
  // ES contexts announce themselves in GL_VERSION ("OpenGL ES 3.0 ..."); the
  // enumerant queries below report the same numbers either way, so the string
  // is the only place the distinction exists.
  is_gles_ = kVersion.starts_with("OpenGL ES");
  version_major_ = QueryInt(context_, GL_MAJOR_VERSION, -1);
  version_minor_ = 0;
  if (version_major_ >= 0) {
    version_minor_ = QueryInt(context_, GL_MINOR_VERSION, 0);
  } else {
    version_major_ = 0;
    // "OpenGL ES 3.2 ..." needs the number after the ES marker, not at offset
    // 0.
    const char* numbers = is_gles_ ? kVersion.c_str() + 9 : kVersion.c_str();
    ParseVersionString(numbers, &version_major_, &version_minor_);
  }

  // The table is GL 3.3 core by construction, so a merely older driver resolves
  // the subset it has and leaves the rest null -- and a null function pointer
  // is not a diagnostic, it is a crash waiting for whichever frame needs it
  // first. Refusing here turns that into one sentence naming the version.
  //
  // ES is held to 3.0 rather than 3.3 because glad reads the same
  // "OpenGL ES X.Y" prefix: an ES 3.2 context reports 3.2 and would never set
  // VERSION_3_3.
  const bool kTooOld =
      is_gles_ ? context_.VERSION_3_0 == 0 : context_.VERSION_3_3 == 0;
  if (kTooOld) {
    if (error) {
      *error = "context reports " + kVersion +
               "; this module needs OpenGL 3.3 core or OpenGL ES 3.0";
    }
    return false;
  }

  // Extension list, indexed form first. glad fills per-extension flags for the
  // extensions it was generated with; this list is what answers for any name,
  // including ones no table has a column for.
  extensions_.clear();
  const GLint kCount = QueryInt(context_, GL_NUM_EXTENSIONS, 0);
  if (kCount > 0 && context_.GetStringi != nullptr) {
    extensions_.reserve(static_cast<size_t>(kCount));
    for (GLint i = 0; i < kCount; ++i) {
      extensions_.emplace_back(AsString(context_.GetStringi(GL_EXTENSIONS, i)));
    }
  } else {
    SplitExtensionList(AsString(context_.GetString(GL_EXTENSIONS)),
                       &extensions_);
  }

  // Limits worth knowing up front rather than discovering through a failure.
  max_texture_size_ = QueryInt(context_, GL_MAX_TEXTURE_SIZE, 0);
  max_texture_units_ = QueryInt(context_, GL_MAX_TEXTURE_IMAGE_UNITS, 0);

  // The one symbol glad cannot carry, for the reason given in gl_api.h.
  // Resolved through the same interface; null simply means this backend offers
  // no EGLImage import.
  EGLImageTargetTexture2DOES = reinterpret_cast<PfnEGLImageTargetTexture2DOES>(
      ctx->GetProcAddress("glEGLImageTargetTexture2DOES"));

  return true;
}

std::string GlApi::renderer() const {
  return AsString(context_.GetString(GL_RENDERER));
}

std::string GlApi::vendor() const {
  return AsString(context_.GetString(GL_VENDOR));
}

std::string GlApi::version_string() const {
  return AsString(context_.GetString(GL_VERSION));
}

std::string GlApi::shading_language_version() const {
  return AsString(context_.GetString(GL_SHADING_LANGUAGE_VERSION));
}

bool GlApi::HasExtension(const char* name) const {
  if (!name) {
    return false;
  }
  return std::ranges::any_of(
      extensions_, [name](const std::string& e) { return e == name; });
}

bool GlApi::has_debug_output() const {
  // glad fills these two flags from the driver's own extension list, so this is
  // the same question a string scan answered, asked once at load instead of per
  // call.
  return (context_.KHR_debug != 0 || context_.ARB_debug_output != 0) &&
         context_.DebugMessageCallback != nullptr;
}

bool GlApi::has_egl_image() const {
  // No extension-string check: whether EGLImage import is possible is decided
  // by the EGL side (EGL_EXT_image_dma_buf_import) and surfaced when the entry
  // point resolves. On a plain desktop GL context this is simply null.
  return EGLImageTargetTexture2DOES != nullptr;
}

bool GlApi::has_copy_image() const {
  // The two pointers are resolved independently, which is why the generated
  // loader does NOT use glad's --alias mode: it would have collapsed them into
  // one pointer at the cost of pulling 75 extensions into the table.
  return context_.CopyImageSubData != nullptr ||
         context_.CopyImageSubDataNV != nullptr;
}

bool GlApi::has_anisotropic_filter() const {
  return context_.EXT_texture_filter_anisotropic != 0 &&
         context_.TexParameterf != nullptr;
}

bool GlApi::has_srgb_decode() const {
  return context_.EXT_texture_sRGB_decode != 0;
}

GLenum GlApi::DrainErrors() const {
  GLenum last = GL_NO_ERROR;
  // Bounded: a driver that never clears its queue would otherwise hang startup.
  for (int i = 0; i < 64; ++i) {
    const auto kError = context_.GetError();
    if (kError == GL_NO_ERROR) {
      break;
    }
    last = kError;
  }
  return last;
}

}  // namespace avbase::gl
