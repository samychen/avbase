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
//
// The file also carries the stage-4 runtime track-selection tests: what the
// pipeline does when a host selects a track whose aggregate index belongs to
// a SECONDARY source (video/audio refused before routing; text selected and
// delivered tagged with its source).

#include <atomic>
#include <memory>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/demuxer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/pipeline_status.h"
#include "media/base/timed_text.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/pipeline_impl.h"
#include "tests/support/fake_demuxer_stream.h"
#include "tests/support/pipeline_fixture.h"
#include "tests/support/synthetic_demuxer.h"
#include "tests/support/text_fake_demuxer.h"

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

  // Starts the pipeline with a synthetic video+audio source 0 and a TEXT-ONLY
  // source 1 (docs/14 stage 4): the shape whose subtitle track only the
  // aggregate numbering can address. The text factory comes from the fixture
  // (with_text_factory_ must be set before BuildGraph, which this does), and
  // the text source stays observable so a test can script a cue into it.
  void StartPipelineWithTextSecondSource() {
    with_text_factory_ = true;
    BuildGraph();
    auto primary = std::make_unique<test::SyntheticDemuxer>(spec_);
    auto second = std::make_unique<test::TextFakeDemuxer>(/*first_index=*/0);
    text_demuxer_ = second.get();
    std::vector<std::unique_ptr<Demuxer>> sources;
    sources.push_back(std::move(primary));
    sources.push_back(std::move(second));
    pipeline_->Start(std::move(sources), renderer_factory_.get(),
                     RendererType::kRendererImpl, runner_, &client_);
  }

  // One track select through the pipeline, waited out. The select is POSTED
  // to the media sequence, so the callback arrives during the pump. Dispatches
  // through the typed public wrappers (SelectTrack itself is private).
  PipelineStatus SelectAndWait(DemuxerStreamType type, int index) {
    std::atomic<bool> done{false};
    PipelineStatus status = PipelineStatus::kOk;
    auto cb = base::BindOnce(
        [](std::atomic<bool>* d, PipelineStatus* s, PipelineStatus v) {
          d->store(true);
          *s = v;
        },
        &done, &status);
    switch (type) {
    case DemuxerStreamType::kVideo:
      pipeline_->SelectVideoTrack(index, std::move(cb));
      break;
    case DemuxerStreamType::kAudio:
      pipeline_->SelectAudioTrack(index, std::move(cb));
      break;
    case DemuxerStreamType::kText:
      pipeline_->SelectTextTrack(index, std::move(cb));
      break;
    default:
      ADD_FAILURE() << "no public select wrapper for this stream type";
      return status;
    }
    EXPECT_TRUE(PumpUntil([&done] { return done.load(); }))
        << "select callback never ran; events:\n"
        << client_.EventLog();
    return status;
  }

  test::SyntheticDemuxer* demuxer0_ = nullptr;
  test::SyntheticDemuxer* demuxer1_ = nullptr;
  test::TextFakeDemuxer* text_demuxer_ = nullptr;
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

// The statistics tick is a PUSH with two of its fields REPLACED: no single
// source can answer for the whole file, so the composite owns bytes_read and
// seek_count and PipelineImpl folds them in on the way out
// (pipeline_impl_host.cc). Round 100 deleted the pull-side copy of these
// numbers, which was the last reader that could notice a dropped fold; this
// assertion is now the only one.
TEST_F(PipelineMultiSourceTest, TheStatisticsTickCarriesTheCompositeCounters) {
  StartTwoSourcePipeline();
  WaitUntilStarted();
  SeekAndWait(base::Seconds(5));

  // The expectation is the same expression CompositeDemuxer::GetStats() sums,
  // so this states "the fold happened", not "the composite adds correctly"
  // (composite_demuxer_unittest covers that). Zero would make the two
  // indistinguishable, hence the guard.
  const uint64_t child_seeks =
      demuxer0_->GetStats().seek_count + demuxer1_->GetStats().seek_count;
  ASSERT_GT(child_seeks, 0u)
      << "no seek reached the sources; a fold and a passthrough would look "
         "the same";

  PipelineStatistics from_renderer;
  from_renderer.seek_count = 1u << 20;  // a value nobody can produce honestly
  from_renderer.video_frames_presented = 7;

  std::atomic<bool> delivered{false};
  // Through RendererClient, which is how the tick really arrives: the
  // pipeline's override is private (only its renderer may report to it), and
  // a fake renderer would have to be pumped for a real 1 s tick.
  runner_->PostTask(FROM_HERE,
                    base::BindOnce(
                        [](RendererClient* c, const PipelineStatistics& s,
                           std::atomic<bool>* done) {
                          c->OnStatisticsUpdate(s);
                          done->store(true);
                        },
                        static_cast<RendererClient*>(pipeline_.get()),
                        from_renderer, &delivered));
  ASSERT_TRUE(PumpUntil([&delivered] { return delivered.load(); }))
      << "the tick never reached the media sequence";

  const PipelineStatistics out = client_.stats();
  EXPECT_EQ(out.seek_count, child_seeks)
      << "the composite's seek count did not replace the renderer's";
  // Fields the composite does NOT own pass through untouched. Without this
  // pair, a pipeline that dropped the tick entirely would fail the line above
  // for the right reason and the one below for none at all.
  EXPECT_EQ(out.video_frames_presented, 7u);
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
// Runtime track selection over the aggregate index space (docs/14 stage 4).
// ---------------------------------------------------------------------------

// Stage 4 made a secondary source's video/audio index RESOLVABLE (FindStream
// answers it), but the renderer's video/audio legs are source 0's only
// (§4.2). Selecting one must be refused BEFORE the composite re-points that
// child's active stream: routing first and refusing after would leave the
// leg reading a stream that no longer receives packets -- "the track
// switched and nothing arrived", reported as kOk. The refusal is the
// pipeline's own, so no renderer error event accompanies it.
TEST_F(PipelineMultiSourceTest,
       SelectingASecondarySourcesVideoTrackIsRefusedBeforeRouting) {
  StartTwoSourcePipeline();
  WaitUntilStarted();

  // Two synthetic sources, video=0 and audio=1 each: stride 2, so source 1's
  // video track is aggregate index 2 -- the number a host reads out of
  // media_info().streams.
  EXPECT_EQ(SelectAndWait(DemuxerStreamType::kVideo, 2),
            PipelineStatus::kTrackSwitchError);
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString();
}

// The same refusal on the audio side (§4.2: the audio leg is source 0 only,
// secondary audio is seen but not selected). Index 3 is source 1's audio
// track under the same stride-2 numbering.
TEST_F(PipelineMultiSourceTest,
       SelectingASecondarySourcesAudioTrackIsRefusedBeforeRouting) {
  StartTwoSourcePipeline();
  WaitUntilStarted();

  EXPECT_EQ(SelectAndWait(DemuxerStreamType::kAudio, 3),
            PipelineStatus::kTrackSwitchError);
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString();
}

// The discrimination half: source 0's own tracks are NOT bounded by the
// stage-4 pre-check. A same-track select is the renderer's no-op path, which
// is what proves the refusals above are about the SOURCE and not about the
// call itself.
TEST_F(PipelineMultiSourceTest, SelectingThePrimarySourcesTracksStillWorks) {
  StartTwoSourcePipeline();
  WaitUntilStarted();

  EXPECT_EQ(SelectAndWait(DemuxerStreamType::kVideo, 0), PipelineStatus::kOk);
  EXPECT_EQ(SelectAndWait(DemuxerStreamType::kAudio, 1), PipelineStatus::kOk);
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString();
}

// The stage-4 glue, end to end: a secondary source's subtitle track is
// selected through the pipeline's aggregate numbering, and the cue reaches
// the host tagged with ITS source -- the number a host needs to route it to
// the right overlay. The composite suite proves SourceIdForIndex answers 1
// for this index and the renderer suite proves OnTracksChanged(..., 1) tags
// the cue; this is the one that proves the pipeline passes the composite's
// answer through instead of a hardcoded 0.
TEST_F(PipelineMultiSourceTest,
       ASecondarySourcesTextCueArrivesTaggedWithItsSource) {
  StartPipelineWithTextSecondSource();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "pipeline never became ready; events:\n"
      << client_.EventLog();

  // Source 0 (synthetic): video 0, audio 1. Source 1 (text-only): text 0, 1.
  // Stride = max child index + 1 = 2, so source 1's first subtitle track is
  // aggregate index 2 -- the number media_info().streams advertises for it.
  text_demuxer_->text_stream(0)->AppendBuffer(
      test::MakeDataBuffer(DemuxerStreamType::kText, /*serial=*/0));

  EXPECT_EQ(SelectAndWait(DemuxerStreamType::kText, 2), PipelineStatus::kOk);

  // The leg pumps on the media sequence during the select's own post, so the
  // cue has arrived by the time the callback has.
  ASSERT_FALSE(client_.cues().empty())
      << "no subtitle cue was delivered for source 1's text track";
  ASSERT_EQ(client_.cues().size(), 1u);
  EXPECT_EQ(client_.cues()[0].source_id, 1)
      << "the cue was not tagged with the source it was decoded from";
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
//   runtime track selection (stage 4):
//   M-C5: in pipeline_track_select.cc DoSelectTrack, delete the video/audio
//         source pre-check (or move it below SetActiveStream).
//         SelectingASecondarySources{Video,Audio}TrackIsRefusedBeforeRouting
//         fails twice: the status comes back kOk (the renderer's late refusal
//         is wrapped in one) and the error event that refusal reports is
//         present. Restore.
//   M-C6: in pipeline_track_select.cc DoSelectTrack, hardcode source_id 0 in
//         the OnTracksChanged call.
//         ASecondarySourcesTextCueArrivesTaggedWithItsSource fails on the
//         cue's source_id (0 not 1) -- the glue neither half-test covers on
//         its own. Restore.
//
// All mutations were run, seen red, reverted, and re-verified green.
// ---------------------------------------------------------------------------

}  // namespace
}  // namespace avbase::media
