// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `media/base/decoder_buffer.h` (BSD-3-Clause).
//
// Replaces the raw AVPacket that ijkplayer passes through every layer of
// ff_ffplay.c. No FFmpeg type appears here; the vendor adapter stores its
// AVPacket inside a Storage subclass and retrieves it with storage_as<T>()
// (docs/02 §9 D2).

#ifndef AVBASE_MEDIA_BASE_DECODER_BUFFER_H_
#define AVBASE_MEDIA_BASE_DECODER_BUFFER_H_

#include <stdint.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/media_constants.h"
#include "media/base/media_types.h"
#include "media/media_export.h"

namespace avbase::media {

// A compressed media sample, between the demuxer and the decoder.
//
// Immutable after construction and always held through
// scoped_refptr<DecoderBuffer>, so handing one to a decoder costs an atomic
// increment and never a copy of the payload.
class AVBASE_MEDIA_EXPORT DecoderBuffer
    : public base::RefCountedThreadSafe<DecoderBuffer> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  // Opaque payload backend. Implemented by platform/ffmpeg (AvPacketStorage,
  // zero-copy over an AVPacket), by media/filters (owned memory) and by
  // embedders (encrypted or custom sources).
  class AVBASE_MEDIA_EXPORT Storage {
   public:
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    virtual ~Storage() = default;

    // Identity without RTTI: the address of a static, so comparison is a
    // single pointer test on a per-frame hot path.
    virtual const void* TypeId() const = 0;
    virtual size_t size() const = 0;
    // Empty span when the payload is not CPU-readable (e.g. a hardware
    // bitstream buffer held in device memory).
    virtual std::span<const uint8_t> data() const = 0;

   protected:
    Storage() = default;
  };

  // Owns a copy of |data|.
  static base::scoped_refptr<DecoderBuffer>
  CopyFrom(const uint8_t* data, size_t size, DemuxerStreamType stream_type,
           int32_t stream_index);
  // Adopts |storage| without copying. This is the zero-copy path used by
  // platform/ffmpeg.
  static base::scoped_refptr<DecoderBuffer>
  FromStorage(std::unique_ptr<Storage> storage, DemuxerStreamType stream_type,
              int32_t stream_index);
  // End-of-stream marker. Carries no payload; IsEndOfStream() is true.
  static base::scoped_refptr<DecoderBuffer> CreateEOSBuffer();

  DecoderBuffer(const DecoderBuffer&) = delete;
  DecoderBuffer& operator=(const DecoderBuffer&) = delete;

  bool IsEndOfStream() const { return is_eos_; }

  // Presentation timestamp, or kNoTimestamp. Use has_valid_timestamp() before
  // trusting it: streams without timestamps are legal (raw H.264 annexb).
  // Setters are for the demuxer adapter that constructs the buffer; everything
  // downstream treats a DecoderBuffer as immutable.
  void set_timestamp(base::TimeDelta ts) { timestamp_ = ts; }
  void set_decode_timestamp(base::TimeDelta dts) { decode_timestamp_ = dts; }
  void set_duration(base::TimeDelta d) { duration_ = d; }
  void set_offset(int64_t offset) { offset_ = offset; }
  void set_side_data(std::vector<uint8_t> side_data) {
    side_data_ = std::move(side_data);
  }

  base::TimeDelta timestamp() const { return timestamp_; }
  // Decode timestamp (AVPacket::dts). May be earlier than |timestamp| when the
  // stream has B-frames.
  base::TimeDelta decode_timestamp() const { return decode_timestamp_; }
  base::TimeDelta duration() const { return duration_; }
  // |timestamp()| when valid, otherwise |decode_timestamp()|.
  base::TimeDelta BestEffortTimestamp() const;
  bool has_valid_timestamp() const {
    return !IsNoTimestamp(timestamp_) || !IsNoTimestamp(decode_timestamp_);
  }

  size_t data_size() const;
  std::span<const uint8_t> data() const;
  std::span<const uint8_t> side_data() const { return side_data_; }
  int64_t offset_in_container() const { return offset_; }

  DemuxerStreamType stream_type() const { return stream_type_; }
  int32_t stream_index() const { return stream_index_; }

  // Seek generation. Bumped by DecoderBufferQueue::Flush(); buffers carrying an
  // older serial must be dropped without being decoded. See docs/04 §4 (R1-R3).
  int32_t serial() const { return serial_; }
  void set_serial(int32_t serial) { serial_ = serial; }

  bool is_keyframe() const { return is_keyframe_; }
  void set_keyframe(bool keyframe) { is_keyframe_ = keyframe; }
  // A discardable buffer may be dropped under memory pressure without
  // breaking decode (non-reference frames).
  bool discardable() const { return discardable_; }
  void set_discardable(bool discardable) { discardable_ = discardable; }

  // Adapter escape hatch. Returns nullptr when the payload is not of type |T|,
  // so a wrong cast is a handled branch rather than undefined behaviour. Only
  // platform/ffmpeg and media/ffmpeg/ffmpeg_* may call this.
  template <typename T>
  T* storage_as() {
    return storage_ && storage_->TypeId() == T::TypeIdStatic()
               ? static_cast<T*>(storage_.get())
               : nullptr;
  }
  template <typename T>
  const T* storage_as() const {
    return storage_ && storage_->TypeId() == T::TypeIdStatic()
               ? static_cast<const T*>(storage_.get())
               : nullptr;
  }
  Storage* storage() { return storage_.get(); }

  std::string AsDebugString() const;

 private:
  friend class base::RefCountedThreadSafe<DecoderBuffer>;
  ~DecoderBuffer();
  DecoderBuffer();
  explicit DecoderBuffer(std::unique_ptr<Storage> storage,
                         DemuxerStreamType stream_type, int32_t stream_index);

  std::unique_ptr<Storage> storage_;
  std::vector<uint8_t> side_data_;
  base::TimeDelta timestamp_{kNoTimestamp};
  base::TimeDelta decode_timestamp_{kNoTimestamp};
  base::TimeDelta duration_;
  int64_t offset_{-1};
  DemuxerStreamType stream_type_{DemuxerStreamType::kUnknown};
  int32_t stream_index_{-1};
  int32_t serial_{0};
  bool is_eos_{false};
  bool is_keyframe_{false};
  bool discardable_{false};
};

// Owned-memory Storage, used by tests and by non-FFmpeg demuxers.
class AVBASE_MEDIA_EXPORT OwnedBufferStorage final
    : public DecoderBuffer::Storage {
 public:
  OwnedBufferStorage(const uint8_t* data, size_t size);
  ~OwnedBufferStorage() override;

  static const void* TypeIdStatic() {
    static const char id = 0;
    return &id;
  }
  const void* TypeId() const override { return TypeIdStatic(); }
  size_t size() const override { return bytes_.size(); }
  std::span<const uint8_t> data() const override { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_DECODER_BUFFER_H_
