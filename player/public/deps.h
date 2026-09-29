// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_PLAYER_PUBLIC_DEPS_H_
#define IJKPP_PLAYER_PUBLIC_DEPS_H_

#include <memory>
#include <vector>

#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/tick_clock.h"
#include "player/public/player_event.h"
#include "player/public/player_export.h"

namespace ijkpp {
namespace media {
class AudioDecoderFactory;
class AudioRendererSinkFactory;
class DataSource;
class Demuxer;
class VideoDecoderFactory;
class VideoRendererSinkFactory;
}  // namespace media

// Routes events to the host's own loop (Android Looper, GLib main context,
// Qt event loop). The default dispatcher is a dedicated FIFO thread, which
// preserves event order and guarantees that no pipeline sequence is ever
// blocked inside a user callback.
class IJKPP_PLAYER_EXPORT EventDispatcher {
 public:
  EventDispatcher(const EventDispatcher&) = delete;
  EventDispatcher& operator=(const EventDispatcher&) = delete;
  virtual void Post(base::OnceClosure task) = 0;
  // Blocks until every queued task has run. Called from ~Player() so the host
  // still receives the final StateChanged{to=kStopped}.
  virtual void Flush() = 0;

 protected:
  EventDispatcher() = default;
  virtual ~EventDispatcher() = default;
};

// Everything ijkpp needs from the outside world. Every field may be null, in
// which case the auto-detected platform default is used — that is what makes
// `Player player; player.SetDataSource(url);` work with zero setup (rule E1).
struct IJKPP_PLAYER_EXPORT Deps {
  Deps();
  Deps(const Deps&) = delete;
  Deps& operator=(const Deps&) = delete;
  Deps(Deps&&) noexcept;
  Deps& operator=(Deps&&) noexcept;
  ~Deps();

  std::vector<std::shared_ptr<media::VideoDecoderFactory>> video_decoder_factories;
  std::vector<std::shared_ptr<media::AudioDecoderFactory>> audio_decoder_factories;
  std::shared_ptr<media::VideoRendererSinkFactory> video_sink_factory;
  std::shared_ptr<media::AudioRendererSinkFactory> audio_sink_factory;

  // Custom byte source (encrypted streams, an in-app downloader, a cache).
  // When set, ijkpp reads through it instead of opening the URI itself.
  std::shared_ptr<media::DataSource> data_source;

  // Injectable monotonic clock. Null means base::DefaultTickClock. Tests use
  // base::SimpleTestTickClock, which is what makes the scheduling logic
  // deterministically testable (design principle P6).
  std::shared_ptr<const base::TickClock> tick_clock;
  std::unique_ptr<base::logging::LoggingDelegate> logging_delegate;
  std::shared_ptr<EventDispatcher> event_dispatcher;

  // Fills in every auto-detectable default for the current platform.
  static Deps CreateDefault();
};

}  // namespace ijkpp

#endif  // IJKPP_PLAYER_PUBLIC_DEPS_H_
