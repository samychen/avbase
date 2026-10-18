// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/qt/qt_gl_context.h"

#include <QOpenGLContext>
#include <QSurface>

namespace avbase::media {

QtGLContext::QtGLContext(QOpenGLContext* context, int drawable_width,
                         int drawable_height)
    : context_(context), width_(drawable_width), height_(drawable_height) {}

void* QtGLContext::GetProcAddress(const char* name) {
  if (context_ == nullptr) {
    return nullptr;
  }
  // QFunctionPointer is void(*)(); converting a function pointer to void* is
  // the same technically-UB-looking step every GL loader performs, because
  // that is the shape the GL entry-point APIs are specified in. It is the only
  // conversion available and every driver relies on it.
  return reinterpret_cast<void*>(context_->getProcAddress(name));
}

bool QtGLContext::MakeCurrent() {
  // Inert by design -- see the header. Reported as success because "the
  // context is current on this thread" is what the caller asked for and what
  // is already true; returning false would make a harmless no-op look like a
  // failure to anything that checks.
  return true;
}

void QtGLContext::ReleaseCurrent() {}

void QtGLContext::SwapBuffers() {}

void QtGLContext::GetDrawableSize(int* width, int* height) {
  if (width != nullptr) {
    *width = width_;
  }
  if (height != nullptr) {
    *height = height_;
  }
}

std::string QtGLContext::GetError() {
  return std::string();
}

}  // namespace avbase::media
