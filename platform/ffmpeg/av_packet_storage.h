// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Zero-copy bridge: wraps an AVPacket inside media::DecoderBuffer so that no
// FFmpeg type appears above platform/. Retrieval is by TypeId address
// comparison, not RTTI (which is disabled project-wide).

#ifndef IJKPP_PLATFORM_FFMPEG_AV_PACKET_STORAGE_H_
#define IJKPP_PLATFORM_FFMPEG_AV_PACKET_STORAGE_H_

#include <memory>
#include <span>

#include "media/base/decoder_buffer.h"
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"

namespace ijkpp::platform::ffmpeg {

class AvPacketStorage final : public media::DecoderBuffer::Storage {
 public:
  AvPacketStorage() = default;
  AvPacketStorage(const AvPacketStorage&) = delete;
  AvPacketStorage& operator=(const AvPacketStorage&) = delete;
  ~AvPacketStorage() override;

  static const void* TypeIdStatic() {
    static const char id = 0;
    return &id;
  }

  // Adopts a heap AVPacket (av_packet_alloc'd). Takes ownership.
  void Adopt(AVPacket* packet);
  // Wraps an existing AVPacket by adding a reference, leaving the caller's
  // copy valid. This is the path FFmpegDemuxer uses right after av_read_frame.
  bool AddRef(const AVPacket* packet);

  AVPacket* raw() { return packet_.get(); }
  const AVPacket* raw() const { return packet_.get(); }

  // media::DecoderBuffer::Storage:
  const void* TypeId() const override { return TypeIdStatic(); }
  size_t size() const override;
  std::span<const uint8_t> data() const override;

 private:
  std::unique_ptr<AVPacket, PacketDeleter> packet_;
};

}  // namespace ijkpp::platform::ffmpeg

#endif  // IJKPP_PLATFORM_FFMPEG_AV_PACKET_STORAGE_H_
