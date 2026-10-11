// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// EGL playback: the EGL backend acceptance example. Embedding mode
// (docs/archive/09 §2) -- the host owns the EGLDisplay / EGLSurface /
// EGLContext and carries them through NativeDisplay::FromEglSurface(); avbase
// renders into the surface through GlPresenter. Audio goes through the null
// sink.
//
// NOTE: this example targets a Linux EGL stack (Mesa/GBM, X11 EGL, ...). It
// is NOT buildable on macOS, which has no EGL development libraries; it is
// gated behind -DAVBASE_ENABLE_EGL=ON and only compiled where EGL is present.
// A real host builds the EGLSurface from its own window system; here a pbuffer
// surface keeps the example self-contained and window-system-free.
//
//   ./play_egl --url video.mp4 [--max-seconds n]
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

#include <EGL/egl.h>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/time/time.h"
#include "media/filters/null_audio_sink.h"
#include "platform/egl/egl_surface.h"
#include "platform/egl/egl_video_sink.h"
#include "player/public/player.h"

namespace {

using namespace std::chrono_literals;  // NOLINT -- an example, not the SDK.

constexpr auto kPollInterval = 10ms;

std::atomic<bool> g_quit{false};
std::atomic<bool> g_error{false};
std::atomic<bool> g_completed{false};

avbase::Player* g_player{nullptr};

// EGL handles the host owns for the playback lifetime.
EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;

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

// A quick GL runtime sanity check using eglGetProcAddress (mirrors
// play_sdl2 / play_glfw). Some drivers let eglCreateContext succeed but fail
// to resolve/compile shaders; catch that before playback starts.
bool GlRuntimeWorks() {
  using GlCreateShader = unsigned int (*)(unsigned int);
  using GlShaderSource =
      void (*)(unsigned int, int, const char* const*, const int*);
  using GlCompileShader = void (*)(unsigned int);
  using GlGetShaderiv = void (*)(unsigned int, unsigned int, int*);
  auto create =
      reinterpret_cast<GlCreateShader>(eglGetProcAddress("glCreateShader"));
  auto source =
      reinterpret_cast<GlShaderSource>(eglGetProcAddress("glShaderSource"));
  auto compile =
      reinterpret_cast<GlCompileShader>(eglGetProcAddress("glCompileShader"));
  auto getiv =
      reinterpret_cast<GlGetShaderiv>(eglGetProcAddress("glGetShaderiv"));
  if (!create || !source || !compile || !getiv) {
    return false;
  }
  // 0x8B31 = GL_VERTEX_SHADER (not 0x8B33, which is GL_GEOMETRY_SHADER).
  const unsigned int shader = create(0x8B31);
  if (shader == 0) {
    return false;
  }
  // Same rule as the desktop probes, but ES contexts need "#version 300 es".
  // Without it the driver falls back to GLSL ES 1.00 and rejects the source.
  const char* src = "#version 300 es\nvoid main() { gl_Position = vec4(0.0); }";
  source(shader, 1, &src, nullptr);
  compile(shader);
  int status = 0;
  getiv(shader, 0x8B81, &status);  // GL_COMPILE_STATUS
  auto delete_shader = reinterpret_cast<void (*)(unsigned int)>(
      eglGetProcAddress("glDeleteShader"));
  if (delete_shader) {
    delete_shader(shader);
  }
  return status != 0;
}

bool CreateEglResources() {
  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY) {
    std::fprintf(stderr, "eglGetDisplay failed\n");
    return false;
  }
  EGLint major = 0;
  EGLint minor = 0;
  if (eglInitialize(g_display, &major, &minor) != EGL_TRUE) {
    std::fprintf(stderr, "eglInitialize failed (0x%x)\n", eglGetError());
    return false;
  }
  const EGLint config_attribs[] = {EGL_SURFACE_TYPE,
                                   EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE,
                                   EGL_OPENGL_ES3_BIT,
                                   EGL_RED_SIZE,
                                   8,
                                   EGL_GREEN_SIZE,
                                   8,
                                   EGL_BLUE_SIZE,
                                   8,
                                   EGL_NONE};
  EGLConfig config = nullptr;
  EGLint num_config = 0;
  if (eglChooseConfig(g_display, config_attribs, &config, 1, &num_config) !=
          EGL_TRUE ||
      num_config < 1) {
    std::fprintf(stderr, "eglChooseConfig failed (0x%x)\n", eglGetError());
    return false;
  }
  const EGLint pbuffer_attribs[] = {EGL_WIDTH, 960, EGL_HEIGHT, 540, EGL_NONE};
  g_surface = eglCreatePbufferSurface(g_display, config, pbuffer_attribs);
  if (g_surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "eglCreatePbufferSurface failed (0x%x)\n",
                 eglGetError());
    return false;
  }
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  g_context =
      eglCreateContext(g_display, config, EGL_NO_CONTEXT, context_attribs);
  if (g_context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "eglCreateContext failed (0x%x)\n", eglGetError());
    return false;
  }
  if (eglMakeCurrent(g_display, g_surface, g_surface, g_context) != EGL_TRUE) {
    std::fprintf(stderr, "eglMakeCurrent failed (0x%x)\n", eglGetError());
    return false;
  }
  if (!GlRuntimeWorks()) {
    std::fprintf(stderr,
                 "GL runtime broken on this host; cannot present via EGL\n");
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return false;
  }
  // Release on the main thread; the render thread takes over in Start().
  eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return true;
}

void DestroyEglResources() {
  if (g_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_context != EGL_NO_CONTEXT) {
      eglDestroyContext(g_display, g_context);
    }
    if (g_surface != EGL_NO_SURFACE) {
      eglDestroySurface(g_display, g_surface);
    }
    eglTerminate(g_display);
  }
  g_display = EGL_NO_DISPLAY;
  g_surface = EGL_NO_SURFACE;
  g_context = EGL_NO_CONTEXT;
}

int PumpUntilDone(int max_seconds) {
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::seconds(max_seconds > 0 ? max_seconds : 86400);
  while (!g_quit.load() && !g_error.load() && !g_completed.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(kPollInterval);
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

  if (!CreateEglResources()) {
    return 1;
  }

  // The host owns the EGL handles; the display carries them through
  // egl_surface.h. The sink borrows them for presentation.
  EglSurface surface;
  surface.display = g_display;
  surface.surface = g_surface;
  surface.context = g_context;
  surface.overlay = nullptr;
  auto display = avbase::media::NativeDisplay::FromEglSurface(&surface);

  auto deps = std::make_unique<avbase::Deps>();
  deps->video_sink_factory =
      std::make_shared<avbase::media::EglVideoSinkFactory>();
  deps->audio_sink_factory =
      std::make_shared<avbase::media::NullAudioSinkFactory>();

  avbase::Player player(avbase::PlayerConfig(), std::move(deps));
  g_player = &player;
  player.SetEventHandler(avbase::base::BindRepeating(&OnPlayerEvent));
  player.SetVideoSurface(display);
  if (const avbase::Status s = player.SetDataSource(options.url); !s) {
    std::fprintf(stderr, "SetDataSource failed: %s\n",
                 s.error().ToString().c_str());
    DestroyEglResources();
    return 1;
  }
  if (const avbase::Status s = player.PrepareAsync(); !s) {
    std::fprintf(stderr, "PrepareAsync failed: %s\n",
                 s.error().ToString().c_str());
    DestroyEglResources();
    return 1;
  }

  const int exit_code = PumpUntilDone(options.max_seconds);
  // StopSync() blocks until the pipeline stopped, so the sink's render thread
  // (and its EGL context) is gone before we tear the EGL resources down here.
  player.StopSync(avbase::base::Seconds(5));

  DestroyEglResources();
  return exit_code;
}
