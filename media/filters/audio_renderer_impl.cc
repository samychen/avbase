// Copyright 2026 The avbase Authors. All rights reserved.
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
// Never compiled. The header carries the gap list and the arithmetic behind why
// the DSP runs on S4 rather than in the device callback.

#include "media/filters/audio_renderer_impl.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/media_constants.h"

namespace avbase::media {

// The device-facing callback (Render), its arithmetic helpers and the device
// error latch live in audio_renderer_impl_render.cc, so the S4 callback stays
// isolated from the decode-pump logic below.

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
    LOG(ERROR) << "avbase.adec: audio decoder failed to initialize: "
               << decoder_stream_.GetDisplayName() << " ("
               << status.AsDebugString() << ")";
    std::move(cb).Run(PipelineStatus::kAudioRendererInitializationError);
    return;
  }
  initialized_ = true;
  LOG(INFO) << "avbase.adec: audio decoder ready ("
            << decoder_stream_.GetDisplayName() << ")";
  std::move(cb).Run(PipelineStatus::kOk);
}

void AudioRendererImpl::StartPlayingFrom(base::TimeDelta time) {
  DCHECK(initialized_);
  if (!initialized_ || stopping_) {
    return;
  }
  // Same adoption as the video side (see the comment there): the flush that
  // came with the seek read the demuxer's serial while the demuxer was still
  // moving, so this is where the new generation is actually picked up.
  const int32_t stream_serial = decoder_stream_.demuxer_stream()->serial();
  if (serial_ != stream_serial) {
    serial_ = stream_serial;
    decoder_stream_.AdoptSerial(serial_);
  }
  decoder_stream_.HoldReads(false);
  algorithm_.FlushBuffers();
  // A seek invalidates the filter's buffered samples; rebuilding it is cheap
  // and simpler than a graph-level flush.
  filter_.reset();
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
  // A seek invalidates the filter's buffered samples; rebuilding it is cheap
  // and simpler than a graph-level flush.
  filter_.reset();
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
  // Same hold as the video side (see the comment there): reads issued before
  // StartPlayingFrom() adopts the new generation would burn it as stale.
  decoder_stream_.HoldReads(true);
  decoder_stream_.Flush(
      serial_, base::BindOnce(
                   [](AudioRendererImpl* self, base::OnceClosure done) {
                     // Runs on S4 after the decoder has dropped everything from
                     // the previous serial. Only now is it safe to reset the
                     // media-time origin, or a straggler buffer could publish a
                     // chunk stamped with the new origin.
                     self->next_chunk_media_time_ = base::TimeDelta();
                     if (done) {
                       std::move(done).Run();
                     }
                   },
                   base::Unretained(this), std::move(closure)));
}

void AudioRendererImpl::SetPaused(bool paused) {
  const bool was = paused_.exchange(paused);
  if (was == paused) {
    return;
  }
  if (!paused) {
    // Resume both halves of the pipeline: the pump may have stopped because
    // the algorithm was dry, and the ring may have drained through Render()
    // while the device was gated.
    PumpDecoder();
    PreStretch();
  }
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

void AudioRendererImpl::StopAndDrainForTeardown(
    base::OnceClosure on_quiescent) {
  Stop();
  // Complete the outstanding read (if any) with kDecodingAborted and run the
  // closure once the reply has been delivered: after this, no task that names
  // this object can be created, so the closure may delete it. Flush also
  // discards decoder state and the ring, which a dying object does not need;
  // the closure runs on this sequence (S4), where the delete is safe.
  const int32_t serial = decoder_stream_.demuxer_stream()->serial();
  // Null-tolerant for the same reason as the video side: a teardown entry
  // point should not CHECK when the caller only wanted the stop.
  decoder_stream_.Flush(serial, base::BindOnce(
                                    [](base::OnceClosure quiescent) {
                                      if (quiescent) {
                                        std::move(quiescent).Run();
                                      }
                                    },
                                    std::move(on_quiescent)));
}

void AudioRendererImpl::SetVolume(float volume) {
  volume_ = std::clamp(volume, 0.0f, 1.0f);
}

void AudioRendererImpl::SetMuted(bool muted) {
  muted_ = muted;
}

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
  if (stopping_ || read_outstanding_) {
    return;
  }
  if (ended_) {
    // End of stream: no more buffers arrive, but the algorithm can still hold
    // frames the ring never took. MarkEndOfStream() only flips the flag that
    // lets the last partial window play out; it does not move the frames. When
    // a decoded buffer is larger than the device period the ring can be full as
    // the EOS marker arrives, the pump then stops on back-pressure, and no
    // other caller runs PreStretch() again -- so the tail never published and
    // buffered_frames() never reached zero, which stopped
    // RendererImpl::CheckForEnded() from reporting OnEnded. DrainRing wakes
    // this path per drained chunk, which is where the tail gets out; see
    // RendererImplTest.EndedIsReportedAfterBothStreamsDrain.
    PreStretch();
    return;
  }
  {
    base::AutoLock scoped(handoff_lock_);
    if (ring_count_ >= kReadyChunks) {
      return;  // back-pressure: do not decode what cannot be published
    }
  }
  read_outstanding_ = true;
  // Weak-bound on purpose: the reply hop back into this object can be the
  // LAST task in a chain that outlives a teardown decision made on S1, and
  // the pump self-post that OnDecoderOutput schedules must not name a freed
  // object (the text-leg tests' teardown caught exactly that: a queued
  // pump task ran after the renderer was destroyed and locked its freed
  // handoff mutex). The weak factory lives on S4, same as these tasks.
  decoder_stream_.Read(base::BindOnce(
      &AudioRendererImpl::OnDecoderOutput, weak_factory_.GetWeakPtr(),
      base::BindOnce(&AudioRendererImpl::PumpDecoder,
                     weak_factory_.GetWeakPtr())));
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
    if (ended_cb_) {
      ended_cb_.Run();
    }
  } else if (!status.is_ok()) {
    if (status.code() == DecoderStatus::Codes::kDecodingAborted) {
      // A flush is in progress (the seek adopts the new generation with one);
      // the video side has the same tolerance for the same reason.
      return;
    }
    LOG(ERROR) << "avbase.adec: decode failed (" << status.AsDebugString()
               << "), reporting to the pipeline";
    // DecoderStream has already run its fallback chain by the time it reports a
    // non-ok status here, so this is terminal rather than recoverable.
    return;
  } else if (buffer) {
    if (filter_graph_.empty()) {
      algorithm_.EnqueueBuffer(std::move(buffer));
    } else {
      // The "af" stage: created on the first buffer, when the stream's real
      // sample rate is known. A filter that fails to start is a config bug --
      // fall back to the unfiltered path and say so once.
      if (!filter_) {
        if (!filter_factory_) {
          LOG(ERROR) << "avbase.afilter: filter_graph set but no stage "
                        "factory was injected (no-ffmpeg build?); playing "
                        "unfiltered";
          filter_graph_.clear();
          algorithm_.EnqueueBuffer(std::move(buffer));
          PreStretch();
          task_runner_->PostTask(FROM_HERE, std::move(pump_again));
          return;
        }
        filter_ = filter_factory_();
        if (!filter_->Initialize(filter_graph_, buffer->sample_rate(),
                                 buffer->channel_count(),
                                 params_.frames_per_buffer())) {
          LOG(ERROR) << "avbase.afilter: graph \"" << filter_graph_
                     << "\" failed to start; playing unfiltered";
          filter_.reset();
          filter_graph_.clear();
          algorithm_.EnqueueBuffer(std::move(buffer));
          PreStretch();
          task_runner_->PostTask(FROM_HERE, std::move(pump_again));
          return;
        }
      }
      std::vector<base::scoped_refptr<AudioBuffer>> filtered;
      if (!filter_->Process(std::move(buffer), &filtered)) {
        return;
      }
      for (auto& filtered_buffer : filtered) {
        if (!filtered_buffer->end_of_stream()) {
          algorithm_.EnqueueBuffer(std::move(filtered_buffer));
        }
      }
    }
  } else {
    // kOk with no buffer is DecoderStream's drained-stream signal; without
    // this branch a fully buffered stream pumped a null read forever and the
    // pipeline never learned the stream had ended.
    ended_ = true;
    if (ended_cb_) {
      ended_cb_.Run();
    }
  }
  PreStretch();
  // Posted, not run inline: DecoderStream delivers from its cache
  // synchronously, so running the pump inline recurses once per buffered
  // frame and overflowed the audio thread's stack on real files. One task hop
  // per buffer costs nothing measurable and makes the pump iterative.
  task_runner_->PostTask(FROM_HERE, std::move(pump_again));
}

void AudioRendererImpl::OnDecoderStreamEvent(DecoderStreamEvent event) {
  if (event == DecoderStreamEvent::kNoDecoderAvailable) {
    LOG(ERROR) << "avbase.adec: every audio decoder candidate failed";
  } else {
    // Δ12: a fallback is not an error, but it must be observable.
    LOG(WARNING) << "avbase.adec: " << GetDecoderStreamEventName(event)
                 << " -> " << decoder_stream_.GetDisplayName();
  }
}

}  // namespace avbase::media
