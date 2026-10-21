// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// `avbase-inspect transcode`: the first production consumer of
// media/transcode.
//
// Until this subcommand existed, the offline transcode line (E1-E5) had no
// caller outside its own unit tests: SelectVideoEncoder() had zero production
// callers, and Transcode() was only ever driven by tests that synthesise
// their input with the very library they then transcode. A library with no
// consumer rots quietly -- an option nobody passes is an option nobody has
// run, and "the code is all there" stops being true the moment the FFmpeg
// whitelist changes under it. (It did: h264_videotoolbox was missing from
// tools/setup_ffmpeg.sh until a test went looking for it.)
//
// Two things this adds on top of calling Transcode():
//   * It verifies its own output by demuxing it again. "The muxer returned
//     OK" and "the file plays" are different claims, and only the second one
//     is worth reporting as success.
//   * On failure it lists the video encoders this FFmpeg build actually has.
//     "Encoder not found" without that list is the fastest way to lose an
//     afternoon on a --disable-everything build, which is exactly what this
//     repository builds.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/test/task_environment.h"
#include "media/base/media_log.h"
#include "media/ffmpeg/av_includes.h"
#include "media/transcode/ffmpeg_transcode_job.h"
#include "tools/inspect/inspect_common.h"

namespace avbase {
namespace {

struct TranscodeArgs {
  std::string input;
  std::string output;
  double start_seconds = 0.0;
  double duration_seconds = 0.0;
  // Defaults are what this repository's FFmpeg whitelist can actually run
  // (mjpeg + aac are the two encoders in tools/setup_ffmpeg.sh). libx264 is
  // the portable choice and works wherever the build has it; on a minimal
  // build it fails with the list of what IS here, which is the point.
  std::string vcodec = "mjpeg";
  std::string acodec = "aac";
  bool drop_video = false;
  bool drop_audio = false;
  int crf = -1;
  int gop = 0;
  std::string preset;
  int64_t vbitrate = 0;
  int64_t abitrate = 0;
  int width = 0;
  int height = 0;
  int fps = 0;
  bool prefer_hardware = false;
  bool faststart = false;
  // KEY=VALUE pairs for the encoder's private options (repeatable).
  std::vector<std::pair<std::string, std::string>> encopts;
};

constexpr int64_t kMaxBitRate = 0x7FFFFFFF;  // AVCodecContext.bit_rate is int.

// L9: atol() silently parsed garbage as 0 and wrapped negatives into huge
// size_t values. Every numeric option goes through these three instead.
bool ParseInt64(const char* text, int64_t* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const int64_t parsed = strtoll(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE) {
    return false;
  }
  *out = static_cast<int64_t>(parsed);
  return true;
}

bool ParseCount(const char* text, int64_t* out) {
  if (!ParseInt64(text, out) || *out < 0) {
    return false;
  }
  return true;
}

bool ParseSeconds(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double parsed = strtod(text, &end);
  // strtod also accepts "nan" and "inf", neither of which is a trim window.
  if (end == text || *end != '\0' || !std::isfinite(parsed) || parsed < 0.0) {
    return false;
  }
  *out = parsed;
  return true;
}

// Returns the value following |flag|, or nullptr after printing why not.
// Consumes the value on success.
const char* ValueAfter(int argc, char** argv, int* i, const char* flag) {
  if (*i + 1 >= argc) {
    fprintf(stderr, "error: %s needs a value\n", flag);
    return nullptr;
  }
  return argv[++*i];
}

bool ParseTranscodeArgs(int argc, char** argv, TranscodeArgs* out) {
  bool have_input = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      Usage();
      return false;
    }
    if (arg == "--verbose") {
      base::logging::SetMinLogLevel(base::logging::LOG_INFO);
    } else if (arg == "--no-video") {
      out->drop_video = true;
    } else if (arg == "--no-audio") {
      out->drop_audio = true;
    } else if (arg == "--hw") {
      out->prefer_hardware = true;
    } else if (arg == "--faststart") {
      out->faststart = true;
    } else if (arg == "-o" || arg == "--output") {
      const char* value = ValueAfter(argc, argv, &i, "-o");
      if (value == nullptr) {
        return false;
      }
      out->output = value;
    } else if (arg == "--vcodec" || arg == "--acodec" || arg == "--preset") {
      const char* value = ValueAfter(argc, argv, &i, arg.c_str());
      if (value == nullptr) {
        return false;
      }
      if (arg == "--vcodec") {
        out->vcodec = value;
      } else if (arg == "--acodec") {
        out->acodec = value;
      } else {
        out->preset = value;
      }
    } else if (arg == "--start" || arg == "--duration") {
      const char* value = ValueAfter(argc, argv, &i, arg.c_str());
      double seconds = 0.0;
      if (value == nullptr || !ParseSeconds(value, &seconds)) {
        fprintf(stderr,
                "error: %s '%s' is not a non-negative number of "
                "seconds\n",
                arg.c_str(), value == nullptr ? "" : value);
        return false;
      }
      if (arg == "--start") {
        out->start_seconds = seconds;
      } else {
        out->duration_seconds = seconds;
      }
    } else if (arg == "--crf" || arg == "--gop" || arg == "--vbitrate" ||
               arg == "--abitrate" || arg == "--width" || arg == "--height" ||
               arg == "--fps") {
      const char* value = ValueAfter(argc, argv, &i, arg.c_str());
      int64_t parsed = 0;
      if (value == nullptr || !ParseCount(value, &parsed)) {
        fprintf(stderr, "error: %s '%s' is not a non-negative integer\n",
                arg.c_str(), value == nullptr ? "" : value);
        return false;
      }
      if (arg == "--crf") {
        out->crf = static_cast<int>(parsed);
      } else if (arg == "--gop") {
        out->gop = static_cast<int>(parsed);
      } else if (arg == "--vbitrate" || arg == "--abitrate") {
        if (parsed > kMaxBitRate) {
          fprintf(stderr,
                  "error: %s %" PRId64 " exceeds the %" PRId64
                  " bits/s the "
                  "encoder API accepts\n",
                  arg.c_str(), parsed, kMaxBitRate);
          return false;
        }
        if (arg == "--vbitrate") {
          out->vbitrate = parsed;
        } else {
          out->abitrate = parsed;
        }
      } else if (arg == "--width") {
        out->width = static_cast<int>(parsed);
      } else if (arg == "--height") {
        out->height = static_cast<int>(parsed);
      } else {
        out->fps = static_cast<int>(parsed);
      }
    } else if (arg == "--encopt") {
      const char* value = ValueAfter(argc, argv, &i, "--encopt");
      if (value == nullptr) {
        return false;
      }
      const std::string pair = value;
      const size_t equals = pair.find('=');
      if (equals == std::string::npos || equals == 0) {
        fprintf(stderr,
                "error: --encopt '%s' is not KEY=VALUE (a private encoder "
                "option needs a name)\n",
                value);
        return false;
      }
      out->encopts.emplace_back(pair.substr(0, equals),
                                pair.substr(equals + 1));
    } else if (!arg.empty() && arg[0] == '-') {
      fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      return false;
    } else if (!have_input) {
      out->input = arg;
      have_input = true;
    } else {
      fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
      return false;
    }
  }
  if (!have_input) {
    fprintf(stderr, "error: transcode needs an input file\n");
    return false;
  }
  if (out->output.empty()) {
    fprintf(stderr,
            "error: transcode needs -o <output> (the container "
            "follows the extension)\n");
    return false;
  }
  // Scaling needs both axes: a width without a height would leave the other
  // at "same as input" and quietly produce a stretched frame.
  if ((out->width == 0) != (out->height == 0)) {
    fprintf(stderr, "error: --width and --height must be given together\n");
    return false;
  }
  if (out->input == out->output) {
    // Transcode() opens the output while the input is still being read; the
    // same path truncates the file out from under the demuxer.
    fprintf(stderr, "error: output must differ from input\n");
    return false;
  }
  return true;
}

media::TranscodeParams BuildParams(const TranscodeArgs& args) {
  media::TranscodeParams params;
  params.output_path = args.output;
  params.start_time_seconds = args.start_seconds;
  params.duration_seconds = args.duration_seconds;
  if (args.faststart) {
    params.muxer_options.emplace_back("movflags", "+faststart");
  }
  // An empty codec name drops the stream, which is ffmpeg's -an / -vn.
  params.video_codec.codec = args.drop_video ? std::string() : args.vcodec;
  params.audio_codec.codec = args.drop_audio ? std::string() : args.acodec;
  params.video_codec.crf = args.crf;
  params.video_codec.gop = args.gop;
  params.video_codec.preset = args.preset;
  params.video_codec.bit_rate = static_cast<int>(args.vbitrate);
  params.video_codec.width = args.width;
  params.video_codec.height = args.height;
  params.video_codec.fps_num = args.fps;
  params.video_codec.fps_den = 1;
  params.video_codec.prefer_hardware = args.prefer_hardware;
  params.video_codec.options = args.encopts;
  params.audio_codec.bit_rate = static_cast<int>(args.abitrate);
  return params;
}

void PrintProgress(int percent) {
  // The callback fires per packet; redrawing an unchanged percentage is
  // noise on a long file.
  static int last = -1;
  if (percent == last) {
    return;
  }
  last = percent;
  printf("\r  %3d%%", percent);
  fflush(stdout);
}

int64_t FileSizeOrMinusOne(const std::string& path) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return -1;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return -1;
  }
  const int64_t size = static_cast<int64_t>(ftell(file));
  fclose(file);
  return size < 0 ? -1 : static_cast<int64_t>(size);
}

// Prints what this build can encode with. Reached only on a failure that is
// plausibly about a missing encoder, where the list is the answer.
void PrintAvailableVideoEncoders() {
  printf("video encoders in this FFmpeg build:\n");
  void* cookie = nullptr;
  const AVCodec* codec = nullptr;
  std::vector<std::string> names;
  while ((codec = av_codec_iterate(&cookie)) != nullptr) {
    if (av_codec_is_encoder(codec) && codec->type == AVMEDIA_TYPE_VIDEO) {
      names.push_back(codec->name);
    }
  }
  if (names.empty()) {
    printf("  (none -- this build has no video encoder at all)\n");
    return;
  }
  std::sort(names.begin(), names.end());
  std::string line = " ";
  for (const std::string& name : names) {
    if (line.size() + name.size() > 72) {
      printf("%s\n", line.c_str());
      line = " ";
    }
    line += " " + name;
  }
  printf("%s\n", line.c_str());
}

// Re-opens the output and reports what is in it. Returns false when the file
// does not open as media at all -- a muxer that returns OK and writes
// something unreadable is exactly the failure this catches.
bool VerifyOutput(const std::string& path) {
  base::test::TaskEnvironment env;
  Pump pump(env);
  InspectHost host;
  auto media_log = base::MakeRefCounted<media::MediaLog>();
  media::FFmpegDemuxer demuxer(media_log);
  const media::MediaInfo* info = nullptr;
  if (!OpenDemuxer(&demuxer, path, &host, &pump,
                   env.GetMainThreadTaskRunnerRef(), &info)) {
    fprintf(stderr,
            "error: transcode reported success but '%s' does not open as "
            "media\n",
            path.c_str());
    return false;
  }
  printf("verified: %s opens as %s, duration=%.3fs, %zu stream(s)\n",
         path.c_str(), info->format_name.c_str(), info->duration.InSecondsF(),
         info->streams.size());
  for (const media::StreamInfo& stream : info->streams) {
    printf("  [%d] %-5s %s", stream.index, StreamKindName(stream.kind),
           stream.codec_name.c_str());
    if (stream.kind == media::StreamKind::kVideo) {
      printf(" %dx%d", stream.coded_size.width, stream.coded_size.height);
    } else if (stream.kind == media::StreamKind::kAudio) {
      printf(" %dHz %dch", stream.sample_rate, stream.channels);
    }
    printf("\n");
  }
  demuxer.Stop();
  return true;
}

}  // namespace

int RunTranscode(int argc, char** argv) {
  TranscodeArgs args;
  if (!ParseTranscodeArgs(argc, argv, &args)) {
    Usage();
    return 1;
  }
  if (FileSizeOrMinusOne(args.input) < 0) {
    fprintf(stderr, "error: cannot read input '%s'\n", args.input.c_str());
    return 1;
  }

  const media::TranscodeParams params = BuildParams(args);
  printf("transcode: %s -> %s\n", args.input.c_str(), args.output.c_str());
  printf("  video: %s   audio: %s",
         args.drop_video ? "(dropped)" : args.vcodec.c_str(),
         args.drop_audio ? "(dropped)" : args.acodec.c_str());
  if (args.prefer_hardware) {
    printf("   [hardware preferred]");
  }
  printf("\n");
  if (args.start_seconds > 0.0 || args.duration_seconds > 0.0) {
    std::string window = "to end";
    if (args.duration_seconds > 0.0) {
      char buffer[64];
      snprintf(buffer, sizeof(buffer), "%.3fs", args.duration_seconds);
      window = buffer;
    }
    printf("  trim: start=%.3fs duration=%s\n", args.start_seconds,
           window.c_str());
  }

  const auto started = std::chrono::steady_clock::now();
  const media::Status status =
      media::Transcode(args.input, params, &PrintProgress);
  printf("\n");
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();

  if (!status.has_value()) {
    const media::MediaError& error = status.error();
    fprintf(stderr, "error: transcode failed: %s\n", error.ToString().c_str());
    if (!args.drop_video) {
      PrintAvailableVideoEncoders();
    }
    return 1;
  }

  const int64_t written = FileSizeOrMinusOne(args.output);
  printf("done in %.2fs, %" PRId64 " bytes written\n", elapsed, written);
  if (written <= 0) {
    fprintf(stderr, "error: transcode reported success but '%s' is empty\n",
            args.output.c_str());
    return 2;
  }
  return VerifyOutput(args.output) ? 0 : 2;
}

}  // namespace avbase
