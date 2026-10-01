// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/synthetic_decoders.h"

#include <cmath>
#include <utility>

#include "base/check.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"

namespace avbase::media::test {
namespace {

constexpr double kPi = 3.14159265358979323846;
// Block centres, in pixels, for the 4x8 matrix the header describes.
constexpr int kBlockCenterOffset = kFrameIndexBlockSize / 2;

// The luma value of one block: fully white for a 1 bit, fully black for a 0.
bool BlockIsSet(const VideoFrame& frame, int row, int col) {
  const int y = row * kFrameIndexBlockSize + kBlockCenterOffset;
  const int x = col * kFrameIndexBlockSize + kBlockCenterOffset;
  const std::span<const uint8_t> plane =
      frame.visible_data(VideoFrame::kYPlane);
  const int32_t stride = frame.stride(VideoFrame::kYPlane);
  const uint8_t value =
      plane[static_cast<size_t>(y) * static_cast<size_t>(stride) +
            static_cast<size_t>(x)];
  return value > 128;
}

}  // namespace

void PaintFrameIndex(VideoFrame* frame, uint32_t index) {
  DCHECK(frame);
  if (!frame->IsMappable()) {
    return;
  }
  std::span<uint8_t> plane = frame->mutable_data(VideoFrame::kYPlane);
  const int32_t stride = frame->stride(VideoFrame::kYPlane);
  for (int bit = 0; bit < kFrameIndexBits; ++bit) {
    const int row = bit / 8;
    const int col = bit % 8;
    // Most significant bit first, so the painted matrix reads left-to-right,
    // top-to-bottom the way the number is written.
    const bool set = ((index >> (kFrameIndexBits - 1 - bit)) & 1u) != 0;
    const uint8_t value = set ? 255 : 0;
    for (int y = row * kFrameIndexBlockSize;
         y < (row + 1) * kFrameIndexBlockSize; ++y) {
      for (int x = col * kFrameIndexBlockSize;
           x < (col + 1) * kFrameIndexBlockSize; ++x) {
        plane[static_cast<size_t>(y) * static_cast<size_t>(stride) +
              static_cast<size_t>(x)] = value;
      }
    }
  }
}

bool ReadFrameIndex(const VideoFrame& frame, uint32_t* index) {
  if (!index || !frame.IsMappable()) {
    return false;
  }
  uint32_t value = 0;
  for (int bit = 0; bit < kFrameIndexBits; ++bit) {
    if (BlockIsSet(frame, bit / 8, bit % 8)) {
      value |= 1u << (kFrameIndexBits - 1 - bit);
    }
  }
  *index = value;
  return true;
}

SyntheticVideoDecoder::SyntheticVideoDecoder(const SyntheticSpec& spec)
    : coded_size_(Size{spec.width, spec.height}),
      frame_duration_(spec.frame_duration()) {}

void SyntheticVideoDecoder::Initialize(const VideoDecoderConfig& /*config*/,
                                       bool /*low_delay*/, CdmContext* /*cdm*/,
                                       InitCB init_cb,
                                       const OutputCB& output_cb,
                                       const WaitingCB& /*waiting_cb*/) {
  output_cb_ = output_cb;
  std::move(init_cb).Run(DecoderStatus());
}

void SyntheticVideoDecoder::Decode(base::scoped_refptr<DecoderBuffer> buffer,
                                   DecodeCB decode_cb) {
  if (!buffer || buffer->IsEndOfStream()) {
    // Video signals end of stream by running the callback for the EOS buffer it
    // was fed; there is no frame to emit (decoder_stream.h).
    std::move(decode_cb).Run(DecoderStatus());
    return;
  }
  uint32_t index = 0;
  if (!ReadIndexPayload(*buffer, &index)) {
    std::move(decode_cb).Run(DecoderStatus(
        DecoderStatus::Codes::kDecodeError,
        "synthetic packet without an index"));
    return;
  }
  // The frame's timestamp comes from the packet, not from a frame counter: that
  // is what makes a seek landing checkable against the same number the
  // container-side test asserts on.
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, coded_size_, coded_size_, Rational{1, 1},
      buffer->timestamp(), frame_duration_, buffer->serial());
  PaintFrameIndex(frame.get(), index);
  ++frames_output_;
  output_cb_.Run(std::move(frame));
  std::move(decode_cb).Run(DecoderStatus());
}

void SyntheticVideoDecoder::Reset(base::OnceClosure closure) {
  std::move(closure).Run();
}

VideoDecoderCapability SyntheticVideoDecoderFactory::GetCapability() const {
  VideoDecoderCapability capability;
  capability.priority = 1;
  capability.max_width = spec_.width;
  capability.max_height = spec_.height;
  return capability;
}

bool SyntheticVideoDecoderFactory::SupportsCodec(
    VideoDecoderType /*type_hint*/) const {
  return true;
}

std::unique_ptr<VideoDecoder> SyntheticVideoDecoderFactory::CreateVideoDecoder(
    const VideoDecoderConfig& /*config*/) {
  return std::make_unique<SyntheticVideoDecoder>(spec_);
}

SyntheticAudioDecoder::SyntheticAudioDecoder(const SyntheticSpec& spec)
    : spec_(spec) {}

void SyntheticAudioDecoder::Initialize(const AudioDecoderConfig& /*config*/,
                                       bool /*has_pending_clear*/,
                                       int32_t /*current_serial*/,
                                       InitCB init_cb,
                                       const OutputCB& output_cb,
                                       const WaitingCB& /*waiting_cb*/) {
  output_cb_ = output_cb;
  std::move(init_cb).Run(DecoderStatus());
}

void SyntheticAudioDecoder::Decode(base::scoped_refptr<DecoderBuffer> buffer,
                                   DecodeCB decode_cb) {
  if (!buffer || buffer->IsEndOfStream()) {
    // Audio signals end of stream with a distinct AudioBuffer.
    output_cb_.Run(AudioBuffer::CreateEOSBuffer());
    std::move(decode_cb).Run(DecoderStatus());
    return;
  }
  uint32_t index = 0;
  if (!ReadIndexPayload(*buffer, &index)) {
    std::move(decode_cb).Run(DecoderStatus(
        DecoderStatus::Codes::kDecodeError,
        "synthetic packet without an index"));
    return;
  }
  const int frames = spec_.audio_frames_per_packet;
  const double tone_hz =
      ExpectedToneHzAt(buffer->timestamp(), spec_.base_tone_hz);
  std::vector<uint8_t> data(static_cast<size_t>(frames) *
                            static_cast<size_t>(spec_.channels) *
                            sizeof(float));
  auto* samples = reinterpret_cast<float*>(data.data());
  // Planar f32: channel planes one after another, which is what kF32P means.
  for (int channel = 0; channel < spec_.channels; ++channel) {
    for (int i = 0; i < frames; ++i) {
      const double phase = 2.0 * kPi * tone_hz *
                           static_cast<double>(i) /
                           static_cast<double>(spec_.sample_rate);
      samples[static_cast<size_t>(channel) * static_cast<size_t>(frames) +
              static_cast<size_t>(i)] =
          static_cast<float>(std::sin(phase) * 0.5);
    }
  }
  const base::TimeDelta timestamp = buffer->timestamp();
  const base::TimeDelta duration = base::Microseconds(
      static_cast<int64_t>(frames) * 1000000 / spec_.sample_rate);
  ++buffers_output_;
  output_cb_.Run(AudioBuffer::Create(
      SampleFormat::kF32P,
      spec_.channels == 1 ? ChannelLayout::kMono : ChannelLayout::kStereo,
      spec_.channels, spec_.sample_rate, frames, timestamp, duration,
      buffer->serial(), std::move(data)));
  std::move(decode_cb).Run(DecoderStatus());
}

void SyntheticAudioDecoder::Reset(base::OnceClosure closure) {
  std::move(closure).Run();
}

std::unique_ptr<AudioDecoder> SyntheticAudioDecoderFactory::CreateAudioDecoder(
    const AudioDecoderConfig& /*config*/) {
  return std::make_unique<SyntheticAudioDecoder>(spec_);
}

}  // namespace avbase::media::test
