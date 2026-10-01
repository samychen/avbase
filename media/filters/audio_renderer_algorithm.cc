// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// (promoted from DRAFT, tenth round). Never compiled. The gap list, the
// algorithm's provenance and the one function in it whose provenance is
// incomplete are all documented in audio_renderer_algorithm.h; the DSP
// primitives this file calls live in wsola_internals.{h,cc}.
//
// The five constants below were read out of Chromium's
// media/filters/audio_renderer_algorithm.cc (fetched from
// chromium.googlesource.com at refs/heads/main, 2026-09-29) and each names the
// identifier it came from. They were not recalled from memory, because R1's
// mitigation 4 ("thresholds are extracted, never hand-copied") applies to any
// ported constant and not only to the A/V-sync ones -- and because the first
// version of tools/gen_options.py got a hand-copied FourCC wrong.
//
// LENGTH: this exceeds MAX_FILE_LINES (invariant C1, limit 500) and carries a
// C1 allowlist entry, currently 660, the way ffmpeg_demuxer.cc does. The first
// version of this note described that entry as still owed, from when the file
// was a DRAFT and drafts are exempt from the size rules; it was written before
// promotion and is corrected here rather than left to mislead the next reader.
// The alternative to an allowlist entry is a further split, and the natural
// seam is the queue-sizing block (SetLatencyHint /
// IsQueueAdequateForPlayback / IsQueueFull / IncreasePlaybackThreshold /
// capacity_ / playback_threshold_), which overlaps M9's BufferController
// three-tier high water mark. Whether that policy belongs here or there is a
// real design question, not a cosmetic one, and it should be settled at M9
// rather than by splitting a file to satisfy a line count.
//
// Do not "fix" the line count by deleting comments: the provenance and the
// reasoning are the point of this file's shape.

#include "media/filters/audio_renderer_algorithm.h"

#include <algorithm>
#include <cmath>

#include "base/check.h"
#include "base/logging.h"
#include "media/filters/wsola_internals.h"

namespace ijkpp::media {
namespace {

// Overlap-and-add window length. Chromium: kOlaWindowSize.
constexpr base::TimeDelta kOlaWindowSize = base::Milliseconds(20);

// Search interval around output_time * playback_rate; the searched region is
// [-delta, +delta], so its length is this value. Chromium:
// kWsolaSearchInterval.
constexpr base::TimeDelta kWsolaSearchInterval = base::Milliseconds(30);

// Queue sizing. Chromium: kStartingCapacity / kMaxCapacity.
constexpr base::TimeDelta kStartingCapacity = base::Milliseconds(200);
constexpr base::TimeDelta kMaxCapacity = base::Seconds(3);

// An interval around the previous optimal block that is excluded from the
// search. Chromium's comment calls the value "rather arbitrary and derived
// heuristically"; it exists to reduce a buzzy artefact, so it is not a knob to
// tune without an A/B listening test. Chromium: kExcludeIntervalLengthFrames.
constexpr int kExcludeIntervalLengthFrames = 160;

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AudioRendererAlgorithm::AudioRendererAlgorithm()
    : queue_(std::make_unique<AudioFrameQueue>()) {}

AudioRendererAlgorithm::~AudioRendererAlgorithm() = default;

void AudioRendererAlgorithm::Initialize(const AudioParameters& params) {
  CHECK(params.is_valid());
  channels_ = params.channels();
  samples_per_second_ = params.sample_rate();
  // ijkpp's AudioParameters has no bitstream flag yet; AC3/EAC3 passthrough
  // (where no post-processing is allowed at all) needs one. Until then this
  // stays false and passthrough formats go through WSOLA, which is wrong for
  // them. Recorded as gap 7 in the header.
  is_bitstream_format_ = false;

  min_playback_threshold_ = params.frames_per_buffer() * 2;
  initial_capacity_ = std::max(
      min_playback_threshold_,
      internal::TimeToFrames(kStartingCapacity, samples_per_second_));
  capacity_ = initial_capacity_;
  playback_threshold_ = initial_capacity_;
  max_capacity_ = std::max(
      initial_capacity_,
      internal::TimeToFrames(kMaxCapacity, samples_per_second_));

  num_candidate_blocks_ =
      internal::TimeToFrames(kWsolaSearchInterval, samples_per_second_);
  ola_window_size_ =
      internal::TimeToFrames(kOlaWindowSize, samples_per_second_);
  // The window must be even so that the hop is exactly half of it; an odd
  // window would make the overlap-add asymmetrical and the output amplitude
  // would ripple at the hop rate. Chromium does the same `+= size & 1`.
  ola_window_size_ += ola_window_size_ & 1;
  ola_hop_size_ = ola_window_size_ / 2;
  // Offset from the left edge of the search block to its centre, plus the
  // half-window that centres a candidate on a given index. Chromium's comment
  // derives this; the arithmetic is copied, not re-derived.
  search_block_center_offset_ =
      num_candidate_blocks_ / 2 + (ola_window_size_ / 2 - 1);

  queue_ = std::make_unique<AudioFrameQueue>();
  ResetWsolaState();
}

void AudioRendererAlgorithm::ResetWsolaState() {
  output_time_ = 0.0;
  search_block_index_ = 0;
  target_block_index_ = 0;
  num_complete_frames_ = 0;
  effective_playback_rate_ = 1.0;
  reached_end_of_stream_ = false;
  ola_window_.clear();
  transition_window_.clear();
  wsola_output_.reset();
  optimal_block_.reset();
  search_block_.reset();
  target_block_.reset();
  scratch_.reset();
}

void AudioRendererAlgorithm::EnqueueBuffer(
    base::scoped_refptr<AudioBuffer> buffer) {
  DCHECK(buffer);
  // An EOS marker carries no audio; letting it into the queue would make
  // frames() and the timestamp bookkeeping lie. AudioRendererImpl reports end
  // of stream through RendererClient::OnEnded() instead.
  DCHECK(!buffer->end_of_stream());
  if (!buffer || buffer->end_of_stream()) {
    return;
  }
  queue_->Append(std::move(buffer));
}

void AudioRendererAlgorithm::FlushBuffers() {
  queue_->Clear();
  ResetWsolaState();
  // Reset the growth triggered by earlier underruns so that a seek does not
  // inherit a queue sized for a bad network. Kept when a latency hint is set,
  // because then the size is the caller's explicit choice, not a reaction.
  if (!latency_hint_) {
    capacity_ = initial_capacity_;
    playback_threshold_ = initial_capacity_;
  }
}

void AudioRendererAlgorithm::SetPreservesPitch(bool preserves_pitch) {
  preserves_pitch_ = preserves_pitch;
}

void AudioRendererAlgorithm::SetVolume(float volume) { volume_ = volume; }

void AudioRendererAlgorithm::SetMuted(bool muted) { muted_ = muted; }

void AudioRendererAlgorithm::MarkEndOfStream() {
  reached_end_of_stream_ = true;
}

void AudioRendererAlgorithm::SetLatencyHint(
    std::optional<base::TimeDelta> latency_hint) {
  latency_hint_ = latency_hint;
  if (!latency_hint) {
    playback_threshold_ = initial_capacity_;
    capacity_ = initial_capacity_;
    return;
  }
  const int hint_frames =
      internal::TimeToFrames(*latency_hint, samples_per_second_);
  if (hint_frames > max_capacity_) {
    playback_threshold_ = max_capacity_;
  } else if (hint_frames < min_playback_threshold_) {
    playback_threshold_ = min_playback_threshold_;
  } else {
    playback_threshold_ = hint_frames;
  }
  capacity_ = std::max(playback_threshold_, initial_capacity_);
  DCHECK_GE(playback_threshold_, min_playback_threshold_);
  DCHECK_LE(playback_threshold_, capacity_);
  DCHECK_LE(capacity_, max_capacity_);
}

// ---------------------------------------------------------------------------
// Queue state
// ---------------------------------------------------------------------------

int AudioRendererAlgorithm::buffered_frames() const {
  return queue_ ? queue_->frames() : 0;
}

base::TimeDelta AudioRendererAlgorithm::buffered_duration() const {
  if (!queue_ || samples_per_second_ <= 0) {
    return base::TimeDelta();
  }
  return base::SecondsD(
      static_cast<double>(queue_->frames()) / samples_per_second_);
}

bool AudioRendererAlgorithm::IsQueueAdequateForPlayback() const {
  return buffered_frames() >= playback_threshold_;
}

bool AudioRendererAlgorithm::IsQueueFull() const {
  return buffered_frames() >= capacity_;
}

void AudioRendererAlgorithm::IncreasePlaybackThreshold() {
  DCHECK(!latency_hint_) << "do not override an explicit latency hint";
  if (latency_hint_ || capacity_ >= max_capacity_) {
    return;
  }
  capacity_ = std::min(2 * capacity_, max_capacity_);
  playback_threshold_ = capacity_;
}

double AudioRendererAlgorithm::DelayInFrames(double playback_rate) const {
  if (playback_rate <= 0.0) {
    return 0.0;
  }
  const int slower_step =
      static_cast<int>(std::ceil(ola_window_size_ * playback_rate));
  const int faster_step =
      static_cast<int>(std::ceil(ola_window_size_ / playback_rate));
  if (ola_window_size_ <= faster_step && slower_step >= ola_window_size_) {
    return buffered_frames();      // passthrough: output frames == input frames
  }
  const double buffered_output = buffered_frames() / playback_rate;
  return (buffered_output - output_time_) + num_complete_frames_;
}

std::optional<base::TimeDelta> AudioRendererAlgorithm::front_timestamp() const {
  return queue_ ? queue_->FrontTimestamp() : std::nullopt;
}

// ---------------------------------------------------------------------------
// FillBuffer
// ---------------------------------------------------------------------------

AudioRendererAlgorithm::FillBufferMode
AudioRendererAlgorithm::ChooseBufferMode(double playback_rate) const {
  if (!preserves_pitch_) {
    return FillBufferMode::kResampler;
  }
  // A rate so close to 1.0 that rounding to whole windows cannot tell the
  // difference goes through the copy path. Chromium's condition, verbatim.
  const int slower_step =
      static_cast<int>(std::ceil(ola_window_size_ * playback_rate));
  const int faster_step =
      static_cast<int>(std::ceil(ola_window_size_ / playback_rate));
  const bool almost_one =
      ola_window_size_ <= faster_step && slower_step >= ola_window_size_;
  return almost_one ? FillBufferMode::kPassthrough : FillBufferMode::kWsola;
}

void AudioRendererAlgorithm::SetFillBufferMode(FillBufferMode mode) {
  if (last_mode_ == mode) {
    return;
  }
  // Leaving WSOLA invalidates every index into the queue, so the state has to
  // go with it; otherwise the next entry starts from an output_time_ that no
  // longer corresponds to the front of the queue and the first window is taken
  // from the wrong place -- audible as a jump.
  if (last_mode_ == FillBufferMode::kWsola) {
    output_time_ = 0.0;
    search_block_index_ = 0;
    target_block_index_ = 0;
    num_complete_frames_ = 0;
    effective_playback_rate_ = 0.0;
    if (wsola_output_) {
      wsola_output_->Zero();
    }
  }
  last_mode_ = mode;
}

int AudioRendererAlgorithm::FillBuffer(AudioBus* dest, int dest_offset,
                                       int requested_frames,
                                       double playback_rate) {
  if (!dest || requested_frames <= 0 || playback_rate <= 0.0) {
    return 0;
  }
  DCHECK_EQ(channels_, dest->channels());
  if (channels_ != dest->channels()) {
    return 0;
  }

  const int initial_input_frames = buffered_frames();
  int rendered = 0;
  // Every path below reads through the queue, and the queue converts into this
  // bus. Creating it here rather than in AllocateWsolaBuffers() is the fix for
  // passthrough returning zero frames.
  EnsureScratch(requested_frames);

  if (is_bitstream_format_) {
    rendered = queue_->ReadFrames(requested_frames, dest_offset, dest,
                                  scratch_.get());
  } else {
    const FillBufferMode mode = ChooseBufferMode(playback_rate);
    SetFillBufferMode(mode);
    switch (mode) {
      case FillBufferMode::kPassthrough:
        rendered = queue_->ReadFrames(
            std::min(buffered_frames(), requested_frames), dest_offset, dest,
            scratch_.get());
        effective_playback_rate_ = 1.0;
        break;
      case FillBufferMode::kResampler:
        // Gap 4: ijkpp has no MultiChannelResampler. Falling through to WSOLA
        // keeps the duration right and the pitch wrong, which is the safer of
        // the two errors for A/V sync, and it is logged so the mistake cannot
        // be mistaken for correct behaviour.
        if (!warned_about_resampler_) {
          warned_about_resampler_ = true;
          LOG(WARNING) << "ijkpp.sync: preserves_pitch=false requested but no "
                          "resampler exists yet; using WSOLA, so pitch will be "
                          "preserved when it should not be";
        }
        rendered = RunWsola(dest, dest_offset, requested_frames,
                            initial_input_frames, playback_rate);
        break;
      case FillBufferMode::kWsola:
        rendered = RunWsola(dest, dest_offset, requested_frames,
                            initial_input_frames, playback_rate);
        break;
    }
  }

  ApplyVolume(dest, dest_offset, rendered);
  return rendered;
}

int AudioRendererAlgorithm::RunWsola(AudioBus* dest, int dest_offset,
                                     int requested_frames,
                                     int initial_input_frames,
                                     double playback_rate) {
  AllocateWsolaBuffers();
  if (!wsola_output_) {
    return 0;
  }
  int rendered = 0;
  do {
    rendered += WriteCompletedFramesTo(requested_frames - rendered,
                                       dest_offset + rendered, dest);
  } while (rendered < requested_frames && RunOneWsolaIteration(playback_rate));

  // At end of stream the queue eventually gets too narrow for even one
  // candidate window, and what is left is handed out unprocessed rather than
  // dropped. Chromium has no equivalent and simply loses the tail; ijkpp keeps
  // it, because a truncated ending is audible on every track while the last
  // few milliseconds at the wrong rate are not. EffectiveSearchBlockFrames()
  // bounds the raw remainder to under one ola_window_size_ -- before that, the
  // whole 2399-frame search block fell through here and the 2x duration test
  // overshot by two thirds of a window.
  if (reached_end_of_stream_ && rendered < requested_frames &&
      !CanPerformWsola()) {
    rendered += queue_->ReadFrames(requested_frames - rendered,
                                   dest_offset + rendered, dest,
                                   scratch_.get());
  }

  // The rate actually achieved by this call: input frames consumed per output
  // frame. It differs from the requested rate for a single call because frames
  // are consumed in whole windows, and a value that stays far from the request
  // means the queue is starving.
  if (rendered > 0) {
    effective_playback_rate_ =
        static_cast<double>(initial_input_frames - buffered_frames()) /
        rendered;
  } else {
    effective_playback_rate_ = playback_rate;
  }
  return rendered;
}

void AudioRendererAlgorithm::ApplyVolume(AudioBus* dest, int dest_offset,
                                         int frames) {
  if (frames <= 0) {
    return;
  }
  const float gain = muted_ ? 0.0f : volume_;
  if (gain == 1.0f) {
    return;
  }
  for (int c = 0; c < dest->channels(); ++c) {
    float* ch = dest->channel(c) + dest_offset;
    for (int n = 0; n < frames; ++n) {
      ch[n] *= gain;
    }
  }
}

void AudioRendererAlgorithm::EnsureScratch(int frames) {
  if (scratch_ && scratch_->frames() >= frames) {
    return;
  }
  scratch_ = AudioBus::Create(channels_, frames);
}

void AudioRendererAlgorithm::AllocateWsolaBuffers() {
  if (wsola_output_) {
    return;
  }
  ola_window_.assign(static_cast<size_t>(ola_window_size_), 0.0f);
  internal::FillPeriodicHanningWindow(&ola_window_);
  transition_window_.assign(static_cast<size_t>(ola_window_size_) * 2, 0.0f);
  internal::FillPeriodicHanningWindow(&transition_window_);

  wsola_output_ =
      AudioBus::Create(channels_, ola_window_size_ + ola_hop_size_);
  wsola_output_->Zero();
  optimal_block_ = AudioBus::Create(channels_, ola_window_size_);
  target_block_ = AudioBus::Create(channels_, ola_window_size_);
  const int search_frames = SearchBlockFrames();
  search_block_ = AudioBus::Create(channels_, search_frames);
  // PeekAudioWithZeroPrepend reads up to search_block_->frames() at a time, so
  // the scratch bus must be at least that large even if FillBuffer() asked for
  // fewer output frames.
  EnsureScratch(search_frames);
}

// ---------------------------------------------------------------------------
// WSOLA iteration
// ---------------------------------------------------------------------------

int AudioRendererAlgorithm::SearchBlockFrames() const {
  return num_candidate_blocks_ + (ola_window_size_ - 1);
}

int AudioRendererAlgorithm::EffectiveSearchBlockFrames() const {
  const int full = SearchBlockFrames();
  if (!reached_end_of_stream_) {
    return full;
  }
  // Subtracting a negative search_block_index_ is deliberate: that index means
  // PeekAudioWithZeroPrepend() prepends -search_block_index_ zeros, so the
  // block holds that many frames of content before the queue's first frame is
  // even reached, and content is what bounds the candidate range.
  const int logical = buffered_frames() - search_block_index_;
  if (logical < ola_window_size_) {
    return 0;
  }
  return std::min(full, logical);
}

bool AudioRendererAlgorithm::CanPerformWsola() const {
  // Shrinking the search block at end of stream, rather than giving up on it,
  // is what keeps the tail time-compressed at the requested rate. Without this
  // the last search_block_size_ frames can never complete a window, so they
  // fall through to the raw drain in RunWsola() -- which at 2x plays the end of
  // every track at 1x and makes the rendered duration overshoot by about half
  // a window. Measured on the 1 s / 2x case in
  // audio_renderer_algorithm_unittest.cc: 26066 frames rendered for 24000
  // expected, versus 25199 with this in place.
  const int search_block_size = EffectiveSearchBlockFrames();
  if (search_block_size == 0) {
    return false;
  }
  const int frames = buffered_frames();
  return target_block_index_ + ola_window_size_ <= frames &&
         search_block_index_ + search_block_size <= frames;
}

bool AudioRendererAlgorithm::TargetIsWithinSearchRegion() const {
  const int search_block_size = EffectiveSearchBlockFrames();
  if (search_block_size == 0) {
    return false;
  }
  return target_block_index_ >= search_block_index_ &&
         target_block_index_ + ola_window_size_ <=
             search_block_index_ + search_block_size;
}

bool AudioRendererAlgorithm::RunOneWsolaIteration(double playback_rate) {
  if (!CanPerformWsola()) {
    return false;
  }
  // Room check that Chromium does not need: its callers always request at least
  // a full device buffer, so num_complete_frames_ never grows past one hop.
  // ijkpp's FillBuffer() takes an arbitrary |requested_frames|, and a caller
  // asking for a handful of frames in a loop would let num_complete_frames_
  // grow without bound and write past the end of wsola_output_. Refusing the
  // iteration is correct behaviour -- the caller simply gets fewer frames this
  // time and drains on the next call.
  if (num_complete_frames_ + ola_window_size_ > wsola_output_->frames()) {
    return false;
  }

  GetOptimalBlock();

  const int hop = ola_hop_size_;
  for (int c = 0; c < channels_; ++c) {
    const float* opt = optimal_block_->channel(c);
    float* out = wsola_output_->channel(c) + num_complete_frames_;
    // Cross-fade the first half: the tail already in the output is weighted by
    // the second half of the window and the new block by the first half.
    // Because the window is periodic Hanning, the two weights sum to 1 at every
    // sample, so the join is amplitude-continuous.
    // Raw pointer: an int subscript on the vector trips the strict flag.
    const float* window = ola_window_.data();
    for (int n = 0; n < hop; ++n) {
      out[n] = out[n] * window[hop + n] + opt[n] * window[n];
    }
    // The second half has nothing to overlap with yet; it becomes the tail that
    // the next iteration cross-fades into.
    for (int n = hop; n < ola_window_size_; ++n) {
      out[n] = opt[n];
    }
  }

  num_complete_frames_ += hop;
  UpdateOutputTime(playback_rate, hop);
  RemoveOldInputFrames(playback_rate);
  return true;
}

void AudioRendererAlgorithm::UpdateOutputTime(double playback_rate,
                                              double time_change) {
  output_time_ += time_change;
  const int search_block_center_index =
      static_cast<int>(output_time_ * playback_rate + 0.5);
  search_block_index_ = search_block_center_index - search_block_center_offset_;
}

void AudioRendererAlgorithm::RemoveOldInputFrames(double playback_rate) {
  const int earliest_used_index =
      std::min(target_block_index_, search_block_index_);
  if (earliest_used_index <= 0) {
    return;
  }
  queue_->SeekFrames(earliest_used_index);
  target_block_index_ -= earliest_used_index;
  // Dropping input frames moves the origin of output_time_, so it has to be
  // shifted by the same amount expressed in output frames.
  const double output_time_change =
      static_cast<double>(earliest_used_index) / playback_rate;
  DCHECK_GE(output_time_, output_time_change);
  UpdateOutputTime(playback_rate, -output_time_change);
}

int AudioRendererAlgorithm::WriteCompletedFramesTo(int requested_frames,
                                                   int dest_offset,
                                                   AudioBus* dest) {
  const int rendered = std::min(num_complete_frames_, requested_frames);
  if (rendered <= 0) {
    return 0;
  }
  const int to_move = wsola_output_->frames() - rendered;
  for (int c = 0; c < channels_; ++c) {
    float* ch = wsola_output_->channel(c);
    float* out = dest->channel(c) + dest_offset;
    for (int n = 0; n < rendered; ++n) {
      out[n] = ch[n];
    }
    // Shift what is left down to the front so the next iteration's cross-fade
    // lands at index 0. Copying out first and shifting second is required: the
    // two ranges overlap.
    for (int n = 0; n < to_move; ++n) {
      ch[n] = ch[n + rendered];
    }
  }
  num_complete_frames_ -= rendered;
  return rendered;
}

void AudioRendererAlgorithm::GetOptimalBlock() {
  int optimal_index = 0;
  if (TargetIsWithinSearchRegion()) {
    // The natural continuation is already inside the search region, so it is by
    // definition the best match and the expensive search is skipped. This is
    // the common case at rates near 1.0.
    optimal_index = target_block_index_;
    PeekAudioWithZeroPrepend(optimal_index, optimal_block_.get());
  } else {
    PeekAudioWithZeroPrepend(target_block_index_, target_block_.get());
    PeekAudioWithZeroPrepend(search_block_index_, search_block_.get());
    const int last_optimal =
        target_block_index_ - ola_hop_size_ - search_block_index_;
    const int half_exclude = kExcludeIntervalLengthFrames / 2;
    optimal_index = internal::OptimalIndex(
        search_block_.get(), target_block_.get(),
        last_optimal - half_exclude, last_optimal + half_exclude,
        EffectiveSearchBlockFrames());
    optimal_index += search_block_index_;
    PeekAudioWithZeroPrepend(optimal_index, optimal_block_.get());

    // Blend the found block with the natural continuation. The optimal block is
    // the most similar to the target but can still be discontinuous where it is
    // spliced in; the target is guaranteed continuous but less similar. The
    // transition window is twice the block length so it acts as a weighting
    // that favours the target at the start of the block and the optimal block
    // at the end.
    // Hoisted for the same reason as |window| above: this is the hot path.
    const float* transition = transition_window_.data();
    for (int c = 0; c < channels_; ++c) {
      float* opt = optimal_block_->channel(c);
      const float* target = target_block_->channel(c);
      for (int n = 0; n < ola_window_size_; ++n) {
        opt[n] = opt[n] * transition[n] +
                 target[n] * transition[ola_window_size_ + n];
      }
    }
  }
  // The next target is one hop ahead of the block just chosen.
  target_block_index_ = optimal_index + ola_hop_size_;
}

void AudioRendererAlgorithm::PeekAudioWithZeroPrepend(int read_offset_frames,
                                                      AudioBus* dest) {
  const int num_frames = dest->frames();
  int write_offset = 0;
  int to_read = num_frames;
  if (read_offset_frames < 0) {
    // A negative index means the search region starts before the front of the
    // queue, which happens right after a flush or at a rate below 1.0. Zero
    // filling keeps the window the right length instead of shifting it.
    const int zeros = std::min(-read_offset_frames, num_frames);
    read_offset_frames = 0;
    to_read -= zeros;
    write_offset = zeros;
    for (int c = 0; c < dest->channels(); ++c) {
      float* ch = dest->channel(c);
      for (int n = 0; n < zeros; ++n) {
        ch[n] = 0.0f;
      }
    }
  }
  // Chromium CHECKs that the read fits inside the queue. A CHECK on the audio
  // thread takes the whole player down over a transient underrun, which for an
  // SDK is the wrong trade: DCHECK in debug so the bug is found, zero-fill in
  // release so playback survives it. PeekFrames already zero-fills the tail.
  //
  // End of stream is exempt, and has to be: EffectiveSearchBlockFrames() lets
  // WSOLA keep running on a search block wider than the queue, so the zero-fill
  // is then the intended content of the block's tail and OptimalIndex is told
  // how many frames are real. Without the exemption the first end-of-stream
  // iteration trips this in every debug build.
  DCHECK(reached_end_of_stream_ ||
         read_offset_frames + to_read <= buffered_frames());
  if (to_read > 0) {
    queue_->PeekFrames(to_read, read_offset_frames, write_offset, dest,
                       scratch_.get());
  }
}

}  // namespace ijkpp::media
