// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The input side of a renderer test: a DemuxerStream whose data is scripted by
// the test, and the MediaResource that hands it out. No FFmpeg, no threads.
//
// media/base/media_resource.h names this exact use: RendererImpl takes a
// MediaResource rather than a Demuxer so that a test "can also be handed a fake
// that returns a fixed stream list, which is what the RendererImpl unit tests
// want". Before this file existed, the tenth round's renderer bugs were only
// reachable by playing a real file (docs/PROGRESS.md §(5) item 1).

#ifndef IJKPP_TESTS_SUPPORT_FAKE_DEMUXER_STREAM_H_
#define IJKPP_TESTS_SUPPORT_FAKE_DEMUXER_STREAM_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_config.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_resource.h"

namespace ijkpp::media::test {

// Configs that pass IsValidConfig(), so the streams below look like real ones.
// The audio one matches what decoder_stream_unittest.cc uses (AAC stereo
// 48 kHz), which keeps the two suites' arithmetic comparable.
AudioDecoderConfig MakeValidAudioConfig();
VideoDecoderConfig MakeValidVideoConfig();

// A zero-filled buffer for |type|. The timestamp is left unset: the
// sub-renderers exercised here are driven by their sinks, not by a clock.
base::scoped_refptr<DecoderBuffer> MakeDataBuffer(DemuxerStreamType type,
                                                  int32_t serial,
                                                  size_t bytes = 16);

// Hands out the buffers it was given, in order, then an EOS marker.
//
// Read() completes inline, which is *stricter* than the real contract (a
// demuxer runs the callback on its media sequence, never inline) and so
// exercises the re-entrancy guards of whoever reads from it. Same choice, and
// the same reason, as ScriptedDemuxerStream in decoder_stream_unittest.cc.
class FakeDemuxerStream final : public DemuxerStream {
 public:
  FakeDemuxerStream(DemuxerStreamType type, AudioDecoderConfig audio_config,
                    VideoDecoderConfig video_config);

  void AppendBuffer(base::scoped_refptr<DecoderBuffer> buffer);

  void set_serial(int32_t serial) { serial_ = serial; }
  void set_stream_index(int32_t index) { stream_index_ = index; }
  void set_supports_config_changes(bool supports) {
    supports_config_changes_ = supports;
  }
  // One Read() hands out at most this many buffers, so a test can leave data
  // behind for a post-flush read.
  void set_max_per_read(uint32_t count) { max_per_read_ = count; }

  int read_count() const { return read_count_.load(); }

  // DemuxerStream.
  void Read(uint32_t count, ReadCB read_cb) override;
  const AudioDecoderConfig& audio_decoder_config() const override {
    return audio_config_;
  }
  const VideoDecoderConfig& video_decoder_config() const override {
    return video_config_;
  }
  DemuxerStreamType type() const override { return type_; }
  int32_t stream_index() const override { return stream_index_; }
  bool SupportsConfigChanges() const override {
    return supports_config_changes_;
  }
  int32_t serial() const override { return serial_; }
  size_t buffered_buffers() const override { return buffers_.size() - next_; }
  size_t buffered_bytes() const override { return 0; }
  base::TimeDelta buffered_duration() const override {
    return base::TimeDelta();
  }

 private:
  const DemuxerStreamType type_;
  AudioDecoderConfig audio_config_;
  VideoDecoderConfig video_config_;
  std::vector<base::scoped_refptr<DecoderBuffer>> buffers_;
  size_t next_ = 0;
  uint32_t max_per_read_ = 8;
  int32_t serial_ = 0;
  int32_t stream_index_ = 0;
  bool supports_config_changes_ = false;
  // Read from the owning sequence (S3/S4) and from the test thread.
  std::atomic<int> read_count_{0};
};

// Returns the stream of the requested type, or nullptr when the test set none,
// which is how the audio-only and video-only cases are expressed. nullptr is
// not an error for a MediaResource; RendererImpl falls back to the external
// clock (media/base/media_resource.h).
class FakeMediaResource final : public MediaResource {
 public:
  FakeMediaResource() = default;

  void set_stream(DemuxerStreamType type, DemuxerStream* stream);

  // MediaResource.
  DemuxerStream* GetStream(DemuxerStreamType type) override;

 private:
  DemuxerStream* audio_ = nullptr;
  DemuxerStream* video_ = nullptr;
};

}  // namespace ijkpp::media::test

#endif  // IJKPP_TESTS_SUPPORT_FAKE_DEMUXER_STREAM_H_
