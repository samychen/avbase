// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/filters/decoder_stream.h` (BSD-3-Clause).
//
// Replaces ijkplayer's per-stream threads: ffplay runs video_thread() and
// audio_thread() as two near-identical ~200-line functions whose only real
// differences are the packet queue, the frame queue and the codec call. Those
// differences are exactly what a Traits type can carry, so one implementation
// serves both.

#ifndef IJKPP_MEDIA_FILTERS_DECODER_STREAM_H_
#define IJKPP_MEDIA_FILTERS_DECODER_STREAM_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_decoder.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_config.h"
#include "media/base/decoder_status.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_types.h"
#include "media/base/video_decoder.h"
#include "media/base/video_decoder_factory.h"
#include "media/base/video_frame.h"
#include "media/base/waiting.h"
#include "media/media_export.h"

namespace ijkpp::media {

// Reported when a DecoderStream has to abandon a decoder mid-stream. ijkplayer
// surfaced several of these as hard errors; here they are recoverable and
// observable (behaviour difference Δ12).
enum class DecoderStreamEvent {
  kNone = 0,
  // A factory's decoder failed to initialize and the next candidate was used.
  kDecoderFallbackOnInit,
  // A working decoder started failing and was replaced.
  kDecoderFallbackOnDecodeError,
  // Every candidate was exhausted.
  kNoDecoderAvailable,
};

IJKPP_MEDIA_EXPORT const char* GetDecoderStreamEventName(DecoderStreamEvent e);

struct IJKPP_MEDIA_EXPORT VideoDecoderStreamTraits {
  using DecoderType = VideoDecoder;
  using OutputType = VideoFrame;
  using ConfigType = VideoDecoderConfig;
  using FactoryType = VideoDecoderFactory;

  static constexpr DemuxerStreamType kStreamType = DemuxerStreamType::kVideo;
  static const char* name() { return "VideoDecoderStream"; }

  static std::unique_ptr<DecoderType> CreateDecoder(FactoryType* factory,
                                                    const ConfigType& config) {
    return factory->CreateVideoDecoder(config);
  }
  // Bridges the one real difference between the two decoder contracts: video
  // takes (low_delay, cdm), audio takes (has_pending_clear, serial).
  static void InitializeDecoder(DecoderType* decoder, const ConfigType& config,
                                int32_t serial,
                                typename DecoderType::InitCB init_cb,
                                const typename DecoderType::OutputCB& output_cb,
                                const WaitingCB& waiting_cb);
  static ConfigType ConfigFromStream(DemuxerStream* stream);
  // VideoFrame has no EOS marker: end of stream is signalled by feeding the
  // decoder an EOS DecoderBuffer and waiting for its DecodeCB.
  static bool IsEndOfStreamOutput(const base::scoped_refptr<VideoFrame>&) {
    return false;
  }
  // VideoDecoder names itself via name(); AudioDecoder via GetDisplayName().
  static std::string DecoderName(DecoderType* decoder) {
    return decoder ? std::string(decoder->name()) : std::string();
  }
};

struct IJKPP_MEDIA_EXPORT AudioDecoderStreamTraits {
  using DecoderType = AudioDecoder;
  using OutputType = AudioBuffer;
  using ConfigType = AudioDecoderConfig;
  using FactoryType = AudioDecoderFactory;

  static constexpr DemuxerStreamType kStreamType = DemuxerStreamType::kAudio;
  static const char* name() { return "AudioDecoderStream"; }

  static std::unique_ptr<DecoderType> CreateDecoder(FactoryType* factory,
                                                    const ConfigType& config) {
    return factory->CreateAudioDecoder(config);
  }
  static void InitializeDecoder(DecoderType* decoder, const ConfigType& config,
                                int32_t serial,
                                typename DecoderType::InitCB init_cb,
                                const typename DecoderType::OutputCB& output_cb,
                                const WaitingCB& waiting_cb);
  static ConfigType ConfigFromStream(DemuxerStream* stream);
  // The audio contract signals EOS with a distinct AudioBuffer, so a stream of
  // decoded buffers has an explicit terminator.
  static bool IsEndOfStreamOutput(const base::scoped_refptr<AudioBuffer>& out) {
    return out && out->end_of_stream();
  }
  static std::string DecoderName(DecoderType* decoder) {
    return decoder ? decoder->GetDisplayName() : std::string();
  }
};

// Owns one decoder plus the queues on either side of it, and drives the
// demuxer -> decoder -> renderer loop.
//
// Why a class rather than two threads (ffplay's model):
//   * Selection and fallback become testable without a running pipeline: the
//     decoder is chosen by ranking, and a failure re-ranks without it.
//   * Back-pressure is explicit. ffplay's frame_queue_signal() blocks the decode
//     thread on a condition variable; here Read() simply is not called again
//     until the renderer wants more, so nothing blocks and nothing is dropped.
//
// Not thread-safe: every method must run on |task_runner_|.
template <typename Traits>
class IJKPP_MEDIA_EXPORT DecoderStream {
 public:
  using DecoderType = typename Traits::DecoderType;
  using OutputType = typename Traits::OutputType;
  using ConfigType = typename Traits::ConfigType;
  using FactoryType = typename Traits::FactoryType;
  using OutputRefPtr = base::scoped_refptr<OutputType>;

  using InitCB = base::OnceCallback<void(DecoderStatus)>;
  using ReadCB = base::OnceCallback<void(DecoderStatus, OutputRefPtr)>;
  using FlushCB = base::OnceClosure;
  using EventCB = base::RepeatingCallback<void(DecoderStreamEvent)>;

  // Watermarks, in output units. ffplay hardcodes these as
  // VIDEO_PICTURE_QUEUE_SIZE (16) and SAMPLE_QUEUE_SIZE (9). ijkpp keeps the
  // same relationship: stop asking the demuxer at |kDecodeWatermark|, start
  // again once the queue drops to |kPreloadWatermark|. Extracted rather than
  // retyped so the correspondence stays checkable.
  static constexpr size_t kDecodeWatermark = 16;
  static constexpr size_t kPreloadWatermark = 8;

  explicit DecoderStream(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner);
  ~DecoderStream();
  DecoderStream(const DecoderStream&) = delete;
  DecoderStream& operator=(const DecoderStream&) = delete;

  // |factories| must already be ranked (DecoderSelector::Select*). They are
  // tried in order until one initializes. |demuxer_stream| must outlive this
  // object.
  void Initialize(DemuxerStream* demuxer_stream, const ConfigType& config,
                  std::vector<base::scoped_refptr<FactoryType>> factories,
                  InitCB init_cb);
  void set_event_cb(EventCB event_cb) { event_cb_ = std::move(event_cb); }

  // Delivers one decoded output, or a status explaining why there is none.
  // Exactly one Read may be outstanding at a time.
  void Read(ReadCB read_cb);

  // Discards everything queued and everything the decoder holds, then runs
  // |closure|. |serial| is the demuxer's new serial, so buffers already in
  // flight can be recognized and dropped.
  void Flush(int32_t serial, FlushCB closure);

  size_t buffered_outputs() const { return decoded_outputs_.size(); }
  bool CanReadWithoutStalling() const;
  std::string GetDisplayName() const;

  // Counters for the inspect CLI and for tests.
  uint64_t decode_errors() const { return decode_errors_; }
  uint64_t fallbacks() const { return fallbacks_; }
  uint64_t outputs_decoded() const { return outputs_decoded_; }
  uint64_t stale_buffers_dropped() const { return stale_dropped_; }
  const DecoderStatus& init_status() const { return init_status_; }
  DemuxerStream* demuxer_stream() const { return demuxer_stream_; }

 private:
  void TryNextFactory(InitCB init_cb);
  void OnDecoderInitialized(InitCB init_cb, DecoderStatus status);
  void OnBufferReady(DemuxerStream::Status status,
                     DemuxerStream::DecoderBufferVector buffers);
  void ReadFromDemuxer();
  void DecodeNextBuffer();
  void OnDecodeDone(DecoderStatus status);
  void OnDecoderOutput(OutputRefPtr output);
  void MaybeDeliver();
  void Deliver(ReadCB read_cb, DecoderStatus status, OutputRefPtr output);
  void Notify(DecoderStreamEvent event);
  // Completes a Flush() once the decoder has discarded its buffered output.
  void OnResetDone(FlushCB closure);

  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  base::raw_ptr<DemuxerStream> demuxer_stream_ = nullptr;
  ConfigType config_;
  std::vector<base::scoped_refptr<FactoryType>> factories_;
  size_t next_factory_ = 0;
  std::unique_ptr<DecoderType> decoder_;
  EventCB event_cb_;

  std::deque<OutputRefPtr> decoded_outputs_;
  DemuxerStream::DecoderBufferVector pending_buffers_;
  size_t pending_buffer_index_ = 0;
  std::deque<ReadCB> pending_reads_;
  bool demuxer_read_in_flight_ = false;
  bool decode_in_flight_ = false;
  bool end_of_stream_ = false;
  bool flushing_ = false;
  int32_t serial_ = 0;

  DecoderStatus init_status_;
  uint64_t decode_errors_ = 0;
  uint64_t fallbacks_ = 0;
  uint64_t outputs_decoded_ = 0;
  uint64_t stale_dropped_ = 0;

  base::WeakPtrFactory<DecoderStream> weak_factory_{this};

  SEQUENCE_CHECKER(sequence_checker_);
};

// Explicit instantiations only: keeping the definitions in the .cc means a
// compile error in the template surfaces once, not in every translation unit.
extern template class IJKPP_MEDIA_EXPORT DecoderStream<VideoDecoderStreamTraits>;
extern template class IJKPP_MEDIA_EXPORT DecoderStream<AudioDecoderStreamTraits>;
using VideoDecoderStream = DecoderStream<VideoDecoderStreamTraits>;
using AudioDecoderStream = DecoderStream<AudioDecoderStreamTraits>;

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_DECODER_STREAM_H_
