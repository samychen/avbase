// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/text_fake_demuxer.h"

#include <utility>

#include "media/base/text_decoder.h"

namespace avbase::media::test {
namespace {

// A codec name that passes TextDecoderConfig::IsValidConfig(), so the streams
// look like real subtitle tracks to the renderer's text leg.
TextDecoderConfig MakeSubripConfig() {
  TextDecoderConfig config;
  config.codec_name = "subrip";
  return config;
}

}  // namespace

TextFakeDemuxer::TextFakeDemuxer(int32_t first_index, size_t stream_count) {
  streams_.reserve(stream_count);
  for (size_t i = 0; i < stream_count; ++i) {
    const int32_t index = first_index + static_cast<int32_t>(i);
    auto stream = std::make_unique<FakeDemuxerStream>(
        DemuxerStreamType::kText, MakeValidAudioConfig(),
        MakeValidVideoConfig(), MakeSubripConfig());
    stream->set_stream_index(index);
    streams_.push_back(std::move(stream));
    StreamInfo info;
    info.index = index;
    info.kind = StreamKind::kText;
    info.codec_name = "subrip";
    media_info_.streams.push_back(std::move(info));
  }
}

DemuxerStream* TextFakeDemuxer::GetStream(DemuxerStreamType type) {
  const std::vector<DemuxerStream*> all = GetStreams(type);
  return all.empty() ? nullptr : all[0];
}

std::vector<DemuxerStream*>
TextFakeDemuxer::GetStreams(DemuxerStreamType type) {
  std::vector<DemuxerStream*> out;
  for (auto& s : streams_) {
    if (s->type() == type) {
      out.push_back(s.get());
    }
  }
  return out;
}

void TextFakeDemuxer::SetActiveStream(DemuxerStreamType type,
                                      int stream_index) {
  last_active_type_ = type;
  last_active_index_ = stream_index;
  ++active_calls_;
}

}  // namespace avbase::media::test
