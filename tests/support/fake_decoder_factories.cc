// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/fake_decoder_factories.h"

#include <utility>

#include "base/functional/bind.h"
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

// Stand-in for a decoder-owned GPU surface. The frame only ever carries the
// pointer; nothing here is readable or writable, which is the whole point --
// it is what makes IsMappable() false and forces the code under test down the
// no-pixels-available path. Static so the address stays valid for the lifetime
// of any frame wrapping it.
int fake_native_payload = 0;

// The geometry every scripted frame carries until |geometry_change_after|
// says otherwise. Named so the change has something to change FROM: a test
// asserting "the stage was built twice" is only meaningful if the two builds
// differ.
constexpr Size kFakeCodedSize{320, 240};

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
    const base::TimeDelta timestamp = base::Milliseconds(i);
    const base::TimeDelta duration = base::Microseconds(33333);
    // Geometry is chosen per frame from the count so far, which is what a
    // resolution change looks like from downstream: the same decoder, a
    // different coded_size, and no warning.
    const bool moved = behaviour_.geometry_change_after >= 0 &&
                       frames_out_ >= behaviour_.geometry_change_after;
    const Size coded = moved ? behaviour_.second_coded_size : kFakeCodedSize;
    ++frames_out_;
    if (behaviour_.hardware_frames) {
      NativeHandle handle;
      handle.kind = NativeHandleKind::kVaapiSurface;
      handle.id = &fake_native_payload;
      // A payload address is all the identity this needs: nothing dereferences
      // it, and the release callback is empty because the bytes it stands for
      // belong to nobody in a test.
      output_cb_.Run(VideoFrame::WrapNativeBuffer(
          handle, VideoFormat::kNV12, coded, coded, Rational{1, 1}, timestamp,
          duration, serial, base::BindOnce([] {}), base::BindRepeating([] {
            return base::scoped_refptr<VideoFrame>();
          })));
      continue;
    }
    output_cb_.Run(VideoFrame::CreateBlackFrame(VideoFormat::kI420, coded,
                                                coded, Rational{1, 1},
                                                timestamp, duration, serial));
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
