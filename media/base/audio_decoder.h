// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Shape mirrors `media/base/video_decoder.h` deliberately: a single generic
// DecoderStream<Traits> owns either decoder type, so the two contracts must be
// substitutable. The one intentional divergence from Chromium's
// media/base/audio_decoder.h is that Decode() takes a DecodeCB rather than an
// OutputCB -- see the comment on Decode() below.

#ifndef AVBASE_MEDIA_BASE_AUDIO_DECODER_H_
#define AVBASE_MEDIA_BASE_AUDIO_DECODER_H_

#include <cstdint>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/audio_buffer.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_config.h"
#include "media/base/decoder_status.h"
#include "media/base/waiting.h"
#include "media/media_export.h"

namespace avbase::media {

// Asynchronous audio decoder contract. Not thread-safe: every method must be
// called on the sequence that owns it.
class AVBASE_MEDIA_EXPORT AudioDecoder {
 public:
  using InitCB = base::OnceCallback<void(DecoderStatus)>;
  using DecodeCB = base::OnceCallback<void(DecoderStatus)>;
  // Called for each decoded buffer, in presentation order.
  using OutputCB =
      base::RepeatingCallback<void(base::scoped_refptr<AudioBuffer>)>;

  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;
  virtual ~AudioDecoder();

  // |current_serial| stamps every produced AudioBuffer so consumers can drop
  // audio that predates a seek. This is an addition over Chromium's contract:
  // ijkplayer's audio path flushes by bumping a serial (ffplay's
  // `is->audioq.serial`), and without it a DecoderStream cannot distinguish a
  // stale buffer from a live one after a flush.
  virtual void Initialize(const AudioDecoderConfig& config,
                          bool has_pending_clear, int32_t current_serial,
                          InitCB init_cb, const OutputCB& output_cb,
                          const WaitingCB& waiting_cb) = 0;

  // Requests |buffer| be decoded. An EOS buffer flushes the decoder: every
  // pending buffer is emitted via output_cb, followed by one EOS AudioBuffer.
  //
  // Deviation from Chromium: FFmpeg decoders here complete synchronously, so
  // decode_cb MAY run before Decode() returns. Callers must not rely on it
  // being deferred.
  virtual void Decode(base::scoped_refptr<DecoderBuffer> buffer,
                      DecodeCB decode_cb) = 0;

  // Aborts pending Decode() calls, then runs |closure|. No calls may be made
  // before |closure| runs.
  virtual void Reset(base::OnceClosure closure) = 0;

  virtual std::string GetDisplayName() const = 0;
  virtual bool IsPlatformDecoder() const { return false; }
  virtual bool CanReadWithoutStalling() const { return true; }
  virtual int GetMaxDecodeRequests() const { return 1; }

 protected:
  AudioDecoder();
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_AUDIO_DECODER_H_
