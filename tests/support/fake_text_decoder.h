// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A TextDecoder whose cues the test chooses, including their timestamps.
//
// WHY A FAKE AND NOT A SUBRIP STRING. The live-cue policy (RendererImpl's
// SetSourceLiveness) is a question about TIMESTAMPS -- "is this cue further
// behind the media clock than the window allows" -- and the answer must not
// depend on a subtitle parser's idea of what a timestamp means. A fake that
// emits exactly the cues it was handed makes the policy's input explicit, which
// is the same reason SyntheticDemuxer encodes the frame index in the payload
// rather than leaving it to a decoder.
//
// The buffer's timestamp becomes the cue's pts, and |extra_cues| are appended
// after it, so a test can produce several cues from one packet exactly as a
// multi-line subtitle does.

#ifndef AVBASE_TESTS_SUPPORT_FAKE_TEXT_DECODER_H_
#define AVBASE_TESTS_SUPPORT_FAKE_TEXT_DECODER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/text_decoder.h"
#include "media/base/timed_text.h"

namespace avbase::media::test {

class FakeTextDecoder final : public TextDecoder {
 public:
  // Cues appended after the buffer's own cue, in order.
  void set_extra_cues(std::vector<TimedTextCue> cues) {
    extra_cues_ = std::move(cues);
  }
  // The text every buffer's own cue carries; empty text keeps the test's
  // assertions about WHICH cue arrived rather than about its wording.
  void set_text(std::string text) { text_ = std::move(text); }
  // When set, Decode() fails with this error instead of producing cues, so a
  // test can tell "the policy dropped it" from "the decoder never made one".
  void set_fail(bool fail) { fail_ = fail; }
  void set_config(const TextDecoderConfig& config) { config_ = config; }

  int decode_calls() const { return decode_calls_; }

  // TextDecoder.
  const char* name() const override { return "FakeTextDecoder"; }
  Status Initialize(const TextDecoderConfig& config) override;
  Status Decode(const DecoderBuffer& buffer,
                std::vector<TimedTextCue>* cues) override;

 private:
  std::vector<TimedTextCue> extra_cues_;
  std::string text_{"cue"};
  TextDecoderConfig config_;
  bool fail_ = false;
  int decode_calls_ = 0;
};

class FakeTextDecoderFactory final : public TextDecoderFactory {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  FakeTextDecoderFactory() = default;

  // The decoder this factory hands out, so a test can script it. Valid after
  // the renderer has created it; a test that needs to script before that
  // scripts the fields it cares about through the setters below.
  void set_text(std::string text) { pending_text_ = std::move(text); }
  void set_extra_cues(std::vector<TimedTextCue> cues) {
    pending_extra_ = std::move(cues);
  }
  void set_fail(bool fail) { pending_fail_ = fail; }

  int create_calls() const { return create_calls_; }
  int decode_calls() const;

  // TextDecoderFactory.
  std::unique_ptr<TextDecoder> CreateTextDecoder(
      const TextDecoderConfig& config) override;
  const char* name() const override { return "FakeTextDecoderFactory"; }

 private:
  std::string pending_text_{"cue"};
  std::vector<TimedTextCue> pending_extra_;
  bool pending_fail_ = false;
  int create_calls_ = 0;
  int decode_calls_ = 0;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_FAKE_TEXT_DECODER_H_
