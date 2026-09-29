// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/decoder_buffer_queue.h"

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace ijkpp::media {
namespace {

const uint8_t kByte = 0x42;

// |pts_ms| is intentionally unnamed: the queue orders by insertion, not by
// timestamp, and -Wunused-parameter (debug preset, -Werror) rejects a named one.
base::scoped_refptr<DecoderBuffer> MakeBuffer(int64_t /*pts_ms*/,
                                              size_t size = 64) {
  std::vector<uint8_t> payload(size, kByte);
  return DecoderBuffer::CopyFrom(payload.data(), payload.size(),
                                 DemuxerStreamType::kVideo, 0);
}

class DecoderBufferQueueTest : public ::testing::Test {
 protected:
  // 4 buffers / 1 KiB: small enough that the limits are exercised by a handful
  // of pushes.
  DecoderBufferQueue queue_{"video", 4, 1024, nullptr};
};

TEST_F(DecoderBufferQueueTest, PushThenPopPreservesOrder) {
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue_.Push(MakeBuffer(i * 33)));
  }
  EXPECT_EQ(queue_.size(), 3u);
  EXPECT_EQ(queue_.bytes(), 3u * 64u);

  for (int i = 0; i < 3; ++i) {
    base::scoped_refptr<DecoderBuffer> out;
    ASSERT_EQ(queue_.Pop(&out), DecoderBufferQueue::PopStatus::kOk);
    ASSERT_TRUE(out);
    EXPECT_EQ(out->data_size(), 64u);
  }
  EXPECT_TRUE(queue_.empty());
}

TEST_F(DecoderBufferQueueTest, TryPushFailsAtTheCountLimit) {
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(queue_.TryPush(MakeBuffer(i)));
  }
  EXPECT_FALSE(queue_.TryPush(MakeBuffer(99)));
  EXPECT_EQ(queue_.size(), 4u);
}

TEST_F(DecoderBufferQueueTest, TryPushFailsOnceTheByteLimitIsReached) {
  // The byte limit is a high-water mark, not a hard ceiling: a push is refused
  // once the queue is already at or over it, which means the last accepted
  // buffer may overshoot by up to one buffer's worth. That is intentional —
  // refusing a push that would cross the line can wedge a producer holding a
  // buffer larger than the whole budget.
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(queue_.TryPush(MakeBuffer(i, 300)));
  }
  EXPECT_EQ(queue_.bytes(), 1200u);   // Over the 1024-byte mark.
  EXPECT_FALSE(queue_.TryPush(MakeBuffer(4, 300)));
  // The count limit has not been reached (4 of 4 is at the limit), so either
  // cap alone is sufficient to refuse.
  EXPECT_EQ(queue_.size(), 4u);
}

TEST_F(DecoderBufferQueueTest, PopWaitsForAPushFromAnotherThread) {
  base::scoped_refptr<DecoderBuffer> popped;
  DecoderBufferQueue::PopStatus status = DecoderBufferQueue::PopStatus::kEmpty;
  std::thread consumer([&]() { status = queue_.Pop(&popped); });

  // Give the consumer time to block, then release it.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(queue_.Push(MakeBuffer(0)));
  consumer.join();

  EXPECT_EQ(status, DecoderBufferQueue::PopStatus::kOk);
  ASSERT_TRUE(popped);
  EXPECT_EQ(queue_.GetStats().pop_waits, 1u);
}

// THE serial invariant. A buffer pushed before a Flush carries the old
// generation and must be recognisable as stale; buffers pushed after carry the
// new one. Getting this wrong is what makes frames from before a seek reach the
// decoder (docs/04 §4 rules R1-R3).
TEST_F(DecoderBufferQueueTest, FlushBumpsSerialAndClears) {
  ASSERT_TRUE(queue_.Push(MakeBuffer(0)));
  ASSERT_TRUE(queue_.Push(MakeBuffer(33)));
  const int32_t serial_before = queue_.serial();

  base::scoped_refptr<DecoderBuffer> held;
  ASSERT_EQ(queue_.Pop(&held), DecoderBufferQueue::PopStatus::kOk);
  EXPECT_EQ(held->serial(), serial_before);

  queue_.Flush();
  EXPECT_EQ(queue_.serial(), serial_before + 1);
  EXPECT_TRUE(queue_.empty());
  EXPECT_EQ(queue_.bytes(), 0u);
  EXPECT_EQ(queue_.GetStats().dropped_by_flush, 1u);

  // The already-handed-out buffer still reports the OLD serial, which is how
  // the consumer detects that it is stale.
  EXPECT_EQ(held->serial(), serial_before);
  EXPECT_LT(held->serial(), queue_.serial());

  ASSERT_TRUE(queue_.Push(MakeBuffer(100)));
  base::scoped_refptr<DecoderBuffer> fresh;
  ASSERT_EQ(queue_.Pop(&fresh), DecoderBufferQueue::PopStatus::kOk);
  EXPECT_EQ(fresh->serial(), serial_before + 1);
}

TEST_F(DecoderBufferQueueTest, SerialSurvivesManyFlushes) {
  for (int i = 0; i < 1000; ++i) {
    queue_.Flush();
  }
  EXPECT_EQ(queue_.serial(), 1000);
}

TEST_F(DecoderBufferQueueTest, FlushWakesBlockedProducer) {
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(queue_.TryPush(MakeBuffer(i)));
  }
  std::atomic<bool> push_returned{false};
  std::thread producer([&]() {
    queue_.Push(MakeBuffer(99));
    push_returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(push_returned.load());

  queue_.Flush();   // Frees every slot, so the producer must wake.
  producer.join();
  EXPECT_TRUE(push_returned.load());
  EXPECT_GE(queue_.GetStats().push_waits, 1u);
}

// The bounded-shutdown guarantee behind Δ15: Abort() must release a producer
// blocked on a full queue immediately, or ~Player() hangs.
TEST_F(DecoderBufferQueueTest, AbortWakesBlockedProducerAndConsumer) {
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(queue_.TryPush(MakeBuffer(i)));
  }
  std::atomic<bool> producer_done{false}, consumer_done{false};
  std::thread producer([&]() {
    queue_.Push(MakeBuffer(99));
    producer_done.store(true);
  });
  std::thread consumer([&]() {
    // Drain first so the consumer ends up blocked on an empty queue.
    base::scoped_refptr<DecoderBuffer> out;
    for (int i = 0; i < 4; ++i) queue_.Pop(&out);
    queue_.Pop(&out);
    consumer_done.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const base::TimeTicks before = base::TimeTicks::Now();
  queue_.Abort();
  producer.join();
  consumer.join();
  EXPECT_LT(base::TimeTicks::Now() - before, base::Seconds(2))
      << "Abort() did not release blocked waiters promptly";
  EXPECT_TRUE(producer_done.load());
  EXPECT_TRUE(consumer_done.load());
}

TEST_F(DecoderBufferQueueTest, OperationsFailFastAfterAbort) {
  queue_.Abort();
  EXPECT_TRUE(queue_.aborted());
  EXPECT_FALSE(queue_.Push(MakeBuffer(0)));
  EXPECT_FALSE(queue_.TryPush(MakeBuffer(0)));
  base::scoped_refptr<DecoderBuffer> out;
  EXPECT_EQ(queue_.Pop(&out), DecoderBufferQueue::PopStatus::kAborted);
}

TEST_F(DecoderBufferQueueTest, EndOfStreamIsReportedAfterDraining) {
  ASSERT_TRUE(queue_.Push(MakeBuffer(0)));
  queue_.MarkEndOfStream();
  EXPECT_TRUE(queue_.end_of_stream());

  base::scoped_refptr<DecoderBuffer> out;
  EXPECT_EQ(queue_.Pop(&out), DecoderBufferQueue::PopStatus::kOk);
  EXPECT_EQ(queue_.Pop(&out), DecoderBufferQueue::PopStatus::kEndOfStream);
  // Repeated reads keep reporting EOS rather than blocking forever.
  EXPECT_EQ(queue_.Pop(&out), DecoderBufferQueue::PopStatus::kEndOfStream);
}

TEST_F(DecoderBufferQueueTest, FlushClearsTheEosMarker) {
  queue_.MarkEndOfStream();
  EXPECT_TRUE(queue_.end_of_stream());
  queue_.Flush();
  EXPECT_FALSE(queue_.end_of_stream());
  ASSERT_TRUE(queue_.Push(MakeBuffer(0)));
  base::scoped_refptr<DecoderBuffer> out;
  EXPECT_EQ(queue_.Pop(&out), DecoderBufferQueue::PopStatus::kOk);
}

TEST_F(DecoderBufferQueueTest, BufferedDurationSpansFirstToLast) {
  auto a = MakeBuffer(0);
  auto b = MakeBuffer(0);
  // Timestamps are assigned by the demuxer adapter; simulate by checking that
  // a queue of timestamp-less buffers reports zero rather than garbage.
  queue_.Push(a);
  queue_.Push(b);
  EXPECT_EQ(queue_.buffered_duration(), base::TimeDelta());
}

TEST_F(DecoderBufferQueueTest, PopUpToReturnsABatch) {
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(queue_.TryPush(MakeBuffer(i)));
  }
  std::vector<base::scoped_refptr<DecoderBuffer>> batch;
  EXPECT_EQ(queue_.PopUpTo(3, &batch), DecoderBufferQueue::PopStatus::kOk);
  EXPECT_EQ(batch.size(), 3u);
  EXPECT_EQ(queue_.size(), 1u);

  batch.clear();
  EXPECT_EQ(queue_.PopUpTo(3, &batch), DecoderBufferQueue::PopStatus::kOk);
  EXPECT_EQ(batch.size(), 1u);

  batch.clear();
  EXPECT_EQ(queue_.PopUpTo(3, &batch), DecoderBufferQueue::PopStatus::kEmpty);
  EXPECT_TRUE(batch.empty());
}

TEST_F(DecoderBufferQueueTest, ConcurrentProducersAndConsumersLoseNothing) {
  DecoderBufferQueue queue{"stress", 64, 0, nullptr};
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 500;

  std::atomic<int> produced{0}, consumed{0};
  std::vector<std::thread> producers, consumers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&queue, &produced]() {
      for (int i = 0; i < kPerProducer; ++i) {
        if (queue.Push(MakeBuffer(i))) produced.fetch_add(1);
      }
    });
  }
  // Consumers must NOT gate their blocking Pop() on a shared counter: with
  // exactly N pushes and N pops, the consumer that observes count == N-1 enters
  // Pop() and then blocks forever once a peer takes the last buffer. Drain
  // until Abort() instead, which is also how production shutdown works.
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&queue, &consumed]() {
      base::scoped_refptr<DecoderBuffer> out;
      for (;;) {
        const auto status = queue.Pop(&out);
        if (status == DecoderBufferQueue::PopStatus::kOk) {
          consumed.fetch_add(1);
        } else if (status == DecoderBufferQueue::PopStatus::kAborted) {
          return;
        }
      }
    });
  }
  for (auto& t : producers) t.join();

  // Abort() discards whatever is still queued, by design: shutdown must not
  // wait for a consumer to catch up. So drain first, then abort — otherwise
  // this test measures the discard rather than the queue's correctness.
  std::vector<base::scoped_refptr<DecoderBuffer>> batch;
  for (;;) {
    batch.clear();
    if (queue.PopUpTo(64, &batch) != DecoderBufferQueue::PopStatus::kOk ||
        batch.empty()) {
      break;
    }
    consumed.fetch_add(static_cast<int>(batch.size()));
  }
  queue.Abort();
  for (auto& t : consumers) t.join();

  EXPECT_EQ(produced.load(), kProducers * kPerProducer);
  EXPECT_EQ(consumed.load(), kProducers * kPerProducer);
}

TEST_F(DecoderBufferQueueTest, PopStatusNames) {
  using S = DecoderBufferQueue::PopStatus;
  EXPECT_STREQ(GetPopStatusName(S::kOk), "ok");
  EXPECT_STREQ(GetPopStatusName(S::kAborted), "aborted");
  EXPECT_STREQ(GetPopStatusName(S::kEndOfStream), "end-of-stream");
}

}  // namespace
}  // namespace ijkpp::media
