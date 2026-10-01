// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/fake_demuxer_stream.h"

#include <algorithm>
#include <vector>

namespace ijkpp::media::test {

AudioDecoderConfig MakeValidAudioConfig() {
  AudioDecoderConfig config;
  config.codec = AudioCodec::kAac;
  config.codec_name = "aac";
  config.channel_layout = ChannelLayout::kStereo;
  config.sample_format = SampleFormat::kF32P;
  config.sample_rate = 48000;
  config.channels = 2;
  return config;
}

VideoDecoderConfig MakeValidVideoConfig() {
  VideoDecoderConfig config;
  config.codec = VideoCodec::kH264;
  config.codec_name = "h264";
  config.coded_size = Size{320, 240};
  config.natural_size = Size{320, 240};
  config.frame_rate = Rational{30, 1};
  return config;
}

base::scoped_refptr<DecoderBuffer> MakeDataBuffer(DemuxerStreamType type,
                                                  int32_t serial,
                                                  size_t bytes) {
  const std::vector<uint8_t> data(bytes, 0xAB);
  auto buffer = DecoderBuffer::CopyFrom(data.data(), data.size(), type, 0);
  buffer->set_serial(serial);
  return buffer;
}

FakeDemuxerStream::FakeDemuxerStream(DemuxerStreamType type,
                                     AudioDecoderConfig audio_config,
                                     VideoDecoderConfig video_config)
    : type_(type),
      audio_config_(std::move(audio_config)),
      video_config_(std::move(video_config)) {}

void FakeDemuxerStream::AppendBuffer(
    base::scoped_refptr<DecoderBuffer> buffer) {
  buffers_.push_back(std::move(buffer));
}

void FakeDemuxerStream::Read(uint32_t count, ReadCB read_cb) {
  ++read_count_;
  DecoderBufferVector out;
  const size_t limit = std::min<size_t>(count, max_per_read_);
  while (out.size() < limit && next_ < buffers_.size()) {
    out.push_back(std::move(buffers_[next_++]));
  }
  if (out.empty()) {
    // Nothing left: the real demuxer would block or emit EOS. Emitting EOS
    // keeps the test deterministic, and "EOS in the same reply as the last
    // buffer" is a case the renderers must handle anyway.
    out.push_back(DecoderBuffer::CreateEOSBuffer());
  }
  std::move(read_cb).Run(Status::kOk, std::move(out));
}

void FakeMediaResource::set_stream(DemuxerStreamType type,
                                   DemuxerStream* stream) {
  switch (type) {
    case DemuxerStreamType::kAudio:
      audio_ = stream;
      return;
    case DemuxerStreamType::kVideo:
      video_ = stream;
      return;
    case DemuxerStreamType::kUnknown:
    case DemuxerStreamType::kText:
      // A renderer has no text path (gap 3 in media/base/media_resource.h), so
      // the fake cannot express one either.
      return;
  }
}

DemuxerStream* FakeMediaResource::GetStream(DemuxerStreamType type) {
  switch (type) {
    case DemuxerStreamType::kAudio:
      return audio_;
    case DemuxerStreamType::kVideo:
      return video_;
    case DemuxerStreamType::kUnknown:
    case DemuxerStreamType::kText:
      return nullptr;
  }
  return nullptr;
}

}  // namespace ijkpp::media::test
