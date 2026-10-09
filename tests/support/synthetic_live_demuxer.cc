// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/synthetic_live_demuxer.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/default_tick_clock.h"
#include "base/time/time.h"
#include "media/base/media_error.h"

namespace avbase::media::test {
namespace {

// How often a parked read re-checks the edge. Short enough that a test
// fast-forwarding a mock clock is not dominated by wakeup granularity, long
// enough not to spin.
constexpr base::TimeDelta kParkSlice = base::Milliseconds(5);

VideoDecoderConfig MakeLiveVideoConfig(const SyntheticLiveSpec& spec) {
  VideoDecoderConfig config;
  config.codec = VideoCodec::kH264;
  config.codec_name = "synthetic-live-video";
  config.coded_size = Size{spec.width, spec.height};
  config.natural_size = config.coded_size;
  config.frame_rate = Rational{spec.fps_num, spec.fps_den};
  return config;
}

AudioDecoderConfig MakeLiveAudioConfig(const SyntheticLiveSpec& spec) {
  AudioDecoderConfig config;
  config.codec = AudioCodec::kAac;
  config.codec_name = "synthetic-live-audio";
  config.channel_layout =
      spec.channels == 1 ? ChannelLayout::kMono : ChannelLayout::kStereo;
  config.sample_format = SampleFormat::kF32P;
  config.sample_rate = spec.sample_rate;
  config.channels = spec.channels;
  return config;
}

TextDecoderConfig MakeLiveTextConfig() {
  TextDecoderConfig config;
  config.codec_name = "synthetic-live-text";
  return config;
}

}  // namespace

SyntheticLiveDemuxer::SyntheticLiveDemuxer(SyntheticLiveSpec spec,
                                           const base::TickClock* tick_clock)
    : spec_(std::move(spec)),
      tick_clock_(tick_clock ? tick_clock
                             : base::DefaultTickClock::GetInstance()) {
  media_info_.is_live = true;
  media_info_.duration = spec_.start_offset;
  media_info_.seekable = false;  // matches IsSeekable() below
  media_info_.format_name = "synthetic-live";
  media_info_.uri = "synthetic-live://test";
  // duration_is_estimate: the edge is a moving target, so any duration read
  // from here is a sample of "now", not a fact about the stream. Consumers that
  // treat duration as authoritative for a live stream are the bug this double
  // exists to catch.
  media_info_.duration_is_estimate = true;

  if (spec_.enable_video) {
    StreamInfo video;
    video.index = 0;
    video.kind = StreamKind::kVideo;
    video.codec_name = "synthetic-live-video";
    video.coded_size = Size{spec_.width, spec_.height};
    video.natural_size = video.coded_size;
    video.frame_rate = Rational{spec_.fps_num, spec_.fps_den};
    video.avg_frame_rate = video.frame_rate;
    media_info_.streams.push_back(video);
  }
  if (spec_.enable_audio) {
    StreamInfo audio;
    audio.index = 1;
    audio.kind = StreamKind::kAudio;
    audio.codec_name = "synthetic-live-audio";
    audio.sample_rate = spec_.sample_rate;
    audio.channels = spec_.channels;
    media_info_.streams.push_back(audio);
  }
  if (spec_.enable_text) {
    StreamInfo text;
    text.index = 2;
    text.kind = StreamKind::kText;
    text.codec_name = "synthetic-live-text";
    media_info_.streams.push_back(text);
  }
}

SyntheticLiveDemuxer::~SyntheticLiveDemuxer() {
  // A parked read holds a callback that names this object; Close() both wakes
  // it and flips closed_, so a double destroyed with a reader still parked
  // cannot leave that callback pointing at freed memory.
  Close();
}

// static
base::TickClock* ClockOf(const base::TickClock* c) {
  return const_cast<base::TickClock*>(c);
}

base::TimeDelta SyntheticLiveDemuxer::Edge() const {
  std::scoped_lock scoped(lock_);
  if (!started_) {
    return spec_.start_offset;
  }
  return spec_.start_offset + (tick_clock_->NowTicks() - started_ticks_);
}

bool SyntheticLiveDemuxer::closed() const {
  std::scoped_lock scoped(lock_);
  return closed_;
}

int64_t SyntheticLiveDemuxer::video_packets() const {
  std::scoped_lock scoped(lock_);
  return video_produced_;
}

int64_t SyntheticLiveDemuxer::audio_packets() const {
  std::scoped_lock scoped(lock_);
  return audio_produced_;
}

int64_t SyntheticLiveDemuxer::text_packets() const {
  std::scoped_lock scoped(lock_);
  return text_produced_;
}

int64_t SyntheticLiveDemuxer::park_timeouts() const {
  std::scoped_lock scoped(lock_);
  return park_timeouts_;
}

void SyntheticLiveDemuxer::Close() {
  base::WaitableEvent* woken = nullptr;
  {
    std::scoped_lock scoped(lock_);
    if (closed_) {
      return;
    }
    closed_ = true;
    woken = &edge_moved_;
  }
  // Signal outside the lock: the parked reader wakes and re-takes it.
  woken->Signal();
}

int64_t SyntheticLiveDemuxer::AvailableLocked(DemuxerStreamType type) const {
  const base::TimeDelta edge =
      started_ ? spec_.start_offset + (tick_clock_->NowTicks() - started_ticks_)
               : spec_.start_offset;
  if (type == DemuxerStreamType::kVideo) {
    if (edge <= base::TimeDelta()) {
      return 0;
    }
    const int64_t frames =
        edge.InMicroseconds() * spec_.fps_num / (spec_.fps_den * 1000000LL);
    return std::max<int64_t>(frames, 0);
  }
  if (type == DemuxerStreamType::kText) {
    // One subtitle packet per second. Sparse on purpose: a subtitle stream
    // that emits as fast as video would not model the thing the cue-expiry
    // policy is about, which is a cue arriving long after its moment.
    return std::max<int64_t>(edge.InSeconds(), 0);
  }
  if (edge <= base::TimeDelta()) {
    return 0;
  }
  const int64_t samples = edge.InMicroseconds() * spec_.sample_rate / 1000000;
  return std::max<int64_t>(samples / spec_.audio_frames_per_packet, 0);
}

int64_t SyntheticLiveDemuxer::ProducedLocked(DemuxerStreamType type) const {
  switch (type) {
  case DemuxerStreamType::kVideo:
    return video_produced_;
  case DemuxerStreamType::kAudio:
    return audio_produced_;
  case DemuxerStreamType::kText:
    return text_produced_;
  case DemuxerStreamType::kUnknown:
    break;
  }
  return 0;
}

void SyntheticLiveDemuxer::ProduceLocked(
    DemuxerStreamType type, int64_t wanted,
    std::vector<base::scoped_refptr<DecoderBuffer>>* out) {
  const int64_t available = AvailableLocked(type);
  int64_t produced = ProducedLocked(type);
  while (produced < available && static_cast<int64_t>(out->size()) < wanted) {
    std::vector<uint8_t> payload(kSyntheticIndexBytes, 0);
    const uint32_t index = static_cast<uint32_t>(produced);
    for (size_t i = 0; i < kSyntheticIndexBytes; ++i) {
      payload[i] = static_cast<uint8_t>((index >> (8 * i)) & 0xFF);
    }
    auto buffer = DecoderBuffer::CopyFrom(
        payload.data(), payload.size(), type,
        type == DemuxerStreamType::kVideo
            ? 0
            : (type == DemuxerStreamType::kAudio ? 1 : 2));
    // Timestamps come from the INDEX, exactly as in SyntheticDemuxer, so the
    // two doubles stamp identical times for identical indices. That is what
    // makes the edge move: the index is bounded by the clock, not by a
    // duration in the spec.
    if (type == DemuxerStreamType::kVideo) {
      buffer->set_timestamp(base::Microseconds(1000000LL * produced *
                                               spec_.fps_den / spec_.fps_num));
      buffer->set_keyframe(spec_.keyframe_interval > 0 &&
                           produced % spec_.keyframe_interval == 0);
      ++video_produced_;
    } else if (type == DemuxerStreamType::kAudio) {
      buffer->set_timestamp(base::Microseconds(1000000LL * produced *
                                               spec_.audio_frames_per_packet /
                                               spec_.sample_rate));
      buffer->set_keyframe(true);
      ++audio_produced_;
    } else {
      buffer->set_timestamp(base::Seconds(produced));
      buffer->set_keyframe(true);
      ++text_produced_;
    }
    buffer->set_serial(serial_);
    ++packets_read_;
    out->push_back(std::move(buffer));
    ++produced;
  }
}

void SyntheticLiveDemuxer::ReadLive(DemuxerStreamType type, uint32_t count,
                                    DemuxerStream::ReadCB read_cb) {
  const int64_t wanted = count > 0 ? count : 1;
  std::vector<base::scoped_refptr<DecoderBuffer>> out;
  bool timed_out = false;
  {
    std::unique_lock<std::mutex> guard(lock_);
    // The ceiling is measured on the REAL clock, deliberately. It exists to
    // stop a test hanging, and hang protection cannot itself depend on a clock
    // the test controls: a case that never advances its mock clock would
    // otherwise park forever, which is the exact failure the ceiling is for.
    // The EDGE still comes from tick_clock_, so a mock-clock test stays
    // deterministic -- only the safety net is real time.
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::microseconds(spec_.max_park.InMicroseconds());
    // Park until the edge has something new. DemuxerStream's contract is
    // 1..count buffers, so "not yet" cannot be an empty reply -- the wait is
    // the honest encoding, and it is what a real demuxer does in
    // av_read_frame. Close() and the ceiling both end it.
    while (!closed_ && AvailableLocked(type) <= ProducedLocked(type)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        ++park_timeouts_;
        timed_out = true;
        break;
      }
      edge_moved_.Reset();
      // Check once more under the lock before releasing it: Close() may have
      // fired between the predicate and here.
      if (closed_ || AvailableLocked(type) > ProducedLocked(type)) {
        break;
      }
      guard.unlock();
      edge_moved_.TimedWait(kParkSlice);
      guard.lock();
    }
    if (!timed_out) {
      ProduceLocked(type, wanted, &out);
    }
  }
  if (timed_out) {
    LOG(ERROR) << "avbase.demux: live read parked past its "
               << spec_.max_park.InMilliseconds() << "ms ceiling";
    std::move(read_cb).Run(DemuxerStream::Status::kError,
                           DemuxerStream::DecoderBufferVector());
    return;
  }
  if (out.empty()) {
    // Closed, or the edge has nothing more: the documented end marker.
    out.push_back(DecoderBuffer::CreateEOSBuffer());
  }
  std::move(read_cb).Run(DemuxerStream::Status::kOk, std::move(out));
}

void SyntheticLiveDemuxer::Initialize(
    const DataSourceDescriptor& source, const DemuxerOptions& options,
    Host* host,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    InitializeCB init_cb) {
  (void)source;
  (void)options;
  host_ = host;
  media_task_runner_ = std::move(media_task_runner);
  {
    std::scoped_lock scoped(lock_);
    started_ticks_ = tick_clock_->NowTicks();
    started_ = true;
  }
  if (spec_.enable_video) {
    video_ = std::make_unique<LiveStream>(this, DemuxerStreamType::kVideo);
  }
  if (spec_.enable_audio) {
    audio_ = std::make_unique<LiveStream>(this, DemuxerStreamType::kAudio);
  }
  if (spec_.enable_text) {
    text_ = std::make_unique<LiveStream>(this, DemuxerStreamType::kText);
  }
  if (!video_ && !audio_ && !text_) {
    std::move(init_cb).Run(base::unexpected(MediaError::Of(
        ErrorCode::kSourceOpenFailed, "no streams enabled",
        "the synthetic live source was built with neither video nor audio",
        "set enable_video or enable_audio in SyntheticLiveSpec")));
    return;
  }
  // Reported through the media sequence, like every other demuxer here: the
  // pipeline's sequence checker is right to reject anything else.
  if (host && media_task_runner) {
    media_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce([](InitializeCB cb) { std::move(cb).Run(OkStatus()); },
                       std::move(init_cb)));
    return;
  }
  std::move(init_cb).Run(OkStatus());
}

void SyntheticLiveDemuxer::StartPlayingFrom(base::TimeDelta time, SeekCB cb) {
  // Refused, and the refusal is the point. A live source has no data before
  // the edge, so "seek to t" has no correct behaviour; the pipeline's chase
  // path uses a non-physical skip precisely because a real seek is unavailable.
  LOG(INFO) << "avbase.demux: live stream cannot seek to " << time.ToString();
  // Reported as a failure, not as a silent no-op: a caller that seeked a live
  // stream has a bug, and the pipeline's chase path exists precisely so that it
  // does not have one. |actual| is the edge, which is the only position a live
  // stream can honestly claim to be at.
  std::move(cb).Run(
      base::unexpected(MediaError::Of(
          ErrorCode::kInvalidState, "a live stream cannot be seeked",
          "synthetic-live was asked to seek to " + time.ToString() +
              ", but it has no data before its live edge",
          "use the pipeline's live-edge chase "
          "(SetLatencyHint on a live source), not a physical "
          "seek")),
      Edge());
}

void SyntheticLiveDemuxer::Flush(base::OnceClosure flush_cb) {
  // A flush bumps the generation, which is what makes buffers already handed
  // out recognisable as stale. The edge does not move backwards.
  {
    std::scoped_lock scoped(lock_);
    ++serial_;
  }
  if (flush_cb) {
    std::move(flush_cb).Run();
  }
}

void SyntheticLiveDemuxer::Reset(base::OnceClosure reset_cb) {
  Close();
  {
    std::scoped_lock scoped(lock_);
    ++serial_;
  }
  if (reset_cb) {
    std::move(reset_cb).Run();
  }
}

void SyntheticLiveDemuxer::Stop() {
  Close();
}

const MediaInfo& SyntheticLiveDemuxer::media_info() const {
  // Recomputed per call, and this is the whole reason the double exists: a
  // consumer that samples the edge twice must see it move. duration is the
  // edge by definition for a live stream, so a pipeline that chases to
  // "duration" is chasing to here.
  std::scoped_lock scoped(lock_);
  media_info_.duration =
      started_ ? spec_.start_offset + (tick_clock_->NowTicks() - started_ticks_)
               : spec_.start_offset;
  return media_info_;
}

DemuxerStats SyntheticLiveDemuxer::GetStats() const {
  std::scoped_lock scoped(lock_);
  DemuxerStats stats;
  stats.packets_demuxed = static_cast<uint64_t>(packets_read_);
  // A live source is never seeked, so this stays zero -- which is itself a
  // useful assertion for a test: a non-zero value here means something seeked.
  stats.seek_count = 0;
  return stats;
}

// The single stream type both legs share. It holds no state of its own beyond
// its config: the edge, the counters and the parking all live in the demuxer,
// so there is exactly one implementation of "wait for the edge" to audit.
class SyntheticLiveDemuxer::LiveStream final : public DemuxerStream {
 public:
  LiveStream(SyntheticLiveDemuxer* owner, DemuxerStreamType type)
      : owner_(owner),
        type_(type),
        video_config_(type == DemuxerStreamType::kVideo
                          ? MakeLiveVideoConfig(owner->spec())
                          : VideoDecoderConfig()),
        audio_config_(type == DemuxerStreamType::kAudio
                          ? MakeLiveAudioConfig(owner->spec())
                          : AudioDecoderConfig()),
        text_config_(type == DemuxerStreamType::kText ? MakeLiveTextConfig()
                                                      : TextDecoderConfig()) {}

  void Read(uint32_t count, ReadCB read_cb) override {
    // Posted, never inline: DemuxerStream's contract. It also matters for the
    // park -- a read that waits for the live edge must not block the caller,
    // which in production is the demux loop and in a test is the pumping
    // thread.
    base::scoped_refptr<base::SequencedTaskRunner> runner =
        owner_->media_task_runner_;
    if (!runner) {
      owner_->ReadLive(type_, count, std::move(read_cb));
      return;
    }
    SyntheticLiveDemuxer* const owner = owner_;
    runner->PostTask(
        FROM_HERE,
        base::BindOnce([](SyntheticLiveDemuxer* o, DemuxerStreamType t,
                          uint32_t n,
                          ReadCB cb) { o->ReadLive(t, n, std::move(cb)); },
                       owner, type_, count, std::move(read_cb)));
  }

  const AudioDecoderConfig& audio_decoder_config() const override {
    return audio_config_;
  }
  const VideoDecoderConfig& video_decoder_config() const override {
    return video_config_;
  }
  const TextDecoderConfig& text_decoder_config() const override {
    return text_config_;
  }
  DemuxerStreamType type() const override { return type_; }
  int32_t stream_index() const override {
    switch (type_) {
    case DemuxerStreamType::kVideo:
      return 0;
    case DemuxerStreamType::kAudio:
      return 1;
    case DemuxerStreamType::kText:
      return 2;
    case DemuxerStreamType::kUnknown:
      break;
    }
    return -1;
  }
  bool SupportsConfigChanges() const override { return false; }
  int32_t serial() const override {
    std::scoped_lock scoped(owner_->lock_);
    return owner_->serial_;
  }
  size_t buffered_buffers() const override {
    std::scoped_lock scoped(owner_->lock_);
    return static_cast<size_t>(std::max<int64_t>(
        owner_->AvailableLocked(type_) - owner_->ProducedLocked(type_), 0));
  }
  size_t buffered_bytes() const override { return 0; }
  base::TimeDelta buffered_duration() const override {
    // Always zero, and that is the honest answer: in a live stream nothing is
    // buffered ahead, because there is no "ahead" to buffer. The latency hint
    // is what paces playback here, not a queue depth.
    return base::TimeDelta();
  }

 private:
  SyntheticLiveDemuxer* const owner_;
  const DemuxerStreamType type_;
  const VideoDecoderConfig video_config_;
  const AudioDecoderConfig audio_config_;
  const TextDecoderConfig text_config_;
};

// Defined here rather than beside its siblings because LiveStream must be a
// complete type before it can convert to DemuxerStream*.
DemuxerStream* SyntheticLiveDemuxer::GetStream(DemuxerStreamType type) {
  switch (type) {
  case DemuxerStreamType::kVideo:
    return video_.get();
  case DemuxerStreamType::kAudio:
    return audio_.get();
  case DemuxerStreamType::kText:
    return text_.get();
  case DemuxerStreamType::kUnknown:
    return nullptr;
  }
  return nullptr;
}

}  // namespace avbase::media::test
