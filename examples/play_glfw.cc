// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// GLFW playback: the GLFW backend acceptance example. Embedding mode
// (docs/09 §2) -- the host owns the GLFWwindow and its GL context,
// avbase renders into it through GlPresenter, and the main thread's only job is
// glfwPollEvents and quitting. Audio goes through the null sink, since GLFW
// ships no audio API of its own.
//
//   ./play_glfw --url video.mp4 [--max-seconds n]
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

#include <GLFW/glfw3.h>

#include "base/check.h"
#include "base/time/time.h"
#include "media/filters/null_audio_sink.h"
#include "platform/glfw/glfw_surface.h"
#include "platform/glfw/glfw_video_sink.h"
#include "player/public/player.h"

namespace {

using namespace std::chrono_literals;  // NOLINT -- an example, not the SDK.

constexpr auto kEventPollInterval = 10ms;
constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;

std::atomic<bool> g_quit{false};
std::atomic<bool> g_error{false};
std::atomic<bool> g_completed{false};

avbase::Player* g_player{nullptr};  // Set in main; outlives the event loop.

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
    std::fprintf(stderr, "usage: %s --url <file-or-url> [--max-seconds n]\n",
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
  } else if (e.type == avbase::EventType::kStats) {
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
  auto display = avbase::media::NativeDisplay::FromGlfwWindow(&surface);

  auto deps = std::make_unique<avbase::Deps>();
  deps->video_sink_factory =
      std::make_shared<avbase::media::GlfwVideoSinkFactory>();
  deps->audio_sink_factory =
      std::make_shared<avbase::media::NullAudioSinkFactory>();

  avbase::Player player(avbase::PlayerConfig(), std::move(deps));
  g_player = &player;
  player.SetEventHandler(avbase::base::BindRepeating(&OnPlayerEvent));
  player.SetVideoSurface(display);
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

  const int exit_code = PumpEventsUntilDone(window, options.max_seconds);
  // StopSync() blocks until the pipeline actually finished stopping, so the
  // sink's render thread (and its GL context) is gone before we destroy the
  // window here.
  player.StopSync(avbase::base::Seconds(5));

  glfwDestroyWindow(window);
  glfwTerminate();
  return exit_code;
}
