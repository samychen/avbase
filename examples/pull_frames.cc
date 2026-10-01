// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// pull_frames: the zero-copy frame-acquisition example (Phase 3).
//
// It injects a custom VideoRendererSink whose render loop keeps the frames
// the compositor hands it -- the same seam a GPU compositor uses -- and
// reports, per frame, whether the pixels stayed GPU-resident
// (VideoFrame::NativeHandle) or were CPU memory. Nothing is read back here:
// calling ToI420() would defeat the point of the example. Exit code 0 when
// the requested number of frames was captured.
//
// On a machine with a working hardware decoder this prints
// `handle=cvpixelbuffer`-style lines and proves the GPU path end to end; on a
// machine without one the fallback chain silently uses the software decoder
// and the lines read `storage=cpu` -- which is itself the fallback contract
// working as designed.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "base/time/time.h"
#include "media/base/video_renderer_sink.h"
#include "player/public/deps.h"
#include "player/public/player.h"

namespace {

using namespace std::chrono_literals;  // NOLINT — an example, not the SDK.

using namespace avbase;  // NOLINT — an example; base/ and media/ live
                         // under avbase.

using avbase::media::NativeHandleKind;
using avbase::media::VideoFrame;
using avbase::media::VideoRendererSink;
using avbase::media::VideoSinkStats;

// The captured frame store. Render() runs on the sink's poll thread; the
// reader is main(), so the reference swap goes through a mutex rather than
// racing two scoped_refptrs on one pointee.
class FrameStore {
 public:
  void Store(base::scoped_refptr<VideoFrame> frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_ = std::move(frame);
    ++frames_seen_;
  }
  base::scoped_refptr<VideoFrame> Load(int64_t* seen) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (seen) {
      *seen = frames_seen_;
    }
    return frame_;
  }
  int64_t native_frames() const { return native_frames_.load(); }
  std::atomic<int64_t> native_frames_{0};

 private:
  mutable std::mutex mutex_;
  base::scoped_refptr<VideoFrame> frame_;
  int64_t frames_seen_{0};
};

// Renders nowhere; polls the upstream (compositor) callback at 60 Hz and
// snapshots whatever frame is current. The frame reference is what keeps the
// GPU surface alive for as long as the reader wants it.
class FrameCaptureSink final : public VideoRendererSink {
 public:
  explicit FrameCaptureSink(FrameStore* store) : store_(store) {}

  void Initialize(RenderCallback* callback) override { upstream_ = callback; }
  void Start() override {
    running_.store(true);
    playing_.store(true);
    worker_ = std::thread([this] { PollLoop(); });
  }
  void Stop() override {
    running_.store(false);
    if (worker_.joinable()) {
      worker_.join();
    }
  }
  void Pause() override { playing_.store(false); }
  void Play() override { playing_.store(true); }
  void Flush() override {}
  void SetOutputTarget(base::scoped_refptr<
                       avbase::media::NativeDisplay> /*display*/) override {}
  bool IsRunning() const override { return running_.load(); }
  bool GetDisplayInterval(base::TimeDelta* interval) const override {
    if (interval) {
      *interval = base::Milliseconds(16);
    }
    return true;
  }
  VideoSinkStats GetStats() const override {
    VideoSinkStats stats;
    stats.frames_presented = static_cast<uint64_t>(presented_.load());
    return stats;
  }
  const char* name() const override { return "frame-capture"; }

 private:
  void PollLoop() {
    while (running_.load()) {
      if (playing_.load() && upstream_) {
        auto frame = upstream_->Render(base::TimeTicks(), base::TimeTicks());
        if (frame) {
          if (!frame->IsMappable()) {
            store_->native_frames_.fetch_add(1);
          }
          store_->Store(std::move(frame));
          presented_.fetch_add(1);
        }
      }
      std::this_thread::sleep_for(16ms);
    }
  }

  RenderCallback* upstream_{nullptr};
  FrameStore* store_;
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> playing_{false};
  std::atomic<int64_t> presented_{0};
};

class FrameCaptureSinkFactory final
    : public avbase::media::VideoRendererSinkFactory {
 public:
  explicit FrameCaptureSinkFactory(FrameStore* store) : store_(store) {}
  std::unique_ptr<VideoRendererSink> Create(
      base::scoped_refptr<avbase::media::NativeDisplay> /*display*/)
      override {
    return std::make_unique<FrameCaptureSink>(store_);
  }
  const char* name() const override { return "frame-capture-factory"; }

 private:
  FrameStore* store_;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <url> [--max-frames n] [--timeout seconds]\n",
                 argv[0]);
    return 2;
  }
  const std::string url = argv[1];
  int max_frames = 30;
  int timeout_seconds = 60;
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    if (flag == "--max-frames") {
      max_frames = std::atoi(argv[i + 1]);
    } else if (flag == "--timeout") {
      timeout_seconds = std::atoi(argv[i + 1]);
    }
  }

  FrameStore store;
  avbase::PlayerConfig config;
  auto deps = std::make_unique<avbase::Deps>();
  deps->video_sink_factory =
      std::make_shared<FrameCaptureSinkFactory>(&store);
  avbase::Player player(config, std::move(deps));

  if (const avbase::Status s = player.SetDataSource(url); !s) {
    std::fprintf(stderr, "SetDataSource failed: %s\n",
                 s.error().ToString().c_str());
    return 1;
  }
  if (const avbase::Status s = player.PrepareSync(); !s) {
    std::fprintf(stderr, "Prepare failed: %s\n",
                 s.error().ToString().c_str());
    return 1;
  }
  player.Start();

  int reported = 0;
  int64_t last_seen = -1;
  const auto deadline = std::chrono::steady_clock::now() + timeout_seconds * 1s;
  while (reported < max_frames) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "timed out after %d frames\n", reported);
      return 1;
    }
    int64_t seen = 0;
    auto frame = store.Load(&seen);
    if (frame && seen != last_seen) {
      last_seen = seen;
      ++reported;
      const auto handle = frame->native_handle();
      std::string storage =
          handle.kind != NativeHandleKind::kNone
              ? std::string("handle=") +
                    avbase::media::GetNativeHandleKindName(handle.kind)
              : std::string("storage=cpu");
      std::printf("frame %3d  %dx%d  ts=%-9s  %s\n", reported,
                  frame->natural_size().width, frame->natural_size().height,
                  frame->timestamp().ToString().c_str(), storage.c_str());
    }
    std::this_thread::sleep_for(10ms);
  }

  player.Stop();
  std::printf("captured %d frames (%lld native-handle)\n", reported,
              static_cast<long long>(store.native_frames()));
  return 0;
}
