// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_QT_QT_GL_CONTEXT_H_
#define AVBASE_PLATFORM_QT_QT_GL_CONTEXT_H_

#include <string>

#include "render/gl/native_gl_context.h"

class QOpenGLContext;

namespace avbase::media {

// NativeGLContext backed by a Qt Quick scene-graph context.
//
// The fourth of these adapters, and the two files next to this one are worth
// comparing with it: SDL2, GLFW and EGL each OWN the swap (an embedder hands
// over a window and avbase paints into it, then calls SwapBuffers), while Qt
// owns the whole frame. A Qt host draws video INSIDE its scene graph, into a
// framebuffer object Qt bound, on a thread Qt runs, and Qt presents once for
// everything in the window. So this adapter is the first one where two of the
// six methods are deliberately inert:
//
//   MakeCurrent()     -- no-op. Qt made the context current before calling the
//                        renderer and will unbind it afterwards; re-making it
//                        here would be a second, redundant bind on a thread
//                        that already holds it.
//   SwapBuffers()     -- no-op. There is no back buffer of ours to swap; Qt
//                        composites this item's framebuffer with the rest of
//                        the scene and presents that. A host that swapped here
//                        would tear a half-drawn window.
//
// That is also why a Qt host cannot use GlPresenter: the presenter's contract
// ends in "blit the canvas to the default framebuffer and swap", which is one
// step past what an item renderer is allowed to do. What a Qt host wants from
// this module is everything before that -- entry points, programs, the YUV
// layer, the quad -- and it drives those itself. See apps/player_qt/
// gl_video_item.cc.
//
// BORROWED semantics, like the other three: the destructor does not touch the
// host's QOpenGLContext.
//
// THREADING: construct and use on the thread that has |context| current (for
// Qt Quick, the scene-graph render thread). A GL context is current on at most
// one thread, and Qt's render thread is the one that holds this.
class QtGLContext final : public gl::NativeGLContext {
 public:
  // |drawable_width| and |drawable_height| are the target's pixel size, which
  // the adapter cannot ask Qt for: it is handed a context, not an item, and
  // QOpenGLContext knows the surface but not which item is being drawn. The
  // renderer passes its framebuffer size, which is the same number after the
  // device pixel ratio.
  QtGLContext(QOpenGLContext* context, int drawable_width, int drawable_height);
  ~QtGLContext() override = default;

  QtGLContext(const QtGLContext&) = delete;
  QtGLContext& operator=(const QtGLContext&) = delete;

  // Resolved through Qt rather than through a platform loader (dlopen /
  // wglGetProcAddress / eglGetProcAddress). Qt has already solved "where are
  // this context's entry points" for its own renderer, including the per-
  // context cases where a symbol is not process-global, so asking it is both
  // shorter and more correct than repeating that work here.
  void* GetProcAddress(const char* name) override;
  bool MakeCurrent() override;
  void ReleaseCurrent() override;
  void SwapBuffers() override;
  void GetDrawableSize(int* width, int* height) override;
  std::string GetError() override;

 private:
  QOpenGLContext* context_;
  int width_;
  int height_;
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_QT_QT_GL_CONTEXT_H_
