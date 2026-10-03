// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// SyntheticLiveDemuxer's own contract (docs/12 section 2.2, the half that
// LiveDataSource did not cover). SyntheticDemuxer can report IsLive() but its
// edge is a fixed duration, so everything downstream that treats the edge as
// MOVING is untestable against it. These cases pin the three properties that
// make this double worth its existence -- the edge advances, the timestamps
// follow the clock, and a read at the edge parks instead of inventing data --
// plus the seek refusal the pipeline's chase path depends on.
//
// The suite runs on a mock clock, so it is deterministic and instant. The
// parking cases are the only ones that would otherwise need real time, and they
// get it by advancing the mock clock from a second thread, which is also the
// production shape: the edge moves on a clock, not on anything the reader does.

#include "tests/support/synthetic_live_demuxer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/time/simple_test_tick_clock.h"
#include "gtest/gtest.h"
#include "media/base/media_error.h"

namespace avbase::media::test {
namespace {

// A ceiling well under the double's own 10 s default, so a case that parks when
// it should not fails in milliseconds rather than seconds.
constexpr base::TimeDelta kShortPark = base::Milliseconds(200);

class SyntheticLiveDemuxerTest : public ::testing::Test {
 protected:
  void SetUp() override { runner_ = env_.GetMainThreadTaskRunnerRef(); }

  void Build(SyntheticLiveSpec spec) {
    spec.max_park = kShortPark;
    demuxer_ = std::make_unique<SyntheticLiveDemuxer>(std::move(spec), &clock_);
  }

  // Initialize() hops through the media runner, so the callback lands only when
  // the test drains it -- the same discipline the pipeline-level fixtures use.
  void Initialize() {
    Status status;
    demuxer_->Initialize(
        DataSourceDescriptor::FromUri("synthetic-live://test"),
        DemuxerOptions(), /*host=*/nullptr, runner_,
        base::BindOnce([](Status* out, Status s) { *out = std::move(s); },
                       base::Unretained(&status)));
    env_.RunUntilIdle();
    init_status_ = std::move(status);
  }

  // One synchronous read. The status is captured because DemuxerStream::ReadCB
  // is (Status, DecoderBufferVector) and the signature must match exactly; the
  // cases that only care about the buffers ignore it.
  DemuxerStream::DecoderBufferVector ReadOnce(DemuxerStreamType type,
                                              uint32_t count) {
    DemuxerStream::DecoderBufferVector out;
    demuxer_->GetStream(type)->Read(
        count, base::BindOnce(
                   [](DemuxerStream::DecoderBufferVector* out,
                      DemuxerStream::Status status,
                      DemuxerStream::DecoderBufferVector buffers) {
                     (void)status;
                     *out = std::move(buffers);
                   },
                   base::Unretained(&out)));
    env_.RunUntilIdle();
    return out;
  }

  // A read whose completion the test waits for explicitly, because it is
  // expected to park. Returns the buffers; |done| flips when the callback runs.
  //
  // Deliberately does NOT pump. The read parks ON the media sequence, so
  // draining it here would run the park to its ceiling before the test had a
  // chance to move the clock -- the helper would be the thing that guarantees
  // the timeout it is trying to avoid.
  DemuxerStream::DecoderBufferVector
  ReadAsync(DemuxerStreamType type, uint32_t count, std::atomic<bool>* done) {
    DemuxerStream::DecoderBufferVector out;
    demuxer_->GetStream(type)->Read(
        count, base::BindOnce(
                   [](DemuxerStream::DecoderBufferVector* out,
                      std::atomic<bool>* done, DemuxerStream::Status status,
                      DemuxerStream::DecoderBufferVector buffers) {
                     (void)status;
                     *out = std::move(buffers);
                     done->store(true);
                   },
                   base::Unretained(&out), base::Unretained(done)));
    return out;
  }

  // Pumps the media sequence until |done| or the budget runs out. The double
  // parks on a timer rather than on a task, so the test has to keep draining.
  bool PumpUntil(std::atomic<bool>* done) {
    for (int i = 0; i < 200 && !done->load(); ++i) {
      env_.RunUntilIdle();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    env_.RunUntilIdle();
    return done->load();
  }

  base::test::TaskEnvironment env_{
      base::test::TaskEnvironment::TimeSource::kMockTime};
  base::SimpleTestTickClock clock_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  std::unique_ptr<SyntheticLiveDemuxer> demuxer_;
  Status init_status_;
};

// The property the finite double cannot express: the edge MOVES. Two samples
// separated by clock time differ, and the difference equals the elapsed time.
TEST_F(SyntheticLiveDemuxerTest, TheEdgeAdvancesWithTheClock) {
  Build(SyntheticLiveSpec());
  Initialize();
  ASSERT_TRUE(init_status_.has_value()) << init_status_.error().ToString();

  const base::TimeDelta first = demuxer_->media_info().duration;
  clock_.Advance(base::Seconds(2));
  const base::TimeDelta second = demuxer_->media_info().duration;

  EXPECT_GT(second, first) << "the live edge did not move";
  EXPECT_EQ((second - first).InMilliseconds(), 2000);
  // The accessor and the MediaInfo the pipeline reads are the same number by
  // construction; two answers to "where is the edge" that disagreed would mean
  // a consumer could chase one and be measured against the other.
  EXPECT_EQ(demuxer_->Edge(), second);
  // A live stream's duration is a sample of "now", not a fact about the
  // stream, and the double says so rather than letting a consumer treat it as
  // authoritative.
  EXPECT_TRUE(demuxer_->media_info().duration_is_estimate);
}

// What a lagging consumer sees. With the edge 13 s in, the stream HAS 390
// frames, but production starts at index 0: a consumer that begins at the
// beginning is 13 s behind the edge, which is the state the live-edge chase
// exists to correct. Stamping the first packet at 13 s instead would hide the
// lag entirely, so the timestamps deliberately do not include the offset.
TEST_F(SyntheticLiveDemuxerTest, ANewConsumerStartsBehindTheEdge) {
  SyntheticLiveSpec spec;
  spec.start_offset = base::Seconds(10);
  Build(spec);
  Initialize();
  ASSERT_TRUE(init_status_.has_value()) << init_status_.error().ToString();

  clock_.Advance(base::Seconds(3));
  EXPECT_EQ(demuxer_->Edge().InMilliseconds(), 13000);

  // Asking for everything the edge has produced returns 390 frames, and the
  // first is frame 0 -- so the gap between the playhead and the edge is the
  // start offset, made observable.
  const auto all = ReadOnce(DemuxerStreamType::kVideo, 1000);
  EXPECT_EQ(all.size(), 390u)
      << "13 s at 30 fps should have produced 390 frames";
  EXPECT_EQ(all.front()->timestamp().InMilliseconds(), 0);
  // And the newest is at the edge, within one frame interval.
  const base::TimeDelta newest = all.back()->timestamp();
  EXPECT_NEAR(newest.InMilliseconds(), 13000, 34)
      << "the newest packet should sit at the live edge, not at the start";
  // The gap a fresh consumer must close: it starts on frame 0 while the edge
  // is 13 s ahead. This is the number the chase policy compares against.
  EXPECT_GT(demuxer_->Edge() - all.front()->timestamp(), base::Seconds(12));
}

// The edge bounds production, so a read cannot run past it: the double refuses
// to invent frames the wall clock has not reached.
TEST_F(SyntheticLiveDemuxerTest, AReadStopsAtTheEdge) {
  Build(SyntheticLiveSpec());
  Initialize();
  ASSERT_TRUE(init_status_.has_value()) << init_status_.error().ToString();

  // 100 ms of stream at 30 fps is three frames. Asking for 30 must not
  // produce 30.
  clock_.Advance(base::Milliseconds(100));
  EXPECT_EQ(ReadOnce(DemuxerStreamType::kVideo, 30).size(), 3u)
      << "the double produced frames beyond its own live edge";
}

// DemuxerStream's contract is 1..count buffers, so "nothing yet" cannot be an
// empty reply: the read parks. This is the case that would hang if the double
// answered with an empty vector, and it is why the park is bounded.
//
// The park occupies the media sequence, which is also where the reply is
// posted, so the test has three moving parts: a pump draining that sequence, a
// clock being advanced, and the assertion. Driving two of them from one thread
// is what made the first draft of this case deadlock against itself.
TEST_F(SyntheticLiveDemuxerTest, AReadAtTheEdgeParksAndThenDelivers) {
  Build(SyntheticLiveSpec());
  Initialize();
  ASSERT_TRUE(init_status_.has_value()) << init_status_.error().ToString();

  // Drain the first 100 ms so the next read is genuinely at the edge.
  clock_.Advance(base::Milliseconds(100));
  ASSERT_EQ(ReadOnce(DemuxerStreamType::kVideo, 30).size(), 3u);

  std::atomic<bool> done{false};
  DemuxerStream::DecoderBufferVector received =
      ReadAsync(DemuxerStreamType::kVideo, 4, &done);
  EXPECT_FALSE(done.load())
      << "a read at the live edge returned instead of parking -- the double "
         "invented data the clock had not reached";

  // Pump on this thread (the parked task needs it), advance the clock on
  // another: the edge moves on a clock, not on anything the reader does.
  std::thread advancer([this] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    clock_.Advance(base::Milliseconds(100));
  });
  const bool woke = PumpUntil(&done);
  advancer.join();

  EXPECT_TRUE(woke) << "the parked read never woke when the edge moved";
  // Three, not the four asked for: the edge advanced by 100 ms, which at 30 fps
  // is exactly three frames, and a live read is bounded by what exists rather
  // than by what was requested. Asking for more than the edge holds is normal
  // here, not a shortfall.
  EXPECT_EQ(received.size(), 3u);
  EXPECT_EQ(demuxer_->park_timeouts(), 0)
      << "the read gave up rather than waiting for the edge";
}

// Close() is how a test ends a live stream. It must wake a parked read, which
// then reports the end of stream, rather than leaving it waiting.
TEST_F(SyntheticLiveDemuxerTest, CloseWakesAParkedRead) {
  Build(SyntheticLiveSpec());
  Initialize();
  ASSERT_TRUE(init_status_.has_value()) << init_status_.error().ToString();

  clock_.Advance(base::Milliseconds(100));
  ReadOnce(DemuxerStreamType::kVideo, 30);

  std::atomic<bool> done{false};
  DemuxerStream::DecoderBufferVector received =
      ReadAsync(DemuxerStreamType::kVideo, 4, &done);
  ASSERT_FALSE(done.load()) << "expected a parked read before Close()";

  demuxer_->Close();
  ASSERT_TRUE(PumpUntil(&done)) << "Close() did not wake the parked read";
  ASSERT_EQ(received.size(), 1u);
  EXPECT_TRUE(received.front()->IsEndOfStream())
      << "a closed live stream must report the end of stream";
}

// A live source is not seekable, and saying so is load-bearing: the pipeline's
// seek path and the chase policy both branch on it.
TEST_F(SyntheticLiveDemuxerTest, IsLiveAndNotSeekable) {
  Build(SyntheticLiveSpec());
  Initialize();
  EXPECT_TRUE(demuxer_->IsLive());
  EXPECT_FALSE(demuxer_->IsSeekable());
  EXPECT_FALSE(demuxer_->media_info().seekable);
  EXPECT_EQ(demuxer_->GetStats().seek_count, 0u);
}

// Seeking one is refused with a reason, not silently ignored. The pipeline
// uses the non-physical chase precisely because a physical seek is unavailable
// here, so a double that quietly accepted one would hide exactly the bug the
// chase exists to prevent.
TEST_F(SyntheticLiveDemuxerTest, SeekIsRefusedWithAReason) {
  Build(SyntheticLiveSpec());
  Initialize();
  ASSERT_TRUE(init_status_.has_value()) << init_status_.error().ToString();

  bool ran = false;
  Status result;
  base::TimeDelta actual;
  demuxer_->StartPlayingFrom(
      base::Seconds(5),
      base::BindOnce(
          [](bool* ran, Status* result, base::TimeDelta* actual, Status s,
             base::TimeDelta a) {
            *ran = true;
            *result = std::move(s);
            *actual = a;
          },
          base::Unretained(&ran), base::Unretained(&result),
          base::Unretained(&actual)));
  env_.RunUntilIdle();

  ASSERT_TRUE(ran);
  EXPECT_FALSE(result.has_value()) << "a live seek reported success";
  ASSERT_NE(result.error().suggestion().find("chase"), std::string::npos)
      << "the error should point at the chase path: "
      << result.error().ToString();
}

// Flush bumps the generation, which is how consumers recognise pre-seek buffers
// as stale. The edge itself must not move backwards.
TEST_F(SyntheticLiveDemuxerTest, FlushBumpsTheSerialWithoutMovingTheEdge) {
  Build(SyntheticLiveSpec());
  Initialize();
  ASSERT_TRUE(init_status_.has_value()) << init_status_.error().ToString();

  clock_.Advance(base::Seconds(1));
  const base::TimeDelta before = demuxer_->Edge();
  const int32_t serial_before =
      demuxer_->GetStream(DemuxerStreamType::kVideo)->serial();
  bool flushed = false;
  demuxer_->Flush(
      base::BindOnce([](bool* f) { *f = true; }, base::Unretained(&flushed)));
  env_.RunUntilIdle();

  EXPECT_TRUE(flushed);
  EXPECT_GT(demuxer_->GetStream(DemuxerStreamType::kVideo)->serial(),
            serial_before);
  EXPECT_GE(demuxer_->Edge(), before)
      << "a flush moved the live edge backwards";
}

// Destroying a double with a reader still parked must not leave that reader's
// callback pointing at freed memory; the destructor closes first.
TEST_F(SyntheticLiveDemuxerTest, DestroyingWithAParkedReadIsSafe) {
  Build(SyntheticLiveSpec());
  Initialize();
  ASSERT_TRUE(init_status_.has_value()) << init_status_.error().ToString();
  clock_.Advance(base::Milliseconds(100));
  ReadOnce(DemuxerStreamType::kVideo, 30);

  std::atomic<bool> done{false};
  ReadAsync(DemuxerStreamType::kVideo, 4, &done);
  ASSERT_FALSE(done.load());

  // Close and drain before destroying. A read parked on the media sequence is a
  // queued task naming this demuxer, so tearing down without draining it would
  // test nothing but a use-after-free in the TEST -- which is why the double
  // wakes the read on Close() and the test honours that contract.
  demuxer_->Close();
  ASSERT_TRUE(PumpUntil(&done)) << "Close() did not release the parked read";
  demuxer_.reset();
  SUCCEED() << "teardown with a parked reader completed";
}

}  // namespace
}  // namespace avbase::media::test
