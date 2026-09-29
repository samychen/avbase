// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// `ijkpp-inspect probe`: container and per-stream information.

#include <inttypes.h>
#include <stdio.h>

#include <string>

#include "base/test/task_environment.h"
#include "media/base/media_log.h"
#include "tools/inspect/inspect_common.h"

namespace ijkpp {

namespace {

using media::MediaInfo;
using media::StreamInfo;

// Prints the video-specific fields of a stream. Split out of RunProbe so that
// neither branch pushes the function past the 80-line limit (invariant C2).
void PrintVideoStream(const StreamInfo& s) {
  printf("      resolution   : %dx%d (coded %dx%d)\n", s.natural_size.width,
         s.natural_size.height, s.coded_size.width, s.coded_size.height);
  printf("      pixel format : %s\n", s.pixel_format.c_str());
  printf("      frame rate   : %d/%d (avg %d/%d)\n", s.frame_rate.num,
         s.frame_rate.den, s.avg_frame_rate.num, s.avg_frame_rate.den);
  printf("      sar          : %d/%d\n", s.sar.num, s.sar.den);
  if (s.rotation != 0) {
    printf("      rotation     : %d\n", s.rotation);
  }
  if (s.has_hdr_metadata) {
    printf("      hdr          : yes\n");
  }
}

void PrintAudioStream(const StreamInfo& s) {
  printf("      sample rate  : %d Hz\n", s.sample_rate);
  printf("      channels     : %d\n", s.channels);
  printf("      sample fmt   : %s\n", s.sample_format.c_str());
  printf("      layout       : %s\n", s.channel_layout.c_str());
}

void PrintContainer(const MediaInfo& info) {
  printf("file        : %s\n", info.uri.c_str());
  printf("format      : %s\n", info.format_name.c_str());
  printf("duration    : %.3f s%s\n", info.duration.InSecondsF(),
         info.duration_is_estimate ? " (estimate)" : "");
  printf("live        : %s\n", info.is_live ? "yes" : "no");
  printf("seekable    : %s\n", info.seekable ? "yes" : "no");
  printf("bitrate     : %" PRId64 " b/s\n", info.bit_rate);
  if (info.file_size >= 0) {
    printf("file size   : %" PRId64 " bytes\n", info.file_size);
  }
  if (!info.metadata.empty()) {
    printf("metadata    :\n");
    for (const auto& [key, value] : info.metadata) {
      printf("  %-16s %s\n", key.c_str(), value.c_str());
    }
  }
  printf("streams     : %zu\n", info.streams.size());
}

}  // namespace

int RunProbe(const Options& opts) {
  base::test::TaskEnvironment env;
  Pump pump(env);
  InspectHost host;
  auto media_log = base::MakeRefCounted<media::MediaLog>();
  media::FFmpegDemuxer demuxer(media_log);
  const MediaInfo* info_ptr = nullptr;
  if (!OpenDemuxer(&demuxer, opts.path, &host, &pump,
                   env.GetMainThreadTaskRunnerRef(), &info_ptr)) {
    return 1;
  }
  const MediaInfo& info = *info_ptr;

  PrintContainer(info);
  for (const StreamInfo& s : info.streams) {
    printf("\n  [%d] %s  codec=%s", s.index, StreamKindName(s.kind),
           s.codec_name.c_str());
    if (!s.language.empty()) {
      printf(" lang=%s", s.language.c_str());
    }
    if (!s.title.empty()) {
      printf(" title=\"%s\"", s.title.c_str());
    }
    printf("\n");
    if (s.kind == media::StreamKind::kVideo) {
      PrintVideoStream(s);
    } else if (s.kind == media::StreamKind::kAudio) {
      PrintAudioStream(s);
    }
    if (!s.duration.is_zero()) {
      printf("      duration     : %.3f s\n", s.duration.InSecondsF());
    }
  }
  demuxer.Stop();
  return host.errors().empty() ? 0 : 2;
}

}  // namespace ijkpp
