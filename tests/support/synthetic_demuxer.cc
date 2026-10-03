// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/synthetic_demuxer.h"

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "media/base/audio_parameters.h"
#include "media/base/video_frame.h"

namespace avbase::media::test {
namespace {

// Timestamps are computed from the index in integer microseconds, never by
// accumulating an interval: 1/30 s is not representable in microseconds, so an
// accumulated clock drifts by a microsecond every few frames and "frame 150 is
// at 5 s exactly" would stop being true. Frame 150 *is* 5 s here.
base::TimeDelta TimeOfFrame(const SyntheticSpec& spec, int64_t index) {
  return base::Microseconds(1000000LL * index * spec.fps_den / spec.fps_num);
}

base::TimeDelta TimeOfAudioPacket(const SyntheticSpec& spec, int64_t index) {
  return base::Microseconds(1000000LL * index * spec.audio_frames_per_packet /
                            spec.sample_rate);
}

// The audio packet whose span contains |time|.
int64_t AudioPacketAt(const SyntheticSpec& spec, base::TimeDelta time) {
  const int64_t frames = time.InMicroseconds() * spec.sample_rate / 1000000;
  return frames / spec.audio_frames_per_packet;
}

}  // namespace

base::TimeDelta SyntheticSpec::frame_duration() const {
  return TimeOfFrame(*this, 1);
}

int64_t SyntheticSpec::frame_count() const {
  const int64_t frames =
      duration.InMicroseconds() * fps_num / (fps_den * 1000000LL);
  return std::max<int64_t>(frames, 0);
}

int64_t SyntheticSpec::audio_packet_count() const {
  const int64_t frames = duration.InMicroseconds() * sample_rate / 1000000LL;
  return (frames + audio_frames_per_packet - 1) / audio_frames_per_packet;
}

// The index, little-endian. Built here and handed to CopyFrom() rather than
// written into an existing buffer: DecoderBuffer's payload is immutable once
// constructed (its data() is a span of const bytes), and that is the right
// contract for everything downstream.
std::vector<uint8_t> IndexPayload(uint32_t index) {
  std::vector<uint8_t> payload(kSyntheticIndexBytes, 0);
  for (size_t i = 0; i < kSyntheticIndexBytes; ++i) {
    payload[i] = static_cast<uint8_t>((index >> (8 * i)) & 0xFF);
  }
  return payload;
}

VideoDecoderConfig MakeVideoConfig(const SyntheticSpec& spec) {
  VideoDecoderConfig config;
  config.codec = VideoCodec::kH264;
  config.codec_name = "synthetic-video";
  config.coded_size = Size{spec.width, spec.height};
  config.natural_size = config.coded_size;
  config.frame_rate = Rational{spec.fps_num, spec.fps_den};
  return config;
}

AudioDecoderConfig MakeAudioConfig(const SyntheticSpec& spec) {
  AudioDecoderConfig config;
  config.codec = AudioCodec::kAac;
  config.codec_name = "synthetic-audio";
  config.channel_layout =
      spec.channels == 1 ? ChannelLayout::kMono : ChannelLayout::kStereo;
  config.sample_format = SampleFormat::kF32P;
  config.sample_rate = spec.sample_rate;
  config.channels = spec.channels;
  return config;
}

bool ReadIndexPayload(const DecoderBuffer& buffer, uint32_t* index) {
  if (!index || buffer.IsEndOfStream()) {
    return false;
  }
  const std::span<const uint8_t> payload = buffer.data();
  if (payload.size() < kSyntheticIndexBytes) {
    return false;
  }
  uint32_t value = 0;
  for (size_t i = 0; i < kSyntheticIndexBytes; ++i) {
    value |= static_cast<uint32_t>(payload[i]) << (8 * i);
  }
  *index = value;
  return true;
}

double ExpectedToneHzAt(base::TimeDelta pts, double base_tone_hz) {
  const int64_t seconds = pts.InMicroseconds() / 1000000;
  return base_tone_hz + static_cast<double>(seconds);
}

SyntheticDemuxer::SyntheticDemuxer(SyntheticSpec spec)
    : spec_(std::move(spec)),
      video_(std::make_unique<VideoStream>(this)),
      audio_(std::make_unique<AudioStream>(this)) {
  media_info_.duration = spec_.duration;
  media_info_.is_live = spec_.live;
  media_info_.seekable = true;
  media_info_.format_name = "synthetic";
  media_info_.uri = "synthetic://test";

  StreamInfo video;
  video.index = 0;
  video.kind = StreamKind::kVideo;
  video.codec_name = "synthetic-video";
  video.duration = spec_.duration;
  video.coded_size = Size{spec_.width, spec_.height};
  video.natural_size = video.coded_size;
  video.frame_rate = Rational{spec_.fps_num, spec_.fps_den};
  video.avg_frame_rate = video.frame_rate;
  if (spec_.enable_video) {
    media_info_.streams.push_back(video);
  }

  StreamInfo audio;
  audio.index = 1;
  audio.kind = StreamKind::kAudio;
  audio.codec_name = "synthetic-audio";
  audio.duration = spec_.duration;
  audio.sample_rate = spec_.sample_rate;
  audio.channels = spec_.channels;
  if (spec_.enable_audio) {
    media_info_.streams.push_back(audio);
  }
}

SyntheticDemuxer::~SyntheticDemuxer() = default;

int64_t SyntheticDemuxer::FrameIndexAt(base::TimeDelta time) const {
  if (time <= base::TimeDelta()) {
    return 0;
  }
  const int64_t frames =
      time.InMicroseconds() * spec_.fps_num / (1000000LL * spec_.fps_den);
  return std::min<int64_t>(frames, spec_.frame_count());
}

base::TimeDelta
SyntheticDemuxer::KeyframeAtOrBefore(base::TimeDelta time) const {
  const int64_t index = FrameIndexAt(time);
  if (spec_.keyframe_interval <= 0) {
    return TimeOfFrame(spec_, index);
  }
  const int64_t keyframe =
      (index / spec_.keyframe_interval) * spec_.keyframe_interval;
  return TimeOfFrame(spec_, keyframe);
}

void SyntheticDemuxer::SetPosition(base::TimeDelta time) {
  std::scoped_lock scoped(lock_);
  SetPositionLocked(time);
}

// Callers hold |lock_|.
void SyntheticDemuxer::SetPositionLocked(base::TimeDelta time) {
  position_ = time;
  next_frame_ = FrameIndexAt(time);
  next_audio_packet_ = AudioPacketAt(spec_, time);
}

void SyntheticDemuxer::Initialize(
    const DataSourceDescriptor& /*source*/, const DemuxerOptions& /*options*/,
    Host* host,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    InitializeCB init_cb) {
  host_ = host;
  if (host_) {
    host_->SetDuration(spec_.duration);
  }
  SetPosition(base::TimeDelta());
  // Never inline: Demuxer's contract says |init_cb| runs on
  // |media_task_runner|,
  // and a double that runs it early would let a test pass on ordering the real
  // demuxer cannot produce.
  if (media_task_runner) {
    media_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce([](InitializeCB cb) { std::move(cb).Run(OkStatus()); },
                       std::move(init_cb)));
    return;
  }
  std::move(init_cb).Run(OkStatus());
}

void SyntheticDemuxer::StartPlayingFrom(base::TimeDelta time, SeekCB cb) {
  base::TimeDelta actual;
  {
    std::scoped_lock scoped(lock_);
    ++seek_count_;
    // A seek is a new generation: consumers drop anything stamped with the old
    // serial, which is the whole mechanism behind "no frame from the flushed
    // generation reaches the display".
    ++serial_;
    // A physical seek lands on a keyframe at or before the request, which is
    // what FFmpegDemuxer does and what the cb's "actual" reports.
    SetPositionLocked(KeyframeAtOrBefore(time));
    actual = position_;
  }
  std::move(cb).Run(OkStatus(), actual);
}

void SyntheticDemuxer::Flush(base::OnceClosure flush_cb) {
  {
    std::scoped_lock scoped(lock_);
    SetPositionLocked(base::TimeDelta());
  }
  std::move(flush_cb).Run();
}

void SyntheticDemuxer::Reset(base::OnceClosure reset_cb) {
  {
    std::scoped_lock scoped(lock_);
    SetPositionLocked(base::TimeDelta());
  }
  std::move(reset_cb).Run();
}

void SyntheticDemuxer::Stop() {}

DemuxerStream* SyntheticDemuxer::GetStream(DemuxerStreamType type) {
  switch (type) {
  case DemuxerStreamType::kVideo:
    return spec_.enable_video ? video_.get() : nullptr;
  case DemuxerStreamType::kAudio:
    return spec_.enable_audio ? audio_.get() : nullptr;
  case DemuxerStreamType::kUnknown:
  case DemuxerStreamType::kText:
    return nullptr;
  }
  return nullptr;
}

DemuxerStats SyntheticDemuxer::GetStats() const {
  std::scoped_lock scoped(lock_);
  DemuxerStats stats;
  stats.packets_demuxed = static_cast<uint64_t>(packets_read_);
  stats.seek_count = static_cast<uint64_t>(seek_count_);
  return stats;
}

base::scoped_refptr<DecoderBuffer> SyntheticDemuxer::MakeVideoPacketLocked() {
  const int64_t index = next_frame_++;
  const std::vector<uint8_t> payload =
      IndexPayload(static_cast<uint32_t>(index));
  auto buffer = DecoderBuffer::CopyFrom(payload.data(), payload.size(),
                                        DemuxerStreamType::kVideo, 0);
  buffer->set_timestamp(TimeOfFrame(spec_, index));
  buffer->set_serial(serial_);
  buffer->set_keyframe(spec_.keyframe_interval > 0 &&
                       index % spec_.keyframe_interval == 0);
  ++packets_read_;
  return buffer;
}

base::scoped_refptr<DecoderBuffer> SyntheticDemuxer::MakeAudioPacketLocked() {
  const int64_t index = next_audio_packet_++;
  const std::vector<uint8_t> payload =
      IndexPayload(static_cast<uint32_t>(index));
  auto buffer = DecoderBuffer::CopyFrom(payload.data(), payload.size(),
                                        DemuxerStreamType::kAudio, 1);
  buffer->set_timestamp(TimeOfAudioPacket(spec_, index));
  buffer->set_serial(serial_);
  // Every audio packet is a sync point: there is no inter-frame dependency to
  // resume from, which is also true of AAC and Opus.
  buffer->set_keyframe(true);
  ++packets_read_;
  return buffer;
}

SyntheticDemuxer::VideoStream::VideoStream(SyntheticDemuxer* owner)
    : owner_(owner), config_(MakeVideoConfig(owner->spec_)) {
  DCHECK(owner_);
}

void SyntheticDemuxer::set_tick_clock(const base::TickClock* clock) {
  std::scoped_lock scoped(lock_);
  tick_clock_ = clock;
  started_ticks_ = clock ? clock->NowTicks() : base::TimeTicks();
}

int64_t SyntheticDemuxer::VideoAvailableLocked() const {
  const int64_t total = spec_.frame_count();
  if (!spec_.paced || !tick_clock_) {
    return total;
  }
  const int64_t elapsed_us =
      (tick_clock_->NowTicks() - started_ticks_).InMicroseconds();
  const int64_t frames =
      elapsed_us * spec_.fps_num / (1000000LL * std::max(1, spec_.fps_den));
  return std::min<int64_t>(total, std::max<int64_t>(frames, 0));
}

int64_t SyntheticDemuxer::AudioAvailableLocked() const {
  const int64_t total = spec_.audio_packet_count();
  if (!spec_.paced || !tick_clock_) {
    return total;
  }
  const int64_t elapsed_us =
      (tick_clock_->NowTicks() - started_ticks_).InMicroseconds();
  const int64_t samples = elapsed_us * spec_.sample_rate / 1000000;
  const int64_t packets = samples / std::max(1, spec_.audio_frames_per_packet);
  return std::min<int64_t>(total, std::max<int64_t>(packets, 0));
}

void SyntheticDemuxer::VideoStream::Read(uint32_t count, ReadCB read_cb) {
  DecoderBufferVector out;
  {
    // Paced: hand out only what the clock says exists, and PARK if that is
    // nothing yet. Parking is what a real demuxer does at a live edge, and it
    // is why a paced read must not be answered with an empty vector: the
    // DemuxerStream contract is 1..count buffers, and an empty reply would be
    // read as end of stream.
    //
    // The lock is RELEASED while parked: the edge advances on the clock, not
    // on anything this lock protects, and holding it would deadlock the
    // teardown path against a read that is waiting for time to pass.
    if (owner_->spec_.paced && owner_->tick_clock_) {
      while (owner_->VideoAvailableLocked() <= owner_->next_frame_ &&
             owner_->next_frame_ < owner_->spec_.frame_count()) {
        owner_->lock_.unlock();
        parked_.TimedWait(base::Milliseconds(2));
        owner_->lock_.lock();
      }
    }
    while (out.size() < count &&
           owner_->next_frame_ < owner_->VideoAvailableLocked()) {
      out.push_back(owner_->MakeVideoPacketLocked());
    }
    if (out.empty()) {
      out.push_back(DecoderBuffer::CreateEOSBuffer());
    }
  }
  std::move(read_cb).Run(Status::kOk, std::move(out));
}

size_t SyntheticDemuxer::VideoStream::buffered_buffers() const {
  std::scoped_lock scoped(owner_->lock_);
  const int64_t remaining = owner_->spec_.frame_count() - owner_->next_frame_;
  return static_cast<size_t>(std::max<int64_t>(remaining, 0));
}

base::TimeDelta SyntheticDemuxer::VideoStream::buffered_duration() const {
  return owner_->spec_.frame_duration() *
         static_cast<int64_t>(buffered_buffers());
}

SyntheticDemuxer::AudioStream::AudioStream(SyntheticDemuxer* owner)
    : owner_(owner), config_(MakeAudioConfig(owner->spec_)) {
  DCHECK(owner_);
}

void SyntheticDemuxer::AudioStream::Read(uint32_t count, ReadCB read_cb) {
  DecoderBufferVector out;
  {
    std::unique_lock<std::mutex> guard(owner_->lock_);
    // Same contract as the video leg, and for the same reasons: a paced read
    // that has nothing yet parks rather than answering with an empty vector,
    // which DemuxerStream's 1..count rule would read as end of stream. The lock
    // is released while parked because the edge advances on the clock.
    if (owner_->spec_.paced && owner_->tick_clock_) {
      while (owner_->AudioAvailableLocked() <= owner_->next_audio_packet_ &&
             owner_->next_audio_packet_ < owner_->spec_.audio_packet_count()) {
        guard.unlock();
        parked_.TimedWait(base::Milliseconds(2));
        guard.lock();
      }
    }
    while (out.size() < count &&
           owner_->next_audio_packet_ < owner_->AudioAvailableLocked()) {
      out.push_back(owner_->MakeAudioPacketLocked());
    }
    if (out.empty()) {
      out.push_back(DecoderBuffer::CreateEOSBuffer());
    }
  }
  std::move(read_cb).Run(Status::kOk, std::move(out));
}

size_t SyntheticDemuxer::AudioStream::buffered_buffers() const {
  std::scoped_lock scoped(owner_->lock_);
  const int64_t remaining =
      owner_->spec_.audio_packet_count() - owner_->next_audio_packet_;
  return static_cast<size_t>(std::max<int64_t>(remaining, 0));
}

base::TimeDelta SyntheticDemuxer::AudioStream::buffered_duration() const {
  const int64_t frames = static_cast<int64_t>(buffered_buffers()) *
                         owner_->spec_.audio_frames_per_packet;
  return base::Microseconds(frames * 1000000LL / owner_->spec_.sample_rate);
}

}  // namespace avbase::media::test
