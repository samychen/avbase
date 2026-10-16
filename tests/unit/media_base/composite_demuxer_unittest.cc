// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CompositeDemuxer (docs/14): the addressing axis and the lifecycle fan-out
// (stage 0), plus the per-source seek landings the stage-2 playback offsets
// are derived from. These tests exist so that the axis is PROVEN to be
// consumed, not merely present -- see the mutation notes at the bottom of the
// file.

#include "media/base/composite_demuxer.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "gtest/gtest.h"
#include "media/base/demuxer_stream.h"
#include "tests/support/fake_demuxer_stream.h"

namespace avbase::media {
namespace {

using test::FakeDemuxerStream;
using test::MakeValidAudioConfig;
using test::MakeValidVideoConfig;

// A Demuxer whose only stream is a scripted video stream, returning a stable
// pointer so the composite's addressing can be observed at the pointer level.
class FakeDemuxer : public Demuxer {
 public:
  FakeDemuxer()
      : video_stream_(std::make_unique<FakeDemuxerStream>(
            DemuxerStreamType::kVideo, MakeValidAudioConfig(),
            MakeValidVideoConfig())) {}

  // Demuxer / MediaResource.
  DemuxerStream* GetStream(DemuxerStreamType type) override {
    if (type == DemuxerStreamType::kVideo) {
      return video_stream_.get();
    }
    return nullptr;
  }
  void Initialize(const DataSourceDescriptor&, const DemuxerOptions&, Host*,
                  base::scoped_refptr<base::SequencedTaskRunner>,
                  InitializeCB cb) override {
    ++init_count_;
    std::move(cb).Run(Status());
  }
  void StartPlayingFrom(base::TimeDelta, SeekCB cb) override {
    ++seek_count_;
    std::move(cb).Run(Status(), seek_actual_);
  }
  void Flush(base::OnceClosure cb) override {
    ++flush_count_;
    std::move(cb).Run();
  }
  void Reset(base::OnceClosure cb) override {
    ++reset_count_;
    std::move(cb).Run();
  }
  void Stop() override { ++stop_count_; }
  const MediaInfo& media_info() const override { return media_info_; }
  base::TimeDelta GetStartTime() const override { return base::TimeDelta(); }
  bool IsLive() const override { return false; }
  bool IsSeekable() const override { return true; }
  DemuxerStats GetStats() const override { return DemuxerStats(); }
  const char* name() const override { return "FakeDemuxer"; }

  FakeDemuxerStream* video_stream() const { return video_stream_.get(); }
  // Where this child's seek lands. Default zero, like a seek to the start.
  void set_seek_actual(base::TimeDelta actual) { seek_actual_ = actual; }
  int init_count() const { return init_count_; }
  int seek_count() const { return seek_count_; }
  int flush_count() const { return flush_count_; }
  int reset_count() const { return reset_count_; }
  int stop_count() const { return stop_count_; }

 private:
  std::unique_ptr<FakeDemuxerStream> video_stream_;
  MediaInfo media_info_;
  base::TimeDelta seek_actual_;
  int init_count_ = 0;
  int seek_count_ = 0;
  int flush_count_ = 0;
  int reset_count_ = 0;
  int stop_count_ = 0;
};

std::unique_ptr<FakeDemuxer> MakeFakeDemuxer() {
  return std::make_unique<FakeDemuxer>();
}

CompositeDemuxer::Source MakeSource(std::unique_ptr<FakeDemuxer> d) {
  CompositeDemuxer::Source s;
  s.demuxer = std::move(d);
  return s;
}

// Builds the source vector by moving (push_back), never copying -- Source holds
// a unique_ptr<Demuxer>, so an initializer_list {a, b} would force a copy and
// fail to compile.
std::vector<CompositeDemuxer::Source>
MakeSources(std::unique_ptr<FakeDemuxer> a, std::unique_ptr<FakeDemuxer> b) {
  std::vector<CompositeDemuxer::Source> v;
  v.reserve(2);
  v.push_back(MakeSource(std::move(a)));
  v.push_back(MakeSource(std::move(b)));
  return v;
}

TEST(CompositeDemuxerTest, AddressingAxisRoutesToCorrectChild) {
  auto d0 = MakeFakeDemuxer();
  auto d1 = MakeFakeDemuxer();
  FakeDemuxer* d0_ptr = d0.get();
  FakeDemuxer* d1_ptr = d1.get();
  FakeDemuxerStream* s0 = d0->video_stream();
  FakeDemuxerStream* s1 = d1->video_stream();
  ASSERT_NE(s0, s1);  // distinct instances let us tell the sources apart

  CompositeDemuxer comp(MakeSources(std::move(d0), std::move(d1)));

  // int, not size_t: the addressing axis is int throughout (GetStream takes an
  // int source_id), and a size_t here would force a narrowing cast on every
  // loop bound that walks it.
  EXPECT_EQ(comp.source_count(), 2);
  // Visible through the BASE pointer, which is how the renderer sees it:
  // RendererImpl::Initialize walks [0, source_count()) on a MediaResource*,
  // never on a CompositeDemuxer.
  EXPECT_EQ(static_cast<const MediaResource&>(comp).source_count(), 2);
  // The axis must reach the right child, not always the first one.
  EXPECT_EQ(comp.GetStream(0, DemuxerStreamType::kVideo), s0);
  EXPECT_EQ(comp.GetStream(1, DemuxerStreamType::kVideo), s1);
  // The no-axis overload answers source 0 (single-source callers unchanged).
  EXPECT_EQ(comp.GetStream(DemuxerStreamType::kVideo), s0);
  // Out-of-range ids are null, not a crash.
  EXPECT_EQ(comp.GetStream(2, DemuxerStreamType::kVideo), nullptr);
  EXPECT_EQ(comp.GetStream(-1, DemuxerStreamType::kVideo), nullptr);
  // source(id) exposes the underlying demuxer.
  EXPECT_EQ(comp.source(0), d0_ptr);
  EXPECT_EQ(comp.source(1), d1_ptr);
  EXPECT_EQ(comp.source(2), nullptr);
}

TEST(CompositeDemuxerTest, InitializeFansOutToAllChildren) {
  auto d0 = MakeFakeDemuxer();
  auto d1 = MakeFakeDemuxer();
  FakeDemuxer* p0 = d0.get();
  FakeDemuxer* p1 = d1.get();
  CompositeDemuxer comp(MakeSources(std::move(d0), std::move(d1)));

  bool called = false;
  comp.Initialize(DataSourceDescriptor(), DemuxerOptions(), /*host=*/nullptr,
                  nullptr,
                  base::BindOnce([&called](Status) { called = true; }));

  EXPECT_TRUE(called);
  EXPECT_EQ(p0->init_count(), 1);
  EXPECT_EQ(p1->init_count(), 1);
}

TEST(CompositeDemuxerTest, StartPlayingFromFansOutToAllChildren) {
  auto d0 = MakeFakeDemuxer();
  auto d1 = MakeFakeDemuxer();
  FakeDemuxer* p0 = d0.get();
  FakeDemuxer* p1 = d1.get();
  CompositeDemuxer comp(MakeSources(std::move(d0), std::move(d1)));

  bool called = false;
  comp.StartPlayingFrom(
      base::Seconds(1),
      base::BindOnce([&called](Status, base::TimeDelta) { called = true; }));

  EXPECT_TRUE(called);
  EXPECT_EQ(p0->seek_count(), 1);
  EXPECT_EQ(p1->seek_count(), 1);
}

// ---- per-source seek landings (docs/14 stage 2) ---------------------------

// The aggregate callback keeps reporting the MAX landing -- unchanged since
// stage 0 -- while seek_actuals() answers the per-source question the stage-2
// playback offset is derived from. Two different questions: the max is where
// the pipeline resumes, the per-child values are which moment of each source
// is on screen at that resume.
TEST(CompositeDemuxerTest, SeekRecordsEachChildsLanding) {
  auto d0 = MakeFakeDemuxer();
  auto d1 = MakeFakeDemuxer();
  d0->set_seek_actual(base::Seconds(1));
  d1->set_seek_actual(base::Seconds(3) + base::Milliseconds(500));
  CompositeDemuxer comp(MakeSources(std::move(d0), std::move(d1)));

  base::TimeDelta aggregate;
  comp.StartPlayingFrom(
      base::Seconds(10),
      base::BindOnce([](base::TimeDelta* out, Status,
                        base::TimeDelta actual) { *out = actual; },
                     &aggregate));

  EXPECT_EQ(aggregate, base::Seconds(3) + base::Milliseconds(500))
      << "the aggregate landing changed: it is the max, as it has always been";
  const std::vector<base::TimeDelta>& actuals = comp.seek_actuals();
  ASSERT_EQ(actuals.size(), 2u);
  EXPECT_EQ(actuals[0], base::Seconds(1));
  EXPECT_EQ(actuals[1], base::Seconds(3) + base::Milliseconds(500));
}

// The same, with the landing order REVERSED: source 1 lands before source 0.
// A per-source table implemented as "whatever the last callback saw", or as
// "the aggregate copied into every slot", can pass the test above by luck of
// the ordering; this one pins the index.
TEST(CompositeDemuxerTest, SeekLandingsAreIndexedNotOrdered) {
  auto d0 = MakeFakeDemuxer();
  auto d1 = MakeFakeDemuxer();
  d0->set_seek_actual(base::Seconds(4));
  d1->set_seek_actual(base::Seconds(1) + base::Milliseconds(500));
  CompositeDemuxer comp(MakeSources(std::move(d0), std::move(d1)));

  comp.StartPlayingFrom(base::Seconds(10),
                        base::BindOnce([](Status, base::TimeDelta) {}));

  const std::vector<base::TimeDelta>& actuals = comp.seek_actuals();
  ASSERT_EQ(actuals.size(), 2u);
  EXPECT_EQ(actuals[0], base::Seconds(4));
  EXPECT_EQ(actuals[1], base::Seconds(1) + base::Milliseconds(500));
}

// A lone wrapped demuxer reports exactly ONE landing, and a second seek
// re-records rather than appending -- the vector is sized per seek, so a
// reader never mistakes a stale landing from an earlier seek for the current
// one. This is also the single-source shape the pipeline sees, where
// ApplySourceOffsets finds fewer than two entries and states no offset.
TEST(CompositeDemuxerTest, SingleSourceSeekRecordsOneLandingPerSeek) {
  auto d = MakeFakeDemuxer();
  FakeDemuxer* p = d.get();
  std::vector<CompositeDemuxer::Source> v;
  v.push_back(MakeSource(std::move(d)));
  CompositeDemuxer comp(std::move(v));

  comp.StartPlayingFrom(base::Seconds(2),
                        base::BindOnce([](Status, base::TimeDelta) {}));
  ASSERT_EQ(comp.seek_actuals().size(), 1u);
  EXPECT_EQ(comp.seek_actuals()[0], base::TimeDelta());

  p->set_seek_actual(base::Seconds(7));
  comp.StartPlayingFrom(base::Seconds(9),
                        base::BindOnce([](Status, base::TimeDelta) {}));
  ASSERT_EQ(comp.seek_actuals().size(), 1u) << "a second seek appended";
  EXPECT_EQ(comp.seek_actuals()[0], base::Seconds(7));
}

TEST(CompositeDemuxerTest, FlushFansOutToAllChildren) {
  auto d0 = MakeFakeDemuxer();
  auto d1 = MakeFakeDemuxer();
  FakeDemuxer* p0 = d0.get();
  FakeDemuxer* p1 = d1.get();
  CompositeDemuxer comp(MakeSources(std::move(d0), std::move(d1)));

  bool called = false;
  comp.Flush(base::BindOnce([&called]() { called = true; }));

  EXPECT_TRUE(called);
  EXPECT_EQ(p0->flush_count(), 1);
  EXPECT_EQ(p1->flush_count(), 1);
}

TEST(CompositeDemuxerTest, ResetFansOutToAllChildren) {
  auto d0 = MakeFakeDemuxer();
  auto d1 = MakeFakeDemuxer();
  FakeDemuxer* p0 = d0.get();
  FakeDemuxer* p1 = d1.get();
  CompositeDemuxer comp(MakeSources(std::move(d0), std::move(d1)));

  bool called = false;
  comp.Reset(base::BindOnce([&called]() { called = true; }));

  EXPECT_TRUE(called);
  EXPECT_EQ(p0->reset_count(), 1);
  EXPECT_EQ(p1->reset_count(), 1);
}

TEST(CompositeDemuxerTest, StopFansOutToAllChildren) {
  auto d0 = MakeFakeDemuxer();
  auto d1 = MakeFakeDemuxer();
  FakeDemuxer* p0 = d0.get();
  FakeDemuxer* p1 = d1.get();
  CompositeDemuxer comp(MakeSources(std::move(d0), std::move(d1)));

  comp.Stop();

  EXPECT_EQ(p0->stop_count(), 1);
  EXPECT_EQ(p1->stop_count(), 1);
}

// The base Demuxer::GetStream(int, type) default must answer source 0's plain
// stream, so a single-source demuxer is usable through the new axis without
// overriding it. This is the assertion M2 (base returns nullptr) breaks.
TEST(CompositeDemuxerTest, SingleSourceDemuxerAxisDefaultsToPlainStream) {
  auto d = MakeFakeDemuxer();
  // Through a Demuxer* the base GetStream(int, type) overload is visible (a
  // derived single-arg override would hide it). The base default must answer
  // source 0's plain stream, so a single-source demuxer is usable through the
  // new axis without overriding it.
  Demuxer* d_ptr = d.get();
  EXPECT_EQ(d_ptr->GetStream(0, DemuxerStreamType::kVideo),
            d_ptr->GetStream(DemuxerStreamType::kVideo));
  EXPECT_EQ(d_ptr->GetStream(0, DemuxerStreamType::kVideo), d->video_stream());
  // ...and a lone demuxer is exactly one source through the base pointer, so
  // a renderer walking the axis over a non-aggregate sees N == 1.
  EXPECT_EQ(d_ptr->source_count(), 1);
}

// ---------------------------------------------------------------------------
// The aggregate index space (docs/14 stage 4).
//
// Each child numbers its streams from its own container order, so source 1's
// text stream and source 0's share an integer. The point of these four tests
// is that the aggregate's numbering is observed DOWNSTREAM of every hop:
// resolution answers a POINTER (that is which child owns the index), routing
// answers which CHILD was told, and the info snapshot answers the number a
// host would have to type to get either.
// ---------------------------------------------------------------------------

// Valid means "carries a codec name" for a text config (TextDecoderConfig::
// IsValidConfig()); the streams below are never decoded, but a half-built one
// would fail for a reason unrelated to what these tests are about.
TextDecoderConfig MakeSubripConfig() {
  TextDecoderConfig config;
  config.codec_name = "subrip";
  return config;
}

// A child with two text streams whose container indices are deliberately NOT
// zero-based, because that is the case that produces the collision: both
// children answer index 2, and only the aggregate can tell them apart.
class TextFakeDemuxer : public Demuxer {
 public:
  explicit TextFakeDemuxer(int32_t first_index) {
    for (int i = 0; i < 2; ++i) {
      FakeDemuxerStream* s = new FakeDemuxerStream(
          DemuxerStreamType::kText, MakeValidAudioConfig(),
          MakeValidVideoConfig(), MakeSubripConfig());
      s->set_stream_index(first_index + i);
      streams_.push_back(std::unique_ptr<FakeDemuxerStream>(s));
      StreamInfo info;
      info.index = first_index + i;
      info.kind = StreamKind::kText;
      info.codec_name = "subrip";
      media_info_.streams.push_back(std::move(info));
    }
  }

  DemuxerStream* GetStream(DemuxerStreamType type) override {
    const std::vector<DemuxerStream*> all = GetStreams(type);
    return all.empty() ? nullptr : all[0];
  }
  std::vector<DemuxerStream*> GetStreams(DemuxerStreamType type) override {
    std::vector<DemuxerStream*> out;
    for (auto& s : streams_) {
      if (s->type() == type) {
        out.push_back(s.get());
      }
    }
    return out;
  }
  void SetActiveStream(DemuxerStreamType type, int stream_index) override {
    last_active_type_ = type;
    last_active_index_ = stream_index;
    ++active_calls_;
  }
  const MediaInfo& media_info() const override { return media_info_; }

  // The unimplemented rest.
  void Initialize(const DataSourceDescriptor&, const DemuxerOptions&, Host*,
                  base::scoped_refptr<base::SequencedTaskRunner>,
                  InitializeCB cb) override {
    std::move(cb).Run(Status());
  }
  void StartPlayingFrom(base::TimeDelta, SeekCB cb) override {
    std::move(cb).Run(Status(), base::TimeDelta());
  }
  void Flush(base::OnceClosure cb) override { std::move(cb).Run(); }
  void Reset(base::OnceClosure cb) override { std::move(cb).Run(); }
  void Stop() override {}
  base::TimeDelta GetStartTime() const override { return base::TimeDelta(); }
  bool IsLive() const override { return false; }
  bool IsSeekable() const override { return true; }
  DemuxerStats GetStats() const override { return DemuxerStats(); }
  const char* name() const override { return "TextFakeDemuxer"; }

  DemuxerStream* stream(size_t i) const { return streams_[i].get(); }
  DemuxerStreamType last_active_type() const { return last_active_type_; }
  int last_active_index() const { return last_active_index_; }
  int active_calls() const { return active_calls_; }

 private:
  std::vector<std::unique_ptr<FakeDemuxerStream>> streams_;
  MediaInfo media_info_;
  DemuxerStreamType last_active_type_{DemuxerStreamType::kUnknown};
  int last_active_index_{-1};
  int active_calls_{0};
};

CompositeDemuxer::Source MakeSource(std::unique_ptr<Demuxer> d) {
  CompositeDemuxer::Source s;
  s.demuxer = std::move(d);
  return s;
}

// Two children, each numbering its subtitle tracks 2 and 3 (the shape a real
// file has once it also carries video=0 and audio=1).
class TwoTextSourcesTest : public ::testing::Test {
 protected:
  TwoTextSourcesTest() {
    auto a = std::make_unique<TextFakeDemuxer>(/*first_index=*/2);
    auto b = std::make_unique<TextFakeDemuxer>(/*first_index=*/2);
    s0_ = a.get();
    s1_ = b.get();
    std::vector<CompositeDemuxer::Source> v;
    v.push_back(MakeSource(std::move(a)));
    v.push_back(MakeSource(std::move(b)));
    comp_ = std::make_unique<CompositeDemuxer>(std::move(v));
  }

  // max(container index) + 1 == 4, so source 1 starts at 4.
  static constexpr int kStride = 4;
  std::unique_ptr<CompositeDemuxer> comp_;
  TextFakeDemuxer* s0_{nullptr};
  TextFakeDemuxer* s1_{nullptr};
};

TEST_F(TwoTextSourcesTest, SourceZeroKeepsItsOwnNumbering) {
  EXPECT_EQ(comp_->FindStream(DemuxerStreamType::kText, 2), s0_->stream(0));
  EXPECT_EQ(comp_->FindStream(DemuxerStreamType::kText, 3), s0_->stream(1));
  // Nothing at these numbers belongs to a source that does not exist.
  EXPECT_EQ(comp_->FindStream(DemuxerStreamType::kText, 8), nullptr);
  EXPECT_EQ(comp_->FindStream(DemuxerStreamType::kText, -1), nullptr);
}

TEST_F(TwoTextSourcesTest, ASecondaryTextStreamIsReachableByNumber) {
  // The index that selected nothing at all before stage 4: source 1's first
  // subtitle track is 1 * stride + 2, and only the aggregate can resolve it.
  EXPECT_EQ(comp_->FindStream(DemuxerStreamType::kText, kStride + 2),
            s1_->stream(0));
  EXPECT_EQ(comp_->FindStream(DemuxerStreamType::kText, kStride + 3),
            s1_->stream(1));
  // The local number alone still answers source 0's, never "one of them".
  EXPECT_EQ(comp_->FindStream(DemuxerStreamType::kText, 2), s0_->stream(0));
}

TEST_F(TwoTextSourcesTest, RoutingReachesTheChildThatOwnsTheIndex) {
  comp_->SetActiveStream(DemuxerStreamType::kText, kStride + 2);
  // Translated back: the child sees its OWN number, which is what its packets
  // carry and what its demux loop compares.
  EXPECT_EQ(s1_->last_active_index(), 2);
  EXPECT_EQ(s1_->last_active_type(), DemuxerStreamType::kText);
  EXPECT_EQ(s1_->active_calls(), 1);
  // The other source was told nothing -- routing is point-to-point, not a
  // broadcast that happens to be harmless.
  EXPECT_EQ(s0_->active_calls(), 0);
}

TEST_F(TwoTextSourcesTest, MergedInfoNumbersEverySourceAndMarksWhichIsWhich) {
  const MediaInfo& info = comp_->media_info();
  ASSERT_EQ(info.streams.size(), 4u);
  // Source-major: a kind's first stream is still source 0's.
  EXPECT_EQ(info.FirstStreamOfKind(StreamKind::kText), 2);
  EXPECT_EQ(info.streams[3].index, kStride + 3);
  const auto owner = info.streams[3].metadata.find("avbase:source");
  ASSERT_NE(owner, info.streams[3].metadata.end());
  EXPECT_EQ(owner->second, "1");
}

// The guard contract this whole change is shaped around: RendererImpl::
// IsPrimaryStream() answers "is it SOURCE 0's stream" by walking GetStreams().
// It is the reason the all-sources question became a separate call instead of
// a widened one -- widening it would let a secondary source's stream into the
// primary leg's track switch.
TEST_F(TwoTextSourcesTest, EnumerationStillAnswersSourceZeroAlone) {
  const std::vector<DemuxerStream*> all =
      comp_->GetStreams(DemuxerStreamType::kText);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0], s0_->stream(0));
  EXPECT_EQ(all[1], s0_->stream(1));
}

// The index-to-source map the renderer consumes to wire the right text leg
// (docs/14 stage 4). Source 0 keeps its own numbers; source 1's aggregate
// numbers (stride + child index) resolve to source 1; out-of-range indices are
// not a source the composite knows, so they fall back to 0 rather than
// fabricating an invalid one.
TEST_F(TwoTextSourcesTest, SourceIdForIndexRoutesToTheOwningSource) {
  EXPECT_EQ(comp_->SourceIdForIndex(DemuxerStreamType::kText, 2), 0);
  EXPECT_EQ(comp_->SourceIdForIndex(DemuxerStreamType::kText, 3), 0);
  EXPECT_EQ(comp_->SourceIdForIndex(DemuxerStreamType::kText, kStride + 2), 1);
  EXPECT_EQ(comp_->SourceIdForIndex(DemuxerStreamType::kText, kStride + 3), 1);
  EXPECT_EQ(comp_->SourceIdForIndex(DemuxerStreamType::kText, 8), 0);
}

// A wrapped single source must number its streams exactly as the bare demuxer
// does, which is what makes stage 4 invisible to every existing caller.
TEST(CompositeDemuxerTest, WrappingOneSourceDoesNotRenumberIt) {
  auto a = std::make_unique<TextFakeDemuxer>(/*first_index=*/2);
  DemuxerStream* owned = a->stream(1);
  std::vector<CompositeDemuxer::Source> v;
  v.push_back(MakeSource(std::move(a)));
  CompositeDemuxer comp(std::move(v));

  EXPECT_EQ(comp.FindStream(DemuxerStreamType::kText, 3), owned);
  EXPECT_EQ(comp.media_info().streams.size(), 2u);
  EXPECT_EQ(comp.media_info().streams[1].index, 3);
}

// ---------------------------------------------------------------------------
// MUTATION PROOF (docs/14). The assertions in this file must TURN RED under the
// following edits, proving each axis is genuinely consumed, not merely present.
// Grouped by the stage that introduced the code under test:
//
//   addressing axis (stage 0):
//   M1: in composite_demuxer.cc, change GetStream(id, type) to return
//       sources_[1 - id].demuxer->GetStream(type). The two addressing asserts
//       (GetStream(0,..)==s0 and GetStream(1,..)==s1) invert and fail. Restore.
//   M2: in demuxer.h, change Demuxer::GetStream(int, type) to
//       `return nullptr;`. SingleSourceDemuxerAxisDefaultsToPlainStream calls
//       the base default (d.GetStream(0,VIDEO) == d.GetStream(VIDEO)); with the
//       base returning null, that equality fails. NOTE: CompositeDemuxer's own
//       GetStream(int, type) override routes to the child's GetStream(type), so
//       the addressing test is unaffected; M2 targets the base default
//       specifically. Restore.
//
//   per-source seek landings (stage 2):
//   M3: in composite_demuxer.cc, write the AGGREGATE into every slot --
//       `seek_actuals_[i] = seek_actual_`, or drop the index from the bound
//       callback and fill all slots with the same value.
//       SeekLandingsAreIndexedNotOrdered fails on the slot that differs from
//       the max (source 1 lands 2.5 s before source 0). Restore.
//
//   aggregate index space (stage 4 -- this revision):
//   M4: revert FindStream to route by source 0 only --
//       `return sources_[0].demuxer->FindStream(type, stream_index);`.
//       ASecondaryTextStreamIsReachableByNumber fails on both secondary-stream
//       lookups (FindStream(stride+2/3) answers null), and
//       RoutingReachesTheChildThatOwnsTheIndex fails too because
//       SetActiveStream validates the index through FindStream first. Restore.
//   M5: drop the boundary translation in SetActiveStream -- pass `stream_index`
//       instead of `stream_index - source_id * stride`. The child is told its
//       own number, so RoutingReachesTheChildThatOwnsTheIndex fails
//       (expects the child saw 2, it saw stride+2). Restore.
//   M6: stop offsetting in MergeMediaInfo -- `info.index = info.index;` and
//       drop the `"avbase:source"` metadata.
//       MergedInfoNumbersEverySourceAndMarksWhichIsWhich fails
//       (streams[3].index is 3 not stride+3, and the source owner lookup is
//       absent). Restore.
//   M7: in composite_demuxer.cc, make SourceIdForIndex always return 0
//       (`return 0;`). SourceIdForIndexRoutesToTheOwningSource fails on the
//       two secondary lookups (kStride+2/3 resolve to 0 not 1) -- the very
//       number the renderer consumes to wire the right text leg. Restore.
//
// All mutations were run, seen red, reverted, and re-verified green.
// ---------------------------------------------------------------------------

}  // namespace
}  // namespace avbase::media
