// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_concat_job.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "media/base/audio_buffer.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/media_log.h"
#include "media/filters/encoded_packet.h"
#include "media/filters/ffmpeg_audio_encoder.h"
#include "media/filters/ffmpeg_demuxer.h"
#include "media/filters/ffmpeg_encode_muxer.h"

namespace avbase::media {
namespace {

// Create a small AAC file with the given frequency. |ext| picks the
// container, which is what decides the timestamp units the demuxer reports
// for these same packets — mp4 uses 1/48000, mkv 1/1000.
std::string CreateSmallAac(const std::string& suffix, double freq_hz,
                           const std::string& ext = ".mp4", int rate = 48000) {
  const int kRate = rate;
  constexpr int kChannels = 2;
  constexpr int kFrames = 1024;
  const int kNumBatches = 15;

  FFmpegAudioEncoder encoder;
  FFmpegAudioEncoder::Params params;
  params.sample_rate = kRate;
  params.channels = kChannels;
  params.bit_rate = 128000;
  EXPECT_TRUE(encoder.Initialize(params));

  std::vector<EncodedPacket> packets;
  int64_t cursor = 0;
  for (int i = 0; i < kNumBatches; ++i) {
    std::vector<uint8_t> data(kFrames * kChannels * sizeof(float));
    auto* planes = reinterpret_cast<float*>(data.data());
    for (int ch = 0; ch < kChannels; ++ch) {
      for (int j = 0; j < kFrames; ++j) {
        const int64_t n = cursor + j;
        planes[ch * kFrames + j] =
            static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * freq_hz *
                                              static_cast<double>(n) / kRate));
      }
    }
    cursor += kFrames;
    auto in = AudioBuffer::Create(
        SampleFormat::kF32P, ChannelLayout::kStereo, kChannels, kRate, kFrames,
        base::SecondsD(static_cast<double>(cursor - kFrames) / kRate),
        base::SecondsD(static_cast<double>(kFrames) / kRate), 0,
        std::move(data));
    EXPECT_TRUE(encoder.Encode(std::move(in), &packets));
  }
  EXPECT_TRUE(encoder.Flush(&packets));

  const std::string path = testing::TempDir() + "concat_in_" + suffix + ext;
  FFmpegEncodeMuxer muxer;
  EXPECT_TRUE(muxer.Open(path));
  FFmpegEncodeMuxer::AudioStreamParams sp;
  sp.sample_rate = kRate;
  sp.channels = kChannels;
  sp.extradata = encoder.extradata();
  const int stream = muxer.AddAudioStream(sp);
  EXPECT_GE(stream, 0);
  for (const auto& pkt : packets) {
    muxer.WritePacket(stream, pkt);
  }
  EXPECT_TRUE(muxer.Finish());
  return path;
}

// Demuxer-based verification helper.
base::TimeDelta GetMediaDuration(base::test::TaskEnvironment* env,
                                 const std::string& path) {
  base::scoped_refptr<MediaLog> media_log = base::MakeRefCounted<MediaLog>();
  FFmpegDemuxer demuxer(media_log);
  class Host final : public Demuxer::Host {
   public:
    void SetDuration(base::TimeDelta) override {}
    void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
    void OnDemuxerError(MediaError) override {}
  } host;
  Status opened = Err(ErrorCode::kNotImplemented, "", {}, {});
  bool done = false;
  demuxer.Initialize(DataSourceDescriptor::FromUri(path), {}, &host,
                     env->GetMainThreadTaskRunnerRef(),
                     base::BindOnce(
                         [](Status* out, bool* flag, Status s) {
                           *out = std::move(s);
                           *flag = true;
                         },
                         &opened, &done));
  for (int i = 0; i < 2000 && !done; ++i) {
    env->RunUntilIdle();
    if (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  if (!opened) {
    return base::TimeDelta();
  }
  return demuxer.media_info().duration;
}

// E4: Concatenate two compatible AAC MP4 files (fast path). The output
// should be roughly twice the duration of one input.
TEST(ConcatJobTest, TwoSegmentsFastPath) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string in1 = CreateSmallAac("a", 440.0);
  const std::string in2 = CreateSmallAac("b", 880.0);
  const std::string out = testing::TempDir() + "concat_out_fast.mp4";

  ConcatParams params;
  params.inputs = {in1, in2};
  params.output_path = out;

  const Status st = Concat(params);
  ASSERT_TRUE(st) << st.error().ToString();

  // The output must be at least as long as *both* inputs combined. This is
  // stricter than it looks and deliberately so: before the seam fix, every
  // boundary lost one frame to a backwards DTS that the muxer rejected —
  // silently, because WritePacket()'s result was ignored — and the output
  // came out SHORTER than the sum of its inputs (619ms for two 320ms
  // inputs). "Longer than one input" passes happily on that broken output;
  // the sum does not.
  const base::TimeDelta dur1 = GetMediaDuration(&env, in1);
  const base::TimeDelta dur2 = GetMediaDuration(&env, in2);
  const base::TimeDelta dur_out = GetMediaDuration(&env, out);
  ASSERT_GT(dur1, base::TimeDelta());
  ASSERT_GT(dur2, base::TimeDelta());
  EXPECT_GE(dur_out, dur1 + dur2 - base::Milliseconds(2));

  std::remove(in1.c_str());
  std::remove(in2.c_str());
  std::remove(out.c_str());
}

// E4: Three segments — two seams. The point is that the defect above scaled
// with the number of boundaries (one frame lost each), so a single-seam
// test could plausibly pass by luck of the tolerance.
TEST(ConcatJobTest, ThreeSegmentsLoseNothingAtTheSeams) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string in1 = CreateSmallAac("t1", 440.0);
  const std::string in2 = CreateSmallAac("t2", 880.0);
  const std::string in3 = CreateSmallAac("t3", 660.0);
  const std::string out = testing::TempDir() + "concat_out_three.mp4";

  ConcatParams params;
  params.inputs = {in1, in2, in3};
  params.output_path = out;

  const Status st = Concat(params);
  ASSERT_TRUE(st) << st.error().ToString();

  const base::TimeDelta total = GetMediaDuration(&env, in1) +
                                GetMediaDuration(&env, in2) +
                                GetMediaDuration(&env, in3);
  const base::TimeDelta dur_out = GetMediaDuration(&env, out);
  EXPECT_GE(dur_out, total - base::Milliseconds(4));

  std::remove(in1.c_str());
  std::remove(in2.c_str());
  std::remove(in3.c_str());
  std::remove(out.c_str());
}

// E4: Single input concat is a remux.
TEST(ConcatJobTest, SingleInputIsRemux) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string in1 = CreateSmallAac("single", 440.0);
  const std::string out = testing::TempDir() + "concat_out_single.mp4";

  ConcatParams params;
  params.inputs = {in1};
  params.output_path = out;

  const Status st = Concat(params);
  ASSERT_TRUE(st) << st.error().ToString();

  const base::TimeDelta dur_out = GetMediaDuration(&env, out);
  EXPECT_GT(dur_out, base::TimeDelta());

  std::remove(in1.c_str());
  std::remove(out.c_str());
}

// E4 + E3c: segments do not have to come from mp4. mkv reports audio
// timestamps in 1/1000 while the output stream is declared 1/sample_rate, so
// a seam can be perfectly continuous in *rebased* numbers and still be 48×
// off in real time. The mp4 run is the self-calibrating reference.
TEST(ConcatJobTest, SegmentsFromOtherContainersKeepTheirDuration) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);

  const std::string r1 = CreateSmallAac("ref1", 440.0, ".mp4");
  const std::string r2 = CreateSmallAac("ref2", 880.0, ".mp4");
  const std::string ref_out = testing::TempDir() + "concat_tbref.mp4";
  ConcatParams refp;
  refp.inputs = {r1, r2};
  refp.output_path = ref_out;
  ASSERT_TRUE(Concat(refp)) << "reference concat failed";
  const double ref_ms = GetMediaDuration(&env, ref_out).InMillisecondsF();
  ASSERT_GT(ref_ms, 100.0) << "reference concat produced nothing";

  const std::string m1 = CreateSmallAac("mkv1", 440.0, ".mkv");
  const std::string m2 = CreateSmallAac("mkv2", 880.0, ".mkv");
  const std::string out = testing::TempDir() + "concat_mkv_out.mp4";
  ConcatParams params;
  params.inputs = {m1, m2};
  params.output_path = out;
  const Status st = Concat(params);
  ASSERT_TRUE(st) << st.error().ToString();

  const double got_ms = GetMediaDuration(&env, out).InMillisecondsF();
  EXPECT_NEAR(got_ms, ref_ms, ref_ms * 0.25)
      << "concat of mkv segments did not convert timestamp units: expected ~"
      << ref_ms << "ms like the mp4 reference, got " << got_ms << "ms";

  std::remove(r1.c_str());
  std::remove(r2.c_str());
  std::remove(ref_out.c_str());
  std::remove(m1.c_str());
  std::remove(m2.c_str());
  std::remove(out.c_str());
}

// E4b: segments whose codec parameters differ cannot be copied straight
// through and take the slow path (each segment normalized through a temp
// file before it is placed on the output timeline).
TEST(ConcatJobTest, IncompatibleParametersTakeTheSlowPath) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string in1 = CreateSmallAac("slow1", 440.0, ".mp4", 48000);
  const std::string in2 = CreateSmallAac("slow2", 880.0, ".mp4", 44100);
  const std::string out = testing::TempDir() + "concat_out_slow.mp4";

  const double d1 = GetMediaDuration(&env, in1).InMillisecondsF();
  const double d2 = GetMediaDuration(&env, in2).InMillisecondsF();
  ASSERT_GT(d1, 100.0) << "input 1 could not be probed";
  ASSERT_GT(d2, 100.0) << "input 2 could not be probed";

  ConcatParams params;
  params.inputs = {in1, in2};
  params.output_path = out;
  const Status st = Concat(params);
  ASSERT_TRUE(st) << st.error().ToString();

  const double got = GetMediaDuration(&env, out).InMillisecondsF();
  EXPECT_NEAR(got, d1 + d2, (d1 + d2) * 0.25)
      << "slow-path concat should span both segments: expected ~" << (d1 + d2)
      << "ms, got " << got << "ms";

  std::remove(in1.c_str());
  std::remove(in2.c_str());
  std::remove(out.c_str());
}

}  // namespace
}  // namespace avbase::media
