// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/ffmpeg/data_source_io.h"

#include <string>

#include "base/memory/raw_ptr.h"
#include "platform/ffmpeg/av_includes.h"

namespace ijkpp::platform::ffmpeg {
namespace {

// The AVIO buffer size. FFmpeg's own file protocol uses 32 KB; probing an MP4
// header plus the mov atom walk fits comfortably, and a larger buffer only
// wastes memory on the memory-buffer case where it buys nothing.
constexpr int kIoBufferSize = 32 * 1024;

media::MediaError UnusableSourceError(const std::string& what) {
  return media::MediaError(
      media::ErrorCode::kSourceOpenFailed,
      "the custom data source cannot serve reads",
      "the AVIO bridge reported: " + what,
      "check the DataSource's ReadBlocking/GetSize/IsSeekable contract "
      "(media/base/data_source.h): reads must block until data or EOF, and "
      "seekability must match what the container needs (MP4 requires seek)");
}

}  // namespace

DataSourceIO::DataSourceIO(base::scoped_refptr<media::DataSource> source,
                           base::AtomicFlag* interrupt)
    : source_(std::move(source)), interrupt_(interrupt) {}

DataSourceIO::~DataSourceIO() {
  Detach();
}

bool DataSourceIO::Attach(AVFormatContext* ctx,
                          media::MediaError* error) {
  int64_t size = -1;
  if (!source_->GetSize(&size)) {
    size = -1;
  }
  const bool seekable = source_->IsSeekable();
  if (!seekable && size < 0) {
    // A streaming source without a length cannot answer AVSEEK_SIZE, and
    // every container probe needs one of the two.
    *error = UnusableSourceError(
        "the source is neither seekable nor sizeable");
    return false;
  }

  // The buffer becomes the AVIOContext's: avio_context_free() frees it (or
  // the grown copy FFmpeg swaps in), so it must come from av_malloc, never
  // from an allocator that would double-free -- the first bridge test's
  // teardown caught exactly that.
  io_buffer_ = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
  pb_ = avio_alloc_context(io_buffer_, kIoBufferSize,
                           /*write_flag=*/0, /*opaque=*/this,
                           /*read_packet=*/&DataSourceIO::ReadPacket,
                           /*write_packet=*/nullptr,
                           /*seek=*/seekable ? &DataSourceIO::Seek : nullptr);
  if (!pb_) {
    *error = UnusableSourceError("avio_alloc_context failed (out of memory)");
    return false;
  }
  ctx->pb = pb_;
  // The flag is what keeps avformat_close_input() from freeing a context we
  // own (and from "reopening" the empty filename).
  ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
  return true;
}

void DataSourceIO::Detach() {
  if (pb_) {
    // A custom pb (avio_alloc_context) has no URLContext behind it -- its
    // opaque is this object -- so avio_closep() would treat `this` as a
    // URLContext and free garbage (found by the first bridge test's
    // teardown). avio_context_free() is the correct finalizer; it also
    // frees any buffer FFmpeg reallocated internally. io_buffer_ stays
    // ours: FFmpeg may have swapped pb->buffer for a grown copy, and the
    // original slice belongs to the vector.
    avio_context_free(&pb_);
  }
}

int DataSourceIO::ReadPacket(void* opaque, uint8_t* buffer, int size) {
  auto* self = static_cast<DataSourceIO*>(opaque);
  if (self->interrupt_->IsSet()) {
    // The same signal the format context's interrupt_callback uses: an
    // aborting read must not hang waiting for bytes that will never come.
    return AVERROR_EXIT;
  }
  const media::DataSource::ReadResult result = self->source_->ReadBlocking(
      self->next_offset_, static_cast<size_t>(size), buffer);
  if (!result.has_value()) {
    return AVERROR_EXTERNAL;
  }
  if (result.value() <= 0) {
    return AVERROR_EOF;
  }
  self->next_offset_ += result.value();
  return result.value();
}

int64_t DataSourceIO::Seek(void* opaque, int64_t offset, int whence) {
  auto* self = static_cast<DataSourceIO*>(opaque);
  // The bridge owns the position: FFmpeg's internal buffer state makes
  // avio_tell() mean "what the demuxer has consumed", not "where the next
  // byte comes from", and the DataSource contract carries the offset in
  // every ReadBlocking call -- so a seek is just re-pointing |next_offset_|.
  int64_t target = -1;
  if (whence == AVSEEK_SIZE) {
    int64_t size = -1;
    if (!self->source_->GetSize(&size)) {
      return AVERROR(ENOSYS);
    }
    return size;
  }
  switch (whence) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = self->next_offset_ + offset; break;
    case SEEK_END: {
      int64_t size = -1;
      if (!self->source_->GetSize(&size) || size < 0) {
        return AVERROR(ENOSYS);
      }
      target = size + offset;
      break;
    }
    default: return AVERROR(ENOSYS);
  }
  if (target < 0) {
    return AVERROR(EINVAL);
  }
  self->next_offset_ = target;
  return target;
}

}  // namespace ijkpp::platform::ffmpeg
