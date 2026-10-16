// Copyright 2026 The avbase authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The pipeline-level half of docs/14 stage 2: a seek lands each source where
// ITS keyframe geometry says, and the pipeline turns the landing DIFFERENCE
// into the per-source playback offset the renderer paces every leg by
// (renderer_impl_reporting.cc). The renderer-level half -- that the offset
// reaches each leg's clock -- lives in renderer_multi_source_unittest.cc;
// this file is where the offset is BORN.
//
// WHY TWO GEOMETRIES. Two sources with identical keyframe intervals land on
// the same position for every seek, the difference is always zero, and every
// assertion below would pass with the derivation deleted. Source 1 therefore
// carries a coarser interval (90 frames = 3 s at 30 fps) than source 0 (30
// frames = 1 s), which is also the realistic case: a second picture rarely
// shares the first one's GOP structure.
//
// No Play(), no sink pumping: the subject is the seek completion path, and
// pulling frames would only add a clock to race against. The waits are the
// fixture's one-sided pumps, which drive S1 while they poll.

#include <atomic>
#include <memory>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/demuxer.h"
#include "media/base/pipeline_status.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/pipeline_impl.h"
#include "tests/support/pipeline_fixture.h"
#include "tests/support/synthetic_demuxer.h"

namespace avbase::media {
namespace {

// 90 frames at 30 fps: a keyframe every 3 s, against source 0's every 1 s.
constexpr int kSecondaryKeyframeInterval = 90;

class PipelineMultiSourceTest : public PipelineTestFixture {
 protected:
  PipelineMultiSourceTest() : PipelineTestFixture(/*ffmpeg_mode=*/false) {}

  // Starts the pipeline with TWO synthetic sources: source 0 from the
  // fixture's spec_, source 1 from the same spec with the coarser keyframe
  // interval. The demuxers stay observable through raw pointers -- the
  // pipeline owns them, and position() is how the test states the landing
  // each one actually reached rather than trusting its own arithmetic about
  // what the seek should have done.
  void StartTwoSourcePipeline() {
    BuildGraph();
    test::SyntheticSpec secondary = spec_;
    secondary.keyframe_interval = kSecondaryKeyframeInterval;
    auto primary = std::make_unique<test::SyntheticDemuxer>(spec_);
    auto second = std::make_unique<test::SyntheticDemuxer>(secondary);
    demuxer0_ = primary.get();
    demuxer1_ = second.get();
    std::vector<std::unique_ptr<Demuxer>> sources;
    sources.push_back(std::move(primary));
    sources.push_back(std::move(second));
    pipeline_->Start(std::move(sources), renderer_factory_.get(),
                     RendererType::kRendererImpl, runner_, &client_);
  }

  void SeekAndWait(base::TimeDelta target) {
    seeked_.store(false);
    pipeline_->Seek(target, base::BindOnce(&PipelineMultiSourceTest::OnSeeked,
                                           base::Unretained(this)));
    ASSERT_TRUE(PumpUntil([this] {
      PumpRound();
      return seeked_.load();
    })) << "seek callback never ran; events:\n"
        << client_.EventLog();
  }

  // Bound as the seek callback, hence public.
  void OnSeeked() { seeked_.store(true); }

  void WaitUntilStarted() {
    ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
        << "pipeline never became ready; events:\n"
        << client_.EventLog();
    ASSERT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                     << client_.EventLog();
  }

  test::SyntheticDemuxer* demuxer0_ = nullptr;
  test::SyntheticDemuxer* demuxer1_ = nullptr;
  std::atomic<bool> seeked_{false};
};

// The core: after a seek, source 1's playback offset is exactly the
// difference between the two landings -- stated both as the concrete value
// the geometry implies and as the general difference, so a hardcoded
// expectation cannot pass by coincidence.
TEST_F(PipelineMultiSourceTest, SeekDerivesTheOffsetFromTheLandingDifference) {
  StartTwoSourcePipeline();
  WaitUntilStarted();

  SeekAndWait(base::Seconds(5));

  // The landings are where each source's own geometry puts them, NOT the
  // requested target: 5 s is frame 150; source 0's interval (30) lands on
  // frame 150 exactly, source 1's (90) on frame 90 (3 s). If the landings
  // were equal the offset would be zero and the assertions below vacuous --
  // which is what this pair of expectations rules out.
  EXPECT_EQ(demuxer0_->position(), base::Seconds(5));
  EXPECT_EQ(demuxer1_->position(), base::Seconds(3));
  EXPECT_EQ(demuxer1_->position(),
            demuxer1_->KeyframeAtOrBefore(base::Seconds(5)));

  // Source 0 is the master timeline: never offset, however the landings
  // differ.
  EXPECT_EQ(av_sync_->source_offset(0), base::TimeDelta());

  // Source 1's offset maps its landing onto source 0's: SourceTime(1, 3 s)
  // must be the master moment 5 s, so both sources show the same moment.
  EXPECT_EQ(av_sync_->source_offset(1), base::Seconds(2));
  EXPECT_EQ(av_sync_->SourceTime(1, base::Seconds(3)), base::Seconds(5));
  EXPECT_EQ(av_sync_->source_offset(1),
            demuxer0_->position() - demuxer1_->position());
}

// A second seek re-derives the offset rather than latching the first: 7 s
// lands source 0 on 7 s and source 1 on 6 s, so the offset must MOVE from
// 2 s to 1 s. A derivation that ran only on the first seek -- or that
// accumulated -- leaves 2 s here.
TEST_F(PipelineMultiSourceTest, ASecondSeeksOffsetIsReDerived) {
  StartTwoSourcePipeline();
  WaitUntilStarted();

  SeekAndWait(base::Seconds(5));
  ASSERT_EQ(av_sync_->source_offset(1), base::Seconds(2));

  SeekAndWait(base::Seconds(7));
  EXPECT_EQ(demuxer0_->position(), base::Seconds(7));
  EXPECT_EQ(demuxer1_->position(), base::Seconds(6));
  EXPECT_EQ(av_sync_->source_offset(1), base::Seconds(1));
}

// A single-source pipeline states no offset: there is no second timeline to
// align to, and the source id the renderer never registered maps
// identically. The coarse interval makes the landing non-trivial (5 s lands
// on 3 s), so a pipeline deriving an offset out of nothing would have
// something to derive it FROM.
TEST_F(PipelineMultiSourceTest, SingleSourceSeeksNoOffset) {
  spec_.keyframe_interval = kSecondaryKeyframeInterval;
  StartPipeline();
  WaitUntilStarted();

  SeekAndWait(base::Seconds(5));

  EXPECT_EQ(av_sync_->source_offset(0), base::TimeDelta());
  EXPECT_EQ(av_sync_->source_offset(1), base::TimeDelta())
      << "a single-source seek stated an offset for a source that does not "
         "exist";
  EXPECT_EQ(av_sync_->SourceTime(1, base::Seconds(3)), base::Seconds(3));
}

// ---------------------------------------------------------------------------
// MUTATION PROOF (docs/14 stage 2, §7.2). The assertions above must TURN RED
// under the following edits, proving the offset is genuinely derived from the
// per-source landings:
//
//   M-C1: in pipeline_impl.cc ApplySourceOffsets, state the AGGREGATE
//         difference instead of the per-source one -- e.g. read
//         composite_->seek_actuals()[0] for every leg. SeekDerivesThe...
//         fails on source 1's offset (it becomes 0, the max against itself).
//         Restore.
//
//   M-C2: invert the subtraction -- actuals[i] - actuals[0]. Both multi-
//         source tests fail on the sign (offset -2 s / -1 s, and
//         SourceTime(1, 3 s) = 1 s instead of 5 s). Restore.
//
//   M-C3: state the offset to the WRONG leg -- SetSourceOffset(0, ...) in
//         place of SetSourceOffset(static_cast<int>(i), ...). Both multi-
//         source tests fail: the offset lands on source 0 (which the
//         controller refuses to shift), so source 1 never receives it and
//         SourceTime(1, 3 s) is 3 s, not 5 s. (A tempting alternative --
//         dropping the size() < 2 guard -- does NOT fail: the loop start at
//         i=1 already skips a single source, so the guard's only real job is
//         the empty-vector bounds check, caught by the code's honest comment
//         rather than by this suite.)
//
//   M-C4: in composite_demuxer.cc, fill every slot with the aggregate.
//         SeekDerivesThe... fails on the offset (0 instead of 2 s). The
//         composite suite's SeekLandingsAreIndexedNotOrdered catches the same
//         edit one layer down (that test asserts the per-index value, not the
//         max, so it goes red where the ordered test would not).
//
// All mutations were run, seen red, reverted, and re-verified green.
// ---------------------------------------------------------------------------

}  // namespace
}  // namespace avbase::media
