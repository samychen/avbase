// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// SDL2 playback: the M11 acceptance example. The host owns the window and
// renderer (embedding is the core mode, docs/archive/09 §2), avbase renders
// into them, and the main thread's only jobs are SDL_PollEvent and quitting.
//
//   ./play_sdl2 --url video.mp4 [--max-seconds n] [--gl] [--stats]
//                [--canvas WxH]
//
// Exit codes: 0 played to completion (or the --max-seconds cap), 1 error.
//
// The whole program is what is left in this file: options come from
// sdl2_demo.h (which also carries the event handlers and window creation),
// so the part a newcomer reads first stays readable end to end.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "SDL.h"

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "examples/scene_demo.h"
#include "examples/sdl2_demo.h"
#include "platform/sdl2/sdl2_audio_sink.h"
#include "platform/sdl2/sdl2_video_sink.h"
#include "platform/sdl2/surface.h"
#include "player/public/player.h"

namespace {

using namespace std::chrono_literals;  // NOLINT — an example, not the SDK.

using avbase::examples::sdl2::CreateSdlWindow;
using avbase::examples::sdl2::FindDemoFont;
using avbase::examples::sdl2::g_completed;
using avbase::examples::sdl2::g_error;
using avbase::examples::sdl2::g_font_path;
using avbase::examples::sdl2::g_overlay;
using avbase::examples::sdl2::g_player;
using avbase::examples::sdl2::g_print_stats;
using avbase::examples::sdl2::g_quit;
using avbase::examples::sdl2::g_scene;
using avbase::examples::sdl2::OnPlayerEvent;
using avbase::examples::sdl2::Options;
using avbase::examples::sdl2::ParseOptions;

constexpr auto kEventPollInterval = 10ms;

// The main thread's only job: pump SDL events until the player completes,
// errors, or the --max-seconds cap expires. Returns the process exit code.
int PumpEventsUntilDone(int max_seconds) {
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::seconds(max_seconds > 0 ? max_seconds : 86400);
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
  std::printf("stopping: %s\n", g_completed.load()
                                    ? "completed"
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
  SDL_GLContext gl_context = nullptr;
  if (!CreateSdlWindow(options.gl, &window, &renderer, &gl_context)) {
    return 1;
  }

  // The host owns the SDL objects and keeps |surface| alive for the whole
  // playback; the display just carries the pointers through (surface.h).
  Sdl2Surface surface;
  surface.window = window;
  surface.renderer = renderer;
  surface.gl_context = gl_context;
  surface.overlay = &g_overlay;
  surface.canvas_width = options.canvas_width;
  surface.canvas_height = options.canvas_height;
  g_font_path = FindDemoFont();
  auto display = avbase::media::NativeDisplay::FromSdl2Window(&surface);

  auto deps = std::make_unique<avbase::Deps>();
  if (!options.scene.empty()) {
    g_scene = std::make_unique<avbase::examples::SceneDemo>(options.scene);
  }
  // The backend is all this host has to name: the scene is on the Player, so
  // the demo does not have to unwrap the sink this factory produces.
  deps->video_sink_factory =
      std::make_shared<avbase::media::Sdl2VideoSinkFactory>();
  deps->audio_sink_factory =
      std::make_shared<avbase::media::Sdl2AudioSinkFactory>();
  avbase::Player player(avbase::PlayerConfig(), std::move(deps));
  g_player = &player;
  if (g_scene != nullptr) {
    // Before SetDataSource/PrepareAsync: that is when the second source
    // becomes a demuxer, so registering later is refused.
    g_scene->set_second_url(options.second);
    g_scene->Install(player);
  }
  player.SetEventHandler(avbase::base::BindRepeating(&OnPlayerEvent));
  g_print_stats.store(options.stats);
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
  // L6: Stop() is asynchronous; tearing SDL resources down against a player
  // that may still be flushing races the sink's teardown. StopSync() blocks
  // until the pipeline actually finished stopping (see player.h for the
  // Stop/StopSync contract). Every source stops with the one pipeline --
  // there is no second producer to quieten first.
  player.StopSync(avbase::base::Seconds(5));
  if (g_scene != nullptr && g_scene->second_source()) {
    avbase::examples::ReportSecondSource(*g_scene->second_source());
  }

  if (gl_context) {
    // The sink released the context in Stop(); destroying it here is the
    // host's half of the ownership split.
    SDL_GL_DeleteContext(gl_context);
  }
  if (renderer) {
    SDL_DestroyRenderer(renderer);
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return exit_code;
}
