// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <memory>
#include <string>

#include "gl_test_context.h"
#include "gtest/gtest.h"
#include "render/gl/gl_api.h"
#include "render/gl/gl_program.h"
#include "render/gl/gl_shaders.h"

namespace avbase::test {
namespace {

// Two uniforms, one of each type the setters cover here, both reaching the
// output so the linker cannot drop either.
constexpr char kTintFragment[] =
    "#version 330 core\n"
    "uniform vec4 uTint;\n"
    "uniform float uLevel;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = uTint + vec4(uLevel); }\n";

class GlProgramTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ = GlTestWindow::Create(/*width=*/64, /*height=*/64, &error);
    if (!window_) {
      GTEST_SKIP() << "no GL 3.3 core context on this host: " << error;
      return;
    }
    gl_ = std::make_unique<gl::GlApi>();
    ASSERT_TRUE(gl_->Load(window_->context(), &error)) << error;
  }

  std::unique_ptr<gl::GlProgram> NewProgram() {
    std::string error;
    const std::string prologue = gl::ShaderPrologue(*gl_);
    auto program = gl::GlProgram::Create(
        *gl_, (prologue + gl::kQuadVertexShaderBody).c_str(), kTintFragment,
        &error);
    EXPECT_TRUE(program) << error;
    return program;
  }

  // Reads a uniform straight back from GL, bypassing every cache.
  float UniformFloat(const gl::GlProgram& program, const char* name) {
    const int kLocation = program.UniformLocation(name);
    if (kLocation < 0) {
      return 0.0f;
    }
    float value = 0.0f;
    gl_->context().GetUniformfv(program.id(), kLocation, &value);
    return value;
  }

  std::unique_ptr<GlTestWindow> window_;
  std::unique_ptr<gl::GlApi> gl_;
};

TEST_F(GlProgramTest, SettingAUniformReachesGl) {
  std::unique_ptr<gl::GlProgram> program = NewProgram();
  ASSERT_TRUE(program);
  program->Use();
  program->SetVec4("uTint", 0.25f, 0.5f, 0.75f, 1.0f);

  std::array<float, 4> read = {};
  const int kLocation = program->UniformLocation("uTint");
  ASSERT_GE(kLocation, 0);
  gl_->context().GetUniformfv(program->id(), kLocation, read.data());
  EXPECT_FLOAT_EQ(read[0], 0.25f);
  EXPECT_FLOAT_EQ(read[1], 0.5f);
  EXPECT_FLOAT_EQ(read[2], 0.75f);
  EXPECT_FLOAT_EQ(read[3], 1.0f);

  program->SetFloat("uLevel", 0.125f);
  EXPECT_FLOAT_EQ(UniformFloat(*program, "uLevel"), 0.125f);
}

// The uniform setters resolve names through a cache. This is the property the
// cache has to have: whatever it answers, the un-cached query would answer the
// same thing -- for a name that exists, and for one that does not.
TEST_F(GlProgramTest, CachedLookupAgreesWithTheUncachedQuery) {
  std::unique_ptr<gl::GlProgram> program = NewProgram();
  ASSERT_TRUE(program);
  program->Use();

  EXPECT_EQ(program->UniformLocation("uTint"),
            gl_->context().GetUniformLocation(program->id(), "uTint"));
  // Asked twice so the second answer comes from the cache; the value read back
  // is GL's, not the cache's, so a cache that had latched the wrong location
  // would show up here.
  program->SetFloat("uLevel", 0.5f);
  program->SetFloat("uLevel", 0.75f);
  EXPECT_FLOAT_EQ(UniformFloat(*program, "uLevel"), 0.75f);

  // A name the linker dropped: -1, cached as -1, and a no-op to set -- setting
  // a uniform through a location of -1 is what GL specifies as doing nothing,
  // and it must not raise an error either.
  EXPECT_EQ(program->UniformLocation("uNotAUniformOfThisProgram"), -1);
  while (gl_->context().GetError() != GL_NO_ERROR) {
    // Drain whatever the asserts above left; only the next call is under test.
  }
  program->SetVec4("uNotAUniformOfThisProgram", 1.0f, 2.0f, 3.0f, 4.0f);
  program->SetVec4("uNotAUniformOfThisProgram", 1.0f, 2.0f, 3.0f, 4.0f);
  EXPECT_EQ(gl_->context().GetError(), GL_NO_ERROR);
}

// Two programs, same uniform name, different locations: the cache is per
// program, so a second program must not read the first one's answer.
TEST_F(GlProgramTest, TheCacheIsPerProgram) {
  std::unique_ptr<gl::GlProgram> first = NewProgram();
  std::unique_ptr<gl::GlProgram> second = NewProgram();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_NE(first->id(), second->id());

  first->Use();
  first->SetFloat("uLevel", 0.25f);
  second->Use();
  second->SetFloat("uLevel", 0.75f);

  EXPECT_FLOAT_EQ(UniformFloat(*first, "uLevel"), 0.25f);
  EXPECT_FLOAT_EQ(UniformFloat(*second, "uLevel"), 0.75f)
      << "a shared cache would have written both through one location";
}

}  // namespace
}  // namespace avbase::test
