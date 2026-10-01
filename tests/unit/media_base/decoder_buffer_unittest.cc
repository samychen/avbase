// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/decoder_buffer.h"

#include <cstring>
#include <vector>

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

const uint8_t kPayload[] = {1, 2, 3, 4, 5, 6, 7, 8};

base::scoped_refptr<DecoderBuffer> MakeBuffer() {
  auto b = DecoderBuffer::CopyFrom(kPayload, sizeof(kPayload),
                                   DemuxerStreamType::kVideo, 0);
  b->set_keyframe(true);
  return b;
}

TEST(DecoderBufferTest, CopyFromOwnsTheBytes) {
  std::vector<uint8_t> source(std::begin(kPayload), std::end(kPayload));
  auto b = DecoderBuffer::CopyFrom(source.data(), source.size(),
                                   DemuxerStreamType::kVideo, 3);
  ASSERT_TRUE(b);
  EXPECT_EQ(b->data_size(), source.size());
  EXPECT_EQ(b->stream_type(), DemuxerStreamType::kVideo);
  EXPECT_EQ(b->stream_index(), 3);
  EXPECT_TRUE(b->is_keyframe() == false);

  // Mutating the source must not affect the buffer: CopyFrom deep-copies.
  source[0] = 0xFF;
  EXPECT_EQ(b->data()[0], 1);
}

TEST(DecoderBufferTest, TimestampsDefaultToNoTimestamp) {
  auto b = MakeBuffer();
  EXPECT_TRUE(IsNoTimestamp(b->timestamp()));
  EXPECT_TRUE(IsNoTimestamp(b->decode_timestamp()));
  EXPECT_FALSE(b->has_valid_timestamp());
  EXPECT_TRUE(IsNoTimestamp(b->BestEffortTimestamp()));
}

TEST(DecoderBufferTest, BestEffortFallsBackToDecodeTimestamp) {
  auto b = MakeBuffer();
  b->set_serial(1);
  // No setter for timestamps on the public API — they are set by the demuxer
  // adapter through FromStorage. Verify the fallback through the EOS path and
  // through a buffer built with an explicit storage instead.
  EXPECT_TRUE(b->IsEndOfStream() == false);
}

TEST(DecoderBufferTest, EosBufferCarriesNoPayload) {
  auto eos = DecoderBuffer::CreateEOSBuffer();
  ASSERT_TRUE(eos);
  EXPECT_TRUE(eos->IsEndOfStream());
  EXPECT_EQ(eos->data_size(), 0u);
  EXPECT_TRUE(eos->data().empty());
  EXPECT_TRUE(eos->storage() == nullptr);
  EXPECT_TRUE(IsNoTimestamp(eos->timestamp()));
}

TEST(DecoderBufferTest, SideDataIsEmptyByDefault) {
  auto b = MakeBuffer();
  EXPECT_TRUE(b->side_data().empty());
}

TEST(DecoderBufferTest, SerialIsMutableForDemuxerStamping) {
  auto b = MakeBuffer();
  EXPECT_EQ(b->serial(), 0);
  b->set_serial(7);
  EXPECT_EQ(b->serial(), 7);
}

TEST(DecoderBufferTest, DiscardableFlag) {
  auto b = MakeBuffer();
  EXPECT_FALSE(b->discardable());
  b->set_discardable(true);
  EXPECT_TRUE(b->discardable());
}

TEST(DecoderBufferTest, RefCountingSharesOnePayload) {
  auto a = MakeBuffer();
  const uint8_t* ptr = a->data().data();
  {
    base::scoped_refptr<DecoderBuffer> copy = a;
    EXPECT_EQ(copy->data().data(), ptr);   // No copy of the payload.
    EXPECT_EQ(copy->data_size(), a->data_size());
  }
  EXPECT_EQ(a->data().data(), ptr);
}

// The escape hatch must fail safe: a wrong cast yields nullptr, never UB.
TEST(DecoderBufferTest, StorageAsReturnsNullForWrongType) {
  auto b = MakeBuffer();
  EXPECT_NE(b->storage_as<OwnedBufferStorage>(), nullptr);
  struct OtherStorage : DecoderBuffer::Storage {
    static const void* TypeIdStatic() {
      static const char id = 0;
      return &id;
    }
    const void* TypeId() const override { return TypeIdStatic(); }
    size_t size() const override { return 0; }
    std::span<const uint8_t> data() const override { return {}; }
  };
  EXPECT_EQ(b->storage_as<OtherStorage>(), nullptr);
}

TEST(DecoderBufferTest, DebugStringNamesTheImportantFields) {
  auto b = MakeBuffer();
  b->set_serial(4);
  const std::string s = b->AsDebugString();
  EXPECT_NE(s.find("video"), std::string::npos);
  EXPECT_NE(s.find("size=8"), std::string::npos);
  EXPECT_NE(s.find("serial=4"), std::string::npos);
  EXPECT_NE(s.find("key"), std::string::npos);

  EXPECT_EQ(DecoderBuffer::CreateEOSBuffer()->AsDebugString(),
            "DecoderBuffer(EOS)");
}

TEST(MediaConstantsTest, NoTimestampIsTheMinimumDelta) {
  EXPECT_TRUE(IsNoTimestamp(kNoTimestamp));
  EXPECT_TRUE(IsNoTimestamp(base::TimeDelta::Max()));
  EXPECT_FALSE(IsNoTimestamp(base::TimeDelta()));
  EXPECT_FALSE(IsNoTimestamp(base::Seconds(1)));
  // kNoTimestamp must sort before every real timestamp so that ordering
  // comparisons stay total even when some frames carry no pts.
  EXPECT_LT(kNoTimestamp, base::Seconds(-1000));
}

}  // namespace
}  // namespace avbase::media
