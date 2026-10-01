// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// SDL2 playback: the M11 acceptance example. The host owns the window and
// renderer (embedding is the core mode, docs/09 §2), avbase renders into them,
// and the main thread's only jobs are SDL_PollEvent and quitting.
//
//   ./play_sdl2 --url video.mp4 [--max-seconds n]
//
// Exit codes: 0 played to completion (or the --max-seconds cap), 1 error.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "SDL.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "player/public/player.h"
#include "platform/sdl2/sdl2_audio_sink.h"
#include "platform/sdl2/sdl2_video_sink.h"
#include "platform/sdl2/surface.h"

namespace {

using namespace std::chrono_literals;  // NOLINT — an example, not the SDK.

constexpr auto kEventPollInterval = 10ms;
constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;

// Set from the player's event dispatch thread, read by the event loop.
std::atomic<bool> g_quit{false};
std::atomic<bool> g_error{false};
std::atomic<bool> g_completed{false};

struct Options {
  std::string url;
  int max_seconds{0};
};

bool ParseOptions(int argc, char** argv, Options* out) {
  DCHECK(out);
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--url" && i + 1 < argc) {
      out->url = argv[++i];
    } else if (arg == "--max-seconds" && i + 1 < argc) {
      out->max_seconds = std::atoi(argv[++i]);
    }
  }
  if (out->url.empty()) {
    std::fprintf(stderr, "usage: %s --url <file-or-url> "
                         "[--max-seconds n]\n", argv[0]);
    return false;
  }
  return true;
}

void OnPlayerEvent(const avbase::PlayerEvent& e) {
  if (e.type == avbase::EventType::kError) {
    if (const auto* payload = avbase::AsError(e)) {
      std::fprintf(stderr, "error: %s\n", payload->error.ToString().c_str());
    }
    g_error.store(true);
  } else if (e.type == avbase::EventType::kCompleted) {
    g_completed.store(true);
  } else if (e.type == avbase::EventType::kStats) {
    std::printf("position %s / %s\n", e.media_time.ToString().c_str(), "--");
  }
}

// Creates the window and renderer the host owns (rule 1 in surface.h).
bool CreateSdlWindow(SDL_Window** window, SDL_Renderer** renderer) {
  DCHECK(window);
  DCHECK(renderer);
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  *window = SDL_CreateWindow("avbase", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, kWindowWidth,
                             kWindowHeight,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  *renderer = SDL_CreateRenderer(
      *window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!*window || !*renderer) {
    std::fprintf(stderr, "SDL window/renderer failed: %s\n", SDL_GetError());
    return false;
  }
  return true;
}

// The main thread's only job: pump SDL events until the player completes,
// errors, or the --max-seconds cap expires. Returns the process exit code.
int PumpEventsUntilDone(int max_seconds) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(max_seconds > 0 ? max_seconds
                                                             : 86400);
  while (!g_quit.load() && !g_error.load() && !g_completed.load() &&
         std::chrono::steady_clock::now() < deadline) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_q)) {
        g_quit.store(true);
      }
    }
    std::this_thread::sleep_for(kEventPollInterval);
  }
  std::printf("stopping: %s\n",
              g_completed.load() ? "completed"
                                 : (g_error.load() ? "error" : "quit"));
  return g_error.load() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    return 2;
  }

  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  if (!CreateSdlWindow(&window, &renderer)) {
    return 1;
  }

  // The host owns both SDL objects and keeps |surface| alive for the whole
  // playback; the display just carries the pointers through (surface.h).
  Sdl2Surface surface;
  surface.window = window;
  surface.renderer = renderer;
  auto display = avbase::media::NativeDisplay::FromSdl2Window(&surface);

  auto deps = std::make_unique<avbase::Deps>();
  deps->video_sink_factory =
      std::make_shared<avbase::media::Sdl2VideoSinkFactory>();
  deps->audio_sink_factory =
      std::make_shared<avbase::media::Sdl2AudioSinkFactory>();

  avbase::Player player(avbase::PlayerConfig(), std::move(deps));
  player.SetEventHandler(avbase::base::BindRepeating(&OnPlayerEvent));
  player.SetVideoSurface(display);
  if (const avbase::Status s = player.SetDataSource(options.url); !s) {
    std::fprintf(stderr, "SetDataSource failed: %s\n",
                 s.error().ToString().c_str());
    return 1;
  }
  if (const avbase::Status s = player.PrepareAsync(); !s) {
    std::fprintf(stderr, "PrepareAsync failed: %s\n",
                 s.error().ToString().c_str());
    return 1;
  }

  const int exit_code = PumpEventsUntilDone(options.max_seconds);
  player.Stop();

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return exit_code;
}
