// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_TEXT_DECODER_H_
#define AVBASE_MEDIA_BASE_TEXT_DECODER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_config.h"
#include "media/base/media_error.h"
#include "media/base/timed_text.h"
#include "media/media_export.h"

namespace avbase::media {

// The text-track half of the decoder family. Deliberately SYNCHRONOUS, unlike
// Audio/VideoDecoder: subtitle packets are tiny, decoders are stateless
// push-through, and the text leg has no pacing problem to solve -- an async
// contract here would be ceremony without a consumer (the leg lives on S1 and
// returns cues directly to the client).
class AVBASE_MEDIA_EXPORT TextDecoder {
 public:
  TextDecoder(const TextDecoder&) = delete;
  TextDecoder& operator=(const TextDecoder&) = delete;
  virtual ~TextDecoder();

  virtual const char* name() const = 0;
  virtual Status Initialize(const TextDecoderConfig& config) = 0;
  // Decodes one demuxer buffer (one subtitle packet) into zero or more cues.
  // An empty cue list is a normal result (blank display period, formatting
  // packet), not an error.
  virtual Status Decode(const DecoderBuffer& buffer,
                        std::vector<TimedTextCue>* cues) = 0;

 protected:
  TextDecoder() = default;
};

// Mirrors VideoDecoderFactory; factories exist because RendererImpl (inside
// avbase_media) must not construct the FFmpeg implementation itself -- the
// factory object is built by whoever links the FFmpeg layer.
class AVBASE_MEDIA_EXPORT TextDecoderFactory
    : public base::RefCountedThreadSafe<TextDecoderFactory> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  TextDecoderFactory(const TextDecoderFactory&) = delete;
  TextDecoderFactory& operator=(const TextDecoderFactory&) = delete;

  virtual std::unique_ptr<TextDecoder> CreateTextDecoder(
      const TextDecoderConfig& config) = 0;
  virtual const char* name() const = 0;

 protected:
  friend class base::RefCountedThreadSafe<TextDecoderFactory>;
  TextDecoderFactory() = default;
  virtual ~TextDecoderFactory() = default;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_TEXT_DECODER_H_
