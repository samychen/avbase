// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// DecoderStream tests. Everything here is fake -- no FFmpeg, no threads -- so
// the selection, fallback, watermark and flush behaviour can be asserted
// deterministically. That is the point of moving off ffplay's two hand-written
// decode threads: those behaviours were untestable there.

#include "media/filters/decoder_stream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"
#include "media/base/audio_decoder_factory.h"

namespace avbase::media {
namespace {

// ---- fakes ----------------------------------------------------------------

// Hands out a scripted list of DecoderBuffers. Read() completes inline, which
// is stricter than the real contract ("never runs its callback inline") and so
// exercises DecoderStream's re-entrancy guards.
class ScriptedDemuxerStream final : public DemuxerStream {
 public:
  explicit ScriptedDemuxerStream(DemuxerStream::DecoderBufferVector buffers)
      : buffers_(std::move(buffers)) {}

  // Caps how many buffers one Read() returns, so a test can leave data behind
  // for a post-flush read. The real demuxer may return anywhere in 1..count.
  void set_max_per_read(uint32_t n) { max_per_read_ = n; }

  void Read(uint32_t count, ReadCB read_cb) override {
    ++read_count_;
    const uint32_t limit = std::min(count, max_per_read_);
    DemuxerStream::DecoderBufferVector out;
    while (out.size() < limit && next_ < buffers_.size()) {
      out.push_back(std::move(buffers_[next_++]));
    }
    if (out.empty()) {
      // Nothing left: the real demuxer would block or emit EOS. Emitting EOS
      // keeps the test deterministic.
      out.push_back(DecoderBuffer::CreateEOSBuffer());
    }
    std::move(read_cb).Run(Status::kOk, std::move(out));
  }

  const AudioDecoderConfig& audio_decoder_config() const override {
    return audio_config_;
  }
  const VideoDecoderConfig& video_decoder_config() const override {
    return video_config_;
  }
  DemuxerStreamType type() const override { return DemuxerStreamType::kAudio; }
  int32_t stream_index() const override { return 0; }
  bool SupportsConfigChanges() const override { return false; }
  int32_t serial() const override { return serial_; }
  size_t buffered_buffers() const override { return buffers_.size() - next_; }
  size_t buffered_bytes() const override { return 0; }
  base::TimeDelta buffered_duration() const override {
    return base::TimeDelta();
  }

  void set_serial(int32_t serial) { serial_ = serial; }
  int read_count() const { return read_count_; }

 private:
  AudioDecoderConfig audio_config_;
  VideoDecoderConfig video_config_;
  DemuxerStream::DecoderBufferVector buffers_;
  size_t next_ = 0;
  int32_t serial_ = 0;
  int read_count_ = 0;
  uint32_t max_per_read_ = 8;
};

// Turns each input buffer into |outputs_per_buffer| AudioBuffers. Can be made
// to fail initialization or to fail every decode, to drive the fallback paths.
class FakeAudioDecoder final : public AudioDecoder {
 public:
  struct Behaviour {
    bool init_fails{false};
    bool decode_fails{false};
    // Holds the DecodeCB instead of running it inline, so a Read can still be
    // outstanding when Flush() arrives.
    bool defer_decode{false};
    int outputs_per_buffer{2};
    int* init_attempts{nullptr};
    int* decode_calls{nullptr};
  };

  explicit FakeAudioDecoder(Behaviour behaviour) : behaviour_(behaviour) {}

  std::string GetDisplayName() const override { return "FakeAudioDecoder"; }

  // |config| is unnamed: the fake accepts whatever it is handed, and
  // -Wunused-parameter (debug preset, -Werror) rejects a named one.
  void Initialize(const AudioDecoderConfig& /*config*/, bool, int32_t serial,
                  InitCB init_cb, const OutputCB& output_cb,
                  const WaitingCB&) override {
    if (behaviour_.init_attempts) {
      ++*behaviour_.init_attempts;
    }
    serial_ = serial;
    output_cb_ = output_cb;
    if (behaviour_.init_fails) {
      std::move(init_cb).Run(DecoderStatus(
          DecoderStatus::Codes::kUnsupportedCodec, "fake init failure"));
      return;
    }
    std::move(init_cb).Run(DecoderStatus());
  }

  void Decode(base::scoped_refptr<DecoderBuffer> buffer,
              DecodeCB decode_cb) override {
    if (behaviour_.decode_calls) {
      ++*behaviour_.decode_calls;
    }
    if (behaviour_.decode_fails) {
      std::move(decode_cb).Run(DecoderStatus(DecoderStatus::Codes::kDecodeError,
                                             "fake decode failure"));
      return;
    }
    if (behaviour_.defer_decode) {
      deferred_.push_back(std::move(decode_cb));
      return;
    }
    if (!buffer || buffer->IsEndOfStream()) {
      output_cb_.Run(AudioBuffer::CreateEOSBuffer());
      std::move(decode_cb).Run(DecoderStatus());
      return;
    }
    // Stamp from the buffer, exactly as FFmpegAudioDecoder does: the serial
    // travels with the data, so a Flush() that bumps it takes effect on the
    // very next buffer without re-initializing the decoder.
    const int32_t buffer_serial = buffer->serial();
    for (int i = 0; i < behaviour_.outputs_per_buffer; ++i) {
      output_cb_.Run(AudioBuffer::Create(
          SampleFormat::kF32P, ChannelLayout::kStereo, 2, 48000, 1024,
          base::Milliseconds(i), base::Microseconds(21333), buffer_serial,
          std::vector<uint8_t>(1024 * 4 * 2, 0)));
    }
    std::move(decode_cb).Run(DecoderStatus());
  }

  void Reset(base::OnceClosure closure) override {
    ++reset_count_;
    // Chromium's contract: Reset() aborts pending Decode() calls, running their
    // callbacks with kDecodingAborted, before |closure| runs.
    for (auto& cb : deferred_) {
      std::move(cb).Run(
          DecoderStatus(DecoderStatus::Codes::kDecodingAborted, "reset"));
    }
    deferred_.clear();
    std::move(closure).Run();
  }

  size_t deferred_count() const { return deferred_.size(); }

  int reset_count() const { return reset_count_; }

 private:
  Behaviour behaviour_;
  OutputCB output_cb_;
  std::vector<DecodeCB> deferred_;
  int32_t serial_ = 0;
  int reset_count_ = 0;
};

class FakeAudioDecoderFactory final : public AudioDecoderFactory {
 public:
  explicit FakeAudioDecoderFactory(std::string name,
                                   FakeAudioDecoder::Behaviour behaviour,
                                   bool declines = false)
      : name_(std::move(name)), behaviour_(behaviour), declines_(declines) {}

  std::unique_ptr<AudioDecoder>
  CreateAudioDecoder(const AudioDecoderConfig&) override {
    ++create_calls_;
    return declines_ ? nullptr : std::make_unique<FakeAudioDecoder>(behaviour_);
  }
  const char* name() const override { return name_.c_str(); }

  int create_calls() const { return create_calls_; }

 private:
  std::string name_;
  FakeAudioDecoder::Behaviour behaviour_;
  bool declines_;
  int create_calls_ = 0;
};

base::scoped_refptr<DecoderBuffer> MakeBuffer(int32_t serial, int n = 16) {
  std::vector<uint8_t> data(static_cast<size_t>(n), 0xAB);
  auto buffer = DecoderBuffer::CopyFrom(data.data(), data.size(),
                                        DemuxerStreamType::kAudio, 0);
  buffer->set_serial(serial);
  return buffer;
}

class DecoderStreamTest : public ::testing::Test {
 protected:
  void SetUp() override { runner_ = env_.GetMainThreadTaskRunnerRef(); }

  // Drains a stream to EOS, collecting outputs. Bounded so a stall in the
  // implementation fails the test instead of hanging it.
  void ReadUntilEos(AudioDecoderStream* stream, int max_reads = 5000) {
    for (int i = 0; i < max_reads; ++i) {
      bool done = false;
      DecoderStatus status;
      base::scoped_refptr<AudioBuffer> output;
      stream->Read(base::BindOnce(
          [](bool* flag, DecoderStatus* s, base::scoped_refptr<AudioBuffer>* o,
             DecoderStatus got, base::scoped_refptr<AudioBuffer> out) {
            *flag = true;
            *s = got;
            *o = std::move(out);
          },
          &done, &status, &output));
      env_.RunUntilIdle();
      if (!done) {
        ADD_FAILURE() << "Read() never completed at iteration " << i;
        return;
      }
      if (!output) {
        // kOk + null output is the EOS signal.
        ASSERT_TRUE(status.is_ok()) << status.AsDebugString();
        return;
      }
      ++outputs_;
    }
    ADD_FAILURE() << "stream did not reach EOS within " << max_reads
                  << " reads";
  }

  base::test::TaskEnvironment env_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  int outputs_ = 0;
};

// ---- tests ----------------------------------------------------------------

TEST_F(DecoderStreamTest, DecodesWholeStreamThenReportsEndOfStream) {
  DemuxerStream::DecoderBufferVector buffers;
  for (int i = 0; i < 5; ++i) {
    buffers.push_back(MakeBuffer(/*serial=*/0));
  }
  buffers.push_back(DecoderBuffer::CreateEOSBuffer());
  ScriptedDemuxerStream stream(std::move(buffers));

  FakeAudioDecoder::Behaviour behaviour;
  behaviour.outputs_per_buffer = 2;
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories;
  factories.push_back(
      base::MakeRefCounted<FakeAudioDecoderFactory>("fake", behaviour));

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  DecoderStatus init_status;
  bool init_done = false;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce(
          [](bool* flag, DecoderStatus* out, DecoderStatus s) {
            *flag = true;
            *out = s;
          },
          &init_done, &init_status));
  env_.RunUntilIdle();
  ASSERT_TRUE(init_done);
  ASSERT_TRUE(init_status.is_ok()) << init_status.AsDebugString();

  ReadUntilEos(decoder_stream.get());
  // 5 real buffers * 2 outputs each. The EOS buffer contributes no output.
  EXPECT_EQ(10, outputs_);
  EXPECT_EQ(10u, decoder_stream->outputs_decoded());
  EXPECT_EQ(0u, decoder_stream->decode_errors());
}

TEST_F(DecoderStreamTest, FallsBackWhenFirstFactoryDeclinesTheConfig) {
  DemuxerStream::DecoderBufferVector buffers;
  buffers.push_back(MakeBuffer(0));
  buffers.push_back(DecoderBuffer::CreateEOSBuffer());
  ScriptedDemuxerStream stream(std::move(buffers));

  auto declining = base::MakeRefCounted<FakeAudioDecoderFactory>(
      "declining", FakeAudioDecoder::Behaviour(), /*declines=*/true);
  auto working = base::MakeRefCounted<FakeAudioDecoderFactory>(
      "working", FakeAudioDecoder::Behaviour());
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories{declining,
                                                                  working};

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  std::vector<DecoderStreamEvent> events;
  decoder_stream->set_event_cb(
      base::BindRepeating([](std::vector<DecoderStreamEvent>* sink,
                             DecoderStreamEvent e) { sink->push_back(e); },
                          &events));
  bool init_done = false;
  DecoderStatus init_status;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce(
          [](bool* flag, DecoderStatus* out, DecoderStatus s) {
            *flag = true;
            *out = s;
          },
          &init_done, &init_status));
  env_.RunUntilIdle();

  ASSERT_TRUE(init_done);
  EXPECT_TRUE(init_status.is_ok()) << init_status.AsDebugString();
  // A factory that declines is not a fallback event -- it never claimed the
  // config. Only an initialize *failure* is worth surfacing to the user.
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(0u, decoder_stream->fallbacks());
  EXPECT_EQ(1, declining->create_calls());
  EXPECT_EQ(1, working->create_calls());
}

TEST_F(DecoderStreamTest, FallsBackAndReportsEventWhenDecoderInitFails) {
  DemuxerStream::DecoderBufferVector buffers;
  buffers.push_back(MakeBuffer(0));
  buffers.push_back(DecoderBuffer::CreateEOSBuffer());
  ScriptedDemuxerStream stream(std::move(buffers));

  FakeAudioDecoder::Behaviour broken;
  broken.init_fails = true;
  auto first = base::MakeRefCounted<FakeAudioDecoderFactory>("hw", broken);
  auto second = base::MakeRefCounted<FakeAudioDecoderFactory>(
      "sw", FakeAudioDecoder::Behaviour());
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories{first,
                                                                  second};

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  std::vector<DecoderStreamEvent> events;
  decoder_stream->set_event_cb(
      base::BindRepeating([](std::vector<DecoderStreamEvent>* sink,
                             DecoderStreamEvent e) { sink->push_back(e); },
                          &events));
  bool init_done = false;
  DecoderStatus init_status;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce(
          [](bool* flag, DecoderStatus* out, DecoderStatus s) {
            *flag = true;
            *out = s;
          },
          &init_done, &init_status));
  env_.RunUntilIdle();

  ASSERT_TRUE(init_done);
  EXPECT_TRUE(init_status.is_ok()) << init_status.AsDebugString();
  // Δ12: a hardware decoder failing to initialize must be visible, not silent.
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ(DecoderStreamEvent::kDecoderFallbackOnInit, events[0]);
  EXPECT_EQ(1u, decoder_stream->fallbacks());
  EXPECT_NE(std::string::npos,
            decoder_stream->GetDisplayName().find("FakeAudioDecoder"));
}

TEST_F(DecoderStreamTest, ReportsNoDecoderAvailableWhenEveryFactoryFails) {
  ScriptedDemuxerStream stream({MakeBuffer(0)});
  FakeAudioDecoder::Behaviour broken;
  broken.init_fails = true;
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories;
  factories.push_back(
      base::MakeRefCounted<FakeAudioDecoderFactory>("a", broken));
  factories.push_back(
      base::MakeRefCounted<FakeAudioDecoderFactory>("b", broken));

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  std::vector<DecoderStreamEvent> events;
  decoder_stream->set_event_cb(
      base::BindRepeating([](std::vector<DecoderStreamEvent>* sink,
                             DecoderStreamEvent e) { sink->push_back(e); },
                          &events));
  bool init_done = false;
  DecoderStatus init_status;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce(
          [](bool* flag, DecoderStatus* out, DecoderStatus s) {
            *flag = true;
            *out = s;
          },
          &init_done, &init_status));
  env_.RunUntilIdle();

  ASSERT_TRUE(init_done);
  EXPECT_FALSE(init_status.is_ok());
  EXPECT_EQ(DecoderStreamEvent::kNoDecoderAvailable, events.back());
}

TEST_F(DecoderStreamTest, InitializeWithNoFactoriesFailsCleanly) {
  ScriptedDemuxerStream stream({MakeBuffer(0)});
  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  bool init_done = false;
  DecoderStatus init_status;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), {},
      base::BindOnce(
          [](bool* flag, DecoderStatus* out, DecoderStatus s) {
            *flag = true;
            *out = s;
          },
          &init_done, &init_status));
  env_.RunUntilIdle();
  ASSERT_TRUE(init_done);
  EXPECT_FALSE(init_status.is_ok());
  EXPECT_NE(std::string::npos, init_status.description().find("declined"));
}

TEST_F(DecoderStreamTest, FlushDropsBuffersFromThePreviousSeekGeneration) {
  DemuxerStream::DecoderBufferVector buffers;
  // Two seek generations. The serial-0 buffers are still sitting in the
  // demuxer's queue when the user seeks, so DecoderStream receives them *after*
  // Flush(1) -- the case the serial exists for.
  buffers.push_back(MakeBuffer(/*serial=*/0));
  buffers.push_back(MakeBuffer(/*serial=*/0));
  buffers.push_back(MakeBuffer(/*serial=*/1));
  buffers.push_back(MakeBuffer(/*serial=*/1));
  buffers.push_back(DecoderBuffer::CreateEOSBuffer());
  ScriptedDemuxerStream stream(std::move(buffers));
  stream.set_max_per_read(2);

  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories;
  factories.push_back(base::MakeRefCounted<FakeAudioDecoderFactory>(
      "fake", FakeAudioDecoder::Behaviour()));

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  bool init_done = false;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                     &init_done));
  env_.RunUntilIdle();
  ASSERT_TRUE(init_done);

  // Seek before reading anything.
  bool flush_done = false;
  decoder_stream->Flush(
      /*serial=*/1,
      base::BindOnce([](bool* flag) { *flag = true; }, &flush_done));
  env_.RunUntilIdle();
  EXPECT_TRUE(flush_done) << "Flush() must run its closure";

  int seen = 0;
  for (int i = 0; i < 100; ++i) {
    bool done = false;
    base::scoped_refptr<AudioBuffer> output;
    decoder_stream->Read(base::BindOnce(
        [](bool* flag, base::scoped_refptr<AudioBuffer>* out, DecoderStatus,
           base::scoped_refptr<AudioBuffer> o) {
          *flag = true;
          *out = std::move(o);
        },
        &done, &output));
    env_.RunUntilIdle();
    ASSERT_TRUE(done);
    if (!output) {
      break;  // EOS
    }
    EXPECT_EQ(1, output->serial()) << "a pre-seek buffer survived the flush";
    ++seen;
  }
  // 2 surviving buffers * 2 outputs each.
  EXPECT_EQ(4, seen);
  EXPECT_EQ(2u, decoder_stream->stale_buffers_dropped())
      << "both serial-0 buffers should have been dropped";
}

TEST_F(DecoderStreamTest, FlushAbortsAReadThatIsStillOutstanding) {
  // Needs a decoder that defers its DecodeCB: with inline completion no read
  // can ever be outstanding when Flush() runs. The real contract allows
  // deferral, so this path must not hang the caller.
  DemuxerStream::DecoderBufferVector buffers;
  for (int i = 0; i < 4; ++i) {
    buffers.push_back(MakeBuffer(0));
  }
  ScriptedDemuxerStream stream(std::move(buffers));

  FakeAudioDecoder::Behaviour deferred;
  deferred.defer_decode = true;
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories;
  factories.push_back(
      base::MakeRefCounted<FakeAudioDecoderFactory>("fake", deferred));

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  bool init_done = false;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                     &init_done));
  env_.RunUntilIdle();
  ASSERT_TRUE(init_done);

  bool read_done = false;
  DecoderStatus read_status;
  decoder_stream->Read(base::BindOnce(
      [](bool* flag, DecoderStatus* out, DecoderStatus s,
         base::scoped_refptr<AudioBuffer>) {
        *flag = true;
        *out = s;
      },
      &read_done, &read_status));
  env_.RunUntilIdle();
  ASSERT_FALSE(read_done)
      << "the deferred decoder should not have answered yet";

  bool flush_done = false;
  decoder_stream->Flush(
      /*serial=*/1,
      base::BindOnce([](bool* flag) { *flag = true; }, &flush_done));
  env_.RunUntilIdle();
  EXPECT_TRUE(read_done) << "Flush() must complete an outstanding Read";
  EXPECT_FALSE(read_status.is_ok());
  EXPECT_EQ(DecoderStatus::Codes::kDecodingAborted, read_status.code());
  EXPECT_TRUE(flush_done);
  EXPECT_EQ(0u, decoder_stream->buffered_outputs());
}

TEST_F(DecoderStreamTest, DecodeErrorsAreCountedAndEventuallyStopTheStream) {
  DemuxerStream::DecoderBufferVector buffers;
  // Enough buffers to exceed kMaxConsecutiveDecodeErrors (20).
  for (int i = 0; i < 60; ++i) {
    buffers.push_back(MakeBuffer(0));
  }
  ScriptedDemuxerStream stream(std::move(buffers));

  FakeAudioDecoder::Behaviour broken;
  broken.decode_fails = true;
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories;
  factories.push_back(
      base::MakeRefCounted<FakeAudioDecoderFactory>("fake", broken));

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  std::vector<DecoderStreamEvent> events;
  decoder_stream->set_event_cb(
      base::BindRepeating([](std::vector<DecoderStreamEvent>* sink,
                             DecoderStreamEvent e) { sink->push_back(e); },
                          &events));
  bool init_done = false;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                     &init_done));
  env_.RunUntilIdle();
  ASSERT_TRUE(init_done);

  // A single corrupt packet must not kill the stream, but a decoder that never
  // makes progress must eventually stop instead of spinning forever.
  int reads = 0;
  for (int i = 0; i < 200; ++i) {
    bool done = false;
    DecoderStatus status;
    base::scoped_refptr<AudioBuffer> output;
    decoder_stream->Read(base::BindOnce(
        [](bool* flag, DecoderStatus* s, base::scoped_refptr<AudioBuffer>* o,
           DecoderStatus got, base::scoped_refptr<AudioBuffer> out) {
          *flag = true;
          *s = got;
          *o = std::move(out);
        },
        &done, &status, &output));
    env_.RunUntilIdle();
    ASSERT_TRUE(done) << "read " << i << " never completed";
    ++reads;
    if (!status.is_ok()) {
      break;
    }
  }
  EXPECT_GT(decoder_stream->decode_errors(), 0u);
  EXPECT_FALSE(events.empty())
      << "repeated decode failure must surface as an event";
  EXPECT_EQ(DecoderStreamEvent::kDecoderFallbackOnDecodeError, events.back());
  EXPECT_LT(reads, 200) << "the stream spun without ever failing";
}

TEST_F(DecoderStreamTest, WatermarkStopsPullingFromTheDemuxer) {
  DemuxerStream::DecoderBufferVector buffers;
  for (int i = 0; i < 40; ++i) {
    buffers.push_back(MakeBuffer(0));
  }
  ScriptedDemuxerStream stream(std::move(buffers));

  FakeAudioDecoder::Behaviour behaviour;
  behaviour.outputs_per_buffer = 2;
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories;
  factories.push_back(
      base::MakeRefCounted<FakeAudioDecoderFactory>("fake", behaviour));

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  bool init_done = false;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                     &init_done));
  env_.RunUntilIdle();
  ASSERT_TRUE(init_done);

  // One read: the stream should pull ahead to the watermark and no further.
  // Without back-pressure it would drain all 40 buffers (80 outputs) at once,
  // which is exactly the memory growth ffplay avoids with its frame queue.
  bool done = false;
  base::scoped_refptr<AudioBuffer> output;
  decoder_stream->Read(base::BindOnce(
      [](bool* flag, base::scoped_refptr<AudioBuffer>* out, DecoderStatus,
         base::scoped_refptr<AudioBuffer> o) {
        *flag = true;
        *out = std::move(o);
      },
      &done, &output));
  env_.RunUntilIdle();
  ASSERT_TRUE(done);
  EXPECT_TRUE(output);
  // The watermark is soft: it is checked before a decode, and one decode can
  // emit several outputs, so overshoot of up to one buffer is expected. What
  // must not happen is draining the stream -- 40 buffers would be 80 outputs.
  EXPECT_LE(decoder_stream->buffered_outputs(),
            AudioDecoderStream::kDecodeWatermark + 2u);
  EXPECT_LT(decoder_stream->outputs_decoded(), 40u)
      << "back-pressure is not working; the whole stream was decoded at once";
  EXPECT_LT(stream.read_count(), 40 / 8)
      << "the demuxer was read to exhaustion";
}

TEST_F(DecoderStreamTest, EveryBufferSurvivesAWatermarkTruncatedBatch) {
  // The demuxer hands over SEVERAL buffers per read (kBuffersPerRead, 8 for a
  // real one); the watermark stops the pump part-way through a batch. What
  // happens to the rest of that batch is the whole test: it used to be cleared,
  // which silently dropped media that the demuxer had already handed over and
  // would never hand over again.
  //
  // Found from the other end, on the VOD-shaped count case: 89 of 93 batches
  // had seven buffers left, one frame decoded per eight fetched, and a 2x
  // playback test read that as "2x drops frames" (35 presented of 150 asked
  // for).
  //
  // 40 buffers in, 80 outputs out, and nothing in between is allowed to vanish.
  DemuxerStream::DecoderBufferVector buffers;
  for (int i = 0; i < 40; ++i) {
    buffers.push_back(MakeBuffer(/*serial=*/0));
  }
  buffers.push_back(DecoderBuffer::CreateEOSBuffer());
  ScriptedDemuxerStream stream(std::move(buffers));

  FakeAudioDecoder::Behaviour behaviour;
  behaviour.outputs_per_buffer = 2;
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories;
  factories.push_back(
      base::MakeRefCounted<FakeAudioDecoderFactory>("fake", behaviour));

  auto decoder_stream = std::make_unique<AudioDecoderStream>(runner_);
  bool init_done = false;
  decoder_stream->Initialize(
      &stream, AudioDecoderConfig(), std::move(factories),
      base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                     &init_done));
  env_.RunUntilIdle();
  ASSERT_TRUE(init_done);

  ReadUntilEos(decoder_stream.get());
  EXPECT_EQ(80, outputs_);
  EXPECT_EQ(80u, decoder_stream->outputs_decoded())
      << "media reached the demuxer and never reached the decoder: a batch the "
         "watermark cut short lost its tail";
  EXPECT_EQ(0u, decoder_stream->decode_errors());
  // The stream is read in batches and each batch is now consumed before the
  // next is fetched, so the read count follows the batches rather than running
  // away from the consumer.
  EXPECT_LE(stream.read_count(), 8);
}

}  // namespace
}  // namespace avbase::media
