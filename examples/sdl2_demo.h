// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The SDL2 host's boilerplate, split out of play_sdl2.cc.
//
// WHY THIS FILE EXISTS. docs/10 §12 holds the acceptance example's main file
// under 300 lines -- "a newcomer can read the whole program" is the API
// usability metric (G13: clone to first frame in 15 minutes). play_sdl2.cc
// grew past that carrying HOST POLICY that is not the SDK's story: option
// parsing, event logging, subtitle rasterization, the GL runtime probe and
// window creation. That code moved here; what remains in play_sdl2.cc is the
// program: build a window, hand the SDK its deps, prepare, pump events, stop.
//
// Same arrangement as scene_demo.h next to it: host policy lives in examples/,
// never in the library, and the library never learns what a demo flag is.
//
// The file-scope state below is shared with play_sdl2.cc on purpose: the event
// handlers run on the player's dispatch thread and the event loop runs on the
// main thread, which is exactly the two-thread shape docs/04 §2 describes for
// a host. inline variables (C++17) keep one definition between the two TUs.

#ifndef AVBASE_EXAMPLES_SDL2_DEMO_H_
#define AVBASE_EXAMPLES_SDL2_DEMO_H_

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "SDL.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "examples/scene_demo.h"
#include "media/base/timed_text.h"
#include "platform/sdl2/surface.h"
#include "platform/sdl2/text_raster.h"
#include "player/public/player.h"

namespace avbase::examples::sdl2 {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;

// Set from the player's event dispatch thread, read by the event loop.
inline std::atomic<bool> g_quit{false};
inline std::atomic<bool> g_error{false};
inline std::atomic<bool> g_completed{false};
// --stats: print the full PlaybackStats JSON on each one-second tick instead
// of the position line.
inline std::atomic<bool> g_print_stats{false};

// The subtitle overlay the host publishes into (surface.h's TextOverlaySlot
// contract): kTimedText events rasterize into it, kStats media time expires
// it. Lives as long as |surface| in play_sdl2.cc.
inline avbase::gl::TextOverlaySlot g_overlay;
inline avbase::media::TimedTextCue g_current_cue;
inline std::string g_font_path;
inline bool g_text_track_selected{false};
inline int g_video_width{0};
inline int g_video_height{0};
inline avbase::Player* g_player{nullptr};  // Set in main; outlives the loop.

// --scene: the multi-source demo. Held by the process rather than by main
// because the scene outlives main's locals -- the player owns the scene.
inline std::unique_ptr<avbase::examples::SceneDemo> g_scene;

// The second source used to live here -- the SceneSource the layer's frames
// arrive in, the URL they come from, and the second Player instance that
// decoded them. All three are gone as of docs/14 stage 3: the second source is
// a second DEMUXER IN THE SAME PIPELINE, so there is no second Player to hold,
// no window for it to not have, and no teardown order to respect. --second's
// URL now goes to Player::AddSource() inside SceneDemo::Install().

// A first-cut font resolution: the demo is not the SDK. A real host points
// at the user's configured font; these cover this machine's defaults.
inline std::string FindDemoFont() {
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

inline void OnTimedText(const avbase::TimedTextPayload& payload) {
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
  avbase::gl::TextOverlay overlay;
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
    // kWindowWidth/kWindowHeight above: scaling the overlay to fractions of
    // the window is the same default the window was created with.
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
  bool stats{false};
  // --canvas WxH: compose at a fixed resolution on the GL path and scale to
  // the window. Needs --gl; the SDL_Renderer path has no off-screen target.
  int canvas_width{0};
  int canvas_height{0};
  // --scene pip|fade: add a second layer to the GL path's composition. Needs
  // --gl for the same reason --canvas does. See examples/scene_demo.h.
  std::string scene;
  // --second <url>: where the second layer's frames come from. Empty means
  // the delayed-copy fallback in SceneDemo. Needs --gl.
  std::string second;
};

inline bool ParseOptions(int argc, char** argv, Options* out) {
  DCHECK(out);
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--url" && i + 1 < argc) {
      out->url = argv[++i];
    } else if (arg == "--max-seconds" && i + 1 < argc) {
      out->max_seconds = std::atoi(argv[++i]);
    } else if (arg == "--canvas" && i + 1 < argc) {
      if (std::sscanf(argv[++i], "%dx%d", &out->canvas_width,
                      &out->canvas_height) != 2 ||
          out->canvas_width <= 0 || out->canvas_height <= 0) {
        std::fprintf(stderr, "--canvas wants WxH, e.g. --canvas 1920x1080\n");
        return false;
      }
    } else if (arg == "--scene" && i + 1 < argc) {
      out->scene = argv[++i];
      if (!avbase::examples::SceneDemo::IsKnownMode(out->scene)) {
        std::fprintf(stderr, "--scene wants pip or fade\n");
        return false;
      }
    } else if (arg == "--second" && i + 1 < argc) {
      out->second = argv[++i];
    } else if (arg == "--gl") {
      out->gl = true;
    } else if (arg == "--stats") {
      out->stats = true;
    }
  }
  if (out->url.empty()) {
    std::fprintf(stderr,
                 "usage: %s --url <file-or-url> "
                 "[--max-seconds n] [--gl] [--stats] [--canvas WxH]\n"
                 "       [--scene pip|fade] [--second <url>]\n",
                 argv[0]);
    return false;
  }
  if ((out->canvas_width > 0 || out->canvas_height > 0) && !out->gl) {
    std::fprintf(stderr,
                 "--canvas needs --gl: the SDL_Renderer path has no "
                 "off-screen composition target\n");
    return false;
  }
  if (!out->scene.empty() && !out->gl) {
    std::fprintf(stderr,
                 "--scene needs --gl: the SDL_Renderer path draws straight "
                 "to the window and has nowhere to compose a second layer\n");
    return false;
  }
  // Same reason, and a stronger one: a second layer needs somewhere to BE,
  // and the SDL_Renderer path has no composition surface -- every frame it
  // draws goes straight to the window.
  if (!out->second.empty() && !out->gl) {
    std::fprintf(stderr,
                 "--second needs --gl for the same reason --scene "
                 "does: the SDL_Renderer path has no composition "
                 "surface to put a second layer on\n");
    return false;
  }
  return true;
}

inline void OnPlayerEvent(const avbase::PlayerEvent& e) {
  if (e.type == avbase::EventType::kError) {
    if (const auto* payload = avbase::AsError(e)) {
      std::fprintf(stderr, "error: %s\n", payload->error.ToString().c_str());
    }
    g_error.store(true);
  } else if (e.type == avbase::EventType::kCompleted) {
    g_completed.store(true);
    // The kPrepared branch that used to be here STARTED A SECOND PLAYER for
    // --second. There is nothing left for it to do: the second source belongs
    // to this pipeline, so preparing one prepared both.
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
          g_player->SelectTrack(avbase::media::DemuxerStreamType::kText,
                                text_index);
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
    if (g_print_stats.load()) {
      if (const auto* payload = avbase::AsStats(e)) {
        std::printf("stats %s\n", payload->stats.ToJson().c_str());
      }
      return;
    }
    std::printf("position %s / %s\n", e.media_time.ToString().c_str(), "--");
  }
}

// A 10-second functional check that the GL runtime actually works: on some
// hosts SDL_GL_CreateContext succeeds but shader-object entry points fail at
// call time. Compile a trivial shader and catch that before playback starts,
// so --gl can fall back instead of presenting nothing.
//
// Deliberately takes no context argument: SDL_GL_GetProcAddress resolves
// against whichever context is current, which is the one SDL_GL_CreateContext
// just installed. A parameter would only imply the probe can choose one.
inline bool GlRuntimeWorks() {
  auto create = reinterpret_cast<unsigned int (*)(unsigned int)>(
      SDL_GL_GetProcAddress("glCreateShader"));
  auto source = reinterpret_cast<void (*)(unsigned int, int, const char* const*,
                                          const int*)>(
      SDL_GL_GetProcAddress("glShaderSource"));
  auto compile = reinterpret_cast<void (*)(unsigned int)>(
      SDL_GL_GetProcAddress("glCompileShader"));
  auto getiv = reinterpret_cast<void (*)(unsigned int, unsigned int, int*)>(
      SDL_GL_GetProcAddress("glGetShaderiv"));
  if (!create || !source || !compile || !getiv) {
    return false;
  }
  // 0x8B31 = GL_VERTEX_SHADER. Note it is NOT 0x8B33 (GL_GEOMETRY_SHADER):
  // geometry shaders need layout/EmitVertex declarations, so probing with one
  // falsely reports a broken GL runtime.
  const unsigned int shader = create(0x8B31);
  if (shader == 0) {
    return false;
  }
  // "#version" is mandatory: without it a Core Profile context treats the
  // source as GLSL 1.10, which a core profile driver rejects -- the probe
  // would report a broken GL runtime even though the card is fine.
  const char* src =
      "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }";
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
inline bool CreateSdlWindow(bool want_gl, SDL_Window** window,
                            SDL_Renderer** renderer,
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
      if (*gl_context && GlRuntimeWorks()) {
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

}  // namespace avbase::examples::sdl2

#endif  // AVBASE_EXAMPLES_SDL2_DEMO_H_
