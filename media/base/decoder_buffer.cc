// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/decoder_buffer.h"

#include <utility>

#include "base/check.h"

namespace avbase::media {

DecoderBuffer::DecoderBuffer() = default;

DecoderBuffer::DecoderBuffer(std::unique_ptr<Storage> storage,
                             DemuxerStreamType stream_type,
                             int32_t stream_index)
    : storage_(std::move(storage)),
      stream_type_(stream_type),
      stream_index_(stream_index) {}

DecoderBuffer::~DecoderBuffer() = default;

// static
base::scoped_refptr<DecoderBuffer>
DecoderBuffer::CopyFrom(const uint8_t* data, size_t size,
                        DemuxerStreamType stream_type, int32_t stream_index) {
  return FromStorage(std::make_unique<OwnedBufferStorage>(data, size),
                     stream_type, stream_index);
}

// static
base::scoped_refptr<DecoderBuffer>
DecoderBuffer::FromStorage(std::unique_ptr<Storage> storage,
                           DemuxerStreamType stream_type,
                           int32_t stream_index) {
  CHECK(storage);
  return base::scoped_refptr<DecoderBuffer>(
      new DecoderBuffer(std::move(storage), stream_type, stream_index));
}

// static
base::scoped_refptr<DecoderBuffer> DecoderBuffer::CreateEOSBuffer() {
  base::scoped_refptr<DecoderBuffer> buffer(new DecoderBuffer());
  buffer->is_eos_ = true;
  return buffer;
}

base::TimeDelta DecoderBuffer::BestEffortTimestamp() const {
  if (!IsNoTimestamp(timestamp_)) {
    return timestamp_;
  }
  return decode_timestamp_;  // May itself be kNoTimestamp.
}

size_t DecoderBuffer::data_size() const {
  return storage_ ? storage_->size() : 0;
}

std::span<const uint8_t> DecoderBuffer::data() const {
  return storage_ ? storage_->data() : std::span<const uint8_t>();
}

std::string DecoderBuffer::AsDebugString() const {
  if (is_eos_) {
    return "DecoderBuffer(EOS)";
  }
  std::string out = "DecoderBuffer(";
  out += GetDemuxerStreamTypeName(stream_type_);
  out += " idx=" + std::to_string(stream_index_);
  out += " pts=" + std::to_string(timestamp_.InMicroseconds()) + "us";
  out += " dts=" + std::to_string(decode_timestamp_.InMicroseconds()) + "us";
  out += " dur=" + std::to_string(duration_.InMicroseconds()) + "us";
  out += " size=" + std::to_string(data_size());
  out += " serial=" + std::to_string(serial_);
  if (is_keyframe_) {
    out += " key";
  }
  out += ")";
  return out;
}

OwnedBufferStorage::OwnedBufferStorage(const uint8_t* data, size_t size) {
  if (data && size > 0) {
    bytes_.assign(data, data + size);
  }
}

OwnedBufferStorage::~OwnedBufferStorage() = default;

}  // namespace avbase::media
