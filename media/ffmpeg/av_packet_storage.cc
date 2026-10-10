// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/av_packet_storage.h"

#include "media/ffmpeg/compat.h"

namespace avbase::media::ffmpeg {

AvPacketStorage::~AvPacketStorage() = default;

void AvPacketStorage::Adopt(AVPacket* packet) {
  packet_.reset(packet);
}

bool AvPacketStorage::AddRef(const AVPacket* packet) {
  if (!packet) {
    return false;
  }
  // MakePacket CHECKs internally; the only failure mode left is the ref
  // itself. av_packet_ref increments the buffer refcount rather than copying
  // payload bytes, so this stays O(1) on the demux hot path.
  PacketPtr copy = MakePacket();
  if (av_packet_ref(copy.get(), packet) < 0) {
    return false;
  }
  packet_.reset(copy.release());
  return true;
}

size_t AvPacketStorage::size() const {
  return packet_ ? static_cast<size_t>(packet_->size) : 0;
}

std::span<const uint8_t> AvPacketStorage::data() const {
  if (!packet_ || !packet_->data || packet_->size <= 0) {
    return {};
  }
  return {packet_->data, static_cast<size_t>(packet_->size)};
}

}  // namespace avbase::media::ffmpeg
