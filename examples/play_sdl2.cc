// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// SDL2 playback: the M11 acceptance example. The host owns the window and
// renderer (embedding is the core mode, docs/09 §2), avbase renders into them,
// and the main thread's only jobs are SDL_PollEvent and quitting.
//
//   ./play_sdl2 --url video.mp4 [--max-seconds n] [--gl]
//
// Exit codes: 0 played to completion (or the --max-seconds cap), 1 error.

#include <algorithm>
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
#include "media/base/timed_text.h"
#include "platform/sdl2/sdl2_audio_sink.h"
#include "platform/sdl2/sdl2_video_sink.h"
#include "platform/sdl2/surface.h"
#include "platform/sdl2/text_raster.h"
#include "player/public/player.h"

namespace {

using namespace std::chrono_literals;  // NOLINT — an example, not the SDK.

constexpr auto kEventPollInterval = 10ms;
constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;

// Set from the player's event dispatch thread, read by the event loop.
std::atomic<bool> g_quit{false};
std::atomic<bool> g_error{false};
std::atomic<bool> g_completed{false};

// The subtitle overlay the host publishes into (surface.h's TextOverlaySlot
// contract): kTimedText events rasterize into it, kStats media time expires
// it. Lives as long as |surface| below.
TextOverlaySlot g_overlay;
avbase::media::TimedTextCue g_current_cue;
std::string g_font_path;
bool g_text_track_selected{false};
int g_video_width{0};
int g_video_height{0};
avbase::Player* g_player{nullptr};  // Set in main; outlives the event loop.

// A first-cut font resolution: the demo is not the SDK. A real host points
// at the user's configured font; these cover this machine's defaults.
std::string FindDemoFont() {
  for (const char* path : {"/System/Library/Fonts/Helvetica.ttc",
                           "/System/Library/Fonts/Supplemental/Arial.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
    if (std::FILE* f = std::fopen(path, "rb")) {
      std::fclose(f);
      return path;
    }
  }
  return "";
}

void OnTimedText(const avbase::TimedTextPayload& payload) {
  if (g_font_path.empty()) {
    return;
  }
  std::fprintf(stderr, "subtitle: %s\n", payload.text.c_str());
  avbase::media::TextRaster raster;
  avbase::media::RgbaBitmap bitmap;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  if (!raster.Rasterize(g_font_path, 28, white, payload.text, &bitmap) ||
      bitmap.rgba.empty()) {
    g_overlay.Clear();
    g_current_cue = avbase::media::TimedTextCue();
    return;
  }
  TextOverlay overlay;
  overlay.width = bitmap.width;
  overlay.height = bitmap.height;
  overlay.rgba = std::move(bitmap.rgba);
  // Placement: an ASS-positioned cue (nonzero w/h, in video pixels) is
  // honoured against the video size; anything else goes bottom-center, the
  // subtitle convention. Fraction units, because the sink scales to the
  // drawable on both presentation paths.
  if (payload.w > 0 && payload.h > 0 && g_video_width > 0 &&
      g_video_height > 0) {
    overlay.x = std::min(1.0f, static_cast<float>(payload.x) /
                                   static_cast<float>(g_video_width));
    overlay.y = std::min(1.0f, static_cast<float>(payload.y) /
                                   static_cast<float>(g_video_height));
    overlay.w =
        std::min(1.0f - overlay.x, static_cast<float>(payload.w) /
                                       static_cast<float>(g_video_width));
    overlay.h =
        std::min(1.0f - overlay.y, static_cast<float>(payload.h) /
                                       static_cast<float>(g_video_height));
  } else {
    constexpr int kWindowWidth = 960, kWindowHeight = 540;
    overlay.w =
        std::min(0.9f, static_cast<float>(overlay.width) / kWindowWidth);
    overlay.h =
        std::min(0.35f, static_cast<float>(overlay.height) / kWindowHeight);
    overlay.x = (1.0f - overlay.w) / 2.0f;
    overlay.y = 1.0f - overlay.h - 0.06f;
  }
  g_overlay.Publish(std::move(overlay));
  g_current_cue.text = payload.text;
  g_current_cue.pts = payload.pts;
  g_current_cue.duration = payload.duration;
}

struct Options {
  std::string url;
  int max_seconds{0};
  // --gl: present through the YUV→RGB shader path (surface.h's gl_context)
  // instead of SDL_Renderer. Default off keeps the verified SDL_Renderer
  // path the default.
  bool gl{false};
};

bool ParseOptions(int argc, char** argv, Options* out) {
  DCHECK(out);
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--url" && i + 1 < argc) {
      out->url = argv[++i];
    } else if (arg == "--max-seconds" && i + 1 < argc) {
      out->max_seconds = std::atoi(argv[++i]);
    } else if (arg == "--gl") {
      out->gl = true;
    }
  }
  if (out->url.empty()) {
    std::fprintf(stderr,
                 "usage: %s --url <file-or-url> "
                 "[--max-seconds n] [--gl]\n",
                 argv[0]);
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
  } else if (e.type == avbase::EventType::kStateChanged) {
    // One-shot: auto-select the first text track once the pipeline is
    // prepared, so the demo shows subtitles without extra flags. A real
    // host drives this from its own track-picker UI.
    const auto* state = avbase::AsStateChanged(e);
    if (!g_text_track_selected && state &&
        state->to == avbase::PlayerState::kPrepared) {
      g_text_track_selected = true;
      if (const auto info = g_player->media_info()) {
        const int text_index =
            info->FirstStreamOfKind(avbase::media::StreamKind::kText);
        if (text_index >= 0) {
          const auto st = g_player->SelectTrack(
              avbase::media::DemuxerStreamType::kText, text_index);
          std::fprintf(stderr, "DBG select(%d)=%d %s\n", text_index,
                       st.has_value() ? 1 : 0,
                       st.has_value() ? "" : st.error().ToString().c_str());
        }
      }
    }
  } else if (e.type == avbase::EventType::kVideoSizeChanged) {
    if (const auto* payload = avbase::AsVideoSizeChanged(e)) {
      g_video_width = payload->natural_size.width;
      g_video_height = payload->natural_size.height;
    }
  } else if (e.type == avbase::EventType::kTimedText) {
    if (const auto* payload = avbase::AsTimedText(e)) {
      OnTimedText(*payload);
    }
  } else if (e.type == avbase::EventType::kStats) {
    // Expire the published cue once the media clock passes its end. The 1 Hz
    // stats cadence makes this coarse by up to a second; a host that cares
    // schedules against its own clock instead.
    if (g_current_cue.duration > avbase::base::TimeDelta() &&
        e.media_time > g_current_cue.pts + g_current_cue.duration) {
      g_overlay.Clear();
      g_current_cue = avbase::media::TimedTextCue();
    }
    std::printf("position %s / %s\n", e.media_time.ToString().c_str(), "--");
  }
}

// A 10-second functional check that the GL runtime actually works: on some
// hosts SDL_GL_CreateContext succeeds but shader-object entry points fail at
// call time. Compile a trivial shader and catch that before playback starts,
// so --gl can fall back instead of presenting nothing.
bool GlRuntimeWorks(SDL_GLContext context) {
  using GlCreateShader = unsigned int (*)(unsigned int);
  using GlShaderSource =
      void (*)(unsigned int, int, const char* const*, const int*);
  using GlCompileShader = void (*)(unsigned int);
  using GlGetShaderiv = void (*)(unsigned int, unsigned int, int*);
  auto create =
      reinterpret_cast<GlCreateShader>(SDL_GL_GetProcAddress("glCreateShader"));
  auto source =
      reinterpret_cast<GlShaderSource>(SDL_GL_GetProcAddress("glShaderSource"));
  auto compile = reinterpret_cast<GlCompileShader>(
      SDL_GL_GetProcAddress("glCompileShader"));
  auto getiv =
      reinterpret_cast<GlGetShaderiv>(SDL_GL_GetProcAddress("glGetShaderiv"));
  if (!create || !source || !compile || !getiv) {
    return false;
  }
  const unsigned int shader = create(0x8B33);  // GL_VERTEX_SHADER
  if (shader == 0) {
    return false;
  }
  const char* src = "void main() { gl_Position = vec4(0.0); }";
  source(shader, 1, &src, nullptr);
  compile(shader);
  int status = 0;
  getiv(shader, 0x8B81, &status);  // GL_COMPILE_STATUS
  auto delete_shader = reinterpret_cast<void (*)(unsigned int)>(
      SDL_GL_GetProcAddress("glDeleteShader"));
  if (delete_shader) {
    delete_shader(shader);
  }
  return status != 0;
}

// Creates the window the host owns (rule 1 in surface.h). With |want_gl| the
// window is created GL-capable and an SDL_GLContext (GL 3.3 core) is created
// for the sink's shader path; the SDL_Renderer is skipped in that mode (the
// two presentation backends cannot share a window). If the GL runtime turns
// out to be broken, the context is discarded and the SDL_Renderer path is
// used instead -- playback always wins over shader purity.
bool CreateSdlWindow(bool want_gl, SDL_Window** window, SDL_Renderer** renderer,
                     SDL_GLContext* gl_context) {
  DCHECK(window);
  DCHECK(renderer);
  DCHECK(gl_context);
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  if (want_gl) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    *window = SDL_CreateWindow(
        "avbase", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWindowWidth,
        kWindowHeight,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL);
    if (*window) {
      *gl_context = SDL_GL_CreateContext(*window);
      if (*gl_context && GlRuntimeWorks(*gl_context)) {
        return true;
      }
      if (!*gl_context) {
        std::fprintf(stderr, "SDL GL context failed: %s\n", SDL_GetError());
      } else {
        std::fprintf(stderr,
                     "GL runtime broken on this host (shader creation fails);"
                     " falling back to SDL_Renderer\n");
        SDL_GL_MakeCurrent(*window, nullptr);
        SDL_GL_DeleteContext(*gl_context);
        *gl_context = nullptr;
      }
      SDL_DestroyWindow(*window);
      *window = nullptr;
    } else {
      std::fprintf(stderr, "SDL GL window failed: %s\n", SDL_GetError());
    }
    // Fall through to the SDL_Renderer path.
  }
  *window = SDL_CreateWindow(
      "avbase", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWindowWidth,
      kWindowHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
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
  g_font_path = FindDemoFont();
  auto display = avbase::media::NativeDisplay::FromSdl2Window(&surface);

  auto deps = std::make_unique<avbase::Deps>();
  deps->video_sink_factory =
      std::make_shared<avbase::media::Sdl2VideoSinkFactory>();
  deps->audio_sink_factory =
      std::make_shared<avbase::media::Sdl2AudioSinkFactory>();

  avbase::Player player(avbase::PlayerConfig(), std::move(deps));
  g_player = &player;
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
  // L6: Stop() is asynchronous; tearing SDL resources down against a player
  // that may still be flushing races the sink's teardown. StopSync() blocks
  // until the pipeline actually finished stopping (see player.h for the
  // Stop/StopSync contract).
  player.StopSync(base::Seconds(5));

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
