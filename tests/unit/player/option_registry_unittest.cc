// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/option_registry.h"

#include "gtest/gtest.h"

namespace avbase {
namespace {

const OptionRegistry& Registry() {
  return OptionRegistry::GetInstance();
}

// The four buffer keys were dead no-ops until BufferConfig was wired into
// BufferController / the demuxer queue / the PipelineImpl gate. These tests pin
// that the string keys now reach the typed fields (and back).

TEST(OptionRegistryBufferTest, PacketBufferingSetsEnabled) {
  PlayerConfig config;
  // ijkplayer passes "0"/"1"; accept both int and string forms.
  EXPECT_TRUE(
      Registry()
          .SetInt(&config, OptionCategory::kPlayer, "packet-buffering", 0)
          .has_value());
  EXPECT_FALSE(config.buffer.enabled);
  EXPECT_TRUE(
      Registry()
          .SetString(&config, OptionCategory::kPlayer, "packet-buffering", "1")
          .has_value());
  EXPECT_TRUE(config.buffer.enabled);
}

TEST(OptionRegistryBufferTest, InfbufSetsUnlimited) {
  PlayerConfig config;
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "infbuf", 1)
                  .has_value());
  EXPECT_TRUE(config.buffer.unlimited);
}

TEST(OptionRegistryBufferTest, MaxBufferSizeSetsBytes) {
  PlayerConfig config;
  EXPECT_TRUE(
      Registry()
          .SetInt(&config, OptionCategory::kPlayer, "max-buffer-size", 4096)
          .has_value());
  EXPECT_EQ(config.buffer.max_bytes, 4096u);
}

TEST(OptionRegistryBufferTest, MinFramesSetsFloor) {
  PlayerConfig config;
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "min-frames", 12)
                  .has_value());
  EXPECT_EQ(config.buffer.min_frames, 12u);
}

TEST(OptionRegistryBufferTest, OutOfRangeMaxBufferIsRejected) {
  PlayerConfig config;
  const Status s = Registry().SetInt(&config, OptionCategory::kPlayer,
                                     "max-buffer-size", -1);
  EXPECT_FALSE(s.has_value()) << "a negative byte cap must be refused";
}

TEST(OptionRegistryBufferTest, UnknownStringValueIsRejected) {
  PlayerConfig config;
  const Status s = Registry().SetString(&config, OptionCategory::kPlayer,
                                        "packet-buffering", "maybe");
  EXPECT_FALSE(s.has_value()) << "a non-boolean string must be refused";
}

TEST(OptionRegistryBufferTest, DumpRoundTripsTheKeys) {
  PlayerConfig config;
  const auto dump = Registry().Dump(config, OptionCategory::kPlayer);
  EXPECT_EQ(dump.count("packet-buffering"), 1u);
  EXPECT_EQ(dump.count("max-buffer-size"), 1u);
  EXPECT_EQ(dump.count("min-frames"), 1u);
  EXPECT_EQ(dump.count("infbuf"), 1u);
}

// ---- ValidateConfig: min-frames vs the packet-queue depth ------------------

TEST(ValidateConfigBufferTest, MinFramesBeyondQueueDepthIsFlagged) {
  PlayerConfig config;
  config.buffer.min_frames = 300;  // queue is 256 deep
  const std::vector<ConfigIssue> issues = ValidateConfig(config);
  bool found = false;
  for (const ConfigIssue& issue : issues) {
    if (issue.field == "buffer.min_frames") {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "a min-frames floor the queue can never reach is a "
                        "deadlock and must be reported";
}

TEST(ValidateConfigBufferTest, MinFramesBeyondDepthAllowedWhenGateDisabled) {
  PlayerConfig config;
  config.buffer.enabled = false;  // packet-buffering off => gate never waits
  config.buffer.min_frames = 300;
  const std::vector<ConfigIssue> issues = ValidateConfig(config);
  for (const ConfigIssue& issue : issues) {
    EXPECT_NE(issue.field, "buffer.min_frames");
  }
}

TEST(ValidateConfigBufferTest, DefaultConfigHasNoIssues) {
  PlayerConfig config;
  EXPECT_TRUE(ValidateConfig(config).empty());
}

}  // namespace
}  // namespace avbase
