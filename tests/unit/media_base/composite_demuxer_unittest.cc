// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CompositeDemuxer (docs/14 stage 0): the addressing axis and the lifecycle
// fan-out. These tests exist so that the axis is PROVEN to be consumed, not
// merely present -- see the mutation notes at the bottom of the file.

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
    std::move(cb).Run(Status(), base::TimeDelta());
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
  int init_count() const { return init_count_; }
  int seek_count() const { return seek_count_; }
  int flush_count() const { return flush_count_; }
  int reset_count() const { return reset_count_; }
  int stop_count() const { return stop_count_; }

 private:
  std::unique_ptr<FakeDemuxerStream> video_stream_;
  MediaInfo media_info_;
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
std::vector<CompositeDemuxer::Source> MakeSources(
    std::unique_ptr<FakeDemuxer> a, std::unique_ptr<FakeDemuxer> b) {
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

  EXPECT_EQ(comp.source_count(), 2u);
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
  comp.Initialize(
      DataSourceDescriptor(), DemuxerOptions(), /*host=*/nullptr, nullptr,
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
  EXPECT_EQ(d_ptr->GetStream(0, DemuxerStreamType::kVideo),
            d->video_stream());
}

// ---------------------------------------------------------------------------
// MUTATION PROOF (docs/14, stage 0, §3.7). The assertions above must TURN RED
// under the following edits, proving the axis is genuinely consumed:
//
//   M1: in composite_demuxer.cc, change GetStream(id, type) to return
//       sources_[1 - id].demuxer->GetStream(type). The two addressing asserts
//       (GetStream(0,..)==s0 and GetStream(1,..)==s1) invert and fail. Restore.
//
//   M2: in demuxer.h, change Demuxer::GetStream(int, type) to
//       `return nullptr;`.
//       SingleSourceDemuxerAxisDefaultsToPlainStream calls the base default
//       (d.GetStream(0,VIDEO) == d.GetStream(VIDEO)); with the base returning
//       null, that equality fails. NOTE: CompositeDemuxer's own GetStream(int,
//       type) override is NOT on this path -- it routes to the child's
//       GetStream(type) -- so the addressing test is unaffected; M2 targets the
//       base default specifically. Restore.
//
// Both mutations were run, seen red, reverted, and re-verified green.
// ---------------------------------------------------------------------------

}  // namespace
}  // namespace avbase::media
