// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The mailbox, including the property that no return code carries: that a
// frame which was replaced or dropped is accounted for exactly once. The
// counts are the only place that can say so, which is why they are asserted
// against an identity and not against a number.

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "media/base/latest_frame_queue.h"
#include "media/base/video_frame.h"

namespace avbase::media {
namespace {

base::scoped_refptr<VideoFrame> MakeFrame(int serial) {
  const Size size{16, 16};
  return VideoFrame::CreateBlackFrame(VideoFormat::kI420, size, size,
                                      Rational{1, 1}, base::TimeDelta(),
                                      base::TimeDelta(), serial);
}

// Every frame published is in exactly one of four places. Asserted after each
// scenario rather than per count, because a count that is right by accident
// (two wrong in opposite directions) passes individually and fails here.
void ExpectAccounted(const LatestFrameQueue& q) {
  const uint64_t pending = q.has_pending() ? 1 : 0;
  EXPECT_EQ(q.published_frames(), q.consumed_frames() + q.superseded_frames() +
                                      q.cleared_frames() + pending);
}

TEST(LatestFrameQueue, AnEmptyQueueHandsBackNothing) {
  LatestFrameQueue q;
  EXPECT_FALSE(q.has_pending());
  EXPECT_EQ(q.Consume(), nullptr);
  EXPECT_EQ(q.published_frames(), 0u);
  ExpectAccounted(q);
}

TEST(LatestFrameQueue, ConsumeTakesTheNewestAndEmpties) {
  LatestFrameQueue q;
  ASSERT_EQ(q.Publish(MakeFrame(1)), nullptr);  // Nothing to supersede.
  EXPECT_TRUE(q.has_pending());

  base::scoped_refptr<VideoFrame> taken = q.Consume();
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->serial(), 1);
  EXPECT_FALSE(q.has_pending());
  EXPECT_EQ(q.Consume(), nullptr);  // Destructive: not handed out twice.
  EXPECT_EQ(q.consumed_frames(), 1u);
  ExpectAccounted(q);
}

TEST(LatestFrameQueue, PublishReturnsTheFrameItSuperseded) {
  LatestFrameQueue q;
  ASSERT_EQ(q.Publish(MakeFrame(1)), nullptr);
  base::scoped_refptr<VideoFrame> superseded = q.Publish(MakeFrame(2));
  ASSERT_TRUE(superseded);
  EXPECT_EQ(superseded->serial(), 1);

  // The newer one is what the consumer gets -- the replaced one is gone from
  // the slot, and handing it back is the only way anybody can know it existed.
  base::scoped_refptr<VideoFrame> taken = q.Consume();
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->serial(), 2);
  EXPECT_EQ(q.superseded_frames(), 1u);
  EXPECT_EQ(q.consumed_frames(), 1u);
  ExpectAccounted(q);
}

TEST(LatestFrameQueue, ClearDropsTheWaitingFrameAndCountsItApart) {
  LatestFrameQueue q;
  q.Publish(MakeFrame(1));
  base::scoped_refptr<VideoFrame> dropped = q.Clear();
  ASSERT_TRUE(dropped);
  EXPECT_EQ(dropped->serial(), 1);
  EXPECT_FALSE(q.has_pending());

  EXPECT_EQ(q.cleared_frames(), 1u);
  EXPECT_EQ(q.superseded_frames(), 0u);  // A seek is not a slow consumer.
  ExpectAccounted(q);
}

// A producer that runs ahead must not build a backlog: the slot holds one
// frame no matter how many are published, and the surplus is reported as
// superseded rather than being kept to be displayed late.
TEST(LatestFrameQueue, AProducerThatOutrunsTheConsumerSupersedes) {
  LatestFrameQueue q;
  for (int i = 0; i < 100; ++i) {
    q.Publish(MakeFrame(i));
  }
  EXPECT_TRUE(q.has_pending());
  EXPECT_EQ(q.published_frames(), 100u);
  EXPECT_EQ(q.superseded_frames(), 99u);
  EXPECT_EQ(q.consumed_frames(), 0u);

  base::scoped_refptr<VideoFrame> taken = q.Consume();
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->serial(), 99);  // The newest, not the oldest.
  ExpectAccounted(q);
}

// Two threads, no sleeping on the outcome: the producer runs ahead on
// purpose and the test asserts on the accounting identity, which holds for
// every interleaving. A test that waited for a specific count would be
// asserting on a race.
TEST(LatestFrameQueue, ProducerAndConsumerOnDifferentThreads) {
  LatestFrameQueue q;
  constexpr int kPublished = 2000;
  std::atomic<bool> stop{false};
  std::atomic<int> taken{0};

  std::thread producer([&]() {
    for (int i = 0; i < kPublished; ++i) {
      q.Publish(MakeFrame(i));
    }
    stop.store(true);
  });
  std::thread consumer([&]() {
    // Runs slower than the producer on purpose, so superseding is expected
    // rather than incidental.
    while (!stop.load()) {
      if (q.Consume()) {
        taken.fetch_add(1);
      }
    }
  });
  producer.join();
  consumer.join();
  // Drain what the producer finished with after the consumer saw |stop|.
  while (q.Consume()) {
    taken.fetch_add(1);
  }

  EXPECT_EQ(q.published_frames(), static_cast<uint64_t>(kPublished));
  EXPECT_GT(taken.load(), 0);
  EXPECT_EQ(static_cast<uint64_t>(taken.load()), q.consumed_frames());
  EXPECT_GT(q.superseded_frames(), 0u);  // The consumer did fall behind.
  ExpectAccounted(q);
}

// The same identity under several producers, which is what a compositor with
// one queue per source looks like from the queue's point of view: concurrent
// producers, one consumer, no frames lost from the count.
TEST(LatestFrameQueue, SeveralProducersStayAccounted) {
  LatestFrameQueue q;
  constexpr int kProducers = 4;
  constexpr int kEach = 500;
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&q]() {
      for (int i = 0; i < kEach; ++i) {
        q.Publish(MakeFrame(i));
      }
    });
  }
  for (std::thread& t : producers) {
    t.join();
  }
  EXPECT_EQ(q.published_frames(), static_cast<uint64_t>(kProducers * kEach));
  ExpectAccounted(q);
}

}  // namespace
}  // namespace avbase::media
