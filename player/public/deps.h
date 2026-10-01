// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PUBLIC_DEPS_H_
#define AVBASE_PLAYER_PUBLIC_DEPS_H_

#include <memory>
#include <vector>

#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/tick_clock.h"
#include "player/public/player_event.h"
#include "player/public/player_export.h"

namespace avbase {
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
class AVBASE_PLAYER_EXPORT EventDispatcher {
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

// Everything avbase needs from the outside world. Every field may be null, in
// which case the auto-detected platform default is used — that is what makes
// `Player player; player.SetDataSource(url);` work with zero setup (rule E1).
struct AVBASE_PLAYER_EXPORT Deps {
  Deps();
  Deps(const Deps&) = delete;
  Deps& operator=(const Deps&) = delete;
  Deps(Deps&&) noexcept;
  Deps& operator=(Deps&&) noexcept;
  ~Deps();

  // OWNERSHIP VOCABULARY FOR THIS STRUCT (see STYLE.md §3 and the three verbs
  // at the bottom of base/memory/scoped_refptr.h).
  //
  // A field whose type derives from base::RefCountedThreadSafe MUST be held in
  // base::scoped_refptr. media::DataSource, media::VideoDecoderFactory and
  // media::AudioDecoderFactory all do, and all three carry
  // REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE() with protected constructors and
  // protected destructors. Holding one in std::shared_ptr is not a style
  // preference but two distinct failures: the field cannot be assigned at all
  // (shared_ptr needs an accessible destructor, and these are protected), and
  // if it somehow could be, the two smart pointers would keep two independent
  // counts on one object -- a double free or a use-after-free.
  //
  // These three were std::shared_ptr until the ninth round. The evidence that
  // this was drift rather than a decision: this header already included
  // base/memory/scoped_refptr.h without using it, the comment in deps.cc
  // already said "Deps holds scoped_refptrs to forward-declared interfaces",
  // and Player::SetVideoSurface() in the same frozen surface already takes
  // scoped_refptr<NativeDisplay> -- another RefCountedThreadSafe type.
  //
  // The remaining shared_ptr fields below (sink factories, tick clock, event
  // dispatcher) name types that are NOT ref-counted, so shared_ptr is
  // functional there. They still deviate from STYLE.md §3, which marks
  // shared_ptr "not recommended"; changing them is churn on a frozen header
  // without a defect to fix, so it is left to the M8 interface review as a
  // recorded decision rather than done silently here.
  std::vector<base::scoped_refptr<media::VideoDecoderFactory>>
      video_decoder_factories;
  std::vector<base::scoped_refptr<media::AudioDecoderFactory>>
      audio_decoder_factories;
  std::shared_ptr<media::VideoRendererSinkFactory> video_sink_factory;
  std::shared_ptr<media::AudioRendererSinkFactory> audio_sink_factory;

  // Custom byte source (encrypted streams, an in-app downloader, a cache).
  // When set, avbase reads through it instead of opening the URI itself.
  // Build one with base::MakeRefCounted<media::MemoryDataSource>(...), or with
  // your own DataSource subclass; M9's RetryDataSource and M18's
  // CacheDataSource are decorators that wrap an existing scoped_refptr.
  base::scoped_refptr<media::DataSource> data_source;

  // Injectable monotonic clock. Null means base::DefaultTickClock. Tests use
  // base::SimpleTestTickClock, which is what makes the scheduling logic
  // deterministically testable (design principle P6).
  std::shared_ptr<const base::TickClock> tick_clock;
  std::unique_ptr<base::logging::LoggingDelegate> logging_delegate;
  std::shared_ptr<EventDispatcher> event_dispatcher;

  // Fills in every auto-detectable default for the current platform.
  static Deps CreateDefault();
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_DEPS_H_
