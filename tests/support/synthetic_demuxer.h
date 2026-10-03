// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A deterministic source with no container and no codec: the M10 deliverable
// docs/07 §5 specifies, and the other reason media/base/media_resource.h exists
// ("a fake that returns a fixed stream list"). A real file can test that
// playback works; only a source whose every byte the test chose can test
// *where*
// a seek landed and *which* frame is on screen.
//
// THE ENCODING IS THE POINT. Each packet carries its own index in its payload,
// so a decoder built on top (tests/support/synthetic_decoders.h, next slice)
// can
// paint that index into the frame's pixels -- after which "which frame is on
// screen" is answerable from the frame itself, not from a counter the pipeline
// keeps about itself. Audio works the same way in the frequency domain: the
// tone
// is 440 Hz plus the whole second of the packet's presentation time, so the
// audio timeline can be read back out of the samples.
//
// SCOPE OF THIS FILE, stated because a test double that lies is worse than no
// test double: cadence, keyframe geometry, seek landing and the packet-level
// half of the encoding are here and tested in
// tests/unit/media_filters/synthetic_demuxer_unittest.cc. The pixel half needs
// the synthetic decoder that reads its output, and the fault-injection fields
// docs/07 §5 lists (fail_read_at_packet, stall_at, pts_discontinuities,
// resolution_change_at_half) are absent rather than inert -- a hook nobody
// fires
// is how a double starts drifting away from the thing it stands in for.

#ifndef AVBASE_TESTS_SUPPORT_SYNTHETIC_DEMUXER_H_
#define AVBASE_TESTS_SUPPORT_SYNTHETIC_DEMUXER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_info.h"

namespace avbase::media::test {

// One video frame per 1/fps seconds, one audio packet per
// |audio_frames_per_packet| audio frames, a keyframe every
// |keyframe_interval| frames. Defaults are 10 s of 320x240 at 30 fps with
// 48 kHz stereo, which is what docs/07 §5's checklist counts in.
struct SyntheticSpec {
  int width = 320;
  int height = 240;
  int fps_num = 30;
  int fps_den = 1;
  int sample_rate = 48000;
  int channels = 2;
  int audio_frames_per_packet = 1024;
  base::TimeDelta duration = base::Seconds(10);
  // Live simulation: IsLive() reports true, so the pipeline's live-edge
  // chase (docs/12 section 2.1) evaluates against the finite duration as a
  // stand-in edge. For an edge that genuinely MOVES, see
  // synthetic_live_demuxer.h.
  bool live = false;
  // Feed at 1x realtime, measured against a tick clock (see
  // SyntheticDemuxer::set_tick_clock).
  //
  // WHY THIS EXISTS, and it is not a detail. Unpaced, this source hands out
  // packets as fast as the decoder asks, so the media clock runs AHEAD of the
  // wall clock -- measured 3.56 s of media in ~2.5 s of wall -- and the
  // compositor correctly presents only the frames the master clock says are
  // due. A test then counts 30 frames where it expected 300 and concludes the
  // pipeline is dropping frames, when the pipeline is fine and the SOURCE is
  // racing.
  //
  // The same lesson the throttle suite learned from the other end: a fake that
  // delivers as fast as asked cannot express a real-time regime, and any
  // assertion about counts or pacing silently becomes an assertion about the
  // fake.
  bool paced = false;
  // Stream enablement (docs/07 section 5's audio-only / video-only cases):
  // a disabled stream reports nullptr from GetStream and disappears from
  // MediaInfo, exactly as if the container had none. RendererImpl falls
  // back to the external clock for video-only, per ffplay.
  bool enable_video = true;
  bool enable_audio = true;
  double base_tone_hz = 440.0;
  int keyframe_interval = 30;

  base::TimeDelta frame_duration() const;
  int64_t frame_count() const;
  int64_t audio_packet_count() const;
};

// 32-bit little-endian, at the front of every packet's payload. The packet
// factories below write it; ReadIndexPayload() is how a test checks the same
// thing from the outside, and how the synthetic decoder will read it once it
// exists.
constexpr size_t kSyntheticIndexBytes = 4;
bool ReadIndexPayload(const DecoderBuffer& buffer, uint32_t* index);

// The tone a packet at |pts| carries: |base_tone_hz| plus the whole seconds of
// |pts|. A test asserts on this (or on an FFT of the decoded samples) instead
// of
// trusting a timestamp the pipeline carried along.
double ExpectedToneHzAt(base::TimeDelta pts, double base_tone_hz = 440.0);

class SyntheticDemuxer final : public Demuxer {
 public:
  explicit SyntheticDemuxer(SyntheticSpec spec);
  ~SyntheticDemuxer() override;

  const SyntheticSpec& spec() const { return spec_; }

  // The clock |paced| is measured against. Setting it also starts the clock:
  // the first frame is available immediately, and availability grows from
  // there. Null (the default) leaves the source unpaced, which is what the
  // existing suites expect.
  void set_tick_clock(const base::TickClock* clock);

  // Where both streams read from next. StartPlayingFrom() moves it; a test uses
  // this directly when it only needs to look at one stream.
  void SetPosition(base::TimeDelta time);
  base::TimeDelta position() const { return position_; }
  int seek_count() const { return seek_count_; }
  // The current seek generation. Both streams report it and every packet
  // carries it, which is what lets a consumer drop a buffer from before a seek
  // (media/base/demuxer_stream.h: "a consumer must drop any whose serial is
  // older than the value returned here after a Flush").
  int32_t serial() const {
    std::scoped_lock scoped(lock_);
    return serial_;
  }
  int64_t packets_read() const {
    std::scoped_lock scoped(lock_);
    return packets_read_;
  }

  // The frame a keyframe seek to |time| lands on: the last keyframe at or
  // before
  // it. Exposed so a test can state the landing it expects rather than deriving
  // the geometry twice and asserting that its own arithmetic agrees with
  // itself.
  base::TimeDelta KeyframeAtOrBefore(base::TimeDelta time) const;
  int64_t FrameIndexAt(base::TimeDelta time) const;

  // Demuxer.
  void
  Initialize(const DataSourceDescriptor& source, const DemuxerOptions& options,
             Host* host,
             base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
             InitializeCB init_cb) override;
  void StartPlayingFrom(base::TimeDelta time, SeekCB cb) override;
  void Flush(base::OnceClosure flush_cb) override;
  void Reset(base::OnceClosure reset_cb) override;
  void Stop() override;
  DemuxerStream* GetStream(DemuxerStreamType type) override;
  const MediaInfo& media_info() const override { return media_info_; }
  base::TimeDelta GetStartTime() const override { return base::TimeDelta(); }
  bool IsLive() const override { return media_info_.is_live; }
  bool IsSeekable() const override { return true; }
  DemuxerStats GetStats() const override;
  const char* name() const override { return "SyntheticDemuxer"; }

 private:
  // Both streams read the owner's cursor, so a seek moves the two of them
  // together and neither can drift. They are called on the media sequence only
  // (DemuxerStream's contract), so no lock guards them.
  class VideoStream final : public DemuxerStream {
   public:
    VideoStream(SyntheticDemuxer* owner);
    void Read(uint32_t count, ReadCB read_cb) override;
    const AudioDecoderConfig& audio_decoder_config() const override {
      return empty_audio_config_;
    }
    const VideoDecoderConfig& video_decoder_config() const override {
      return config_;
    }
    DemuxerStreamType type() const override {
      return DemuxerStreamType::kVideo;
    }
    int32_t stream_index() const override { return 0; }
    bool SupportsConfigChanges() const override { return false; }
    int32_t serial() const override {
      std::scoped_lock scoped(owner_->lock_);
      return owner_->serial_;
    }
    size_t buffered_buffers() const override;
    size_t buffered_bytes() const override { return 0; }
    base::TimeDelta buffered_duration() const override;

   private:
    SyntheticDemuxer* const owner_;
    const VideoDecoderConfig config_;
    const AudioDecoderConfig empty_audio_config_;
    // Signalled on Close/teardown so a parked read wakes instead of waiting
    // out the clock.
    base::WaitableEvent parked_{
        base::WaitableEvent::ResetPolicy::kManualReset,
        base::WaitableEvent::InitialState::kNotSignaled};
  };

  class AudioStream final : public DemuxerStream {
   public:
    AudioStream(SyntheticDemuxer* owner);
    void Read(uint32_t count, ReadCB read_cb) override;
    const AudioDecoderConfig& audio_decoder_config() const override {
      return config_;
    }
    const VideoDecoderConfig& video_decoder_config() const override {
      return empty_video_config_;
    }
    DemuxerStreamType type() const override {
      return DemuxerStreamType::kAudio;
    }
    int32_t stream_index() const override { return 1; }
    bool SupportsConfigChanges() const override { return false; }
    int32_t serial() const override {
      std::scoped_lock scoped(owner_->lock_);
      return owner_->serial_;
    }
    size_t buffered_buffers() const override;
    size_t buffered_bytes() const override { return 0; }
    base::TimeDelta buffered_duration() const override;

   private:
    SyntheticDemuxer* const owner_;
    const AudioDecoderConfig config_;
    const VideoDecoderConfig empty_video_config_;
    base::WaitableEvent parked_{
        base::WaitableEvent::ResetPolicy::kManualReset,
        base::WaitableEvent::InitialState::kNotSignaled};
  };

  // Packet factories. The index travels in the payload, the geometry in the
  // timestamp and the keyframe flag. Not const: producing a packet is what
  // advances the cursor. The Locked variants require |lock_| held; the Read()
  // methods call them under one lock so a batch is a consistent snapshot.
  // Packets available at the current edge. Unpaced that is the whole file;
  // paced it is what the clock says exists, which is what makes a count
  // assertion mean what it says.
  int64_t VideoAvailableLocked() const;
  int64_t AudioAvailableLocked() const;
  base::scoped_refptr<DecoderBuffer> MakeVideoPacketLocked();
  base::scoped_refptr<DecoderBuffer> MakeAudioPacketLocked();
  void SetPositionLocked(base::TimeDelta time);

  const SyntheticSpec spec_;
  // Set by set_tick_clock(). Null leaves the source unpaced.
  const base::TickClock* tick_clock_{nullptr};
  base::TimeTicks started_ticks_;
  MediaInfo media_info_;
  std::unique_ptr<VideoStream> video_;
  std::unique_ptr<AudioStream> audio_;
  Host* host_ = nullptr;
  // Frame index for video, packet index for audio, advanced by Read().
  int64_t next_frame_ = 0;
  int64_t next_audio_packet_ = 0;
  base::TimeDelta position_;
  int32_t serial_ = 0;
  // Guards every mutable member below the streams' Read()s: the pipeline's
  // two decoder sequences (S3/S4) read the two streams CONCURRENTLY, and S1
  // seeks while they do. The real demuxer locks internally; the double must
  // not lie about that (TSan caught S3 and S4 racing here the first time
  // HoldReads() released both streams at once).
  mutable std::mutex lock_;
  int seek_count_ = 0;
  int64_t packets_read_ = 0;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_SYNTHETIC_DEMUXER_H_
