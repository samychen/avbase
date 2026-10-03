// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Signature mirrors Chromium's `media/base/demuxer_stream.h` (BSD-3-Clause),
// including the Read(count, ReadCB) shape and the Status enum.

#ifndef AVBASE_MEDIA_BASE_DEMUXER_STREAM_H_
#define AVBASE_MEDIA_BASE_DEMUXER_STREAM_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_config.h"
#include "media/media_export.h"

namespace avbase::media {

enum class StreamLiveness { kUnknown = 0, kRecorded, kLive };
AVBASE_MEDIA_EXPORT const char* GetStreamLivenessName(StreamLiveness liveness);

// One elementary stream inside a container.
//
// Read() is asynchronous and never runs its callback inline: the result is
// posted to the sequence that created the stream. Callers request up to |count|
// buffers and may receive 1..count back; the last one may be the EOS marker.
class AVBASE_MEDIA_EXPORT DemuxerStream {
 public:
  // Status returned in the Read() callback.
  //  kOk            : |buffers| holds 1..count buffers, the last may be EOS.
  //  kAborted       : the read was cancelled by Flush(); |buffers| is empty.
  //  kConfigChanged : the decoder config changed mid-stream; re-query
  //                   video_decoder_config()/audio_decoder_config() before
  //                   reading again. Only returned when
  //                   SupportsConfigChanges().
  //  kError         : fatal; playback should fail.
  enum class Status { kOk, kAborted, kConfigChanged, kError };
  static const char* GetStatusName(Status status);

  using DecoderBufferVector = std::vector<base::scoped_refptr<DecoderBuffer>>;
  using ReadCB = base::OnceCallback<void(Status, DecoderBufferVector)>;

  DemuxerStream(const DemuxerStream&) = delete;
  DemuxerStream& operator=(const DemuxerStream&) = delete;

  virtual void Read(uint32_t count, ReadCB read_cb) = 0;

  virtual const AudioDecoderConfig& audio_decoder_config() const = 0;
  virtual const VideoDecoderConfig& video_decoder_config() const = 0;
  // Text tracks only. The base returns an invalid config so single-text-free
  // demuxers need no override; FFmpegDemuxerStream populates it from the
  // container's subtitle track header.
  virtual const TextDecoderConfig& text_decoder_config() const {
    static const TextDecoderConfig empty;
    return empty;
  }
  virtual DemuxerStreamType type() const = 0;
  virtual int32_t stream_index() const = 0;
  virtual StreamLiveness liveness() const;
  virtual bool SupportsConfigChanges() const = 0;

  // Current seek generation. Buffers returned by Read() carry the serial that
  // was in effect when they were demuxed; a consumer must drop any whose serial
  // is older than the value returned here after a Flush(). docs/04 §4 R1-R3.
  virtual int32_t serial() const = 0;

  // Buffering state, for BufferController's three-tier high water mark.
  virtual size_t buffered_buffers() const = 0;
  virtual size_t buffered_bytes() const = 0;
  virtual base::TimeDelta buffered_duration() const = 0;

 protected:
  DemuxerStream() = default;
  virtual ~DemuxerStream() = default;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_DEMUXER_STREAM_H_
