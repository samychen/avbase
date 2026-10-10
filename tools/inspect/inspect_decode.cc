// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// `avbase-inspect decode`: demux and decode, then report what actually
// came out.
//
// The interesting output is not the frame count -- it is the timestamp
// monotonicity check. A decoder that returns frames with non-monotonic pts is
// the signature of a missing time base (bug #26), and it is invisible in the
// frame count: the right number of frames arrives, all of them stamped zero.

#include <inttypes.h>
#include <stdio.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "media/base/audio_buffer.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_status.h"
#include "media/base/media_constants.h"
#include "media/base/media_log.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/ffmpeg_audio_decoder.h"
#include "media/ffmpeg/ffmpeg_video_decoder.h"
#include "tools/inspect/inspect_common.h"

namespace avbase {

namespace {

using media::AudioBuffer;
using media::AudioDecoderConfig;
using media::DecoderBuffer;
using media::DecoderStatus;
using media::DemuxerStream;
using media::DemuxerStreamType;
using media::VideoDecoderConfig;
using media::VideoFrame;

// Feeds |stream| through |decoder| until |limit| outputs arrive or EOS. Shared
// by the video and audio paths because both FFmpeg decoders now have the same
// shape: OutputCB at Initialize, Decode(buffer, DecodeCB).
//
// Returns false if the demuxer stopped answering, which would otherwise look
// like a clean end of stream.
template <typename Decoder, typename Output>
bool PumpDecoder(DemuxerStream* stream, Decoder* decoder, Pump* pump,
                 size_t limit, std::vector<base::scoped_refptr<Output>>* sink) {
  bool eos = false;
  for (size_t round = 0; round < 200000 && sink->size() < limit && !eos;
       ++round) {
    bool answered = false;
    DemuxerStream::DecoderBufferVector pending;
    stream->Read(8, base::BindOnce(
                        [](bool* flag, bool* eos_flag,
                           DemuxerStream::DecoderBufferVector* out,
                           DemuxerStream::Status,
                           DemuxerStream::DecoderBufferVector buffers) {
                          *flag = true;
                          for (const auto& b : buffers) {
                            if (b->IsEndOfStream()) {
                              *eos_flag = true;
                            }
                          }
                          *out = std::move(buffers);
                        },
                        &answered, &eos, &pending));
    if (!pump->Until([&answered] { return answered; })) {
      fprintf(stderr, "error: demuxer read timed out\n");
      return false;
    }
    for (auto& buffer : pending) {
      bool done = false;
      decoder->Decode(
          buffer, base::BindOnce(
                      [](bool* flag, DecoderStatus) { *flag = true; }, &done));
      pump->Until([&done] { return done; });
    }
  }
  return true;
}

// Counts how many timestamps went backwards. Non-monotonic pts means the
// renderer will treat frames as late and drop them, so this is reported as a
// hard failure rather than a curiosity.
template <typename Output, typename TimestampOf>
size_t CountNonMonotonic(const std::vector<base::scoped_refptr<Output>>& items,
                         TimestampOf ts_of, size_t* valid) {
  size_t non_monotonic = 0;
  *valid = 0;
  base::TimeDelta previous;
  for (size_t i = 0; i < items.size(); ++i) {
    const base::TimeDelta ts = ts_of(*items[i]);
    if (media::IsNoTimestamp(ts)) {
      continue;
    }
    if (*valid > 0 && ts < previous) {
      ++non_monotonic;
    }
    previous = ts;
    ++*valid;
  }
  return non_monotonic;
}

// Returns 0 on success, 2 when something looked wrong.
int DecodeVideo(DemuxerStream* stream, Pump* pump,
                base::scoped_refptr<base::SequencedTaskRunner> runner,
                size_t limit) {
  const VideoDecoderConfig config = stream->video_decoder_config();
  printf("\nvideo stream [%d] codec=%s\n", stream->stream_index(),
         config.codec_name.c_str());

  media::FFmpegVideoDecoder decoder(runner);
  std::vector<base::scoped_refptr<VideoFrame>> frames;
  DecoderStatus init_status;
  bool init_done = false;
  decoder.Initialize(config, /*low_delay=*/false, /*cdm=*/nullptr,
                     base::BindOnce(
                         [](bool* flag, DecoderStatus* out, DecoderStatus s) {
                           *flag = true;
                           *out = s;
                         },
                         &init_done, &init_status),
                     base::BindRepeating(
                         [](std::vector<base::scoped_refptr<VideoFrame>>* sink,
                            base::scoped_refptr<VideoFrame> frame) {
                           if (frame) {
                             sink->push_back(std::move(frame));
                           }
                         },
                         &frames),
                     media::WaitingCB());
  pump->Until([&init_done] { return init_done; });
  if (!init_status.is_ok()) {
    printf("  initialize: FAILED -- %s\n", init_status.AsDebugString().c_str());
    return 2;
  }

  const bool ok = PumpDecoder(stream, &decoder, pump, limit, &frames);
  bool flushed = false;
  decoder.Decode(DecoderBuffer::CreateEOSBuffer(),
                 base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                                &flushed));
  pump->Until([&flushed] { return flushed; });

  size_t valid = 0;
  const size_t non_monotonic = CountNonMonotonic(
      frames, [](const VideoFrame& f) { return f.timestamp(); }, &valid);
  printf("  initialize  : ok (%s)\n", decoder.name());
  printf("  frames      : %zu\n", frames.size());
  if (!frames.empty()) {
    printf("  size        : %dx%d format=%s\n",
           frames.front()->natural_size().width,
           frames.front()->natural_size().height,
           media::GetVideoFormatName(frames.front()->format()));
    printf("  first pts   : %.6f s\n",
           frames.front()->timestamp().InSecondsF());
    printf("  last pts    : %.6f s\n", frames.back()->timestamp().InSecondsF());
  }
  printf("  non-monotonic pts: %zu%s\n", non_monotonic,
         non_monotonic ? "   <-- timestamps are wrong" : "");
  return (!ok || non_monotonic > 0 || frames.empty()) ? 2 : 0;
}

int DecodeAudio(DemuxerStream* stream, Pump* pump, size_t limit) {
  const AudioDecoderConfig config = stream->audio_decoder_config();
  printf("\naudio stream [%d] codec=%s\n", stream->stream_index(),
         config.codec_name.c_str());

  media::FFmpegAudioDecoder decoder;
  std::vector<base::scoped_refptr<AudioBuffer>> buffers;
  DecoderStatus init_status;
  bool init_done = false;
  decoder.Initialize(config, /*has_pending_clear=*/false, /*serial=*/0,
                     base::BindOnce(
                         [](bool* flag, DecoderStatus* out, DecoderStatus s) {
                           *flag = true;
                           *out = s;
                         },
                         &init_done, &init_status),
                     base::BindRepeating(
                         [](std::vector<base::scoped_refptr<AudioBuffer>>* sink,
                            base::scoped_refptr<AudioBuffer> buffer) {
                           // The terminal EOS marker is not audio; keeping it
                           // out of the sink means every count below is a real
                           // buffer.
                           if (buffer && !buffer->end_of_stream()) {
                             sink->push_back(std::move(buffer));
                           }
                         },
                         &buffers),
                     media::WaitingCB());
  pump->Until([&init_done] { return init_done; });
  if (!init_status.is_ok()) {
    printf("  initialize: FAILED -- %s\n", init_status.AsDebugString().c_str());
    return 2;
  }

  const bool ok = PumpDecoder(stream, &decoder, pump, limit, &buffers);

  int64_t total_frames = 0;
  for (const auto& b : buffers) {
    total_frames += b->frame_count();
  }
  size_t valid = 0;
  const size_t non_monotonic = CountNonMonotonic(
      buffers, [](const AudioBuffer& b) { return b.timestamp(); }, &valid);

  printf("  initialize  : ok (%s)\n", decoder.GetDisplayName().c_str());
  printf("  buffers     : %zu\n", buffers.size());
  printf("  samples     : %" PRId64 "\n", total_frames);
  printf("  format      : %s %s %dHz %dch\n",
         media::GetSampleFormatName(config.sample_format),
         media::GetChannelLayoutName(config.channel_layout), config.sample_rate,
         config.channels);
  if (config.sample_rate > 0) {
    printf("  decoded     : %.3f s\n",
           static_cast<double>(total_frames) / config.sample_rate);
  }
  if (valid == 0) {
    printf("  non-monotonic pts: n/a (no timestamps at all)\n");
    return 2;
  }
  printf("  non-monotonic pts: %zu%s\n", non_monotonic,
         non_monotonic ? "   <-- timestamps are wrong" : "");
  return (!ok || non_monotonic > 0 || buffers.empty()) ? 2 : 0;
}

}  // namespace

int RunDecode(const Options& opts) {
  base::test::TaskEnvironment env;
  Pump pump(env);
  InspectHost host;
  auto media_log = base::MakeRefCounted<media::MediaLog>();
  media::FFmpegDemuxer demuxer(media_log);
  const media::MediaInfo* info_ptr = nullptr;
  if (!OpenDemuxer(&demuxer, opts.path, &host, &pump,
                   env.GetMainThreadTaskRunnerRef(), &info_ptr)) {
    return 1;
  }
  printf("file: %s  duration=%.3fs\n", opts.path.c_str(),
         info_ptr->duration.InSecondsF());

  int exit_code = 0;
  if (!opts.audio_only) {
    DemuxerStream* stream = demuxer.GetStream(DemuxerStreamType::kVideo);
    if (!stream) {
      printf("\nvideo: no video stream\n");
    } else {
      exit_code |= DecodeVideo(stream, &pump, env.GetMainThreadTaskRunnerRef(),
                               opts.limit);
    }
  }
  if (!opts.video_only) {
    DemuxerStream* stream = demuxer.GetStream(DemuxerStreamType::kAudio);
    if (!stream) {
      printf("\naudio: no audio stream\n");
    } else {
      exit_code |= DecodeAudio(stream, &pump, opts.limit);
    }
  }

  demuxer.Stop();
  if (!host.errors().empty()) {
    printf("\n%d demuxer error(s) occurred\n",
           static_cast<int>(host.errors().size()));
    exit_code |= 2;
  }
  return exit_code;
}

}  // namespace avbase
