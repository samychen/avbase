// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The last hop: PipelineStatistics -> PlaybackStats -> JSON.
//
// WHY THIS FILE EXISTS. The display layer's counters (frames refused for want
// of a surface, submit failures, the measured refresh rate) were counted for
// rounds and reachable by nobody. Fixing that took a field in each of three
// structs and one line per struct that copies it -- and a field that reaches
// the middle struct but not the last one is exactly the shape of the original
// defect, one layer in. These tests pin the two hops that are pure data, so a
// future edit that adds a field and forgets to copy it fails here instead of
// in the field.
//
// WHAT IS NOT CLAIMED. Nothing about the renderer side (that the sink's
// numbers reach PipelineStatistics at all) -- tests/unit/media_filters/
// renderer_sink_stats_unittest.cc owns that with a real renderer. Nor the
// event delivery: OnStatisticsUpdate is called directly here, so a broken
// statistics tick is out of frame.

#include "player/public/playback_stats.h"

#include <cstdint>
#include <string>

#include "gtest/gtest.h"
#include "media/base/renderer_client.h"
#include "player/player_impl.h"
#include "player/public/player_config.h"

namespace avbase {
namespace {

// The mapping is a copy of five fields, but it is a copy that has to be
// written by hand: the two structs share names by convention, not by type, and
// nothing in the compiler connects them. Values are distinct on purpose so a
// crossed wire (submit failures landing in the drop counter) cannot pass.
TEST(PlaybackStatsMappingTest, TheDisplayLayersCountersReachTheSnapshot) {
  PlayerImpl impl(PlayerConfig(), nullptr);
  media::PipelineStatistics stats;
  stats.video_frames_presented = 120;
  stats.video_frames_dropped = 3;
  stats.video_frames_repeated = 7;
  stats.video_frames_dropped_by_sink = 9;
  stats.video_frames_submit_failures = 2;
  stats.measured_refresh_hz = 59.94;
  // The four reasons the compositor dropped, each with its own value: they
  // partition |frames_dropped| and a crossed wire would show up here first.
  stats.video_frames_dropped_late = 13;
  stats.video_frames_dropped_fps = 5;
  stats.video_frames_dropped_stale_serial = 17;
  stats.video_frames_dropped_accurate_seek = 19;

  // Through the interface the PIPELINE uses, not through PlayerImpl: the
  // override is declared in a private section, and going via Pipeline::Client
  // is the same virtual call a real statistics tick makes -- a test-only
  // setter on the product class would test a path production never takes.
  media::Pipeline::Client* client = &impl;
  client->OnStatisticsUpdate(stats);
  const PlaybackStats out = impl.GetPlaybackStats();

  // The pipeline's own counts still land where they did.
  EXPECT_EQ(out.frames_presented, 120u);
  EXPECT_EQ(out.frames_dropped, 3u);
  EXPECT_EQ(out.frames_repeated, 7u);
  // And the display layer's counts arrive separately.
  EXPECT_EQ(out.frames_dropped_by_sink, 9u);
  EXPECT_EQ(out.frames_submit_failures, 2u);
  EXPECT_DOUBLE_EQ(out.measured_refresh_hz, 59.94);
  // And so do the reasons, each on its own field. frames_dropped_late used to
  // exist on PlaybackStats with nothing ever assigning it: a counter that
  // always reads zero is read as evidence that nothing was late.
  EXPECT_EQ(out.frames_dropped_late, 13u);
  EXPECT_EQ(out.frames_dropped_fps, 5u);
  EXPECT_EQ(out.frames_dropped_stale_serial, 17u);
  EXPECT_EQ(out.frames_dropped_accurate_seek, 19u);
}

// ToJson is a hand-written string builder, so a new field is invisible to a
// log-scraping consumer unless somebody writes its line. These are the exact
// keys docs/05 table 6 and a host's dashboard are written against.
TEST(PlaybackStatsJsonTest, TheJsonCarriesTheDisplayLayersCounters) {
  PlaybackStats stats;
  stats.frames_dropped_by_sink = 11;
  stats.frames_submit_failures = 4;
  stats.measured_refresh_hz = 59.94;
  stats.frames_dropped_late = 13;
  stats.frames_dropped_fps = 5;
  stats.frames_dropped_stale_serial = 17;
  stats.frames_dropped_accurate_seek = 19;

  const std::string json = stats.ToJson();
  EXPECT_NE(json.find("\"frames_dropped_by_sink\":11"), std::string::npos);
  EXPECT_NE(json.find("\"frames_submit_failures\":4"), std::string::npos);
  EXPECT_NE(json.find("\"measured_refresh_hz\":59.940"), std::string::npos);
  EXPECT_NE(json.find("\"frames_dropped_late\":13"), std::string::npos);
  EXPECT_NE(json.find("\"frames_dropped_fps\":5"), std::string::npos);
  EXPECT_NE(json.find("\"frames_dropped_stale_serial\":17"),
            std::string::npos);
  EXPECT_NE(json.find("\"frames_dropped_accurate_seek\":19"),
            std::string::npos);
  // A counter that never moved is still reported: "absent" and "zero" are
  // different answers to a consumer parsing this, and only one of them is true.
  const std::string empty = PlaybackStats().ToJson();
  EXPECT_NE(empty.find("\"frames_dropped_by_sink\":0"), std::string::npos);
  EXPECT_NE(empty.find("\"frames_submit_failures\":0"), std::string::npos);
}

}  // namespace
}  // namespace avbase
