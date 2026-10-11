// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/render_perf_stats.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

TEST(UploadPathNameTest, EveryEnumeratorHasAName) {
  EXPECT_STREQ(UploadPathName(UploadPath::kNone), "none");
  EXPECT_STREQ(UploadPathName(UploadPath::kCpuSync), "cpu_sync");
  EXPECT_STREQ(UploadPathName(UploadPath::kPbo), "pbo");
  EXPECT_STREQ(UploadPathName(UploadPath::kHwZeroCopy), "hw_zero_copy");
}

TEST(DurationEwmaTest, FirstSampleSeedsTheAverage) {
  DurationEwma ewma;
  // The seed matters: an EMA started at 0 would report ~1.25 ms after the
  // first 10 ms frame instead of 10 ms, making the first second of playback
  // look cheaper than it is.
  ewma.Add(10.0);
  EXPECT_DOUBLE_EQ(ewma.average_ms(), 10.0);
  EXPECT_DOUBLE_EQ(ewma.max_ms(), 10.0);
  EXPECT_EQ(ewma.samples(), 1u);
}

TEST(DurationEwmaTest, ConvergesTowardAConstantRate) {
  DurationEwma ewma;
  for (int i = 0; i < 100; ++i) {
    ewma.Add(4.0);
  }
  EXPECT_NEAR(ewma.average_ms(), 4.0, 1e-9);
  EXPECT_DOUBLE_EQ(ewma.max_ms(), 4.0);
  EXPECT_EQ(ewma.samples(), 100u);
}

TEST(DurationEwmaTest, MovesPartWayTowardASpike) {
  DurationEwma ewma;
  ewma.Add(10.0);
  ewma.Add(20.0);
  // alpha = 1/8: 10 + (20 - 10) * 0.125 = 11.25, and 0.125 is exact in binary.
  EXPECT_DOUBLE_EQ(ewma.average_ms(), 11.25);
}

TEST(DurationEwmaTest, MaxRemembersTheWorstSampleTheAverageHides) {
  DurationEwma ewma;
  ewma.Add(1.0);
  ewma.Add(30.0);
  ewma.Add(1.0);
  // The average is dominated by the steady state; the max is what a stutter
  // report needs, so it must survive later cheap frames.
  EXPECT_LT(ewma.average_ms(), 5.0);
  EXPECT_DOUBLE_EQ(ewma.max_ms(), 30.0);
}

TEST(DurationEwmaTest, ResetClearsEverything) {
  DurationEwma ewma;
  ewma.Add(7.0);
  ewma.Reset();
  EXPECT_DOUBLE_EQ(ewma.average_ms(), 0.0);
  EXPECT_DOUBLE_EQ(ewma.max_ms(), 0.0);
  EXPECT_EQ(ewma.samples(), 0u);
  // A sample after Reset seeds again rather than blending with the old value.
  ewma.Add(3.0);
  EXPECT_DOUBLE_EQ(ewma.average_ms(), 3.0);
}

TEST(RenderPerfStatsTest, JsonCarriesEveryStageAndThePath) {
  RenderPerfStats stats;
  stats.upload.Add(0.25);
  stats.compose.Add(0.5);
  stats.blit.Add(0.125);
  stats.present.Add(16.0);
  stats.present.Add(24.0);
  stats.upload_path = UploadPath::kCpuSync;

  const std::string json = stats.ToJson();
  EXPECT_NE(json.find("\"upload_ms\":0.250"), std::string::npos);
  EXPECT_NE(json.find("\"compose_ms\":0.500"), std::string::npos);
  EXPECT_NE(json.find("\"blit_ms\":0.125"), std::string::npos);
  // 16 + (24 - 16) * 0.125 = 17, all three values exact in binary.
  EXPECT_NE(json.find("\"present_ms\":17.000"), std::string::npos);
  EXPECT_NE(json.find("\"max_present_ms\":24.000"), std::string::npos);
  EXPECT_NE(json.find("\"upload_path\":\"cpu_sync\""), std::string::npos);
}

// The blit is the pass the off-screen canvas added, so its absence has to be
// distinguishable from a zero cost: a sink that never composes (the SDL2 2D
// path) reports 0, while a path that does and is measured reports a small
// positive number. This asserts the JSON keeps them apart.
TEST(RenderPerfStatsTest, AnUncomposedBlitIsZeroAndNotMissing) {
  RenderPerfStats stats;
  const std::string json = stats.ToJson();
  EXPECT_NE(json.find("\"blit_ms\":0.000"), std::string::npos);
}

}  // namespace
}  // namespace avbase::media
