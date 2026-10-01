// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_FFMPEG_DATA_SOURCE_IO_H_
#define AVBASE_PLATFORM_FFMPEG_DATA_SOURCE_IO_H_

#include <stdint.h>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/atomic_flag.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"

struct AVFormatContext;
struct AVIOContext;

namespace avbase::platform::ffmpeg {

// Bridges a media::DataSource into FFmpeg's AVIOContext, so the demuxer can
// read through host-supplied byte sources -- a memory buffer, an fd, or an
// application's own downloader -- instead of FFmpeg's protocol layer. This
// closes the M4 leftover (docs/PROGRESS, "DataSource 后端的 AVIOContext 桥")
// and is what lets the M9 test rig (ThrottledDataSource) drive a real
// FFmpegDemuxer.
//
// THREADING. The object lives on the demux thread, end to end: Attach(),
// the two static callbacks (which call DataSource::ReadBlocking -- the D3
// contract's blocking variant), and Detach() never run anywhere else. The
// |interrupt| flag is the demuxer's: when it is set, a blocking read returns
// AVERROR_EXIT immediately, which is what bounds Stop() alongside the
// format context's own interrupt_callback.
//
// LIFECYCLE. Attach() sets ctx->pb and AVFMT_FLAG_CUSTOM_IO, so
// avformat_close_input() will NOT free the context; Detach() flushes and
// frees it. Call Detach() before the DataSource reference is released --
// actually, it is the other way: ~DataSourceIO releases the reference, and
// Detach() must have run first, or FFmpeg could still hold read pointers
// into a source that is gone.
class DataSourceIO {
 public:
  DataSourceIO(base::scoped_refptr<media::DataSource> source,
               base::AtomicFlag* interrupt);
  DataSourceIO(const DataSourceIO&) = delete;
  DataSourceIO& operator=(const DataSourceIO&) = delete;
  ~DataSourceIO();

  // Wraps |ctx|'s I/O. |ctx| must not have opened its own pb yet, and the
  // source must report its size and seekability honestly -- an unseekable
  // source fed an MP4 fails at probe time with an actionable error rather
  // than halfway through playback.
  bool Attach(AVFormatContext* ctx, media::MediaError* error);
  // Flushes and frees the AVIOContext. Idempotent.
  void Detach();

  // The wrapped source, for the demuxer's Stop() to Abort().
  media::DataSource* source() const { return source_.get(); }

 private:
  static int ReadPacket(void* opaque, uint8_t* buffer, int size);
  static int64_t Seek(void* opaque, int64_t offset, int whence);

  // The next offset ReadBlocking will serve. Owned here, not derived from
  // avio_tell(): FFmpeg's buffer state and the DataSource's byte offsets are
  // different quantities, and conflating them corrupts every seek.
  int64_t next_offset_{0};

  base::scoped_refptr<media::DataSource> source_;
  base::raw_ptr<base::AtomicFlag> interrupt_;
  AVIOContext* pb_{nullptr};
  // av_malloc'd in Attach(); ownership passes to the AVIOContext, and
  // avio_context_free() releases it (or the grown copy FFmpeg swapped in).
  uint8_t* io_buffer_{nullptr};
};

}  // namespace avbase::platform::ffmpeg

#endif  // AVBASE_PLATFORM_FFMPEG_DATA_SOURCE_IO_H_
