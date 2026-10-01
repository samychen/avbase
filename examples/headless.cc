// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Headless playback: the whole decode -> sync -> render path runs against the
// null sinks, nothing appears on screen, and the process exits 0 when the
// player reports kCompleted. This is the "does it actually play" smoke test
// that needs no window system, and the CI e2e target once e2e-linux opens.

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/time/time.h"
#include "player/public/player.h"

namespace {

using namespace std::chrono_literals;  // NOLINT — an example, not the SDK.

// How often the state is polled. The polling seam is deliberate: this example
// drives the same public surface a UI would.
constexpr auto kPollInterval = 50ms;

struct Options {
  std::string url;
  int timeout_seconds{60};
  double rate{1.0};
  double seek_to{-1.0};
};

bool ParseOptions(int argc, char** argv, Options* out) {
  DCHECK(out);
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <url> [--timeout seconds] [--rate x]\n",
                 argv[0]);
    return false;
  }
  out->url = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--timeout") {
      out->timeout_seconds = std::atoi(argv[i + 1]);
    } else if (flag == "--rate") {
      out->rate = std::atof(argv[i + 1]);
    } else if (flag == "--seek") {
      out->seek_to = std::atof(argv[i + 1]);
    }
  }
  return true;
}

void ReportErrorEvent(const avbase::PlayerEvent& e) {
  if (e.type != avbase::EventType::kError) {
    return;
  }
  if (const auto* payload = avbase::AsError(e)) {
    std::fprintf(stderr, "error event: %s\n",
                 payload->error.ToString().c_str());
  }
}

void PrintMediaInfo(avbase::Player& player) {
  if (auto info = player.media_info()) {
    std::printf("container: %s, duration: %s, streams: %zu\n",
                info->format_name.c_str(),
                info->duration.ToString().c_str(), info->streams.size());
  }
  const avbase::media::Size natural = player.video_natural_size();
  std::printf("video: %dx%d\n", natural.width, natural.height);
}

// Polls until the player reports kCompleted or kError, printing a position line
// once per media second. Returns the process exit code.
int WaitForEnd(avbase::Player& player, const Options& options) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(options.timeout_seconds);
  int last_printed_second = -1;
  while (std::chrono::steady_clock::now() < deadline) {
    if (player.state() == avbase::PlayerState::kCompleted) {
      std::printf("\ncompleted at media time %s\n",
                  player.GetMediaTime().ToString().c_str());
      return 0;
    }
    if (player.state() == avbase::PlayerState::kError) {
      std::fprintf(stderr, "\nplayer entered kError\n");
      return 1;
    }
    const int second = static_cast<int>(player.GetMediaTime().InSecondsF());
    if (second != last_printed_second) {
      last_printed_second = second;
      std::printf("\rposition: %s / %s (buffered %s)   ",
                  player.GetMediaTime().ToString().c_str(),
                  player.GetDuration().ToString().c_str(),
                  player.GetBufferedTime().ToString().c_str());
      std::fflush(stdout);
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  std::fprintf(stderr, "\ntimeout after %ds\n", options.timeout_seconds);
  return 1;
}

// Starts playback, honors --seek and --rate, then waits for the end.
int StartAndWait(avbase::Player& player, const Options& options) {
  if (options.rate != 1.0) {
    player.SetPlaybackRate(options.rate);
  }
  if (player.state() != avbase::PlayerState::kStarted) {
    player.Start();
  }
  if (options.seek_to >= 0.0) {
    const auto result =
        player.SeekTo(avbase::base::SecondsD(options.seek_to),
                      avbase::SeekMode::kPreviousKeyframe,
                      avbase::Player::SeekCB());
    if (!result) {
      std::fprintf(stderr, "SeekTo failed: %s\n",
                   result.error().ToString().c_str());
      return 1;
    }
    std::printf("seeking to %.2fs (request %" PRId64 ")\n", options.seek_to,
                *result);
  }
  return WaitForEnd(player, options);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    return 2;
  }

  avbase::Player player;
  player.SetEventHandler(avbase::base::BindRepeating(&ReportErrorEvent));
  if (const avbase::Status source = player.SetDataSource(options.url);
      !source) {
    std::fprintf(stderr, "SetDataSource failed: %s\n",
                 source.error().ToString().c_str());
    return 1;
  }
  const avbase::Status prepared = player.PrepareSync(avbase::base::Seconds(15));
  if (!prepared) {
    std::fprintf(stderr, "prepare failed: %s\n",
                 prepared.error().ToString().c_str());
    return 1;
  }
  PrintMediaInfo(player);
  return StartAndWait(player, options);
}
