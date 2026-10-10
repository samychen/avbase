// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_transcode_job.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/media_log.h"
#include "media/ffmpeg/ffmpeg_audio_encoder.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"
#include "media/ffmpeg/ffmpeg_encode_muxer.h"
#include "media/ffmpeg/ffmpeg_video_encoder.h"

namespace avbase::media {
namespace {

// Helper: create a small file with AAC audio for use as transcode input.
// |ext| selects the container, which is what decides the *stream timebase*
// the demuxer later reports for these very same packets — see
// CopyModeRescalesAcrossContainerTimebases below.
std::string CreateTestAudioFile(const std::string& suffix,
                                const std::string& ext = ".mp4") {
  FFmpegAudioEncoder encoder;
  FFmpegAudioEncoder::Params params;
  params.sample_rate = 48000;
  params.channels = 2;
  params.bit_rate = 128000;
  EXPECT_TRUE(encoder.Initialize(params));

  constexpr int kRate = 48000;
  constexpr int kChannels = 2;
  constexpr int kFrames = 1024;
  std::vector<EncodedPacket> packets;
  int64_t cursor = 0;
  for (int i = 0; i < 20; ++i) {
    std::vector<uint8_t> data(kFrames * kChannels * sizeof(float));
    auto* planes = reinterpret_cast<float*>(data.data());
    for (int ch = 0; ch < kChannels; ++ch) {
      for (int j = 0; j < kFrames; ++j) {
        const int64_t n = cursor + j;
        planes[ch * kFrames + j] =
            static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 *
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

  const std::string path = testing::TempDir() + "transcode_in_" + suffix + ext;
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

// E3: Transcode with -c copy (stream copy). The output should have the same
// audio parameters as the input.
TEST(TranscodeJobTest, CopyModePreservesAudioParameters) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestAudioFile("copy");
  const std::string output = testing::TempDir() + "transcode_out_copy.mp4";

  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "copy";
  params.video_codec.codec = "";  // Drop video.

  const Status st = Transcode(input, params);
  ASSERT_TRUE(st) << st.error().ToString();

  // Verify the output has the same sample rate.
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
  demuxer.Initialize(DataSourceDescriptor::FromUri(output), {}, &host,
                     env.GetMainThreadTaskRunnerRef(),
                     base::BindOnce(
                         [](Status* out, bool* flag, Status s) {
                           *out = std::move(s);
                           *flag = true;
                         },
                         &opened, &done));
  for (int i = 0; i < 2000 && !done; ++i) {
    env.RunUntilIdle();
    if (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_TRUE(opened) << opened.error().ToString();
  DemuxerStream* audio = demuxer.GetStream(DemuxerStreamType::kAudio);
  ASSERT_TRUE(audio);
  EXPECT_EQ(audio->audio_decoder_config().sample_rate, 48000);

  std::remove(input.c_str());
  std::remove(output.c_str());
}

// E3: Transcode with re-encode (AAC → AAC). The output should be playable
// and have the expected sample rate.
TEST(TranscodeJobTest, ReEncodeProducesValidOutput) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestAudioFile("reenc");
  const std::string output = testing::TempDir() + "transcode_out_reenc.mp4";

  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "aac";
  params.audio_codec.bit_rate = 64000;
  params.video_codec.codec = "";

  const Status st = Transcode(input, params);
  ASSERT_TRUE(st) << st.error().ToString();

  // Verify the output is readable.
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
  demuxer.Initialize(DataSourceDescriptor::FromUri(output), {}, &host,
                     env.GetMainThreadTaskRunnerRef(),
                     base::BindOnce(
                         [](Status* out, bool* flag, Status s) {
                           *out = std::move(s);
                           *flag = true;
                         },
                         &opened, &done));
  for (int i = 0; i < 2000 && !done; ++i) {
    env.RunUntilIdle();
    if (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_TRUE(opened) << opened.error().ToString();
  DemuxerStream* audio = demuxer.GetStream(DemuxerStreamType::kAudio);
  ASSERT_TRUE(audio);
  EXPECT_EQ(audio->audio_decoder_config().sample_rate, 48000);

  std::remove(input.c_str());
  std::remove(output.c_str());
}

// E3: Trim (-t) should produce a shorter output.
TEST(TranscodeJobTest, TrimLimitsDuration) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestAudioFile("trim");
  const std::string output = testing::TempDir() + "transcode_out_trim.mp4";

  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "copy";
  params.video_codec.codec = "";
  params.duration_seconds = 0.1;  // Only 100ms of ~430ms total.

  const Status st = Transcode(input, params);
  ASSERT_TRUE(st) << st.error().ToString();

  // Verify the output is shorter than the full input.
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
  demuxer.Initialize(DataSourceDescriptor::FromUri(output), {}, &host,
                     env.GetMainThreadTaskRunnerRef(),
                     base::BindOnce(
                         [](Status* out, bool* flag, Status s) {
                           *out = std::move(s);
                           *flag = true;
                         },
                         &opened, &done));
  for (int i = 0; i < 2000 && !done; ++i) {
    env.RunUntilIdle();
    if (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_TRUE(opened) << opened.error().ToString();
  const base::TimeDelta out_dur = demuxer.media_info().duration;
  // Trimmed output should be much shorter than 400ms.
  EXPECT_LT(out_dur, base::Milliseconds(300));

  std::remove(input.c_str());
  std::remove(output.c_str());
}

// Opens |path| with the demuxer and returns the container snapshot. This is
// deliberately the *whole* MediaInfo and not just the duration: the trim
// bug below hid inside a duration that looked perfectly correct.
MediaInfo Probe(base::test::TaskEnvironment* env, const std::string& path) {
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
    ADD_FAILURE() << "cannot reopen " << path << ": "
                  << opened.error().ToString();
    return {};
  }
  return demuxer.media_info();
}

// E3: -ss must land the output at the START of the timeline. The obvious
// "seek the demuxer and write what comes out" keeps the post-seek
// timestamps, so a 200ms trim produces an output whose timeline still
// begins at 200ms — 426ms reported for what should be a ~226ms file, i.e.
// two thirds silence and the trim invisible to every duration-based check.
TEST(TranscodeJobTest, StartTrimRebasesTimelineToZero) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestAudioFile("ss");
  const std::string output = testing::TempDir() + "transcode_out_ss.mp4";

  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "copy";
  params.video_codec.codec = "";
  params.start_time_seconds = 0.2;  // Skip the first 200ms of ~430ms.

  const Status st = Transcode(input, params);
  ASSERT_TRUE(st) << st.error().ToString();

  const MediaInfo in = Probe(&env, input);
  const MediaInfo out_info = Probe(&env, output);
  ASSERT_GT(in.duration, base::TimeDelta());
  ASSERT_GT(out_info.duration, base::TimeDelta());
  // (a) The copy path must drop what -ss skipped rather than shorten the
  //     file from the tail.
  EXPECT_LE(out_info.duration, in.duration - base::Milliseconds(150));
  // (b) And the surviving audio must sit at the START of the timeline. This
  //     is the assertion the duration check above cannot make on its own:
  //     the buggy output reported 234ms — a perfectly plausible duration —
  //     while actually beginning 200ms in, i.e. two thirds of dead air that
  //     no duration-based check can see.
  EXPECT_LE(out_info.start_time, base::Milliseconds(1))
      << "output timeline starts at " << out_info.start_time
      << "; -ss must rebase, not merely stop writing";

  std::remove(input.c_str());
  std::remove(output.c_str());
}

// Helper: a small mjpeg-in-matroska file used as video transcode input.
// matroska because the pinned build has no mp4 video encoder dependency
// beyond mjpeg, and mjpeg because it is one of exactly two video encoders
// actually linked into this FFmpeg (measured with avcodec_find_encoder:
// mjpeg yes, libx264/libx265/libvpx/h264_videotoolbox/hevc_videotoolbox no).
std::string CreateTestVideoFile(const std::string& suffix) {
  const Size size{128, 96};
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "mjpeg";
  params.width = size.width;
  params.height = size.height;
  params.fps_num = 30;
  params.fps_den = 1;
  EXPECT_TRUE(encoder.Initialize(params));

  std::vector<EncodedPacket> packets;
  for (int i = 0; i < 10; ++i) {
    auto frame = VideoFrame::CreateBlackFrame(
        VideoFormat::kI420, size, size, Rational{1, 1},
        base::Milliseconds(i * 33), base::Milliseconds(33), 0);
    for (int y = 0; y < size.height; ++y) {
      for (int x = 0; x < size.width; ++x) {
        frame->mutable_data(
            VideoFrame::kYPlane)[static_cast<size_t>(y) *
                                     static_cast<size_t>(size.width) +
                                 static_cast<size_t>(x)] =
            static_cast<uint8_t>((x * 2 + y) & 0xff);
      }
    }
    EXPECT_TRUE(encoder.Encode(std::move(frame), &packets));
  }
  EXPECT_TRUE(encoder.Flush(&packets));

  const std::string path =
      testing::TempDir() + "transcode_in_" + suffix + ".mkv";
  FFmpegEncodeMuxer muxer;
  EXPECT_TRUE(muxer.Open(path));
  FFmpegEncodeMuxer::VideoStreamParams sp;
  sp.codec_name = "mjpeg";
  sp.width = size.width;
  sp.height = size.height;
  sp.time_base_num = 1;
  sp.time_base_den = 30;
  const int stream = muxer.AddVideoStream(sp);
  EXPECT_GE(stream, 0);
  for (const auto& pkt : packets) {
    muxer.WritePacket(stream, pkt);
  }
  EXPECT_TRUE(muxer.Finish());
  return path;
}

// A factory that answers kH264 with "mjpeg" — deliberately NOT the encoder
// that was asked for, so whatever the job resolves is visible in the output
// container and cannot be mistaken for the requested name.
class RenamingEncoderFactory final : public media::ffmpeg::VideoEncoderFactory {
 public:
  RenamingEncoderFactory() = default;

  EncoderCapability GetCapability() const override {
    EncoderCapability cap;
    cap.priority = 100;
    return cap;
  }
  bool SupportsCodec(media::VideoCodec, media::HwCodecMask) const override {
    return true;
  }
  std::string EncoderNameFor(media::VideoCodec codec) const override {
    return codec == media::VideoCodec::kH264 ? "mjpeg" : std::string();
  }
  std::unique_ptr<media::FFmpegVideoEncoder>
  CreateEncoder(const std::string&, int, int, int, int, int, const std::string&,
                int, int) override {
    return nullptr;  // Transcode builds its own encoder from the name.
  }
  const char* name() const override { return "test-renaming"; }

 private:
  friend class base::RefCountedThreadSafe<media::ffmpeg::VideoEncoderFactory>;
  ~RenamingEncoderFactory() override = default;
};

// E5: the encoder factory must actually DECIDE the encoder. The job is
// asked for "libx264" while the injected factory answers "mjpeg", so the
// output container records whatever the factory resolved — proving the
// selection result reaches the encoder rather than being computed and
// thrown away.
TEST(TranscodeJobTest, EncoderFactoryDecidesTheEncoderName) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestVideoFile("factory");
  const std::string output = testing::TempDir() + "transcode_out_factory.mkv";

  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "";         // Drop audio.
  params.video_codec.codec = "libx264";  // Not what will be used.
  params.video_codec.factories.push_back(
      base::MakeRefCounted<RenamingEncoderFactory>());

  const Status st = Transcode(input, params);
  ASSERT_TRUE(st) << st.error().ToString();

  const MediaInfo info = Probe(&env, output);
  const std::vector<const StreamInfo*>* videos = nullptr;
  std::vector<const StreamInfo*> found = info.StreamsOfKind(StreamKind::kVideo);
  videos = &found;
  ASSERT_EQ(videos->size(), 1u);
  EXPECT_EQ((*videos)[0]->codec_name, "mjpeg")
      << "the factory's resolved name must reach the encoder; the requested "
         "one would have produced h264";

  std::remove(input.c_str());
  std::remove(output.c_str());
}

// E3c: stream copy must RESCALE timestamps, not forward the raw integers.
//
// The muxer's source timebase for audio is always 1/sample_rate, but the
// demuxer reports a different stream timebase per container for the very
// same AAC payload. Measured on this build:
//     mp4  1/48000   (coincides with the muxer's assumption — why this
//                     defect stayed invisible in every test so far)
//     mkv  1/1000
//     adts 1/28224000
// Passing the raw numbers through turns 448ms of audio into 9ms (mkv) or
// 263 seconds (adts). The mp4 case is used as the self-calibrating
// reference so the tolerance is not a magic number.
TEST(TranscodeJobTest, CopyModeRescalesAcrossContainerTimebases) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);

  // Reference: the same payload copied out of an mp4.
  const std::string ref_in = CreateTestAudioFile("tb_ref", ".mp4");
  const std::string ref_out = testing::TempDir() + "transcode_tb_ref.mp4";
  TranscodeParams ref_params;
  ref_params.output_path = ref_out;
  ref_params.audio_codec.codec = "copy";
  ref_params.video_codec.codec = "";
  ASSERT_TRUE(Transcode(ref_in, ref_params)) << "reference copy failed";
  const double ref_ms = Probe(&env, ref_out).duration.InMillisecondsF();
  ASSERT_GT(ref_ms, 100.0) << "reference copy produced nothing";

  // The same payload in containers whose audio timebase differs.
  for (const char* ext : {".mkv", ".aac"}) {
    const std::string name = std::string("tb_") + (ext + 1);
    const std::string input = CreateTestAudioFile(name, ext);
    const std::string output =
        testing::TempDir() + "transcode_tb_" + (ext + 1) + ".mp4";

    TranscodeParams params;
    params.output_path = output;
    params.audio_codec.codec = "copy";
    params.video_codec.codec = "";
    const Status st = Transcode(input, params);
    ASSERT_TRUE(st) << "copy out of " << ext << ": " << st.error().ToString();

    const double got_ms = Probe(&env, output).duration.InMillisecondsF();
    EXPECT_NEAR(got_ms, ref_ms, ref_ms * 0.25)
        << "stream copy out of " << ext
        << " did not rescale timestamps: expected ~" << ref_ms
        << "ms like the mp4 reference, got " << got_ms << "ms";

    std::remove(input.c_str());
    std::remove(output.c_str());
  }
  std::remove(ref_in.c_str());
  std::remove(ref_out.c_str());
}

// E3/E4b: asking for a different sample rate must RESAMPLE, not relabel.
// The encoder is opened at the requested rate but the decoded frames stay
// at the input's, so without resampling the same audio is re-stamped into a
// different-length timeline: 16 frames of 44100 audio asked for 24000 come
// out 682ms long instead of 371ms. Duration is the observable, and the
// error is far too large for any sane tolerance to swallow.
TEST(TranscodeJobTest, SampleRateChangeResamplesRatherThanRelabels) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestAudioFile("sr44100", ".mp4");
  // Re-encode the same payload at 44100 first; our muxer writes it out and
  // the demuxer reads it back with a real duration to compare against.
  const std::string src441 = testing::TempDir() + "transcode_sr_src.mp4";
  TranscodeParams mk;
  mk.output_path = src441;
  mk.audio_codec.codec = "aac";
  mk.audio_codec.sample_rate = 44100;
  mk.video_codec.codec = "";
  ASSERT_TRUE(Transcode(input, mk)) << "preparing the 44100 source failed";
  const double in_ms = Probe(&env, src441).duration.InMillisecondsF();
  ASSERT_GT(in_ms, 100.0) << "source has no duration";

  const std::string output = testing::TempDir() + "transcode_sr_out.mp4";
  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "aac";
  params.audio_codec.sample_rate = 24000;  // Deliberately far from 44100.
  params.video_codec.codec = "";
  const Status st = Transcode(src441, params);
  ASSERT_TRUE(st) << st.error().ToString();

  const double got_ms = Probe(&env, output).duration.InMillisecondsF();

  // The assertion is on the RATIO, not on a tight tolerance, because the
  // exact figure legitimately moves: a resample can round the content up by
  // one AAC frame (1024 samples = 43ms at 24kHz) and the container reports
  // the encoder's priming delay on top of that. What must NOT happen is the
  // length tracking the ratio of the rates — relabelling 44100 audio as
  // 24000 makes it exactly 44100/24000 = 1.84× longer (measured: 938ms for
  // a 488ms source before the resampler existed).
  const double ratio = got_ms / in_ms;
  EXPECT_LT(ratio, 1.35)
      << "output length tracks the sample-rate ratio (1.84) instead of the "
         "audio: source was "
      << in_ms << "ms, output is " << got_ms
      << "ms — the frames were relabelled instead of resampled";
  EXPECT_GT(ratio, 0.85) << "resampling lost audio: source was " << in_ms
                         << "ms, output is " << got_ms << "ms";

  std::remove(input.c_str());
  std::remove(src441.c_str());
  std::remove(output.c_str());
}

// E3: Progress callback should fire and reach 100.
TEST(TranscodeJobTest, ProgressCallbackReaches100) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestAudioFile("prog");
  const std::string output = testing::TempDir() + "transcode_out_prog.mp4";

  int max_progress = 0;
  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "copy";
  params.video_codec.codec = "";

  const Status st = Transcode(input, params, [&max_progress](int pct) {
    max_progress = std::max(max_progress, pct);
  });
  ASSERT_TRUE(st) << st.error().ToString();
  EXPECT_EQ(max_progress, 100);

  std::remove(input.c_str());
  std::remove(output.c_str());
}

// E3b: the async handle drives the same pump on its own thread, reports 100%
// progress and the final status through done_cb, and leaves a real output.
// The second Start() reuses the same handle, which is the path that has to
// reap the previous thread before it can install the next one.
TEST(TranscodeJobTest, AsyncJobRunsAndReportsCompletion) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestAudioFile("async");
  const std::string output = testing::TempDir() + "transcode_out_async.mp4";

  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "copy";
  params.video_codec.codec = "";

  TranscodeJob job;
  for (int run = 0; run < 2; ++run) {
    std::atomic<int> last_pct{-1};
    std::atomic<bool> done{false};
    Status final_status = Err(ErrorCode::kNotImplemented, "", {}, {});
    ASSERT_TRUE(job.Start(
        input, params, [&last_pct](int pct) { last_pct.store(pct); },
        [&final_status, &done](Status s) {
          final_status = std::move(s);
          done.store(true);
        }))
        << "run " << run;
    job.Wait();
    ASSERT_TRUE(done.load()) << "run " << run;
    ASSERT_TRUE(final_status)
        << "run " << run << ": " << final_status.error().ToString();
    EXPECT_EQ(last_pct.load(), 100) << "run " << run;
    EXPECT_FALSE(job.IsRunning()) << "run " << run;
  }

  // The output is a real, readable container — not just a created file.
  EXPECT_GT(Probe(&env, output).duration.InMillisecondsF(), 100.0);

  std::remove(input.c_str());
  std::remove(output.c_str());
}

// E3b: Cancel() from the first progress tick (i.e. from inside the worker's
// own callback) stops the job and the completion status is kCancelled, not a
// silently truncated success.
TEST(TranscodeJobTest, CancelStopsTheJobWithCancelledStatus) {
  base::test::TaskEnvironment env(
      base::test::TaskEnvironment::TimeSource::kRealTime);
  const std::string input = CreateTestAudioFile("cancel");
  const std::string output = testing::TempDir() + "transcode_out_cancel.mp4";

  TranscodeParams params;
  params.output_path = output;
  params.audio_codec.codec = "copy";
  params.video_codec.codec = "";

  TranscodeJob job;
  std::atomic<bool> done{false};
  std::atomic<int> progress_calls{0};
  Status final_status = Err(ErrorCode::kNotImplemented, "", {}, {});
  ASSERT_TRUE(job.Start(
      input, params,
      [&job, &progress_calls](int /*pct*/) {
        // 21 input packets guarantee the boundary check gets its chance.
        if (progress_calls.fetch_add(1) == 0) {
          job.Cancel();
        }
      },
      [&final_status, &done](Status s) {
        final_status = std::move(s);
        done.store(true);
      }));
  job.Wait();

  ASSERT_TRUE(done.load());
  ASSERT_FALSE(final_status);
  EXPECT_EQ(final_status.error().code(), ErrorCode::kCancelled);
  EXPECT_FALSE(job.IsRunning());

  std::remove(input.c_str());
  std::remove(output.c_str());
}

}  // namespace
}  // namespace avbase::media
