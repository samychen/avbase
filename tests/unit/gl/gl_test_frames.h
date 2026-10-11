// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Shared fixtures for the GL pixel tests: build a frame of a known colour,
// compose it, read the result back.
//
// These live in one header on purpose. Every one of them encodes a convention
// that is invisible when it is wrong -- glReadPixels counts y from the bottom,
// YV12's plane 1 is V while the shader's unit 1 is U, NV12 has no plane 2 --
// and three private copies would be three chances to get one of them subtly
// different, in tests whose whole job is to catch subtleties.

#ifndef AVBASE_TESTS_UNIT_GL_GL_TEST_FRAMES_H_
#define AVBASE_TESTS_UNIT_GL_GL_TEST_FRAMES_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "media/base/video_frame.h"
#include "render/gl/gl_api.h"

// Deliberately avbase::test, the same namespace gl_test_context.h uses. A
// second test namespace nested under avbase::media would shadow the "test::"
// spelling every GL test already writes for GlTestWindow, and the shadowing
// surfaces as an error about the fixture class rather than about namespaces.
namespace avbase::test {

// A rendered pixel, as the three channels the tests compare.
struct Rgb {
  int r{0};
  int g{0};
  int b{0};
  bool operator==(const Rgb& other) const {
    return r == other.r && g == other.g && b == other.b;
  }
};

inline std::string Describe(const Rgb& c) {
  return "(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
         std::to_string(c.b) + ")";
}

// Fills |frame| so that every pixel has the same logical (Y, U, V), whatever
// the format's plane layout is. Logical means the video's U and V, not the
// plane index: YV12's plane 1 holds V, and NV12's plane 1 holds both words
// interleaved, and this is the helper that knows that.
inline bool Fill(const base::scoped_refptr<media::VideoFrame>& frame, uint8_t y,
                 uint8_t u, uint8_t v) {
  const int width = frame->coded_size().width;
  const int height = frame->coded_size().height;
  const std::span<uint8_t> luma =
      frame->mutable_data(media::VideoFrame::kYPlane);
  if (luma.empty()) {
    return false;
  }
  const int32_t y_stride = frame->stride(media::VideoFrame::kYPlane);
  for (int row = 0; row < height; ++row) {
    std::fill_n(luma.begin() + static_cast<size_t>(row) * y_stride, width, y);
  }

  if (frame->format() == media::VideoFormat::kNV12) {
    const std::span<uint8_t> uv =
        frame->mutable_data(media::VideoFrame::kUPlane);
    const int32_t uv_stride = frame->stride(media::VideoFrame::kUPlane);
    if (uv.empty()) {
      return false;
    }
    for (int row = 0; row < height / 2; ++row) {
      uint8_t* dst = uv.data() + static_cast<size_t>(row) * uv_stride;
      for (int col = 0; col < width / 2; ++col) {
        dst[col * 2] = u;
        dst[col * 2 + 1] = v;
      }
    }
    return true;
  }

  // I420 and YV12: one byte per sample, plane 1 is V for YV12.
  const media::VideoFrame::Plane u_plane =
      frame->format() == media::VideoFormat::kYV12 ? media::VideoFrame::kVPlane
                                                   : media::VideoFrame::kUPlane;
  const media::VideoFrame::Plane v_plane =
      frame->format() == media::VideoFormat::kYV12 ? media::VideoFrame::kUPlane
                                                   : media::VideoFrame::kVPlane;
  for (const auto& [plane, value] :
       {std::pair<media::VideoFrame::Plane, uint8_t>{u_plane, u},
        std::pair<media::VideoFrame::Plane, uint8_t>{v_plane, v}}) {
    const std::span<uint8_t> chroma = frame->mutable_data(plane);
    if (chroma.empty()) {
      return false;
    }
    const int32_t stride = frame->stride(plane);
    for (int row = 0; row < height / 2; ++row) {
      std::fill_n(chroma.begin() + static_cast<size_t>(row) * stride, width / 2,
                  value);
    }
  }
  return true;
}

// An owned frame of |format| whose every pixel is the logical colour (y, u, v).
// Null if the frame or a plane could not be created.
inline base::scoped_refptr<media::VideoFrame>
MakeSolidFrame(media::VideoFormat format, int width, int height, uint8_t y,
               uint8_t u, uint8_t v) {
  const media::Size size{width, height};
  base::scoped_refptr<media::VideoFrame> frame =
      media::VideoFrame::CreateBlackFrame(
          format, size, size, media::Rational{1, 1}, base::TimeDelta(),
          base::TimeDelta(), 0);
  if (!frame || !Fill(frame, y, u, v)) {
    return nullptr;
  }
  return frame;
}

// An owned frame whose TOP half is |top_luma| and bottom half |bottom_luma|,
// with neutral chroma throughout.
//
// This is the frame that makes orientation assertable: a flip moves the two
// halves past each other, and the difference between the halves is what the
// assertion reads. A uniform frame passes whether the path flips or not, which
// is why every orientation test uses this shape and not MakeSolidFrame.
inline base::scoped_refptr<media::VideoFrame>
MakeTopHalfFrame(media::VideoFormat format, int width, int height,
                 uint8_t top_luma, uint8_t bottom_luma,
                 uint8_t neutral_chroma = 128) {
  base::scoped_refptr<media::VideoFrame> frame = MakeSolidFrame(
      format, width, height, top_luma, neutral_chroma, neutral_chroma);
  if (!frame) {
    return nullptr;
  }
  const std::span<uint8_t> luma =
      frame->mutable_data(media::VideoFrame::kYPlane);
  const int32_t stride = frame->stride(media::VideoFrame::kYPlane);
  if (luma.empty()) {
    return nullptr;
  }
  for (int row = height / 2; row < height; ++row) {
    std::fill_n(luma.begin() + static_cast<size_t>(row) * stride, width,
                bottom_luma);
  }
  return frame;
}

// One RGBA pixel out of the CURRENTLY BOUND framebuffer.
//
// glReadPixels counts y from the BOTTOM of the target, so y = height - 1 is
// the top row. Every test that names a row has to hold that in mind, which is
// why it is stated here rather than assumed.
//
// |read_buffer| defaults to the colour attachment, which is what an
// off-screen target is read through; a test that stops before SwapBuffers and
// reads the window passes GL_BACK instead.
inline Rgb ReadPixel(const gl::GlApi& api, int x, int y,
                     unsigned int read_buffer = GL_COLOR_ATTACHMENT0) {
  std::vector<uint8_t> pixel(4, 0);
  api.context().ReadBuffer(read_buffer);
  api.context().PixelStorei(GL_PACK_ALIGNMENT, 1);
  api.context().ReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
  return Rgb{pixel[0], pixel[1], pixel[2]};
}

}  // namespace avbase::test

#endif  // AVBASE_TESTS_UNIT_GL_GL_TEST_FRAMES_H_
