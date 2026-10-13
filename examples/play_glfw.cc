// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// GLFW playback: the GLFW backend acceptance example. Embedding mode
// (docs/archive/09 §2) -- the host owns the GLFWwindow and its GL context,
// avbase renders into it through GlPresenter, and the main thread's only job is
// glfwPollEvents and quitting. Audio goes through the null sink, since GLFW
// ships no audio API of its own.
//
//   ./play_glfw --url video.mp4 [--max-seconds n] [--stats] [--canvas WxH]
//                [--scene pip|fade] [--second video2.mp4]
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

// GLFW pulls in the platform's GL header by default. Nothing in this example
// calls GL directly today -- but the moment it does (a screenshot, a probe
// shader), the platform header collides with glad's and the build fails with
// "OpenGL (gl.h) header already included". GLFW_INCLUDE_NONE is the supported
// way to say "no GL header, thanks", and it must come first.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "examples/scene_demo.h"
#include "media/filters/null_audio_sink.h"
#include "platform/glfw/glfw_surface.h"
#include "platform/glfw/glfw_video_sink.h"
#include "player/public/deps.h"
#include "player/public/player.h"

namespace {

using namespace std::chrono_literals;  // NOLINT -- an example, not the SDK.

constexpr auto kEventPollInterval = 10ms;
constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;

std::atomic<bool> g_quit{false};
std::atomic<bool> g_error{false};
std::atomic<bool> g_completed{false};
// --stats: print the full PlaybackStats JSON on each one-second tick instead
// of the position line. Off by default so ordinary runs stay readable.
std::atomic<bool> g_print_stats{false};

avbase::Player* g_player{nullptr};  // Set in main; outlives the event loop.

// --scene: the multi-source demo. Held by the process rather than by main
// because the scene's callback outlives main's locals -- the player owns the
// scene, and it outlives the frame it was configured on.
std::unique_ptr<avbase::examples::SceneDemo> g_scene;

// The source the second layer's frames arrive in, when --second was given.
// Registered straight away on the player's scene rather than smuggled out of a
// sink factory; see examples/scene_demo.h on why neither is hard any more.
std::shared_ptr<avbase::SceneSource> g_second_source;
// Its URL, kept next to it so the kPrepared handler can act on it.
std::string g_second_url;
// The second pipeline itself, torn down before the first player's sink so the
// mailbox outlives its producer.
std::unique_ptr<avbase::Player> g_second_player;

struct Options {
  std::string url;
  int max_seconds{0};
  bool stats{false};
  // --canvas WxH composes at a fixed resolution and scales to the window;
  // 0 (the default) means the composition surface follows the window.
  int canvas_width{0};
  int canvas_height{0};
  // --scene pip|fade adds a second layer to the composition so the whole
  // scene path runs on a real window. Empty means the plain single-source
  // path. See examples/scene_demo.h.
  std::string scene;
  // --second <url>: where the second layer's frames come from. Empty means
  // the delayed-copy fallback in SceneDemo.
  std::string second;
  // --vf0 <graph>: a CPU avfilter graph applied to every decoded frame, e.g.
  // hflip. Empty (the default) means unfiltered. It cannot run on a
  // hardware-decoded frame -- see the note in video_renderer_impl.cc.
  std::string vf0;
};

bool ParseOptions(int argc, char** argv, Options* out) {
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
    } else if (arg == "--vf0" && i + 1 < argc) {
      out->vf0 = argv[++i];
    } else if (arg == "--stats") {
      out->stats = true;
    }
  }
  if (out->url.empty()) {
    std::fprintf(stderr,
                 "usage: %s --url <file-or-url> [--max-seconds n] [--stats]\n"
                 "       [--canvas WxH] [--scene pip|fade] [--second <url>]\n"
                 "       [--vf0 <graph>]\n",
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
  } else if (e.type == avbase::EventType::kPrepared) {
    // Not because the source only exists now -- it was registered before
    // Prepare -- but because the first stream has to be running before a
    // second decoder is worth the CPU.
    if (g_second_source) {
      g_second_player =
          avbase::examples::StartSecondSource(g_second_url, g_second_source);
      if (!g_second_player) {
        g_error.store(true);
      }
    }
  } else if (e.type == avbase::EventType::kStats) {
    if (g_print_stats.load()) {
      if (const auto* payload = avbase::AsStats(e)) {
        // One line of JSON per tick, so a benchmark harness can parse it
        // without a second tool.
        std::printf("stats %s\n", payload->stats.ToJson().c_str());
      }
      return;
    }
    std::printf("position %s / --\n", e.media_time.ToString().c_str());
  }
}

// A quick GL runtime sanity check (mirrors play_sdl2's): some drivers let
// glfwCreateWindow succeed but fail to resolve/compile shaders. Compile a
// trivial shader before playback so we report it instead of presenting
// nothing.
bool GlRuntimeWorks() {
  using GlCreateShader = unsigned int (*)(unsigned int);
  using GlShaderSource =
      void (*)(unsigned int, int, const char* const*, const int*);
  using GlCompileShader = void (*)(unsigned int);
  using GlGetShaderiv = void (*)(unsigned int, unsigned int, int*);
  auto create =
      reinterpret_cast<GlCreateShader>(glfwGetProcAddress("glCreateShader"));
  auto source =
      reinterpret_cast<GlShaderSource>(glfwGetProcAddress("glShaderSource"));
  auto compile =
      reinterpret_cast<GlCompileShader>(glfwGetProcAddress("glCompileShader"));
  auto getiv =
      reinterpret_cast<GlGetShaderiv>(glfwGetProcAddress("glGetShaderiv"));
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
      glfwGetProcAddress("glDeleteShader"));
  if (delete_shader) {
    delete_shader(shader);
  }
  return status != 0;
}

bool CreateGlfwWindow(GLFWwindow** window) {
  DCHECK(window);
  if (glfwInit() != GLFW_TRUE) {
    std::fprintf(stderr, "glfwInit failed\n");
    return false;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
  *window = glfwCreateWindow(kWindowWidth, kWindowHeight, "avbase (glfw)",
                             nullptr, nullptr);
  if (!*window) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return false;
  }
  // Make the context current once so the probe can resolve entry points; the
  // sink re-makes it current on its own render thread.
  glfwMakeContextCurrent(*window);
  if (!GlRuntimeWorks()) {
    std::fprintf(stderr,
                 "GL runtime broken on this host; cannot present via GLFW\n");
    glfwMakeContextCurrent(nullptr);
    glfwDestroyWindow(*window);
    glfwTerminate();
    *window = nullptr;
    return false;
  }
  // Release on the main thread; the render thread takes over in Start().
  glfwMakeContextCurrent(nullptr);
  return true;
}

// The main thread's only job: pump GLFW events until the player completes,
// errors, or the --max-seconds cap expires. Returns the process exit code.
int PumpEventsUntilDone(GLFWwindow* window, int max_seconds) {
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::seconds(max_seconds > 0 ? max_seconds : 86400);
  while (!g_quit.load() && !g_error.load() && !g_completed.load() &&
         std::chrono::steady_clock::now() < deadline) {
    glfwPollEvents();
    if (glfwWindowShouldClose(window) ||
        glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
      g_quit.store(true);
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

  GLFWwindow* window = nullptr;
  if (!CreateGlfwWindow(&window)) {
    return 1;
  }

  // The host owns the GLFW window; the display just carries the pointer
  // through (glfw_surface.h). The sink borrows it for presentation.
  GlfwSurface surface;
  surface.window = window;
  surface.overlay = nullptr;
  surface.canvas_width = options.canvas_width;
  surface.canvas_height = options.canvas_height;
  auto display = avbase::media::NativeDisplay::FromGlfwWindow(&surface);

  auto deps = std::make_unique<avbase::Deps>();
  if (!options.scene.empty()) {
    g_scene = std::make_unique<avbase::examples::SceneDemo>(options.scene);
  }
  // The GL backend is all this host needs to name; nothing here reaches for the
  // SINK the way it used to. The scene is on the Player, so the demo installs
  // its hook and registers its source directly, and gets a real answer instead
  // of a cast when this player's sink cannot composite.
  deps->video_sink_factory =
      std::make_shared<avbase::media::GlfwVideoSinkFactory>();
  deps->audio_sink_factory =
      std::make_shared<avbase::media::NullAudioSinkFactory>();

  // --vf0 is the only way in from the command line; PlayerConfig is the same
  // field a host or an SDK caller sets directly.
  avbase::PlayerConfig config;
  config.video.filter_graph = options.vf0;
  avbase::Player player(config, std::move(deps));
  g_player = &player;
  g_print_stats.store(options.stats);
  player.SetEventHandler(avbase::base::BindRepeating(&OnPlayerEvent));
  player.SetVideoSurface(display);
  g_second_url = options.second;
  if (g_scene != nullptr) {
    // Any time, including before SetDataSource: neither the hook nor the
    // source is a pipeline object, so there is nothing to wait for.
    g_scene->set_second_url(options.second);
    g_scene->Install(player);
    g_second_source = g_scene->second_source();
  }
  if (const avbase::Status s = player.SetDataSource(options.url); !s) {
    std::fprintf(stderr, "SetDataSource failed: %s\n",
                 s.error().ToString().c_str());
    player.StopSync(avbase::base::Seconds(5));
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }
  if (const avbase::Status s = player.PrepareAsync(); !s) {
    std::fprintf(stderr, "PrepareAsync failed: %s\n",
                 s.error().ToString().c_str());
    player.StopSync(avbase::base::Seconds(5));
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }
  if (options.stats) {
    // AFTER prepare, so it says whether the hook and the source were actually
    // plumbed into the renderer that got built -- the one number that
    // separates "the PiP is misconfigured" from "there is no PiP at all".
    std::fprintf(stderr, "scene: %s\n", player.scene()->Describe().c_str());
  }

  const int exit_code = PumpEventsUntilDone(window, options.max_seconds);
  if (options.stats && g_scene != nullptr) {
    std::fprintf(stderr, "scene: hook placed %llu frame(s), scene now %s\n",
                 static_cast<unsigned long long>(g_scene->frames_placed()),
                 player.scene()->Describe().c_str());
  }
  // The second pipeline first: it publishes into a mailbox the first player's
  // sink owns, so it has to be quiet before that sink is destroyed.
  avbase::examples::StopSecondSource(std::move(g_second_player),
                                     g_second_source);
  g_second_source.reset();
  // StopSync() blocks until the pipeline actually finished stopping, so the
  // sink's render thread (and its GL context) is gone before we destroy the
  // window here.
  player.StopSync(avbase::base::Seconds(5));

  glfwDestroyWindow(window);
  glfwTerminate();
  return exit_code;
}
