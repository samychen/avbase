// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The other half of the synthetic source: decoders that do not decode, they
// decode *the encoding*. The packet's index (tests/support/synthetic_demuxer.h)
// is painted into the frame's own pixels, and the packet's tone is generated
// from its timestamp, so a test can ask "which frame is on screen" and "is the
// audio where it should be" of the frame and the samples themselves.
//
// That is the difference this buys over a counter: a frame reaching the display
// through the pipeline carries its origin with it, so an assertion about a seek
// landing stays true even if the pipeline mis-stamps a serial, reorders a queue
// or repeats a frame.
//
// The index lives in the Y plane's top-left corner as a 4x8 matrix of 8x8
// blocks, most significant bit first: block (r, c) covers rows [8r, 8r+8) and
// columns [8c, 8c+8), and is fully white for a 1 bit and fully black for a 0.
// 32 bits is 4 billion frames; the matrix has room for 64, and using the lower
// 32 keeps both the painter and the reader below a screen of code.

#ifndef AVBASE_TESTS_SUPPORT_SYNTHETIC_DECODERS_H_
#define AVBASE_TESTS_SUPPORT_SYNTHETIC_DECODERS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_decoder.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/decoder_buffer.h"
#include "media/base/video_decoder.h"
#include "media/base/video_decoder_factory.h"
#include "media/base/video_frame.h"
#include "tests/support/synthetic_demuxer.h"

namespace avbase::media::test {

constexpr int kFrameIndexBits = 32;
constexpr int kFrameIndexBlockSize = 8;

// Both are no-ops on a frame that is not mappable (a hardware-backed frame);
// they return false rather than guessing, because a test that silently read
// zeros would look like "frame 0 is on screen".
void PaintFrameIndex(VideoFrame* frame, uint32_t index);
bool ReadFrameIndex(const VideoFrame& frame, uint32_t* index);

class SyntheticVideoDecoder final : public VideoDecoder {
 public:
  explicit SyntheticVideoDecoder(const SyntheticSpec& spec);

  const char* name() const override { return "SyntheticVideoDecoder"; }
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

  int frames_output() const { return frames_output_; }

 private:
  const Size coded_size_;
  const base::TimeDelta frame_duration_;
  OutputCB output_cb_;
  int frames_output_ = 0;
};

class SyntheticVideoDecoderFactory final : public VideoDecoderFactory {
 public:
  explicit SyntheticVideoDecoderFactory(const SyntheticSpec& spec)
      : spec_(spec) {}

  VideoDecoderCapability GetCapability() const override;
  bool SupportsCodec(VideoDecoderType type_hint) const override;
  std::unique_ptr<VideoDecoder> CreateVideoDecoder(
      const VideoDecoderConfig& config) override;
  const char* name() const override { return "SyntheticVideoDecoderFactory"; }

 private:
  const SyntheticSpec spec_;
};

class SyntheticAudioDecoder final : public AudioDecoder {
 public:
  explicit SyntheticAudioDecoder(const SyntheticSpec& spec);

  std::string GetDisplayName() const override {
    return "SyntheticAudioDecoder";
  }
  void Initialize(const AudioDecoderConfig& config, bool has_pending_clear,
                  int32_t current_serial, InitCB init_cb,
                  const OutputCB& output_cb, const WaitingCB& waiting_cb)
      override;
  void Decode(base::scoped_refptr<DecoderBuffer> buffer,
              DecodeCB decode_cb) override;
  void Reset(base::OnceClosure closure) override;

  int buffers_output() const { return buffers_output_; }

 private:
  const SyntheticSpec spec_;
  OutputCB output_cb_;
  int buffers_output_ = 0;
};

class SyntheticAudioDecoderFactory final : public AudioDecoderFactory {
 public:
  explicit SyntheticAudioDecoderFactory(const SyntheticSpec& spec)
      : spec_(spec) {}

  std::unique_ptr<AudioDecoder> CreateAudioDecoder(
      const AudioDecoderConfig& config) override;
  const char* name() const override { return "SyntheticAudioDecoderFactory"; }

 private:
  const SyntheticSpec spec_;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_SYNTHETIC_DECODERS_H_
