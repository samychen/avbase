// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/decoder_stream.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/video_decoder_factory.h"

namespace ijkpp::media {

const char* GetDecoderStreamEventName(DecoderStreamEvent e) {
  switch (e) {
    case DecoderStreamEvent::kNone: return "none";
    case DecoderStreamEvent::kDecoderFallbackOnInit:
      return "decoder-fallback-on-init";
    case DecoderStreamEvent::kDecoderFallbackOnDecodeError:
      return "decoder-fallback-on-decode-error";
    case DecoderStreamEvent::kNoDecoderAvailable: return "no-decoder-available";
  }
  return "unknown";
}

// ---- traits ---------------------------------------------------------------

// static
void VideoDecoderStreamTraits::InitializeDecoder(
    DecoderType* decoder, const ConfigType& config, int32_t /*serial*/,
    typename DecoderType::InitCB init_cb,
    const typename DecoderType::OutputCB& output_cb,
    const WaitingCB& waiting_cb) {
  // VideoDecoder carries the serial on each DecoderBuffer instead, so it has no
  // use for it here; low_delay is false because a DecoderStream always wants the
  // decoder's full reorder buffer available.
  decoder->Initialize(config, /*low_delay=*/false, /*cdm_context=*/nullptr,
                      std::move(init_cb), output_cb, waiting_cb);
}

// static
VideoDecoderConfig VideoDecoderStreamTraits::ConfigFromStream(
    DemuxerStream* stream) {
  return stream ? stream->video_decoder_config() : VideoDecoderConfig();
}

// static
void AudioDecoderStreamTraits::InitializeDecoder(
    DecoderType* decoder, const ConfigType& config, int32_t serial,
    typename DecoderType::InitCB init_cb,
    const typename DecoderType::OutputCB& output_cb,
    const WaitingCB& waiting_cb) {
  decoder->Initialize(config, /*has_pending_clear=*/false, serial,
                      std::move(init_cb), output_cb, waiting_cb);
}

// static
AudioDecoderConfig AudioDecoderStreamTraits::ConfigFromStream(
    DemuxerStream* stream) {
  return stream ? stream->audio_decoder_config() : AudioDecoderConfig();
}

// ---- DecoderStream --------------------------------------------------------

namespace {

// How many buffers to request per demuxer read. ffplay reads one packet per
// loop iteration, which costs a virtual call per packet; batching amortizes it
// without changing when data becomes available.
constexpr uint32_t kBuffersPerRead = 8;

// Consecutive decode failures tolerated before the decoder is abandoned. One
// corrupt packet must not tear down a working decoder; a decoder that cannot
// make progress at all must be replaced.
constexpr uint64_t kMaxConsecutiveDecodeErrors = 20;

}  // namespace

template <typename Traits>
DecoderStream<Traits>::DecoderStream(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner)
    : task_runner_(std::move(task_runner)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

template <typename Traits>
DecoderStream<Traits>::~DecoderStream() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

template <typename Traits>
std::string DecoderStream<Traits>::GetDisplayName() const {
  std::string name = Traits::name();
  if (decoder_) {
    name += "(";
    name += Traits::DecoderName(decoder_.get());
    name += ")";
  }
  return name;
}

template <typename Traits>
bool DecoderStream<Traits>::CanReadWithoutStalling() const {
  return !decoded_outputs_.empty() || end_of_stream_ || decode_in_flight_ ||
         !pending_buffers_.empty();
}

template <typename Traits>
void DecoderStream<Traits>::Notify(DecoderStreamEvent event) {
  if (event_cb_) {
    event_cb_.Run(event);
  }
}

template <typename Traits>
void DecoderStream<Traits>::Initialize(
    DemuxerStream* demuxer_stream, const ConfigType& config,
    std::vector<base::scoped_refptr<FactoryType>> factories, InitCB init_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  demuxer_stream_ = demuxer_stream;
  config_ = config;
  factories_ = std::move(factories);
  next_factory_ = 0;
  TryNextFactory(std::move(init_cb));
}

template <typename Traits>
void DecoderStream<Traits>::TryNextFactory(InitCB init_cb) {
  if (next_factory_ >= factories_.size()) {
    Notify(DecoderStreamEvent::kNoDecoderAvailable);
    init_status_ = DecoderStatus(
        DecoderStatus::Codes::kUnknownError,
        std::string(Traits::name()) + ": all " +
            std::to_string(factories_.size()) + " decoder factories declined");
    LOG(WARNING) << "[decoder-stream] " << init_status_.description();
    std::move(init_cb).Run(init_status_);
    return;
  }
  base::scoped_refptr<FactoryType> factory = factories_[next_factory_++];
  std::unique_ptr<DecoderType> decoder =
      Traits::CreateDecoder(factory.get(), config_);
  if (!decoder) {
    // The factory looked at the config and declined. That is not an error, just
    // a "not me" -- move on.
    LOG(INFO) << "[decoder-stream] factory '" << factory->name()
              << "' declined the config";
    TryNextFactory(std::move(init_cb));
    return;
  }
  decoder_ = std::move(decoder);
  Traits::InitializeDecoder(
      decoder_.get(), config_, serial_,
      base::BindOnce(&DecoderStream::OnDecoderInitialized,
                     weak_factory_.GetWeakPtr(), std::move(init_cb)),
      base::BindRepeating(&DecoderStream::OnDecoderOutput,
                          weak_factory_.GetWeakPtr()),
      // Waiting is informational here: the DecoderStream's own back-pressure is
      // driven by Read(), not by decoder stalls.
      WaitingCB());
}

template <typename Traits>
void DecoderStream<Traits>::OnDecoderInitialized(InitCB init_cb,
                                                 DecoderStatus status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!status.is_ok()) {
    LOG(WARNING) << "[decoder-stream] " << GetDisplayName()
                 << " failed to initialize: " << status.AsDebugString();
    decoder_.reset();
    // A decoder we actually created and then lost is worth surfacing (Δ12). A
    // factory that returned nullptr is not: that is ordinary capability
    // negotiation, and reporting it would make kAuto look broken on every
    // machine without hardware decode.
    Notify(DecoderStreamEvent::kDecoderFallbackOnInit);
    ++fallbacks_;
    // Another candidate may still succeed, so keep going rather than failing.
    TryNextFactory(std::move(init_cb));
    return;
  }
  init_status_ = status;
  LOG(INFO) << "[decoder-stream] using " << GetDisplayName();
  std::move(init_cb).Run(status);
}

template <typename Traits>
void DecoderStream<Traits>::Read(ReadCB read_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_reads_.push_back(std::move(read_cb));
  MaybeDeliver();
  // Still unsatisfied: pull more input. ReadFromDemuxer() no-ops when a read is
  // already in flight or the watermark is reached, so this is cheap to call.
  if (!pending_reads_.empty() && !flushing_) {
    ReadFromDemuxer();
    DecodeNextBuffer();
  }
}

template <typename Traits>
void DecoderStream<Traits>::MaybeDeliver() {
  while (!pending_reads_.empty()) {
    if (!decoded_outputs_.empty()) {
      ReadCB read_cb = std::move(pending_reads_.front());
      pending_reads_.pop_front();
      OutputRefPtr output = std::move(decoded_outputs_.front());
      decoded_outputs_.pop_front();
      // Delivering is what frees watermark room, so resume decoding here.
      // DecodeNextBuffer() pulls from the demuxer itself when it runs out,
      // which keeps one authority for "how much work to have in flight".
      if (decoded_outputs_.size() < kDecodeWatermark && !end_of_stream_) {
        DecodeNextBuffer();
      }
      Deliver(std::move(read_cb), DecoderStatus(), std::move(output));
      continue;
    }
    // Nothing buffered. EOS is reported as kOk with a null output, matching
    // Chromium's DecoderStream: the caller distinguishes "no data yet" from
    // "no data ever" by whether a Read is still outstanding.
    const bool drained = end_of_stream_ && !decode_in_flight_ &&
                         pending_buffer_index_ >= pending_buffers_.size();
    if (drained) {
      ReadCB read_cb = std::move(pending_reads_.front());
      pending_reads_.pop_front();
      Deliver(std::move(read_cb), DecoderStatus(), nullptr);
      continue;
    }
    break;
  }
}

template <typename Traits>
void DecoderStream<Traits>::Deliver(ReadCB read_cb, DecoderStatus status,
                                    OutputRefPtr output) {
  std::move(read_cb).Run(std::move(status), std::move(output));
}

template <typename Traits>
void DecoderStream<Traits>::ReadFromDemuxer() {
  if (demuxer_read_in_flight_ || !demuxer_stream_ || end_of_stream_ ||
      flushing_) {
    return;
  }
  if (decoded_outputs_.size() >= kDecodeWatermark) {
    return;
  }
  demuxer_read_in_flight_ = true;
  // DemuxerStream::Read's contract delivers the reply on the demuxer's media
  // sequence (S1), but this object and its sequence checker live on the
  // owning renderer's sequence (S3/S4). The first draft bound OnBufferReady
  // directly, which the checker caught as a violation -- and it was not just
  // a formality: pending_reads_/decoded_outputs_ would have been mutated
  // concurrently. Hop the reply onto our own runner before touching state.
  demuxer_stream_->Read(
      kBuffersPerRead,
      base::BindOnce(
          [](base::scoped_refptr<base::SequencedTaskRunner> runner,
             base::OnceCallback<void(DemuxerStream::Status,
                                     DemuxerStream::DecoderBufferVector)> cb,
             DemuxerStream::Status status,
             DemuxerStream::DecoderBufferVector buffers) {
            runner->PostTask(FROM_HERE, base::BindOnce(std::move(cb), status,
                                                       std::move(buffers)));
          },
          task_runner_,
          base::BindOnce(&DecoderStream::OnBufferReady,
                         weak_factory_.GetWeakPtr())));
}

template <typename Traits>
void DecoderStream<Traits>::OnBufferReady(
    DemuxerStream::Status status,
    DemuxerStream::DecoderBufferVector buffers) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  demuxer_read_in_flight_ = false;
  if (flushing_) {
    // These buffers predate the flush; dropping them is the whole point of the
    // serial (see the comment on Flush()).
    stale_dropped_ += buffers.size();
    MaybeDeliver();
    return;
  }
  if (status != DemuxerStream::Status::kOk) {
    if (status == DemuxerStream::Status::kAborted) {
      return;  // A flush is in progress; Flush() owns the state now.
    }
    const char* name = DemuxerStream::GetStatusName(status);
    LOG(WARNING) << "[decoder-stream] demuxer read failed: " << name;
    end_of_stream_ = true;
    while (!pending_reads_.empty()) {
      ReadCB read_cb = std::move(pending_reads_.front());
      pending_reads_.pop_front();
      Deliver(std::move(read_cb),
              DecoderStatus(DecoderStatus::Codes::kDecodeError,
                            std::string("demuxer read: ") + name),
              nullptr);
    }
    return;
  }

  for (auto& buffer : buffers) {
    if (!buffer) {
      continue;
    }
    // A buffer from before the current seek generation is stale even though the
    // demuxer handed it over: it was already in flight when Flush() ran.
    if (!buffer->IsEndOfStream() && buffer->serial() != serial_) {
      ++stale_dropped_;
      continue;
    }
    if (buffer->IsEndOfStream()) {
      end_of_stream_ = true;
    }
    pending_buffers_.push_back(std::move(buffer));
  }
  DecodeNextBuffer();
  MaybeDeliver();
}

template <typename Traits>
void DecoderStream<Traits>::DecodeNextBuffer() {
  if (decode_in_flight_ || flushing_ || !decoder_) {
    return;
  }
  // This is the real back-pressure point: output accumulates here, so gating
  // only ReadFromDemuxer() would let an already-fetched batch decode past the
  // watermark. DecodeNextBuffer() is called again from MaybeDeliver() once the
  // consumer has drained enough, so stopping here cannot stall the pipeline.
  if (decoded_outputs_.size() >= kDecodeWatermark) {
    return;
  }
  // The watermark belongs in the loop condition, not only at function entry: a
  // decoder that completes inline (every FFmpeg decoder here does) returns from
  // Decode() with its output already queued, so an entry-only check lets the
  // whole fetched batch decode past the limit. Measured before this fix: 32
  // buffered outputs against a watermark of 16.
  while (pending_buffer_index_ < pending_buffers_.size() &&
         decoded_outputs_.size() < kDecodeWatermark) {
    base::scoped_refptr<DecoderBuffer> buffer =
        std::move(pending_buffers_[pending_buffer_index_++]);
    if (!buffer) {
      continue;
    }
    decode_in_flight_ = true;
    decoder_->Decode(std::move(buffer),
                     base::BindOnce(&DecoderStream::OnDecodeDone,
                                    weak_factory_.GetWeakPtr()));
    // The FFmpeg decoders complete inline, so decode_in_flight_ may already be
    // false here; DecodeNextBuffer() is re-entrant safe because of that flag.
    if (decode_in_flight_) {
      return;
    }
  }
  // Exhausted the batch: release it and ask for more.
  pending_buffers_.clear();
  pending_buffer_index_ = 0;
  if (!end_of_stream_) {
    ReadFromDemuxer();
  }
}

template <typename Traits>
void DecoderStream<Traits>::OnDecodeDone(DecoderStatus status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  decode_in_flight_ = false;
  if (flushing_) {
    return;
  }
  if (!status.is_ok()) {
    ++decode_errors_;
    LOG(WARNING) << "[decoder-stream] decode failed (" << decode_errors_
                 << " consecutive): " << status.AsDebugString();
    if (decode_errors_ >= kMaxConsecutiveDecodeErrors) {
      // The decoder is not making progress at all. Abandon it and let the
      // caller see a clean failure rather than an infinite stall.
      Notify(DecoderStreamEvent::kDecoderFallbackOnDecodeError);
      ++fallbacks_;
      end_of_stream_ = true;
      init_status_ = status;
      while (!pending_reads_.empty()) {
        ReadCB read_cb = std::move(pending_reads_.front());
        pending_reads_.pop_front();
        Deliver(std::move(read_cb), status, nullptr);
      }
      return;
    }
    DecodeNextBuffer();
    MaybeDeliver();
    return;
  }
  decode_errors_ = 0;
  DecodeNextBuffer();
  MaybeDeliver();
}

template <typename Traits>
void DecoderStream<Traits>::OnDecoderOutput(OutputRefPtr output) {
  if (!output || flushing_) {
    // A null output is the decoders' "this buffer produced nothing" signal
    // (EAGAIN); it is not an error and not a frame.
    return;
  }
  if (Traits::IsEndOfStreamOutput(output)) {
    end_of_stream_ = true;
    MaybeDeliver();
    return;
  }
  decoded_outputs_.push_back(std::move(output));
  ++outputs_decoded_;
  MaybeDeliver();
}

template <typename Traits>
void DecoderStream<Traits>::Flush(int32_t serial, FlushCB closure) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  flushing_ = true;
  serial_ = serial;
  decoded_outputs_.clear();
  pending_buffers_.clear();
  pending_buffer_index_ = 0;
  end_of_stream_ = false;
  decode_errors_ = 0;
  // Outstanding reads can never be satisfied by pre-flush data, so complete
  // them with kDecodingAborted; a caller waiting on Read() must not hang across
  // a seek.
  while (!pending_reads_.empty()) {
    ReadCB read_cb = std::move(pending_reads_.front());
    pending_reads_.pop_front();
    Deliver(std::move(read_cb),
            DecoderStatus(DecoderStatus::Codes::kDecodingAborted,
                          "flushed by seek"),
            nullptr);
  }
  if (decoder_) {
    decoder_->Reset(base::BindOnce(&DecoderStream::OnResetDone,
                                   weak_factory_.GetWeakPtr(),
                                   std::move(closure)));
    return;
  }
  flushing_ = false;
  std::move(closure).Run();
}

template <typename Traits>
void DecoderStream<Traits>::OnResetDone(FlushCB closure) {
  flushing_ = false;
  std::move(closure).Run();
}

// No IJKPP_MEDIA_EXPORT here: the attribute belongs on the `extern template`
// declarations in the header, where the type is still being introduced. Repeating
// it on an instantiation definition is ignored by GCC and rejected under
// -Werror=attributes.
template class DecoderStream<VideoDecoderStreamTraits>;
template class DecoderStream<AudioDecoderStreamTraits>;

}  // namespace ijkpp::media
