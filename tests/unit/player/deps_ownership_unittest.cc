// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round).
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
//
// WHAT THIS PINS DOWN. player::Deps held media::DataSource,
// media::VideoDecoderFactory and media::AudioDecoderFactory in
// std::shared_ptr. All three are base::RefCountedThreadSafe with
// REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE(), protected constructors and
// protected destructors -- so those fields could not be assigned at all, and
// if they could have been, two smart pointers would have kept two independent
// counts on one object. The fields are scoped_refptr now. These tests are the
// executable form of that fix: the first one is a statement that used to be a
// compile error.

#include "player/public/deps.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/audio_decoder.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/data_source.h"
#include "media/base/video_decoder.h"
#include "media/base/video_decoder_factory.h"

#include "gtest/gtest.h"

namespace ijkpp {
namespace {

// Minimal factories. CreateVideoDecoder/CreateAudioDecoder return nullptr on
// purpose: these tests are about ownership of the factory objects, not about
// decoding, and a factory that declines is a legal answer (DecoderSelector
// treats nullptr as "try the next one").
class FakeVideoFactory final : public media::VideoDecoderFactory {
 public:
  media::VideoDecoderCapability GetCapability() const override { return {}; }
  bool SupportsCodec(media::VideoDecoderType) const override { return false; }
  std::unique_ptr<media::VideoDecoder> CreateVideoDecoder(
      const media::VideoDecoderConfig&) override {
    return nullptr;
  }
  const char* name() const override { return "fake-video"; }
  // ref_count() is protected in RefCountedThreadSafeBase, so a derived test
  // type exposes it; HasOneRef() is already public.
  int refs() const { return ref_count(); }
};

class FakeAudioFactory final : public media::AudioDecoderFactory {
 public:
  std::unique_ptr<media::AudioDecoder> CreateAudioDecoder(
      const media::AudioDecoderConfig&) override {
    return nullptr;
  }
  const char* name() const override { return "fake-audio"; }
  int refs() const { return ref_count(); }
};

// This assignment is the fix. Before the ninth round it did not compile:
// std::shared_ptr<media::DataSource> cannot be constructed from a
// MakeRefCounted result, and DataSource's destructor is protected.
TEST(DepsOwnershipTest, DataSourceFieldAcceptsARefCountedSource) {
  static constexpr uint8_t kBytes[4] = {0, 1, 2, 3};
  Deps deps;
  deps.data_source =
      base::MakeRefCounted<media::MemoryDataSource>(kBytes, sizeof(kBytes));

  ASSERT_TRUE(deps.data_source);
  EXPECT_TRUE(deps.data_source->HasOneRef());
  int64_t size = 0;
  EXPECT_TRUE(deps.data_source->GetSize(&size));
  EXPECT_EQ(4, size);
  EXPECT_TRUE(deps.data_source->IsSeekable());
}

TEST(DepsOwnershipTest, DecoderFactoryVectorsAcceptRefCountedFactories) {
  Deps deps;
  deps.video_decoder_factories.push_back(
      base::MakeRefCounted<FakeVideoFactory>());
  deps.audio_decoder_factories.push_back(
      base::MakeRefCounted<FakeAudioFactory>());

  ASSERT_EQ(1u, deps.video_decoder_factories.size());
  ASSERT_EQ(1u, deps.audio_decoder_factories.size());
  EXPECT_TRUE(deps.video_decoder_factories[0]->HasOneRef());
  EXPECT_TRUE(deps.audio_decoder_factories[0]->HasOneRef());
  EXPECT_STREQ("fake-video", deps.video_decoder_factories[0]->name());
  EXPECT_STREQ("fake-audio", deps.audio_decoder_factories[0]->name());
}

// The failure mode the old spelling would have produced: a second owner that
// does not participate in the same count. With one pointer type there is
// exactly one count, and it goes to zero exactly once.
TEST(DepsOwnershipTest, SharingAFactoryBetweenTwoDepsKeepsOneCount) {
  auto factory = base::MakeRefCounted<FakeVideoFactory>();
  ASSERT_EQ(1, factory->refs());

  Deps a;
  Deps b;
  a.video_decoder_factories.push_back(factory);
  EXPECT_EQ(2, factory->refs());
  b.video_decoder_factories.push_back(factory);
  EXPECT_EQ(3, factory->refs());

  a.video_decoder_factories.clear();
  EXPECT_EQ(2, factory->refs());
  b.video_decoder_factories.clear();
  EXPECT_EQ(1, factory->refs());   // Only this test's own reference is left.
}

// Deps is move-only (copy deleted, move defaulted). Moving must transfer the
// references rather than duplicate or drop them -- relevant because Player
// takes `std::unique_ptr<Deps>` and PlayerImpl stores it.
TEST(DepsOwnershipTest, MovingDepsTransfersOwnershipExactly) {
  static constexpr uint8_t kBytes[2] = {7, 8};
  Deps source;
  source.data_source =
      base::MakeRefCounted<media::MemoryDataSource>(kBytes, sizeof(kBytes));
  source.video_decoder_factories.push_back(
      base::MakeRefCounted<FakeVideoFactory>());
  auto* factory = source.video_decoder_factories[0].get();
  ASSERT_EQ(1, static_cast<FakeVideoFactory*>(factory)->refs());

  Deps destination(std::move(source));

  EXPECT_FALSE(source.data_source);
  EXPECT_TRUE(source.video_decoder_factories.empty());
  ASSERT_TRUE(destination.data_source);
  EXPECT_TRUE(destination.data_source->HasOneRef());
  ASSERT_EQ(1u, destination.video_decoder_factories.size());
  // Still exactly one reference: the move transferred it, it did not copy it.
  EXPECT_EQ(1, static_cast<FakeVideoFactory*>(factory)->refs());
}

// Design rule E1: a null field means "auto-detect the platform default", which
// is what makes `Player player;` work with zero setup. CreateDefault() must
// therefore NOT populate the injectable extension points -- only the clock,
// which has a process-wide default.
TEST(DepsOwnershipTest, CreateDefaultLeavesInjectionPointsNull) {
  Deps deps = Deps::CreateDefault();

  EXPECT_FALSE(deps.data_source);
  EXPECT_TRUE(deps.video_decoder_factories.empty());
  EXPECT_TRUE(deps.audio_decoder_factories.empty());
  EXPECT_FALSE(deps.video_sink_factory);
  EXPECT_FALSE(deps.audio_sink_factory);
  EXPECT_FALSE(deps.logging_delegate);
  EXPECT_FALSE(deps.event_dispatcher);

  // The clock is the one field with a process-wide default.
  ASSERT_TRUE(deps.tick_clock);
  EXPECT_FALSE(deps.tick_clock->NowTicks().is_null());
}

}  // namespace
}  // namespace ijkpp
