// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Signature mirrors Chromium's `media/base/video_decoder.h` (BSD-3-Clause),
// including the "decode_cb is never run inline" and "output_cb may run before
// or after decode_cb" guarantees.
//
// Replaces ijkplayer's ffpipenode hand-written vtable (pipeline/ffpipenode.c
// plus ffpipenode_internal.h), and its synchronous run_sync/flush pair. The
// async shape is not stylistic: hardware decoders (MediaCodec, VideoToolbox,
// VAAPI) are inherently asynchronous, and a synchronous interface would force
// each of them to spawn a thread just to look synchronous.

#ifndef AVBASE_MEDIA_BASE_VIDEO_DECODER_H_
#define AVBASE_MEDIA_BASE_VIDEO_DECODER_H_

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_config.h"
#include "media/base/decoder_status.h"
#include "media/base/media_types.h"
#include "media/base/video_frame.h"
#include "media/base/waiting.h"
#include "media/media_export.h"

namespace avbase::media {

class CdmContext;   // DRM placeholder; not implemented (docs/08 §4).

// WaitingReason/WaitingCB live in media/base/waiting.h so AudioDecoder can
// share them without depending on this header.

class AVBASE_MEDIA_EXPORT VideoDecoder {
 public:
  using InitCB = base::OnceCallback<void(DecoderStatus)>;
  // Called for each decoded frame. Must be invoked as soon as the frame is
  // ready, without thread trampolining: an extra hop per frame measurably hurts
  // delivery timing on high-frame-rate material.
  using OutputCB = base::RepeatingCallback<void(base::scoped_refptr<VideoFrame>)>;
  using DecodeCB = base::OnceCallback<void(DecoderStatus)>;

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;
  virtual ~VideoDecoder();

  // Initializes with |config|. Notes (identical to Chromium's):
  //  1) Re-initialization drops all internally buffered frames.
  //  2) Must not be called while a decode or reset is pending.
  //  3) No calls may be made before |init_cb| runs.
  //  4) |init_cb| MAY run before this method returns.
  // |low_delay| forbids queueing beyond what reordering requires; initialization
  // fails if the decoder cannot comply.
  virtual void Initialize(const VideoDecoderConfig& config, bool low_delay,
                          CdmContext* cdm_context, InitCB init_cb,
                          const OutputCB& output_cb,
                          const WaitingCB& waiting_cb) = 0;

  // Requests |buffer| be decoded. Guarantees:
  //  - |decode_cb| is NEVER run from within this method;
  //  - |decode_cb| runs even if Decode() is never called again;
  //  - |output_cb| may run before or after |decode_cb|, including before
  //    Decode() returns;
  //  - an EOS buffer flushes: |output_cb| runs for every pending frame, then
  //    |decode_cb| runs.
  // Up to GetMaxDecodeRequests() calls may be in flight.
  virtual void Decode(base::scoped_refptr<DecoderBuffer> buffer,
                      DecodeCB decode_cb) = 0;

  // Aborts pending Decode() calls (their callbacks run with kDecodingAborted)
  // before |closure| runs. No calls may be made before |closure| runs.
  virtual void Reset(base::OnceClosure closure) = 0;

  virtual bool NeedsBitstreamConversion() const;
  virtual bool CanReadWithoutStalling() const;
  virtual int GetMaxDecodeRequests() const;
  virtual VideoDecoderType GetDecoderType() const = 0;
  virtual const char* name() const = 0;

 protected:
  VideoDecoder();
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_VIDEO_DECODER_H_
