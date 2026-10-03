// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/fake_decoder_factories.h"

#include <utility>

#include "media/base/audio_buffer.h"
#include "media/base/audio_parameters.h"
#include "media/base/waiting.h"

namespace avbase::media::test {
namespace {

// 1024 frames of stereo f32, silent. Sizes match the local fake in
// decoder_stream_unittest.cc so the two suites feed the renderers the same
// thing.
constexpr int kFramesPerBuffer = 1024;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

}  // namespace

FakeAudioDecoder::FakeAudioDecoder(const FakeDecoderBehaviour& behaviour,
                                   FakeDecoderCounters* counters)
    : behaviour_(behaviour), counters_(counters) {}

void FakeAudioDecoder::Initialize(const AudioDecoderConfig& /*config*/,
                                  bool /*has_pending_clear*/,
                                  int32_t /*current_serial*/, InitCB init_cb,
                                  const OutputCB& output_cb,
                                  const WaitingCB& /*waiting_cb*/) {
  ++counters_->init_calls;
  output_cb_ = output_cb;
  if (behaviour_.init_fails) {
    std::move(init_cb).Run(DecoderStatus(
        DecoderStatus::Codes::kUnsupportedCodec, "fake init failure"));
    return;
  }
  std::move(init_cb).Run(DecoderStatus());
}

void FakeAudioDecoder::Decode(base::scoped_refptr<DecoderBuffer> buffer,
                              DecodeCB decode_cb) {
  ++counters_->decode_calls;
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
    // The audio contract signals EOS with a distinct AudioBuffer; a DecodeCB
    // alone would leave the stream looking merely idle.
    output_cb_.Run(AudioBuffer::CreateEOSBuffer());
    std::move(decode_cb).Run(DecoderStatus());
    return;
  }
  const int32_t serial = buffer->serial();
  for (int i = 0; i < behaviour_.outputs_per_buffer; ++i) {
    output_cb_.Run(AudioBuffer::Create(
        SampleFormat::kF32P, ChannelLayout::kStereo, kChannels, kSampleRate,
        kFramesPerBuffer, base::Milliseconds(i), base::Microseconds(21333),
        serial, std::vector<uint8_t>(kFramesPerBuffer * 4 * kChannels, 0)));
  }
  std::move(decode_cb).Run(DecoderStatus());
}

void FakeAudioDecoder::Reset(base::OnceClosure closure) {
  ++counters_->reset_calls;
  // Chromium's contract: Reset aborts pending Decode() calls, running their
  // callbacks with kDecodingAborted, before |closure| runs.
  for (DecodeCB& cb : deferred_) {
    std::move(cb).Run(
        DecoderStatus(DecoderStatus::Codes::kDecodingAborted, "reset"));
  }
  deferred_.clear();
  std::move(closure).Run();
}

FakeAudioDecoderFactory::FakeAudioDecoderFactory(FakeDecoderBehaviour behaviour,
                                                 std::string name)
    : behaviour_(behaviour), name_(std::move(name)) {}

std::unique_ptr<AudioDecoder> FakeAudioDecoderFactory::CreateAudioDecoder(
    const AudioDecoderConfig& /*config*/) {
  ++create_calls_;
  if (declines_) {
    return nullptr;
  }
  return std::make_unique<FakeAudioDecoder>(behaviour_, &counters_);
}

FakeVideoDecoder::FakeVideoDecoder(const FakeDecoderBehaviour& behaviour,
                                   FakeDecoderCounters* counters)
    : behaviour_(behaviour), counters_(counters) {}

void FakeVideoDecoder::Initialize(const VideoDecoderConfig& /*config*/,
                                  bool /*low_delay*/, CdmContext* /*cdm*/,
                                  InitCB init_cb, const OutputCB& output_cb,
                                  const WaitingCB& /*waiting_cb*/) {
  ++counters_->init_calls;
  output_cb_ = output_cb;
  if (behaviour_.init_fails) {
    std::move(init_cb).Run(DecoderStatus(
        DecoderStatus::Codes::kUnsupportedCodec, "fake init failure"));
    return;
  }
  std::move(init_cb).Run(DecoderStatus());
}

void FakeVideoDecoder::Decode(base::scoped_refptr<DecoderBuffer> buffer,
                              DecodeCB decode_cb) {
  ++counters_->decode_calls;
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
    // No frame to emit: for video the EOS marker *is* the flushed DecodeCB.
    std::move(decode_cb).Run(DecoderStatus());
    return;
  }
  const int32_t serial = buffer->serial();
  for (int i = 0; i < behaviour_.outputs_per_buffer; ++i) {
    output_cb_.Run(VideoFrame::CreateBlackFrame(
        VideoFormat::kI420, Size{320, 240}, Size{320, 240}, Rational{1, 1},
        base::Milliseconds(i), base::Microseconds(33333), serial));
  }
  std::move(decode_cb).Run(DecoderStatus());
}

void FakeVideoDecoder::Reset(base::OnceClosure closure) {
  ++counters_->reset_calls;
  for (DecodeCB& cb : deferred_) {
    std::move(cb).Run(
        DecoderStatus(DecoderStatus::Codes::kDecodingAborted, "reset"));
  }
  deferred_.clear();
  std::move(closure).Run();
}

void FakeVideoDecoder::RunDeferredDecodes() {
  std::vector<DecodeCB> pending;
  pending.swap(deferred_);
  for (DecodeCB& cb : pending) {
    std::move(cb).Run(DecoderStatus());
  }
}

FakeVideoDecoderFactory::FakeVideoDecoderFactory(FakeDecoderBehaviour behaviour,
                                                 std::string name)
    : behaviour_(behaviour), name_(std::move(name)) {}

VideoDecoderCapability FakeVideoDecoderFactory::GetCapability() const {
  VideoDecoderCapability capability;
  capability.priority = priority_;
  capability.hardware = hardware_;
  return capability;
}

bool FakeVideoDecoderFactory::SupportsCodec(
    VideoDecoderType /*type_hint*/) const {
  return !declines_;
}

std::unique_ptr<VideoDecoder> FakeVideoDecoderFactory::CreateVideoDecoder(
    const VideoDecoderConfig& /*config*/) {
  ++create_calls_;
  if (declines_) {
    return nullptr;
  }
  auto decoder = std::make_unique<FakeVideoDecoder>(behaviour_, &counters_);
  last_decoder_ = decoder.get();
  return decoder;
}

}  // namespace avbase::media::test
