// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Decoders that do not decode. The renderer suites are about orchestration --
// who is initialized first, where a callback lands, when EOS propagates -- so
// the decoders have to be scriptable rather than correct. Each turns one input
// buffer into |outputs_per_buffer| outputs and can be told to fail
// initialization, fail every decode, or hold its DecodeCB so that a read can
// still be outstanding when Flush arrives.
//
// The audio fake's shape follows the local FakeAudioDecoder in
// tests/unit/media_filters/decoder_stream_unittest.cc deliberately: that suite
// keeps its own copy (its behaviour knobs drive decoder selection, not the
// renderer), and the two are meant to stay comparable.

#ifndef AVBASE_TESTS_SUPPORT_FAKE_DECODER_FACTORIES_H_
#define AVBASE_TESTS_SUPPORT_FAKE_DECODER_FACTORIES_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/audio_decoder.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_config.h"
#include "media/base/decoder_status.h"
#include "media/base/media_types.h"
#include "media/base/video_decoder.h"
#include "media/base/video_decoder_factory.h"
#include "media/base/video_frame.h"

namespace avbase::media::test {

// Call counts live in the factory, which the test holds; the decoders it
// creates get a pointer to them. Without this a test could not see whether the
// renderer re-initialized a decoder or dropped one on the floor.
struct FakeDecoderCounters {
  // Atomic because the decoders increment them from S3/S4 while the test reads
  // them from its own thread (a TSan run of the renderer suite reported the
  // plain ints).
  std::atomic<int> init_calls{0};
  std::atomic<int> decode_calls{0};
  std::atomic<int> reset_calls{0};
};

struct FakeDecoderBehaviour {
  bool init_fails{false};
  bool decode_fails{false};
  // Holds the DecodeCB instead of running it, so a Flush can arrive while a
  // decode is outstanding.
  bool defer_decode{false};
  // AudioBuffers (audio) or VideoFrames (video) emitted per input buffer.
  int outputs_per_buffer{2};
};

class FakeAudioDecoder final : public AudioDecoder {
 public:
  FakeAudioDecoder(const FakeDecoderBehaviour& behaviour,
                   FakeDecoderCounters* counters);

  // AudioDecoder.
  std::string GetDisplayName() const override { return "FakeAudioDecoder"; }
  void Initialize(const AudioDecoderConfig& config, bool has_pending_clear,
                  int32_t current_serial, InitCB init_cb,
                  const OutputCB& output_cb,
                  const WaitingCB& waiting_cb) override;
  void Decode(base::scoped_refptr<DecoderBuffer> buffer,
              DecodeCB decode_cb) override;
  void Reset(base::OnceClosure closure) override;

  size_t deferred_count() const { return deferred_.size(); }

 private:
  const FakeDecoderBehaviour behaviour_;
  FakeDecoderCounters* const counters_;
  OutputCB output_cb_;
  std::vector<DecodeCB> deferred_;
};

class FakeAudioDecoderFactory final : public AudioDecoderFactory {
 public:
  explicit FakeAudioDecoderFactory(
      FakeDecoderBehaviour behaviour = {},
      std::string name = "FakeAudioDecoderFactory");

  // Declines every CreateAudioDecoder() call, for the fallback-chain tests.
  void set_declines(bool declines) { declines_ = declines; }
  const FakeDecoderCounters& counters() const { return counters_; }
  int create_calls() const { return create_calls_; }

  // AudioDecoderFactory.
  std::unique_ptr<AudioDecoder>
  CreateAudioDecoder(const AudioDecoderConfig& config) override;
  const char* name() const override { return name_.c_str(); }

 private:
  const FakeDecoderBehaviour behaviour_;
  const std::string name_;
  bool declines_ = false;
  int create_calls_ = 0;
  FakeDecoderCounters counters_;
};

class FakeVideoDecoder final : public VideoDecoder {
 public:
  FakeVideoDecoder(const FakeDecoderBehaviour& behaviour,
                   FakeDecoderCounters* counters);

  // VideoDecoder.
  const char* name() const override { return "FakeVideoDecoder"; }
  VideoDecoderType GetDecoderType() const override {
    return VideoDecoderType::kMock;
  }
  void Initialize(const VideoDecoderConfig& config, bool low_delay,
                  CdmContext* cdm_context, InitCB init_cb,
                  const OutputCB& output_cb,
                  const WaitingCB& waiting_cb) override;
  void Decode(base::scoped_refptr<DecoderBuffer> buffer,
              DecodeCB decode_cb) override;
  void Reset(base::OnceClosure closure) override;

  size_t deferred_count() const { return deferred_.size(); }
  // Runs the DecodeCBs this decoder is holding, oldest first. A video decoder
  // signals EOS by running the callback for the EOS buffer it was fed
  // (decoder_stream.h: "VideoFrame has no EOS marker").
  void RunDeferredDecodes();

 private:
  const FakeDecoderBehaviour behaviour_;
  FakeDecoderCounters* const counters_;
  OutputCB output_cb_;
  std::vector<DecodeCB> deferred_;
};

class FakeVideoDecoderFactory final : public VideoDecoderFactory {
 public:
  explicit FakeVideoDecoderFactory(
      FakeDecoderBehaviour behaviour = {},
      std::string name = "FakeVideoDecoderFactory");

  void set_declines(bool declines) { declines_ = declines; }
  // Declares this factory as a hardware path, so DecoderSelector's ranking has
  // something to sort. Without it every fake looks like software and a test
  // cannot tell "preferred hardware" from "fell through to software".
  void set_hardware(bool hardware) { hardware_ = hardware; }
  void set_priority(int priority) { priority_ = priority; }
  const FakeDecoderCounters& counters() const { return counters_; }
  int create_calls() const { return create_calls_; }

  // VideoDecoderFactory.
  VideoDecoderCapability GetCapability() const override;
  bool SupportsCodec(VideoDecoderType type_hint) const override;
  std::unique_ptr<VideoDecoder>
  CreateVideoDecoder(const VideoDecoderConfig& config) override;
  const char* name() const override { return name_.c_str(); }

 private:
  const FakeDecoderBehaviour behaviour_;
  const std::string name_;
  bool declines_ = false;
  bool hardware_ = false;
  int priority_ = 1;
  int create_calls_ = 0;
  FakeDecoderCounters counters_;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_FAKE_DECODER_FACTORIES_H_
