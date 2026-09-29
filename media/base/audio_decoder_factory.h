// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_MEDIA_BASE_AUDIO_DECODER_FACTORY_H_
#define IJKPP_MEDIA_BASE_AUDIO_DECODER_FACTORY_H_

#include <memory>

#include "base/memory/ref_counted.h"
#include "media/base/media_types.h"
#include "media/media_export.h"

namespace ijkpp::media {

class AudioDecoder;
struct AudioDecoderConfig;

class IJKPP_MEDIA_EXPORT AudioDecoderFactory
    : public base::RefCountedThreadSafe<AudioDecoderFactory> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  AudioDecoderFactory(const AudioDecoderFactory&) = delete;
  AudioDecoderFactory& operator=(const AudioDecoderFactory&) = delete;

  virtual std::unique_ptr<AudioDecoder> CreateAudioDecoder(
      const AudioDecoderConfig& config) = 0;
  virtual const char* name() const = 0;

 protected:
  friend class base::RefCountedThreadSafe<AudioDecoderFactory>;
  AudioDecoderFactory() = default;
  virtual ~AudioDecoderFactory() = default;
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_AUDIO_DECODER_FACTORY_H_
