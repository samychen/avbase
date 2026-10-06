// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Structure mirrors Chromium's `media/filters/audio_renderer_impl.h`
// (BSD-3-Clause). The threading does not, for a reason that is arithmetic
// rather than a matter of taste -- see "WHY THE DSP IS NOT IN THE CALLBACK"
// below.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// (milestone M7, docs/08 §2).
// Written in an environment with no compiler, so it has never been built or
// run. Excluded from every CMake target on purpose: a file that cannot compile
// must not be reachable from a build.
//
// ---------------------------------------------------------------------------
// WHY THE DSP IS NOT IN THE CALLBACK (a contradiction in the design docs)
// ---------------------------------------------------------------------------
// docs/04 §6.3 and §8 both say Render() calls
// `AudioRendererAlgorithm::FillBuffer()` inline, holding
// `AudioRendererAlgorithm::lock_` for "< 5 us". The same documents, plus
// media/base/audio_renderer_sink.h, docs/07 §4 and §10, and the M7 DoD in
// docs/08, all require the callback to finish in **< 100 us** (docs/07 §10
// marks `BM_AudioRenderCallback` with a ★ and docs/08 lists
// `RenderCallbackBudget p99 < 100us` as a gate).
//
// Both cannot hold. At 48 kHz stereo the WSOLA geometry from Chromium's own
// constants is ola_window = 960 frames, hop = 480, num_candidate_blocks = 1440,
// so search_block = 2399 frames and one GetOptimalBlock() scores 1440
// candidates x 960 frames x 2 channels x ~6 flops = ~16.6 Mflop. Filling a
// 1024-frame device buffer needs ~2.1 iterations, i.e. ~18 ms at a scalar
// 2 GFLOP/s -- about **177x the 100 us budget**, and the "5 us under lock"
// figure is smaller still.
//
// So this class pre-stretches. S4 (the `avbase-audio` sequence) owns the
// decoder stream and the algorithm, runs FillBuffer() there, and publishes
// finished chunks into a small ring. S7 (the device thread) only pops a
// chunk, copies
// it, scales it and advances the clock. That is: * within the 100 us budget
// with room to spare (the copy is ~2k floats); * consistent with Δ13, whose
// complaint about ffplay is precisely that `sdl_audio_callback` did
// `swr_convert` -- milliseconds of DSP -- on the device thread. Δ13's remedy
// column says "只做 FillBuffer + Scale", but FillBuffer IS the milliseconds;
// the intent (no DSP on the device thread) is met by "只做 copy + Scale", which
// is strictly stronger. * the lock is held for an O(1) ring index update, which
// is the only reading of "持锁 < 5 us" that can actually be satisfied. docs/04
// §6.3 and §8 need updating to say so. Recorded in docs/PROGRESS.md. The cost
// of pre-stretching is latency: audio sits in the ring for up to |kReadyChunks|
// * frames_per_buffer before reaching the device, which is why the ring is
// small (4 x 21 ms at 1024/48k = 85 ms) and why Flush() drops it. A live stream
// that cannot tolerate that should set config.net.live_max_latency, which is
// M9's concern, not this class's.
// ---------------------------------------------------------------------------
// GAPS -- close these before this file leaves DRAFT
// ---------------------------------------------------------------------------
// 1. Compile it, then benchmark `Render()`. The 100 us claim above is
// arithmetic, not measurement; `BM_AudioRenderCallback` (docs/07 §10) is what
// settles it. 2. Media time is tracked by counting frames from the first
// buffer's timestamp. Chromium uses AudioTimestampHelper, which stays exact
// across priming padding and rounding; avbase has no equivalent, so this drifts
// by up to a frame per buffer. Fine for the audio clock (AvSyncController
// re-anchors on every call), not fine for anything that reports position. 3.
// Preroll is not modelled. Chromium distinguishes "decoded enough to start"
// from "playing"; here StartPlayingFrom() begins pumping immediately and
// Initialize's callback fires when the decoder is ready. RendererImpl (M7) may
// need the distinction to satisfy OnBufferingStateChange(kHaveEnough). 4.
// Mid-stream config changes (sample rate or channel count) are detected and
// reported as kAudioRendererInitializationError rather than handled.
// DecoderStream already surfaces kConfigChanged; wiring it means
// re-Initialize()ing the algorithm and the sink, which is real work. 5. No
// SetChannelMask() passthrough, because AudioRendererAlgorithm does not have
// one either (its gap 5). 6. Depends on the DRAFT media/base/pipeline_status.h
// for PipelineStatus, so the two must leave DRAFT together.

#ifndef AVBASE_MEDIA_FILTERS_AUDIO_RENDERER_IMPL_H_
#define AVBASE_MEDIA_FILTERS_AUDIO_RENDERER_IMPL_H_

#include <stdint.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/demuxer_stream.h"
#include "media/base/pipeline_status.h"
#include "media/filters/audio_renderer_algorithm.h"
#include "media/filters/decoder_stream.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/audio_filter_stage.h"
#include "media/media_export.h"

namespace avbase::media {

// Turns decoded audio into samples at the device, and keeps the audio clock.
//
// WHAT IT REPLACES. ffplay's `audio_decode_frame()` plus
// `sdl_audio_callback()`: two functions that between them resample, walk the
// sample queue under a mutex, update `is->audio_clock` with no synchronisation
// at all, and do it on the device thread. Here the decode pump and the device
// callback are separate sequences with one bounded ring between them, and the
// clock goes through AvSyncController's seqlock (Δ14) instead of a bare
// double.
// THREADING. Two sequences touch this object: S4 `avbase-audio`  -- everything
// except Render(): Initialize, the decoder pump, FillBuffer, StartPlayingFrom,
// Flush, Stop. S7 device thread  -- Render() and OnRenderError() only.
// Cross-sequence state is exactly: the ready ring (under handoff_lock_), and
// the four std::atomics below. Everything else is S4-exclusive and needs no
// lock.
class AVBASE_MEDIA_EXPORT AudioRendererImpl final
    : public AudioRendererSink::RenderCallback {
 public:
  using InitializeCB = base::OnceCallback<void(PipelineStatus)>;

  // Number of pre-stretched chunks held between S4 and S7. Four is a trade:
  // enough to absorb a scheduling hiccup on S4, small enough that the added
  // latency (~85 ms at 1024 frames / 48 kHz) does not upset A/V sync or a
  // latency hint. See the note on pre-stretch latency above.
  static constexpr int kReadyChunks = 4;

  // |factories| must already be ranked by DecoderSelector. |av_sync| is
  // borrowed and must outlive this object; RendererImpl owns it (docs/03 §10.1
  // fixes the destruction order that makes that safe).
  AudioRendererImpl(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner,
      std::vector<base::scoped_refptr<AudioDecoderFactory>> factories,
      AvSyncController* av_sync);
  // config.audio.filter_graph ("af") plus the FFmpeg-side stage factory,
  // injected by the host: the media layer never names the FFmpeg
  // implementation (G2). An empty graph, or a graph without a factory, means
  // the decode output feeds the algorithm directly.
  void set_filter_graph(std::string graph, AudioFilterStageFactory factory) {
    filter_graph_ = std::move(graph);
    filter_factory_ = std::move(factory);
  }
  AudioRendererImpl(const AudioRendererImpl&) = delete;
  AudioRendererImpl& operator=(const AudioRendererImpl&) = delete;
  ~AudioRendererImpl() override;

  // Runs on S4. |sink| is adopted and started by StartPlayingFrom(), not here,
  // so that a failed decode never leaves a device open. |cb| runs on S4 and is
  // never run inline.
  void Initialize(DemuxerStream* stream, const AudioParameters& params,
                  base::scoped_refptr<AudioRendererSink> sink, InitializeCB cb);

  // Runs on S4. Begins decoding at |time| and starts the sink.
  void StartPlayingFrom(base::TimeDelta time);
  // Runs on S4. Drops the ring, the algorithm's queue and all clock state.
  void Flush(base::OnceClosure closure);
  // Runs on S4. Stops the sink first, so that after it returns Render() cannot
  // be called again (AudioRendererSink's own contract), and only then releases
  // the decoder.
  void Stop();
  // Runtime track-switch teardown: Stop() plus the guarantee that the
  // completion closure runs on S4 only after every self-posted task of this
  // object has drained -- the outstanding decoder read (whose reply task
  // would otherwise outlive the object) is completed with kDecodingAborted
  // through DecoderStream::Flush, and the pump cannot repost from Stop()'s
  // stopping_ flag. The closure is where the caller deletes the object.
  // Deliberately NOT AudioRendererImpl::Flush(): that would call
  // av_sync_->Flush() and disturb the live clock the replacement renderer
  // is about to drive.
  void StopAndDrainForTeardown(base::OnceClosure on_quiescent);

  // Runs on S4. Pausing gates the device path, not the decoder path: the
  // device callback returns 0 (the sink plays silence and consumes nothing),
  // so no frame is consumed, no clock update reaches AvSyncController, and the
  // audio clock freezes at its last anchor once RendererImpl zeroes its speed.
  // Decoding continues to fill the ring, which is what makes unpause instant
  // and bounds the post-pause startup latency to whatever was already decoded.
  // |ended_cb| fires once on the audio sequence when the decoder stream
  // reports end of stream (RendererImpl fires OnEnded from it).
  void SetPaused(bool paused);
  void set_ended_cb(base::RepeatingClosure cb) { ended_cb_ = std::move(cb); }

  // Callable from any thread; all four are atomics because the device thread
  // reads them inside Render() and must not take a lock to do it.
  void SetVolume(float volume);
  void SetMuted(bool muted);
  void SetPlaybackRate(double rate);
  void SetPreservesPitch(bool preserves_pitch);

  // Live-stream latency target, forwarded to the algorithm, which owns the
  // queue sizing it clamps. nullopt restores the default. This declaration
  // was missing from the first draft while RendererImpl already called it --
  // the mechanical review checked that every used *type* was visible, not
  // that every called *member* existed, which is a gap only a compiler
  // closes.
  // Current user-facing settings, so a renderer replacement (track switch)
  // can re-apply what the user had set on the outgoing instance. All four are
  // sequence-independent (atomics).
  float volume() const { return volume_.load(); }
  bool muted() const { return muted_.load(); }
  double playback_rate() const { return playback_rate_.load(); }
  bool preserves_pitch() const { return preserves_pitch_.load(); }

  void SetLatencyHint(std::optional<base::TimeDelta> latency_hint);

  // Media time of the last sample handed to the device. Read through
  // AvSyncController rather than from here when a caller is on another
  // sequence; this accessor is for RendererImpl on S4.
  base::TimeDelta GetMediaTime() const;

  bool initialized() const { return initialized_; }
  bool ended() const { return ended_; }
  // The device callback's own verdict: true from the first short/empty
  // period until it serves a full one. Event-driven (published at the
  // exact moment the state flips), so RendererImpl's starvation merge
  // never depends on sampling an oscillating queue depth.
  bool starved() const { return starved_.load(); }
  uint64_t underruns() const { return underruns_.load(); }
  uint64_t frames_rendered() const { return frames_rendered_.load(); }
  int buffered_frames() const;

  // ---- AudioRendererSink::RenderCallback, i.e. S7 -------------------------
  // Contract (media/base/audio_renderer_sink.h): must not block, allocate, take
  // any lock other than handoff_lock_, or call back into Player. Budget 100 us.
  int Render(base::TimeDelta delay, base::TimeTicks delay_timestamp,
             const AudioGlitchInfo& glitch_info, AudioBus* dest) override;
  void OnRenderError() override;

 private:
  // One pre-stretched chunk: |frames| of already time-stretched, not yet
  // volume-scaled audio, plus the media time of its first frame.
  struct ReadyChunk {
    std::unique_ptr<AudioBus> bus;
    int frames{0};
    base::TimeDelta media_time;
  };

  void OnDecoderInitialized(InitializeCB cb, DecoderStatus status);
  void PumpDecoder();
  void OnDecoderOutput(base::OnceClosure pump_again, DecoderStatus status,
                       base::scoped_refptr<AudioBuffer> buffer);
  void OnDecoderStreamEvent(DecoderStreamEvent event);
  // S4: runs FillBuffer() until the ring is full or the algorithm is dry.
  void PreStretch();
  // S4: publishes one chunk. Returns false when the ring is full.
  bool PublishChunk();
  // Media time consumed by |frames| of output at |rate|. Shared by the ring
  // half (audio_renderer_ring.cc) and Render(), which live in different
  // translation units since the file hit the C1 limit for the second time.
  static base::TimeDelta OutputFramesToMediaTime(int frames, double rate,
                                                 int sample_rate);
  // S7: copies at most |dest->frames()| frames out of the ring and posts a
  // consumer wake-up whenever back-pressure clears. Returns the number of
  // frames written; when that is non-zero, |first_media_micros| holds the
  // media time of the first frame handed over, which re-anchors the clock.
  int DrainRing(AudioBus* dest, int64_t* first_media_micros);

  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  std::vector<base::scoped_refptr<AudioDecoderFactory>> factories_;
  base::raw_ptr<AvSyncController> av_sync_;

  DecoderStream<AudioDecoderStreamTraits> decoder_stream_;
  AudioRendererAlgorithm algorithm_;
  // The "af" filter stage, created lazily on the first decoded buffer (the
  // graph needs the stream's real sample rate, which the decoder knows first).
  std::string filter_graph_;
  AudioFilterStageFactory filter_factory_;
  std::unique_ptr<AudioFilterStage> filter_;
  base::scoped_refptr<AudioRendererSink> sink_;

  AudioParameters params_;
  bool initialized_{false};
  bool started_{false};
  bool ended_{false};
  bool stopping_{false};
  int32_t serial_{0};
  // Atomic because Render() (S7) checks it; set from S4 via SetPaused().
  std::atomic<bool> paused_{false};
  base::RepeatingClosure ended_cb_;

  // ---- shared with S7; handoff_lock_ guards all four ----
  mutable base::Lock handoff_lock_;
  ReadyChunk ring_[kReadyChunks];
  int ring_head_{0};     // index of the chunk Render() reads next
  int ring_count_{0};    // chunks currently published
  int front_offset_{0};  // frames already consumed from ring_[ring_head_]

  // ---- shared with S7; atomic because Render() reads them lock-free ----
  std::atomic<float> volume_{1.0f};
  std::atomic<bool> muted_{false};
  std::atomic<double> playback_rate_{1.0};
  std::atomic<bool> preserves_pitch_{true};
  std::atomic<bool> render_error_{false};

  // Written by S7 inside Render(), read by S4 for statistics. Relaxed is
  // enough: these are counters, not ordering constraints.
  std::atomic<uint64_t> underruns_{0};
  // Same S7 write / any-thread read discipline. Paused does not count:
  // silence by policy is not starvation.
  std::atomic<bool> starved_{false};
  std::atomic<uint64_t> frames_rendered_{0};

  // True while a DecoderStream::Read is in flight. DecoderStream's contract is
  // "exactly one Read may be outstanding at a time", and violating it is not
  // recoverable, so the pump tracks it explicitly rather than inferring it from
  // the ring's fullness.
  bool read_outstanding_{false};
  // Invalidates the pump's self-posted tasks at destruction. The factory is
  // destroyed on S4 (where the dtor runs and where every task that uses the
  // weak pointers runs), so invalidation and the checks are serialized.
  base::WeakPtrFactory<AudioRendererImpl> weak_factory_{this};

  // S4-only: where the next pre-stretched chunk starts, in media time.
  base::TimeDelta next_chunk_media_time_;
  // Written by S7 inside Render(), read by GetMediaTime() on S4. Microseconds
  // rather than a TimeDelta because std::atomic<TimeDelta> is not something the
  // project relies on; the conversion happens at the two ends.
  mutable std::atomic<int64_t> last_media_time_micros_{0};
  // Scratch bus that FillBuffer() writes into before the chunk is published.
  std::unique_ptr<AudioBus> stretch_bus_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_AUDIO_RENDERER_IMPL_H_
