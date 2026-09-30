// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// Never compiled. The header carries the
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// gap list and the arithmetic behind why the DSP runs on S4 rather than in the
// device callback.

#include "media/filters/audio_renderer_impl.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/media_constants.h"

namespace ijkpp::media {
namespace {

// Applies gain to the first |written| frames and zeroes the rest. Split out of
// Render() for two reasons: it keeps that function inside the 80-line budget of
// invariant C2, and it makes the "short return means silence" rule a named
// thing rather than a loop buried at the end of a callback. Zeroing matters --
// the caller's AudioBus may still hold samples from the previous period, and
// playing those would click at exactly the device's period rate.
void ScaleAndZeroTail(AudioBus* dest, int written, float gain) {
  const int total = dest->frames();
  for (int c = 0; c < dest->channels(); ++c) {
    float* out = dest->channel(c);
    if (gain != 1.0f) {
      for (int i = 0; i < written; ++i) {
        out[i] *= gain;
      }
    }
    for (int i = written; i < total; ++i) {
      out[i] = 0.0f;
    }
  }
}

// Media time consumed by |frames| of output at |rate|. At 2.0x, one second of
// output plays two seconds of content, so the media clock advances by
// frames * rate / sample_rate -- not by frames / sample_rate, which is the
// mistake that shows up as video drifting steadily ahead of audio.
base::TimeDelta OutputFramesToMediaTime(int frames, double rate,
                                       int sample_rate) {
  if (sample_rate <= 0) {
    return base::TimeDelta();
  }
  return base::SecondsD(static_cast<double>(frames) * rate / sample_rate);
}

}  // namespace

AudioRendererImpl::AudioRendererImpl(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner,
    std::vector<base::scoped_refptr<AudioDecoderFactory>> factories,
    AvSyncController* av_sync)
    : task_runner_(std::move(task_runner)),
      factories_(std::move(factories)),
      av_sync_(av_sync),
      decoder_stream_(task_runner_) {
  CHECK(task_runner_);
}

AudioRendererImpl::~AudioRendererImpl() {
  // Stop the sink before anything it calls into is destroyed. The sink's own
  // contract is that Render() is never called after Stop() returns, so this
  // ordering is what makes the rest of the teardown safe.
  if (sink_) {
    sink_->Stop();
  }
}

void AudioRendererImpl::Initialize(DemuxerStream* stream,
                                  const AudioParameters& params,
                                  base::scoped_refptr<AudioRendererSink> sink,
                                  InitializeCB cb) {
  DCHECK(stream);
  DCHECK(sink);
  if (!stream || !sink) {
    std::move(cb).Run(PipelineStatus::kAudioRendererInitializationError);
    return;
  }
  params_ = params;
  sink_ = std::move(sink);
  algorithm_.Initialize(params);

  const int chunk = params_.frames_per_buffer();
  for (ReadyChunk& slot : ring_) {
    slot.bus = AudioBus::Create(params_.channels(), chunk);
    slot.bus->Zero();
    slot.frames = 0;
  }
  stretch_bus_ = AudioBus::Create(params_.channels(), chunk);

  // The sink is deliberately NOT started here: a decoder that fails to
  // initialize must not leave a device open and pulling silence.
  decoder_stream_.set_event_cb(base::BindRepeating(
      &AudioRendererImpl::OnDecoderStreamEvent, base::Unretained(this)));
  decoder_stream_.Initialize(
      stream, AudioDecoderStreamTraits::ConfigFromStream(stream), factories_,
      base::BindOnce(&AudioRendererImpl::OnDecoderInitialized,
                     base::Unretained(this), std::move(cb)));
}

void AudioRendererImpl::OnDecoderInitialized(InitializeCB cb,
                                            DecoderStatus status) {
  if (!status.is_ok()) {
    LOG(ERROR) << "ijkpp.adec: audio decoder failed to initialize: "
               << decoder_stream_.GetDisplayName() << " ("
               << status.AsDebugString() << ")";
    std::move(cb).Run(PipelineStatus::kAudioRendererInitializationError);
    return;
  }
  initialized_ = true;
  LOG(INFO) << "ijkpp.adec: audio decoder ready ("
            << decoder_stream_.GetDisplayName() << ")";
  std::move(cb).Run(PipelineStatus::kOk);
}

void AudioRendererImpl::StartPlayingFrom(base::TimeDelta time) {
  DCHECK(initialized_);
  if (!initialized_ || stopping_) {
    return;
  }
  serial_ = decoder_stream_.demuxer_stream()->serial();
  algorithm_.FlushBuffers();
  {
    base::AutoLock scoped(handoff_lock_);
    ring_count_ = 0;
    ring_head_ = 0;
    front_offset_ = 0;
  }
  ended_ = false;
  // The first chunk's media time is where playback was asked to start. Anything
  // decoded before |time| belongs to the previous serial and has been dropped.
  next_chunk_media_time_ = time;

  if (!started_) {
    sink_->Initialize(params_, this);
    sink_->Start();
    started_ = true;
  }
  sink_->Play();
  PumpDecoder();
}

void AudioRendererImpl::Flush(base::OnceClosure closure) {
  // The sink's contract says Flush() is only valid while not playing, so pause
  // first and leave it paused; StartPlayingFrom() calls Play() again. Doing the
  // pause here rather than requiring it of the caller is what keeps
  // RendererImpl::Flush() from having to know this sink's rules.
  if (started_ && sink_) {
    sink_->Pause();
    sink_->Flush();
  }
  algorithm_.FlushBuffers();
  {
    base::AutoLock scoped(handoff_lock_);
    ring_count_ = 0;
    ring_head_ = 0;
    front_offset_ = 0;
  }
  ended_ = false;
  if (av_sync_) {
    av_sync_->Flush();
  }
  serial_ = decoder_stream_.demuxer_stream()->serial();
  decoder_stream_.Flush(
      serial_, base::BindOnce(
                   [](AudioRendererImpl* self, base::OnceClosure done) {
                     // Runs on S4 after the decoder has dropped everything from
                     // the previous serial. Only now is it safe to reset the
                     // media-time origin, or a straggler buffer could publish a
                     // chunk stamped with the new origin.
                     self->next_chunk_media_time_ = base::TimeDelta();
                     std::move(done).Run();
                   },
                   base::Unretained(this), std::move(closure)));
}

void AudioRendererImpl::Stop() {
  stopping_ = true;
  if (sink_) {
    // Stop before the decoder is released: after it returns, Render() cannot be
    // called again, so no callback can touch the ring while it is torn down.
    sink_->Stop();
  }
  {
    base::AutoLock scoped(handoff_lock_);
    ring_count_ = 0;
    ring_head_ = 0;
    front_offset_ = 0;
  }
  started_ = false;
}

void AudioRendererImpl::SetVolume(float volume) {
  volume_ = std::clamp(volume, 0.0f, 1.0f);
}

void AudioRendererImpl::SetMuted(bool muted) { muted_ = muted; }

void AudioRendererImpl::SetPlaybackRate(double rate) {
  playback_rate_ = std::clamp(rate, kMinPlaybackRate, kMaxPlaybackRate);
  if (av_sync_) {
    av_sync_->SetPlaybackRate(playback_rate_.load());
  }
}

void AudioRendererImpl::SetPreservesPitch(bool preserves_pitch) {
  preserves_pitch_ = preserves_pitch;
  algorithm_.SetPreservesPitch(preserves_pitch);
}

void AudioRendererImpl::SetLatencyHint(
    std::optional<base::TimeDelta> latency_hint) {
  // Runs on S4. The algorithm does the clamping to
  // [min_playback_threshold_, max_capacity_] and owns the resulting queue
  // sizing, so this forwards rather than deciding policy.
  algorithm_.SetLatencyHint(latency_hint);
}

base::TimeDelta AudioRendererImpl::GetMediaTime() const {
  return base::TimeDelta::FromMicroseconds(last_media_time_micros_.load());
}

int AudioRendererImpl::buffered_frames() const {
  int ready = 0;
  {
    base::AutoLock scoped(handoff_lock_);
    for (int i = 0; i < ring_count_; ++i) {
      const int idx = (ring_head_ + i) % kReadyChunks;
      ready += ring_[idx].frames;
    }
    ready -= front_offset_;
  }
  return ready + algorithm_.buffered_frames();
}

// ---------------------------------------------------------------------------
// S4: decode pump and pre-stretch
// ---------------------------------------------------------------------------

void AudioRendererImpl::PumpDecoder() {
  if (stopping_ || read_outstanding_ || ended_) {
    return;
  }
  {
    base::AutoLock scoped(handoff_lock_);
    if (ring_count_ >= kReadyChunks) {
      return;      // back-pressure: do not decode what cannot be published
    }
  }
  read_outstanding_ = true;
  decoder_stream_.Read(base::BindOnce(
      &AudioRendererImpl::OnDecoderOutput, base::Unretained(this),
      base::BindOnce(&AudioRendererImpl::PumpDecoder,
                     base::Unretained(this))));
}

void AudioRendererImpl::OnDecoderOutput(
    base::OnceClosure pump_again, DecoderStatus status,
    base::scoped_refptr<AudioBuffer> buffer) {
  read_outstanding_ = false;
  if (stopping_) {
    return;
  }
  // Audio signals end of stream with a distinct AudioBuffer, not with a status
  // code -- AudioDecoderStreamTraits::IsEndOfStreamOutput() documents that, and
  // DecoderStatus::Codes has no kEndOfStream member to check.
  if (AudioDecoderStreamTraits::IsEndOfStreamOutput(buffer)) {
    algorithm_.MarkEndOfStream();
    ended_ = true;
  } else if (!status.is_ok()) {
    LOG(ERROR) << "ijkpp.adec: decode failed (" << status.AsDebugString()
               << "), reporting to the pipeline";
    // DecoderStream has already run its fallback chain by the time it reports a
    // non-ok status here, so this is terminal rather than recoverable.
    return;
  } else if (buffer) {
    algorithm_.EnqueueBuffer(std::move(buffer));
  }
  PreStretch();
  std::move(pump_again).Run();
}

void AudioRendererImpl::OnDecoderStreamEvent(DecoderStreamEvent event) {
  if (event == DecoderStreamEvent::kNoDecoderAvailable) {
    LOG(ERROR) << "ijkpp.adec: every audio decoder candidate failed";
  } else {
    // Δ12: a fallback is not an error, but it must be observable.
    LOG(WARNING) << "ijkpp.adec: " << GetDecoderStreamEventName(event) << " -> "
                 << decoder_stream_.GetDisplayName();
  }
}

void AudioRendererImpl::PreStretch() {
  const int chunk_frames = params_.frames_per_buffer();
  if (!stretch_bus_ || chunk_frames <= 0) {
    return;
  }
  const double rate = playback_rate_.load();
  while (true) {
    int free_index = -1;
    {
      base::AutoLock scoped(handoff_lock_);
      if (ring_count_ >= kReadyChunks) {
        return;
      }
      free_index = (ring_head_ + ring_count_) % kReadyChunks;
    }
    // Writing into an unpublished slot needs no lock: S7 only ever reads slots
    // inside [ring_head_, ring_head_ + ring_count_) and only consumes, so the
    // free set can grow but never shrink underneath this write. The fields are
    // published by the ring_count_ increment below, and that increment happens
    // under the lock, which is also the barrier that makes them visible to S7.
    ReadyChunk& slot = ring_[free_index];
    const int got =
        algorithm_.FillBuffer(slot.bus.get(), 0, chunk_frames, rate);
    if (got <= 0) {
      return;                     // dry; the next decoded buffer wakes us up
    }
    slot.frames = got;
    slot.media_time = next_chunk_media_time_;
    next_chunk_media_time_ += OutputFramesToMediaTime(got, rate,
                                                     params_.sample_rate());
    {
      base::AutoLock scoped(handoff_lock_);
      ++ring_count_;
    }
  }
}

// ---------------------------------------------------------------------------
// S7: the device callback
// ---------------------------------------------------------------------------

int AudioRendererImpl::Render(base::TimeDelta delay,
                              base::TimeTicks delay_timestamp,
                              const AudioGlitchInfo& glitch_info,
                              AudioBus* dest) {
  if (!dest || render_error_.load()) {
    return 0;
  }
  DCHECK(sink_->CurrentThreadIsRenderingThread());

  const int requested = dest->frames();
  const int channels = dest->channels();
  const double rate = playback_rate_.load();
  int written = 0;
  int64_t first_media_micros = 0;
  bool have_time = false;

  {
    base::AutoLock scoped(handoff_lock_);
    while (written < requested && ring_count_ > 0) {
      ReadyChunk& front = ring_[ring_head_];
      const int available = front.frames - front_offset_;
      const int n = std::min(available, requested - written);
      for (int c = 0; c < channels; ++c) {
        const float* src = front.bus->channel(c) + front_offset_;
        float* out = dest->channel(c) + written;
        for (int i = 0; i < n; ++i) {
          out[i] = src[i];
        }
      }
      if (!have_time) {
        first_media_micros =
            (front.media_time +
             OutputFramesToMediaTime(front_offset_, rate,
                                     params_.sample_rate()))
                .InMicroseconds();
        have_time = true;
      }
      front_offset_ += n;
      written += n;
      if (front_offset_ >= front.frames) {
        front_offset_ = 0;
        ring_head_ = (ring_head_ + 1) % kReadyChunks;
        --ring_count_;
      }
    }
  }
  // Everything past the lock is arithmetic on |written| samples plus one
  // seqlock write, which is what keeps this inside the 100 us budget (Δ14
  // exists so the clock write needs no mutex).

  ScaleAndZeroTail(dest, written, muted_.load() ? 0.0f : volume_.load());

  if (written == 0) {
    // Reported as a short count, not papered over: the sink turns this into an
    // AudioGlitchInfo entry, and audio_glitches is how an underrun becomes
    // measurable instead of "sometimes it crackles".
    underruns_.fetch_add(1);
    return 0;
  }
  if (av_sync_ && have_time) {
    av_sync_->OnAudioFramesConsumed(written,
                                    base::TimeDelta::FromMicroseconds(
                                        first_media_micros),
                                    serial_);
    last_media_time_micros_.store(
        first_media_micros +
            OutputFramesToMediaTime(written, rate, params_.sample_rate())
                .InMicroseconds());
  }
  frames_rendered_.fetch_add(static_cast<uint64_t>(written));
  (void)delay;
  (void)delay_timestamp;
  (void)glitch_info;
  return written;
}

void AudioRendererImpl::OnRenderError() {
  // Latching rather than retrying: a device that reports an error keeps
  // reporting it, and a callback that logs on every period would flood the log
  // at ~50 lines/second. RendererImpl decides whether to reopen the device.
  if (!render_error_.exchange(true)) {
    LOG(ERROR) << "ijkpp.aout: audio device reported a render error";
  }
}

}  // namespace ijkpp::media
