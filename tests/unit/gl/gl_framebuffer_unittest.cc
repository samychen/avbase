// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// GlFramebuffer's contract: the same size is a no-op, a new size reallocates,
// and a rejected size leaves the target unallocated rather than half-built.
//
// The idempotence half matters beyond tidiness. The canvas follows the drawable
// when no size was requested, so it is re-requested every frame; a "no-op" that
// actually reallocated would destroy and recreate two GL objects per frame.

#include <memory>
#include <string>
#include <vector>

#include "gl_test_context.h"
#include "gtest/gtest.h"
#include "render/gl/gl_api.h"
#include "render/gl/gl_framebuffer.h"

namespace avbase::gl {
namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;

// The framebuffer that is currently bound, which is where the tests draw.
constexpr int kNoTexture = 0;

class GlFramebufferTest : public ::testing::Test {
 protected:
  void SetUp() override {
    api_ = std::make_unique<GlApi>();
    std::string window_error;
    window_ = test::GlTestWindow::Create(kWidth, kHeight, &window_error);
    if (!window_) {
      GTEST_SKIP() << window_error;
    }
    ASSERT_TRUE(window_->context()->MakeCurrent());
    std::string load_error;
    ASSERT_TRUE(api_->Load(window_->context(), &load_error)) << load_error;
    framebuffer_ = GlFramebuffer::Create(*api_);
    ASSERT_TRUE(framebuffer_);
  }

  void TearDown() override {
    // GL objects belong to the context; drop them while it is still current.
    framebuffer_.reset();
    api_.reset();
  }

  // Fills the bound target with red, so a later read can tell "this is still
  // the same allocation with its contents" from "this is a fresh texture".
  void ClearToRed() {
    api_->context().ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    api_->context().Clear(GL_COLOR_BUFFER_BIT);
  }

  // Red channel at the top-left corner of the bound target.
  int ReadCornerRed() {
    std::vector<uint8_t> pixel(4, 0);
    api_->context().ReadBuffer(GL_COLOR_ATTACHMENT0);
    api_->context().PixelStorei(GL_PACK_ALIGNMENT, 1);
    api_->context().ReadPixels(0, framebuffer_->height() - 1, 1, 1, GL_RGBA,
                               GL_UNSIGNED_BYTE, pixel.data());
    return pixel[0];
  }

  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlApi> api_;
  std::unique_ptr<GlFramebuffer> framebuffer_;
};

TEST_F(GlFramebufferTest, EnsureAllocatesACompleteTarget) {
  std::string error;
  ASSERT_TRUE(framebuffer_->Ensure(kWidth, kHeight, &error)) << error;
  EXPECT_TRUE(framebuffer_->valid());
  EXPECT_NE(framebuffer_->id(), kNoTexture);
  EXPECT_NE(framebuffer_->color_texture(), kNoTexture);
  EXPECT_EQ(framebuffer_->width(), kWidth);
  EXPECT_EQ(framebuffer_->height(), kHeight);
}

// The no-op half, and the reason it is asserted through the CONTENT: GL is free
// to hand the same name back after a delete, so comparing names proves nothing
// in either direction (the first version of this test did exactly that and
// passed against a deliberately reallocating implementation). Contents, on the
// other hand, only survive if nothing was destroyed.
TEST_F(GlFramebufferTest, TheSameSizeKeepsTheRenderedContents) {
  std::string error;
  ASSERT_TRUE(framebuffer_->Ensure(kWidth, kHeight, &error)) << error;
  framebuffer_->Bind();
  ClearToRed();
  ASSERT_EQ(ReadCornerRed(), 255) << "sanity: the clear must be readable";

  ASSERT_TRUE(framebuffer_->Ensure(kWidth, kHeight, &error)) << error;
  framebuffer_->Bind();
  EXPECT_EQ(ReadCornerRed(), 255)
      << "the same size must reuse the target, not recreate it";
}

// Bind() carrying the viewport is the part that stops a resize from drawing
// into a stale region, so the viewport is asserted rather than assumed.
TEST_F(GlFramebufferTest, ANewSizeIsHonouredAndRepointsTheViewport) {
  std::string error;
  ASSERT_TRUE(framebuffer_->Ensure(kWidth, kHeight, &error)) << error;

  const int new_width = kWidth / 2;
  const int new_height = kHeight / 2;
  ASSERT_TRUE(framebuffer_->Ensure(new_width, new_height, &error)) << error;
  EXPECT_EQ(framebuffer_->width(), new_width);
  EXPECT_EQ(framebuffer_->height(), new_height);

  framebuffer_->Bind();
  // Sanity: Bind() must leave OUR framebuffer bound, not the default one.
  int bound = 0;
  api_->context().GetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
  EXPECT_EQ(static_cast<unsigned int>(bound), framebuffer_->id());

  int viewport[4] = {0, 0, 0, 0};
  api_->context().GetIntegerv(GL_VIEWPORT, viewport);
  EXPECT_EQ(viewport[2], new_width);
  EXPECT_EQ(viewport[3], new_height);

  // And the new target is writable at its own size.
  ClearToRed();
  EXPECT_EQ(ReadCornerRed(), 255);
}

TEST_F(GlFramebufferTest, ARejectedSizeLeavesTheTargetEmpty) {
  std::string error;
  EXPECT_FALSE(framebuffer_->Ensure(0, kHeight, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(framebuffer_->valid());
  EXPECT_EQ(framebuffer_->color_texture(), kNoTexture);
}

}  // namespace
}  // namespace avbase::gl
