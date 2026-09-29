// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// `ijkpp-inspect sync`: decode both streams and drive AvSyncController with the
// real timestamps.
//
// This is the subcommand that caught bug #32. The unit tests for the sync
// controller were extensive -- 28 of them, including a seqlock tear detector --
// and every one passed while `Clock::Get()` returned a master clock offset by
// negative uptime, because they all drove a SimpleTestTickClock starting at zero.
// Against a real file on a real host the master read -33185 s, which would have
// marked every video frame as infinitely late and dropped the entire stream.

#include <stdio.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/time/default_tick_clock.h"
#include "media/base/audio_buffer.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_status.h"
#include "media/base/media_log.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/ffmpeg_audio_decoder.h"
#include "tools/inspect/inspect_common.h"

namespace ijkpp {

namespace {

using media::AudioBuffer;
using media::AudioDecoderConfig;
using media::AvSyncController;
using media::DecoderStatus;
using media::DemuxerStream;
using media::DemuxerStreamType;
using MasterType = media::AvSyncController::MasterType;

const char* MasterName(MasterType type) {
  switch (type) {
    case MasterType::kAudio: return "audio";
    case MasterType::kVideo: return "video";
    case MasterType::kExternal: return "external";
  }
  return "unknown";
}

void PrintTableHeader() {
  printf("\n%-6s %-8s %-12s %-12s %-12s %-10s %s\n", "step", "kind",
         "media_time", "master", "av_diff", "adjust", "note");
}

// Reads up to |limit| buffers from |stream| into |pending|, returning false if
// the demuxer stopped answering. Shared by both sync passes.
bool ReadBatch(DemuxerStream* stream, Pump* pump, bool* eos,
               DemuxerStream::DecoderBufferVector* pending) {
  bool answered = false;
  pending->clear();
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
                      &answered, eos, pending));
  if (!pump->Until([&answered] { return answered; })) {
    fprintf(stderr, "error: demuxer read timed out\n");
    return false;
  }
  return true;
}

// Decodes audio and consumes every buffer through the controller, exactly as an
// audio renderer would. Returns the number of steps reported.
size_t SyncFromAudio(DemuxerStream* stream, AvSyncController* controller,
                     Pump* pump, size_t limit, size_t first_step) {
  const AudioDecoderConfig config = stream->audio_decoder_config();
  media::FFmpegAudioDecoder decoder;
  std::vector<base::scoped_refptr<AudioBuffer>> out;
  DecoderStatus init_status;
  bool init_done = false;
  decoder.Initialize(
      config, /*has_pending_clear=*/false, /*serial=*/0,
      base::BindOnce([](bool* flag, DecoderStatus* o, DecoderStatus s) {
        *flag = true;
        *o = s;
      }, &init_done, &init_status),
      base::BindRepeating(
          [](std::vector<base::scoped_refptr<AudioBuffer>>* sink,
             base::scoped_refptr<AudioBuffer> b) {
            if (b && !b->end_of_stream()) {
              sink->push_back(std::move(b));
            }
          },
          &out),
      media::WaitingCB());
  pump->Until([&init_done] { return init_done; });
  if (!init_status.is_ok()) {
    fprintf(stderr, "error: audio decoder init failed: %s\n",
            init_status.AsDebugString().c_str());
    return 0;
  }

  size_t step = first_step;
  bool eos = false;
  for (size_t round = 0; round < 200000 && step < limit && !eos; ++round) {
    DemuxerStream::DecoderBufferVector pending;
    if (!ReadBatch(stream, pump, &eos, &pending)) {
      break;
    }
    for (auto& buffer : pending) {
      bool done = false;
      decoder.Decode(
          buffer,
          base::BindOnce([](bool* f, DecoderStatus) { *f = true; }, &done));
      pump->Until([&done] { return done; });
    }
    // Report on everything decoded this round. The sample adjustment is the
    // value a renderer would apply to stretch or shrink the next buffer; its
    // sign is what bug #29 got backwards.
    for (const auto& b : out) {
      if (step >= limit) {
        break;
      }
      controller->OnAudioFramesConsumed(b->frame_count(), b->timestamp(),
                                        b->serial());
      const int adjust = controller->ComputeAudioSampleAdjustment(
          b->frame_count(), b->sample_rate());
      const auto snap = controller->GetSnapshot();
      printf("%-6zu %-8s %-12.6f %-12.6f %-12.6f %-10d %s\n", ++step, "audio",
             b->timestamp().InSecondsF(), snap.master.InSecondsF(),
             snap.av_diff.InSecondsF(), adjust,
             adjust == 0 ? "" : (adjust > 0 ? "stretch" : "shrink"));
    }
    out.clear();
  }
  return step;
}

// Presents video at its demuxed timestamps. A real renderer paces these against
// the master clock; here the point is to observe the resulting av_diff, so the
// frames are fed as fast as they arrive.
size_t SyncFromVideo(DemuxerStream* stream, AvSyncController* controller,
                     Pump* pump, size_t limit, size_t first_step) {
  size_t step = first_step;
  bool eos = false;
  for (size_t round = 0; round < 200000 && step < limit && !eos; ++round) {
    DemuxerStream::DecoderBufferVector pending;
    if (!ReadBatch(stream, pump, &eos, &pending)) {
      break;
    }
    for (const auto& buffer : pending) {
      if (buffer->IsEndOfStream() || step >= limit) {
        continue;
      }
      controller->OnVideoFramePresented(buffer->timestamp(), buffer->serial());
      const auto snap = controller->GetSnapshot();
      printf("%-6zu %-8s %-12.6f %-12.6f %-12.6f %-10s %s\n", ++step, "video",
             buffer->timestamp().InSecondsF(), snap.master.InSecondsF(),
             snap.av_diff.InSecondsF(), "-",
             snap.master_valid ? "" : "master not yet valid");
    }
  }
  return step;
}

void PrintSummary(const AvSyncController& controller, size_t steps) {
  const auto snap = controller.GetSnapshot();
  printf("\nsteps        : %zu\n", steps);
  printf("final master : %.6f s (valid=%d)\n", snap.master.InSecondsF(),
         snap.master_valid ? 1 : 0);
  printf("final av_diff: %.6f s\n", snap.av_diff.InSecondsF());
  printf("audio clock  : %.6f s  video clock: %.6f s\n",
         snap.audio.pts.InSecondsF(), snap.video.pts.InSecondsF());
}

}  // namespace

int RunSync(const Options& opts) {
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
  DemuxerStream* audio = demuxer.GetStream(DemuxerStreamType::kAudio);
  DemuxerStream* video = demuxer.GetStream(DemuxerStreamType::kVideo);
  if (!audio && !video) {
    fprintf(stderr, "error: no audio or video stream to synchronize\n");
    return 1;
  }

  // Default thresholds: they are ported from ffplay's constants and the point of
  // this subcommand is to observe the shipped behaviour, not a tuned one.
  AvSyncController controller(MasterType::kAudio,
                              base::DefaultTickClock::GetInstance(),
                              AvSyncController::Thresholds{});
  controller.SetStreamAvailability(audio != nullptr, video != nullptr);

  printf("file: %s\n", opts.path.c_str());
  printf("requested master: %s\n", MasterName(controller.requested_master()));
  printf("resolved master : %s (has_audio=%d has_video=%d)\n",
         MasterName(controller.ResolveMasterType()), audio ? 1 : 0,
         video ? 1 : 0);
  PrintTableHeader();

  size_t step = 0;
  if (audio) {
    step = SyncFromAudio(audio, &controller, &pump, opts.limit, step);
  }
  if (video && step < opts.limit) {
    step = SyncFromVideo(video, &controller, &pump, opts.limit, step);
  }

  PrintSummary(controller, step);
  demuxer.Stop();
  return step > 0 ? 0 : 2;
}

}  // namespace ijkpp
