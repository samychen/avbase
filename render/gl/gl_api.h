// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_API_H_
#define AVBASE_RENDER_GL_GL_API_H_

#include <glad/gl.h>

#include <string>
#include <vector>

#include "render/gl/gl_export.h"
#include "render/gl/native_gl_context.h"

namespace avbase::gl {

// A loaded GL 3.3 core context: glad's per-context entry-point table plus the
// facts needed to decide what the driver can do.
//
// The table is generated, not written here. Until the glad swap this class
// declared its entry points as an X-macro list and its enumerants in a
// hand-written gl_constants.h; both are now output of the glad 2.0.8 generator
// (render/gl/third_party/glad/, see the README there for the exact command).
// What is left in this class is only what a generator cannot supply: which
// capabilities the module actually asks for, the strings and limits it reports,
// and the requirement it places on the context.
//
// One GladGLContext per GlApi, one GlApi per GL context. There is no
// process-global table and no initialization order to get right -- the caller
// creates one after making its context current, and destroys it before that
// context goes away. glad's --mx mode is what makes this expressible: its
// default mode would put every pointer in a global, which is a plain bug the
// moment two contexts live on two threads.
class AVBASE_GL_EXPORT GlApi {
 public:
  GlApi() = default;
  GlApi(const GlApi&) = delete;
  GlApi& operator=(const GlApi&) = delete;
  ~GlApi() = default;

  // Fills the table. On failure returns false and, when |error| is non-null,
  // says why. The context must be current on the calling thread.
  //
  // Only two things can fail, and both are about the context as a whole rather
  // than about a symbol: the driver handed out nowhere to start from (no
  // current context), or the context is older than this module can draw on.
  // Individual entry points cannot fail one at a time any more -- the table is
  // GL 3.3 core by construction -- so the question moved from "are my
  // functions there" to "can the module draw here", which is the one that
  // actually mattered.
  bool Load(NativeGLContext* ctx, std::string* error);

  // --- Entry points ---------------------------------------------------------
  // The loaded table, reached as gl.context().Clear(...). A forwarding method
  // per function would buy nothing and would hide that these are pointers the
  // driver handed us, not methods of this class; handing out the table keeps
  // that visible and keeps this class down to the part glad cannot generate.
  //
  // It is an accessor rather than GlApi deriving from GladGLContext for two
  // reasons: a raw pointer does not go through operator->, so the inherited
  // form would read correctly and fail to compile anyway; and this class is
  // exported, and deriving an exported class from one with no visibility
  // attribute trips -Wvisibility -- a warning this tree has already paid for
  // once.
  const GladGLContext& context() const { return context_; }

  // The one entry point glad cannot supply, declared next to the table it
  // belongs to so how much of the surface glad does NOT cover is visible: this
  // much. glEGLImageTargetTexture2DOES comes from GL_OES_EGL_image, an ES
  // extension, and glad rejects it at generation time for a desktop `gl` API --
  // there is no option that would produce it here. It is resolved by hand in
  // Load() through the same NativeGLContext, so EGLImage import works on the
  // backends that offer it and reports unavailable everywhere else.
  //
  // PascalCase and no trailing underscore, deliberately: this is a GL entry
  // point, named as the driver names it, not one of our accessors.
  using PfnEGLImageTargetTexture2DOES = void(GLAD_API_PTR*)(GLenum, void*);
  PfnEGLImageTargetTexture2DOES EGLImageTargetTexture2DOES = nullptr;

  // --- Capability queries (valid after a successful Load) -------------------

  int version_major() const { return version_major_; }
  int version_minor() const { return version_minor_; }
  int max_texture_size() const { return max_texture_size_; }
  int max_texture_units() const { return max_texture_units_; }

  // The driver's own strings, for logs and bug reports. Cheap enough to call
  // per frame, but meant to be read once at startup.
  std::string renderer() const;
  std::string vendor() const;
  std::string version_string() const;
  std::string shading_language_version() const;

  // True when GL's extension list advertises |name| (e.g. "GL_ARB_copy_image").
  // Separate from the per-extension flags glad fills in, because the flags only
  // exist for extensions the generator was told about, while this answers for
  // any name -- including ones the module has no use for yet.
  bool HasExtension(const char* name) const;

  // True for an OpenGL ES context (EGL on mobile, or Mesa's GLES-on-desktop).
  // Matters because GLSL ES is a different dialect from desktop GLSL: the same
  // source will not compile on both, starting with its #version line. See
  // ShaderPrologue() in gl_shaders.h.
  bool is_gles() const { return is_gles_; }

  // Convenience predicates for the features this module knows how to use. Each
  // one is the conjunction of "the extension is there" and "we have the entry
  // point", because either half alone leads to a call that fails.
  bool has_debug_output() const;
  bool has_egl_image() const;
  bool has_copy_image() const;
  bool has_anisotropic_filter() const;
  bool has_srgb_decode() const;

  // Drains the error queue, returning the last error raised (GL_NO_ERROR if the
  // queue was empty). Call after a suspicious sequence rather than on every
  // call: a GL error is a synchronous stall, and GL_NO_ERROR is not free.
  GLenum DrainErrors() const;

 private:
  // glad asks for one symbol at a time and takes a caller-chosen user pointer;
  // that pointer is the NativeGLContext from Load(), which is how the window
  // system stays behind a single interface instead of glad growing a second
  // path to the driver.
  static GLADapiproc ResolveEntryPoint(void* userptr, const char* name);

  GladGLContext context_{};
  int version_major_{0};
  int version_minor_{0};
  int max_texture_size_{0};
  int max_texture_units_{0};
  bool is_gles_{false};
  std::vector<std::string> extensions_;
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_API_H_
