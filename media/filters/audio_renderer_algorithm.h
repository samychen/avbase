// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// ALGORITHM PROVENANCE
// --------------------
// Structure, constants and the six-step iteration are taken from Chromium's
// `media/filters/audio_renderer_algorithm.{h,cc}` (BSD-3-Clause, Copyright
// 2012 The Chromium Authors), fetched from chromium.googlesource.com at
// refs/heads/main on 2026-09-29. Every constant below names the Chromium
// identifier it came from. The code is avbase's own: Chromium's version
// leans on AudioBufferQueue, AudioBus::CopyPartialFramesTo, base::HeapArray,
// MultiChannelResampler and cc::ScopedSubnormalFloatDisabler, none of which
// exist in avbase, so the queue and the window arithmetic are written directly
// against AudioBuffer::ReadFrames() and AudioBus::channel().
//
// ★ONE PROVENANCE GAP, stated rather than papered over: the similarity metric
// lives in Chromium's `media/filters/wsola_internals.cc`, which returned HTTP
// 503 when fetched, so Similarity() below is textbook normalised
// cross-correlation rather than a verified port of
// `internal::SimilarityFloat`. When a Chromium checkout is available, diff the
// two and record the outcome here (and in docs/PROGRESS.md) before this file
// leaves DRAFT. Everything else was read from source, not recalled.
//
// WHAT IT REPLACES. ijkplayer delegates variable-rate audio to SoundTouch, a
// third-party library that has to be compiled in and carries its own licence
// (behaviour difference Δ17). This class removes that dependency: WSOLA is a
// few hundred lines, needs no external code, and -- unlike naive resampling --
// keeps pitch constant while the rate changes, which is what
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// ` is supposed to sound like. STATUS: PROMOTED FROM DRAFT — NOT YET IN THE
// BUILD (milestone M7, docs/08 §2). Written in an environment with no
// compiler, so it has never been built. It is excluded from every CMake
// target on
// purpose: a file that cannot compile must not be reachable from a build (the
// convention docs/PROGRESS.md records from the fifth round). Gaps to close
// before it joins `avbase_media`: 1. Compile it. `-std=c++20 -fno-exceptions
// -fno-rtti -Werror`, plus the Google warning set; the DSP loops are the kind
// of code where a signed/ unsigned comparison or a narrowing conversion shows
// up immediately.
// 2. Verify Similarity() against internal::SimilarityFloat (see the gap above).
// 3. Run docs/07 §3.9's checklist, in particular the FFT pitch assertion --
// that is Δ17's acceptance test and the only thing that proves replacing
// SoundTouch did not change how 2x playback sounds. 4.
// FillBufferMode::kResampler is NOT implemented (avbase has no
// MultiChannelResampler yet), so SetPreservesPitch(false) currently logs once
// and keeps using WSOLA. That is a wrong-pitch-but-not-wrong-duration failure,
// and it must be either implemented or rejected loudly before M13. 5. No
// SetChannelMask() (Chromium uses it to shrink the search space). Purely an
// optimisation, but WSOLA is the expensive path in the audio thread and the p99
// < 100us budget in AudioRendererSinkContract is measured with it. 6.
// PeekFrames() copies through a scratch AudioBus because avbase's AudioBus has
// no CopyPartialFramesTo(src_offset, n, dest_offset, dest). Adding that one
// method to AudioBus would remove a copy per peek. 7. This file is one of
// three; the split mirrors Chromium's own, which keeps each half under the
// 500-line limit (invariant C1) and isolates the one function with a provenance
// gap: wsola_internals.{h,cc}      DSP primitives, incl. Similarity()
// audio_frame_queue.{h,cc}    Chromium's AudioBufferQueue equivalent
// audio_renderer_algorithm.*  buffering, index bookkeeping, FillBuffer() 8.
// avbase's AudioParameters has no bitstream flag, so is_bitstream_format_ is
// hardcoded false and AC3/EAC3 passthrough would be mangled by WSOLA. Adding
// the flag touches a frozen M3 header, so it needs the same review as the other
// cross-file items in docs/PROGRESS.md.

#ifndef AVBASE_MEDIA_FILTERS_AUDIO_RENDERER_ALGORITHM_H_
#define AVBASE_MEDIA_FILTERS_AUDIO_RENDERER_ALGORITHM_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/filters/audio_frame_queue.h"
#include "media/media_export.h"

namespace avbase::media {

// Buffers decoded audio and time-stretches it to match the playback rate.
//
// The owner (AudioRendererImpl, M7) calls EnqueueBuffer() on the audio sequence
// as buffers arrive from DecoderStream, and FillBuffer() from inside
// AudioRendererSink::RenderCallback::Render(). That split is why this class is
// NOT thread-safe and why the sink callback must do as little as possible:
// FillBuffer() is the only work Render() performs (Δ13), everything else stays
// on the audio sequence.
//
// Threading: single sequence. AudioRendererImpl serialises EnqueueBuffer() and
// FillBuffer() -- in practice FillBuffer() runs on the sink's audio thread
// while EnqueueBuffer() runs on the audio sequence, so AudioRendererImpl must
// own a lock or a lock-free handoff. That decision belongs to
// AudioRendererImpl, not here; SEQUENCE_CHECKER cannot express it, so it is a
// documented contract.
class AVBASE_MEDIA_EXPORT AudioRendererAlgorithm {
 public:
  // Which path FillBuffer() took, exposed because the three modes have very
  // different cost and quality, and "why does 1.0x sound different from 1.01x"
  // is otherwise unanswerable from outside.
  enum class FillBufferMode {
    kPassthrough,   // rate ~= 1.0: a single copy, no processing.
    kResampler,     // pitch need not be preserved: resample (NOT IMPLEMENTED).
    kWsola,         // pitch preserved: the overlap-and-add path.
  };

  AudioRendererAlgorithm();
  AudioRendererAlgorithm(const AudioRendererAlgorithm&) = delete;
  AudioRendererAlgorithm& operator=(const AudioRendererAlgorithm&) = delete;
  ~AudioRendererAlgorithm();

  // Must be called before anything else, and again after a config change.
  // |params| supplies channels, sample_rate and frames_per_buffer; the last one
  // sets the minimum playback threshold, below which the queue is never
  // considered adequate (Chromium: min_playback_threshold_).
  void Initialize(const AudioParameters& params);

  // Takes ownership of a decoded buffer. An end-of-stream marker is rejected:
  // EOS is a property of the stream, not of the audio, and
  // AudioRendererImpl reports it through RendererClient::OnEnded().
  void EnqueueBuffer(base::scoped_refptr<AudioBuffer> buffer);

  // Fills up to |requested_frames| of |dest| starting at |dest_offset|,
  // stretched to |playback_rate|. Returns the frames actually written; a short
  // count means the queue ran dry, which the caller reports as an underrun
  // rather than papering over with silence (AudioGlitchInfo::total_glitches).
  // |playback_rate| must be > 0; 0 returns 0 frames.
  int FillBuffer(AudioBus* dest, int dest_offset, int requested_frames,
                 double playback_rate);

  // Drops every buffered frame and all WSOLA state. After a seek the queue
  // holds frames from the previous serial, and feeding them through the
  // overlap-add chain would smear pre-seek audio into the first post-seek
  // window -- audible as a click or a short run of the wrong pitch.
  void FlushBuffers();

  // When true (the default) a rate other than 1.0 keeps pitch and changes
  // duration; when false it should resample, which changes both. See gap 4.
  void SetPreservesPitch(bool preserves_pitch);
  bool preserves_pitch() const { return preserves_pitch_; }

  // Scales output without touching the queue, so muting is instant and
  // reversible without a flush.
  void SetVolume(float volume);
  void SetMuted(bool muted);

  // Live-stream latency target. Clamped to [min_playback_threshold_,
  // max_capacity_]; nullopt restores the default. config.net.live_max_latency.
  void SetLatencyHint(std::optional<base::TimeDelta> latency_hint);

  // Signals that no more buffers will arrive, so the last partial window may be
  // played out instead of being held back waiting for a search block that will
  // never be complete.
  void MarkEndOfStream();

  // ---- Queue state, for BufferController (M9) and PlaybackStats ------------
  int buffered_frames() const;
  base::TimeDelta buffered_duration() const;
  bool is_queue_empty() const { return buffered_frames() == 0; }
  bool IsQueueAdequateForPlayback() const;
  bool IsQueueFull() const;
  // Doubles the playback threshold after an underrun, up to max_capacity_, so a
  // slow source stops oscillating between stall and play. Must not be called
  // when a latency hint is set -- the hint is the caller's explicit choice.
  void IncreasePlaybackThreshold();
  int queue_capacity() const { return capacity_; }
  int queue_playback_threshold() const { return playback_threshold_; }

  // Frames still buffered, converted at |playback_rate|: how much output the
  // queue can still produce. Drives AudioRendererImpl's buffering estimate.
  double DelayInFrames(double playback_rate) const;
  std::optional<base::TimeDelta> front_timestamp() const;

  // Rate actually achieved by the last FillBuffer(), which during WSOLA differs
  // from the requested rate for a single call because frames are consumed in
  // whole windows. Feeds av_diff diagnostics; a value that stays far from the
  // request means the queue is starving.
  double effective_playback_rate() const { return effective_playback_rate_; }
  FillBufferMode last_fill_mode() const { return last_mode_; }
  int samples_per_second() const { return samples_per_second_; }

 private:
  // WSOLA internals. Names match Chromium's so that a reader who knows one can
  // read the other: docs/02 §2 makes Chromium the tie-breaker for design
  // disputes, which only works if the identifiers line up.
  // The WSOLA branch of FillBuffer(). Split out because FillBuffer() also has
  // to handle bitstream passthrough and the volume/mute stage, and keeping all
  // three in one function would put it past the 80-line limit (invariant C2).
  // |initial_input_frames| is the queue depth on entry, needed afterwards to
  // compute effective_playback_rate_.
  int RunWsola(AudioBus* dest, int dest_offset, int requested_frames,
               int initial_input_frames, double playback_rate);
  void ApplyVolume(AudioBus* dest, int dest_offset, int frames);

  bool CanPerformWsola() const;
  bool RunOneWsolaIteration(double playback_rate);
  void GetOptimalBlock();
  void UpdateOutputTime(double playback_rate, double time_change);
  void RemoveOldInputFrames(double playback_rate);
  int WriteCompletedFramesTo(int requested_frames, int dest_offset,
                             AudioBus* dest);
  bool TargetIsWithinSearchRegion() const;
  // Width of the search block: num_candidate_blocks_ candidates, each of which
  // needs a whole ola_window_size_ block to fit, so the last candidate starts
  // at ola_window_size_ - 1 frames from the end. search_block_ is allocated
  // with this width; keeping the arithmetic in one place is what stops the
  // allocation and the bounds checks from drifting apart.
  int SearchBlockFrames() const;
  // How much of the search block is real audio rather than the zero-fill that
  // PeekAudioWithZeroPrepend() puts past the end of the queue. Equal to
  // SearchBlockFrames() except at end of stream, where the queue can be
  // narrower than one search block; 0 means not even one candidate window
  // fits, which is the caller's signal to stop iterating.
  int EffectiveSearchBlockFrames() const;
  void PeekAudioWithZeroPrepend(int read_offset_frames, AudioBus* dest);
  void AllocateWsolaBuffers();
  // The scratch bus is what AudioFrameQueue::PeekFrames() converts into before
  // the caller's offset is applied. It is needed by EVERY read path, not just
  // WSOLA's -- and the first draft created it only inside
  // AllocateWsolaBuffers(), so passthrough and bitstream reads silently
  // returned zero frames. Lazily grown because FillBuffer()'s requested_frames
  // is not known at Initialize() time.
  void EnsureScratch(int frames);
  FillBufferMode ChooseBufferMode(double playback_rate) const;
  void SetFillBufferMode(FillBufferMode mode);
  void ResetWsolaState();

  std::unique_ptr<AudioFrameQueue> queue_;

  int channels_{0};
  int samples_per_second_{0};
  bool is_bitstream_format_{false};
  bool preserves_pitch_{true};
  bool reached_end_of_stream_{false};
  bool warned_about_resampler_{false};
  float volume_{1.0f};
  bool muted_{false};

  // Window geometry, all derived from the two constants in the .cc.
  int ola_window_size_{0};
  int ola_hop_size_{0};
  int num_candidate_blocks_{0};
  int search_block_center_offset_{0};

  // Queue sizing.
  int min_playback_threshold_{0};
  int playback_threshold_{0};
  int initial_capacity_{0};
  int capacity_{0};
  int max_capacity_{0};
  std::optional<base::TimeDelta> latency_hint_;

  // WSOLA runtime state.
  double output_time_{0.0};
  int search_block_index_{0};
  int target_block_index_{0};
  int num_complete_frames_{0};
  double effective_playback_rate_{1.0};
  FillBufferMode last_mode_{FillBufferMode::kPassthrough};

  // Windows and scratch buffers, allocated on the first non-1.0 rate so that
  // straight-through playback never pays for them.
  std::vector<float> ola_window_;
  std::vector<float> transition_window_;
  std::unique_ptr<AudioBus> wsola_output_;
  std::unique_ptr<AudioBus> optimal_block_;
  std::unique_ptr<AudioBus> search_block_;
  std::unique_ptr<AudioBus> target_block_;
  std::unique_ptr<AudioBus> scratch_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_AUDIO_RENDERER_ALGORITHM_H_
