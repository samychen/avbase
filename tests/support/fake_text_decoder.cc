// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/fake_text_decoder.h"

#include <utility>

#include "base/functional/bind.h"

namespace avbase::media::test {

Status FakeTextDecoder::Initialize(const TextDecoderConfig& config) {
  if (!config.IsValidConfig()) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kInvalidArgument, "no text codec named",
        "FakeTextDecoder was initialized with an empty codec_name",
        "check that the container's subtitle track was probed"));
  }
  config_ = config;
  return OkStatus();
}

Status FakeTextDecoder::Decode(const DecoderBuffer& buffer,
                               std::vector<TimedTextCue>* cues) {
  ++decode_calls_;
  if (fail_) {
    return base::unexpected(
        MediaError::Of(ErrorCode::kDecodeFailed, "scripted decode failure",
                       "FakeTextDecoder::set_fail(true) was set",
                       "clear it to let the fake produce cues"));
  }
  // The buffer's own timestamp becomes the cue's pts. That is the whole point
  // of the fake: the live-cue policy reads cue.pts, so the test states the
  // timestamp it wants to be judged on rather than arranging for a parser to
  // produce it.
  TimedTextCue own;
  own.pts = buffer.timestamp();
  own.duration = base::TimeDelta();
  own.text = text_;
  cues->push_back(std::move(own));
  for (TimedTextCue& extra : extra_cues_) {
    cues->push_back(extra);
  }
  return OkStatus();
}

std::unique_ptr<TextDecoder> FakeTextDecoderFactory::CreateTextDecoder(
    const TextDecoderConfig& config) {
  ++create_calls_;
  auto decoder = std::make_unique<FakeTextDecoder>();
  decoder->set_text(pending_text_);
  decoder->set_extra_cues(std::move(pending_extra_));
  decoder->set_fail(pending_fail_);
  decoder->set_config(config);
  return decoder;
}

int FakeTextDecoderFactory::decode_calls() const {
  return decode_calls_;
}

}  // namespace avbase::media::test
