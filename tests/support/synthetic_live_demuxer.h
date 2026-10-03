// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A live source whose EDGE GROWS. SyntheticDemuxer (synthetic_demuxer.h) can
// only pretend to be live: it reports IsLive() true and treats its finite
// duration as the edge, which is enough to exercise the chase DECISION but not
// the thing that makes live playback different -- that the edge moves.
//
// What this double adds, and why a test cannot get it from a finite source:
//
//   * TIMESTAMPS TRACK THE WALL CLOCK. A packet read at wall time T carries
//     pts ~= T, not a counter's worth of fixed intervals. The finite double's
//     timestamps are a pure function of the packet index, so a consumer that
//     ran ahead of real time stayed ahead forever -- exactly the state a live
//     pipeline must never enter, and exactly what the chase logic corrects.
//   * THE EDGE ADVANCES. media_info().duration is recomputed from elapsed wall
//     time on every query, so two samples a moment apart differ. Everything
//     downstream (the chase policy, the latency hint) treats duration as the
//     edge.
//   * A READ AT THE EDGE PARKS. DemuxerStream's contract is 1..count buffers,
//     so "no data yet" is not expressible as an empty reply. This parks until
//     the edge moves, the way a real demuxer parks in av_read_frame. The wait
//     is bounded and Close() wakes it, because an unbounded park in a test is a
//     hang that reports nothing.
//
// WHAT IT IS NOT: a real container, codec, or network. The payload encoding is
// deliberately the same as SyntheticDemuxer's -- the index travels in the first
// four bytes, the tone is a function of the whole second of pts -- so a test
// can move between the two doubles without rewriting its assertions.

#ifndef AVBASE_TESTS_SUPPORT_SYNTHETIC_LIVE_DEMUXER_H_
#define AVBASE_TESTS_SUPPORT_SYNTHETIC_LIVE_DEMUXER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_info.h"
#include "tests/support/synthetic_demuxer.h"

namespace avbase::media::test {

struct SyntheticLiveSpec {
  int width = 320;
  int height = 240;
  int fps_num = 30;
  int fps_den = 1;
  int sample_rate = 48000;
  int channels = 2;
  int audio_frames_per_packet = 1024;
  // How far BEHIND the wall clock the stream starts. Zero means the first
  // packet is stamped "now"; a positive value starts the consumer behind the
  // edge, which is the state the chase exists for.
  base::TimeDelta start_offset = base::TimeDelta();
  int keyframe_interval = 30;
  bool enable_video = true;
  bool enable_audio = true;
  // Ceiling on one parked read. A live demuxer parks by design, so a test that
  // stops advancing the clock must fail rather than hang.
  base::TimeDelta max_park = base::Seconds(10);
};

// A Demuxer that produces media as fast as the clock runs, with no container.
class SyntheticLiveDemuxer final : public Demuxer {
 public:
  // |tick_clock| is what makes the edge move. Tests pass a
  // SimpleTestTickClock to make the whole double deterministic and instant;
  // null uses the real clock, which is what an end-to-end test wants.
  SyntheticLiveDemuxer(SyntheticLiveSpec spec,
                       const base::TickClock* tick_clock);
  ~SyntheticLiveDemuxer() override;

  const SyntheticLiveSpec& spec() const { return spec_; }

  // The current edge: |start_offset| plus the wall time since Initialize().
  // media_info().duration reports exactly this, recomputed per call.
  base::TimeDelta Edge() const;

  // Stops producing and wakes every parked read, which then reports EOS.
  // Sticky, like DataSource::Abort().
  void Close();
  bool closed() const;

  int64_t video_packets() const;
  int64_t audio_packets() const;
  // How many reads gave up waiting. Non-zero in a passing test means the test
  // asserted a condition the double could not satisfy.
  int64_t park_timeouts() const;

  // Demuxer.
  void Initialize(
      const DataSourceDescriptor& source, const DemuxerOptions& options,
      Host* host,
      base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
      InitializeCB init_cb) override;
  // A live stream cannot seek: there is no data before the edge, and going
  // backwards is the one thing a live demuxer must never do. Reports kAborted
  // with that reason instead of pretending to move a cursor.
  void StartPlayingFrom(base::TimeDelta time, SeekCB cb) override;
  void Flush(base::OnceClosure flush_cb) override;
  void Reset(base::OnceClosure reset_cb) override;
  void Stop() override;
  DemuxerStream* GetStream(DemuxerStreamType type) override;
  const MediaInfo& media_info() const override;
  base::TimeDelta GetStartTime() const override { return base::TimeDelta(); }
  bool IsLive() const override { return true; }
  // False, and load-bearing: the pipeline's seek path and the decoder factory
  // ordering both branch on this, and a live source claiming to be seekable
  // would let a caller issue a seek it cannot honour.
  bool IsSeekable() const override { return false; }
  DemuxerStats GetStats() const override;
  const char* name() const override { return "SyntheticLiveDemuxer"; }

 private:
  class LiveStream;

  // The read body, on the demuxer so the locking and parking live in one
  // place: both stream types need it and two copies would be two things to keep
  // honest. Runs on the demuxer's own sequence (the one Initialize() was
  // handed).
  void ReadLive(DemuxerStreamType type, uint32_t count,
                DemuxerStream::ReadCB read_cb);
  // The sequence Initialize() was handed. Read replies are posted there rather
  // than run inline, because DemuxerStream's contract says the callback is
  // never run inline -- a double that replies on the caller's thread would hide
  // every ordering bug the real demuxer cannot have.
  base::scoped_refptr<base::SequencedTaskRunner> media_task_runner_;

  // The number of packets of |type| that exist at the current edge, and the
  // one already handed out. Both require |lock_|.
  int64_t AvailableLocked(DemuxerStreamType type) const;
  int64_t ProducedLocked(DemuxerStreamType type) const;
  // Appends up to |wanted| more packets, or stops early at the edge. Requires
  // |lock_|.
  void ProduceLocked(DemuxerStreamType type, int64_t wanted,
                     std::vector<base::scoped_refptr<DecoderBuffer>>* out);

  const SyntheticLiveSpec spec_;
  const base::TickClock* const tick_clock_;
  // Mutable because media_info() is a const query that reports a moving value
  // -- recomputing it per call is the whole point, and caching it would
  // reintroduce the frozen edge this double exists to avoid.
  mutable MediaInfo media_info_;
  std::unique_ptr<LiveStream> video_;
  std::unique_ptr<LiveStream> audio_;
  Host* host_ = nullptr;
  // Guards every mutable member below. The pipeline reads both streams from
  // two decoder sequences at once (TSan caught exactly that in the sibling
  // double), and the parked reads release this while waiting on the clock.
  mutable std::mutex lock_;
  base::WaitableEvent edge_moved_{
      base::WaitableEvent::ResetPolicy::kManualReset,
      base::WaitableEvent::InitialState::kNotSignaled};
  base::TimeTicks started_ticks_;
  int64_t video_produced_ = 0;
  int64_t audio_produced_ = 0;
  int64_t park_timeouts_ = 0;
  int64_t packets_read_ = 0;
  int32_t serial_ = 0;
  bool started_ = false;
  bool closed_ = false;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_SYNTHETIC_LIVE_DEMUXER_H_
