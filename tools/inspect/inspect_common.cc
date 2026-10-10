// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/inspect/inspect_common.h"

#include <stdlib.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/data_source_descriptor.h"

namespace avbase {

const char* StreamKindName(media::StreamKind kind) {
  switch (kind) {
  case media::StreamKind::kVideo:
    return "video";
  case media::StreamKind::kAudio:
    return "audio";
  case media::StreamKind::kText:
    return "text";
  case media::StreamKind::kUnknown:
    return "unknown";
  }
  return "unknown";
}

void Usage() {
  printf(
      "avbase-inspect -- diagnostics over the real avbase media stack\n"
      "\n"
      "usage:\n"
      "  avbase-inspect probe  <file>\n"
      "      Print container and per-stream information.\n"
      "\n"
      "  avbase-inspect decode <file> [--video|--audio] [--limit N]\n"
      "      Demux and decode, then report per-stream frame counts,\n"
      "      timestamp range and monotonicity.\n"
      "\n"
      "  avbase-inspect sync   <file> [--limit N]\n"
      "      Decode both streams and drive AvSyncController with the\n"
      "      real timestamps, printing the resolved master clock and\n"
      "      the audio sample correction at each step.\n"
      "\n"
      "options:\n"
      "  --limit N   Stop after N decoded outputs per stream (default "
      "100000).\n"
      "  --verbose   Enable avbase INFO logging.\n"
      "  -h, --help  This text.\n");
}

bool ParseArgs(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      Usage();
      return false;
    } else if (arg == "--video") {
      out->video_only = true;
    } else if (arg == "--audio") {
      out->audio_only = true;
    } else if (arg == "--verbose") {
      base::logging::SetMinLogLevel(base::logging::LOG_INFO);
    } else if (arg == "--limit") {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --limit needs a value\n");
        return false;
      }
      // L9: atol() silently parsed garbage as 0 and wrapped negatives into
      // huge size_t values; validate instead.
      const char* value = argv[++i];
      char* end = nullptr;
      errno = 0;
      // strtoull silently accepts a leading '-' and wraps; reject it first.
      if (value[0] == '-') {
        fprintf(stderr, "error: --limit '%s' is not a valid count\n", value);
        return false;
      }
      // LP64: unsigned long long and size_t are both 64-bit, so ERANGE is the
      // only overflow signal; the cast below cannot truncate.
      const unsigned long long parsed = strtoull(value, &end, 10);
      if (end == value || *end != '\0' || errno == ERANGE) {
        fprintf(stderr, "error: --limit '%s' is not a valid count\n", value);
        return false;
      }
      out->limit = static_cast<size_t>(parsed);
    } else if (!arg.empty() && arg[0] == '-') {
      fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      return false;
    } else if (out->subcommand.empty()) {
      out->subcommand = arg;
    } else if (out->path.empty()) {
      out->path = arg;
    } else {
      fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
      return false;
    }
  }
  if (out->subcommand.empty()) {
    Usage();
    return false;
  }
  if (out->path.empty()) {
    fprintf(stderr, "error: %s needs a media file\n", out->subcommand.c_str());
    return false;
  }
  return true;
}

// Host that prints demuxer errors instead of swallowing them. A silent Host
bool OpenDemuxer(media::FFmpegDemuxer* demuxer, const std::string& path,
                 InspectHost* host, Pump* pump,
                 base::scoped_refptr<base::SequencedTaskRunner> runner,
                 const media::MediaInfo** info) {
  media::Status result =
      media::Err(media::ErrorCode::kNotImplemented, "not run", {}, {});
  bool done = false;
  demuxer->Initialize(media::DataSourceDescriptor::FromUri(path),
                      media::DemuxerOptions{}, host, runner,
                      base::BindOnce(
                          [](media::Status* out, bool* flag, media::Status s) {
                            *out = std::move(s);
                            *flag = true;
                          },
                          &result, &done));
  if (!pump->Until([&done] { return done; })) {
    fprintf(stderr, "error: opening '%s' timed out\n", path.c_str());
    return false;
  }
  if (!result.has_value()) {
    fprintf(stderr, "error: cannot open '%s'\n%s\n", path.c_str(),
            result.error().ToString().c_str());
    return false;
  }
  *info = &demuxer->media_info();
  return true;
}

}  // namespace avbase
