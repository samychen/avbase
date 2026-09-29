// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_frame_queue.h"

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace ijkpp::media {
namespace {

base::scoped_refptr<VideoFrame> MakeFrame(int64_t index) {
  return VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{16, 16}, Size{16, 16}, Rational{1, 1},
      base::Milliseconds(index * 33), base::Milliseconds(33), /*serial=*/0);
}

class VideoFrameQueueTest : public ::testing::Test {
 protected:
  VideoFrameQueue queue_{"video", 3};
};

TEST_F(VideoFrameQueueTest, ReserveCommitPop) {
  {
    auto guard = queue_.Reserve();
    ASSERT_TRUE(static_cast<bool>(guard));
    EXPECT_EQ(queue_.reserved_count(), 1u);
    EXPECT_EQ(queue_.size(), 0u);   // Not visible to readers yet.
    guard.Commit(MakeFrame(0));
  }
  EXPECT_EQ(queue_.reserved_count(), 0u);
  EXPECT_EQ(queue_.size(), 1u);

  base::scoped_refptr<VideoFrame> out;
  ASSERT_EQ(queue_.Pop(&out), VideoFrameQueue::PopStatus::kOk);
  ASSERT_TRUE(out);
  EXPECT_EQ(out->timestamp(), base::Milliseconds(0));
}

// THE regression this class exists to prevent. ijkplayer's
// frame_queue_peek_writable() hands out a raw Frame* and relies on the caller
// remembering to call frame_queue_push(); every early return between the two
// permanently loses a slot, and once all slots are lost the decoder blocks
// forever with no error and no log.
TEST_F(VideoFrameQueueTest, UncommittedGuardReturnsItsSlot) {
  for (int i = 0; i < 50; ++i) {
    auto guard = queue_.Reserve();
    ASSERT_TRUE(static_cast<bool>(guard));
    // Simulate an early return: decode error, abort check, resolution change.
    // The guard goes out of scope without Commit().
  }
  EXPECT_EQ(queue_.reserved_count(), 0u);
  EXPECT_EQ(queue_.size(), 0u);
  EXPECT_EQ(queue_.GetStats().slots_returned_uncommitted, 50u);

  // The queue is still fully usable — no slot was lost.
  auto guard = queue_.Reserve();
  ASSERT_TRUE(static_cast<bool>(guard));
  guard.Commit(MakeFrame(0));
  EXPECT_EQ(queue_.size(), 1u);
}

TEST_F(VideoFrameQueueTest, AbandonIsEquivalentToDropping) {
  auto guard = queue_.Reserve();
  ASSERT_TRUE(static_cast<bool>(guard));
  guard.Abandon();
  EXPECT_FALSE(static_cast<bool>(guard));   // Guard is now empty.
  EXPECT_EQ(queue_.reserved_count(), 0u);
  guard.Abandon();   // Idempotent; must not double-return the slot.
  EXPECT_EQ(queue_.GetStats().slots_returned_uncommitted, 1u);
}

TEST_F(VideoFrameQueueTest, MoveTransfersSlotOwnership) {
  VideoFrameQueue::SlotGuard moved_to;
  {
    auto guard = queue_.Reserve();
    ASSERT_TRUE(static_cast<bool>(guard));
    moved_to = std::move(guard);
    EXPECT_FALSE(static_cast<bool>(guard));   // NOLINT: testing moved-from state
  }
  // The moved-from guard must NOT have returned the slot.
  EXPECT_EQ(queue_.reserved_count(), 1u);
  EXPECT_EQ(queue_.GetStats().slots_returned_uncommitted, 0u);

  ASSERT_TRUE(static_cast<bool>(moved_to));
  moved_to.Commit(MakeFrame(0));
  EXPECT_EQ(queue_.size(), 1u);
}

TEST_F(VideoFrameQueueTest, CapacityIsEnforced) {
  std::vector<VideoFrameQueue::SlotGuard> guards;
  for (int i = 0; i < 3; ++i) {
    guards.push_back(queue_.Reserve());
    ASSERT_TRUE(static_cast<bool>(guards.back()));
  }
  VideoFrameQueue::SlotGuard extra;
  EXPECT_FALSE(queue_.TryReserve(&extra));
}

TEST_F(VideoFrameQueueTest, ReserveBlocksUntilASlotFrees) {
  std::vector<VideoFrameQueue::SlotGuard> guards;
  for (int i = 0; i < 3; ++i) {
    guards.push_back(queue_.Reserve());
  }
  std::atomic<bool> reserved{false};
  std::thread producer([&]() {
    auto g = queue_.Reserve();
    reserved.store(static_cast<bool>(g));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(reserved.load());
  guards.clear();   // Returning all three slots must wake the producer.
  producer.join();
  EXPECT_TRUE(reserved.load());
  EXPECT_GE(queue_.GetStats().reserve_waits, 1u);
}

TEST_F(VideoFrameQueueTest, PopBlocksUntilAFrameIsCommitted) {
  base::scoped_refptr<VideoFrame> popped;
  VideoFrameQueue::PopStatus status = VideoFrameQueue::PopStatus::kEmpty;
  std::thread consumer([&]() { status = queue_.Pop(&popped); });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  queue_.Reserve().Commit(MakeFrame(7));
  consumer.join();

  EXPECT_EQ(status, VideoFrameQueue::PopStatus::kOk);
  ASSERT_TRUE(popped);
  EXPECT_EQ(popped->timestamp(), base::Milliseconds(7 * 33));
}

TEST_F(VideoFrameQueueTest, FlushDropsFilledFramesAndBumpsSerial) {
  queue_.Reserve().Commit(MakeFrame(0));
  queue_.Reserve().Commit(MakeFrame(1));
  const int32_t serial_before = queue_.serial();

  queue_.Flush();
  EXPECT_EQ(queue_.serial(), serial_before + 1);
  EXPECT_EQ(queue_.size(), 0u);
  EXPECT_EQ(queue_.GetStats().dropped_by_flush, 2u);
  EXPECT_FALSE(queue_.end_of_stream());
}

// A reserved-but-uncommitted slot belongs to its guard. Flush must not free it,
// or a second producer could reserve the same slot and two frames would race
// into one storage location.
TEST_F(VideoFrameQueueTest, FlushLeavesReservedSlotsToTheirGuards) {
  auto guard = queue_.Reserve();
  ASSERT_TRUE(static_cast<bool>(guard));
  queue_.Reserve().Commit(MakeFrame(0));

  queue_.Flush();
  EXPECT_EQ(queue_.reserved_count(), 1u);   // Still held by |guard|.
  EXPECT_EQ(queue_.size(), 0u);             // The filled one was dropped.

  guard.Commit(MakeFrame(1));
  EXPECT_EQ(queue_.size(), 1u);
}

TEST_F(VideoFrameQueueTest, PeekDoesNotConsume) {
  queue_.Reserve().Commit(MakeFrame(0));
  queue_.Reserve().Commit(MakeFrame(1));

  std::vector<base::scoped_refptr<VideoFrame>> seen;
  EXPECT_EQ(queue_.Peek(&seen, 10), 2u);
  EXPECT_EQ(seen.size(), 2u);
  EXPECT_EQ(queue_.size(), 2u);   // Untouched.

  seen.clear();
  EXPECT_EQ(queue_.Peek(&seen, 1), 1u);
}

TEST_F(VideoFrameQueueTest, EndOfStreamIsReportedWhenEmpty) {
  queue_.MarkEndOfStream();
  EXPECT_TRUE(queue_.end_of_stream());
  base::scoped_refptr<VideoFrame> out;
  EXPECT_EQ(queue_.Pop(&out), VideoFrameQueue::PopStatus::kEndOfStream);
}

TEST_F(VideoFrameQueueTest, EndOfStreamWaitsForPendingFrames) {
  queue_.Reserve().Commit(MakeFrame(0));
  queue_.MarkEndOfStream();
  base::scoped_refptr<VideoFrame> out;
  EXPECT_EQ(queue_.Pop(&out), VideoFrameQueue::PopStatus::kOk);   // Frame first.
  EXPECT_EQ(queue_.Pop(&out), VideoFrameQueue::PopStatus::kEndOfStream);
}

TEST_F(VideoFrameQueueTest, AbortReleasesBlockedReserveAndPop) {
  std::vector<VideoFrameQueue::SlotGuard> guards;
  for (int i = 0; i < 3; ++i) guards.push_back(queue_.Reserve());

  std::atomic<bool> producer_done{false}, consumer_done{false};
  std::thread producer([&]() { queue_.Reserve(); producer_done.store(true); });
  std::thread consumer([&]() {
    base::scoped_refptr<VideoFrame> out;
    queue_.Pop(&out);
    consumer_done.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const base::TimeTicks before = base::TimeTicks::Now();
  queue_.Abort();
  guards.clear();
  producer.join();
  consumer.join();
  EXPECT_LT(base::TimeTicks::Now() - before, base::Seconds(2));
  EXPECT_TRUE(producer_done.load());
  EXPECT_TRUE(consumer_done.load());
  EXPECT_TRUE(queue_.aborted());
}

TEST_F(VideoFrameQueueTest, AfterAbortReserveReturnsAnEmptyGuard) {
  queue_.Abort();
  auto guard = queue_.Reserve();
  EXPECT_FALSE(static_cast<bool>(guard));
  VideoFrameQueue::SlotGuard try_guard;
  EXPECT_FALSE(queue_.TryReserve(&try_guard));
}

TEST_F(VideoFrameQueueTest, ConcurrentProducersAndConsumersLoseNoFrames) {
  VideoFrameQueue queue{"stress", 8};
  constexpr int kProducers = 2;
  constexpr int kConsumers = 2;
  constexpr int kPerProducer = 500;

  std::atomic<int> committed{0}, popped{0};
  std::vector<std::thread> producers, consumers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&queue, &committed, p]() {
      for (int i = 0; i < kPerProducer; ++i) {
        auto guard = queue.Reserve();
        if (!guard) return;
        guard.Commit(MakeFrame(p * kPerProducer + i));
        committed.fetch_add(1);
      }
    });
  }
  // See the note in DecoderBufferQueueTest: a blocking Pop() gated on a shared
  // counter self-deadlocks when pushes and pops are exactly balanced.
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&queue, &popped]() {
      base::scoped_refptr<VideoFrame> out;
      for (;;) {
        const auto status = queue.Pop(&out);
        if (status == VideoFrameQueue::PopStatus::kOk) {
          popped.fetch_add(1);
        } else if (status == VideoFrameQueue::PopStatus::kAborted) {
          return;
        }
      }
    });
  }
  for (auto& t : producers) t.join();

  // Wait for the consumers to drain instead of draining here.
  //
  // The previous version did Peek() then a blocking Pop() for each peeked
  // frame. That is racy: a consumer can take the frame in between, and then the
  // blocking Pop() waits forever because nothing will ever Abort() the queue —
  // the test hangs and ctest reports a timeout. It passed most of the time and
  // failed under CPU starvation, i.e. it was flaky. Polling the queue size has
  // no such window.
  while (queue.size() > 0 || queue.reserved_count() > 0) {
    std::this_thread::sleep_for(base::Milliseconds(1).ToChronoMicros());
  }
  queue.Abort();
  for (auto& t : consumers) t.join();

  EXPECT_EQ(committed.load(), kProducers * kPerProducer);
  EXPECT_EQ(popped.load(), kProducers * kPerProducer);
  EXPECT_EQ(queue.GetStats().slots_returned_uncommitted, 0u);
}

TEST_F(VideoFrameQueueTest, PopStatusNames) {
  using S = VideoFrameQueue::PopStatus;
  EXPECT_STREQ(GetVideoFrameQueuePopStatusName(S::kOk), "ok");
  EXPECT_STREQ(GetVideoFrameQueuePopStatusName(S::kFlushed), "flushed");
  EXPECT_STREQ(GetVideoFrameQueuePopStatusName(S::kEndOfStream), "end-of-stream");
}

}  // namespace
}  // namespace ijkpp::media
