// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The bodies behind the Player facade: construction, teardown bookkeeping
// and the transport controls. The preparing half (SetDataSource, prepare,
// the ready snapshot) lives in player_impl_prepare.cc; the stopping half in
// player_impl_stop.cc; both splits are line-count ones (C1) along seams the
// code already had. What is deliberately NOT here: accurate seek and the
// seek controller (M9), track switching (M9, needs sub-renderer
// re-initialisation), snapshots, and the C ABI. Every one of those is
// rejected with kNotImplemented naming its milestone rather than silently
// doing nothing -- the M8 skeleton's contract, kept.

#include "player/player_impl.h"

#include <string>
#include <utility>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/default_tick_clock.h"
#include "media/base/media_error.h"
#include "media/base/media_log.h"
#if AVBASE_ENABLE_FFMPEG
#include "media/ffmpeg/ffmpeg_decoder_factories.h"
#include "media/ffmpeg/ffmpeg_text_decoder.h"
#endif

#include "player/video_decoder_defaults.h"

namespace avbase {

// const& on purpose: by-value would copy |config| for a move nothing needs.
// NOLINTNEXTLINE
PlayerImpl::PlayerImpl(const PlayerConfig& config, std::unique_ptr<Deps> deps)
    : config_(config),
      deps_(deps ? std::move(deps)
                 : std::make_unique<Deps>(Deps::CreateDefault())),
      media_thread_("avbase-media"),
      video_thread_("avbase-video"),
      audio_thread_("avbase-audio"),
      // Built from config.buffer so the three-tier marks mirror BufferConfig's
      // first/next/last_high_water_mark and packet-buffering maps to enabled.
      // Listed last because buffer_controller_ is declared after the members
      // above (init order follows declaration order; -Wreorder enforces it).
      buffer_controller_(config.buffer) {
  media_thread_.Start();
  video_thread_.Start();
  audio_thread_.Start();
  // Deps::CreateDefault() leaves the clock set; a hand-built Deps may not.
  if (!deps_->tick_clock) {
    deps_->tick_clock = std::shared_ptr<const base::TickClock>(
        base::DefaultTickClock::GetInstance(), [](const base::TickClock*) {});
  }
#if AVBASE_ENABLE_FFMPEG
  // The software FFmpeg decoders are the fallback chain's tail; anything the
  // host injected keeps its place ahead of them (Δ12), and the platform's
  // hardware factories slot in between when the preference allows hardware.
  if (deps_->video_decoder_factories.empty()) {
    if (config_.video.decoder_preference !=
        media::DecoderPreference::kSoftware) {
      auto hw = DefaultHardwareVideoDecoderFactories(
          video_thread_.task_runner(), config_.video.hw_codecs);
      deps_->video_decoder_factories.insert(
          deps_->video_decoder_factories.end(), hw.begin(), hw.end());
    }
    deps_->video_decoder_factories.emplace_back(
        base::MakeRefCounted<media::FFmpegVideoDecoderFactory>(
            video_thread_.task_runner()));
  }
  if (deps_->audio_decoder_factories.empty()) {
    deps_->audio_decoder_factories.emplace_back(
        base::MakeRefCounted<media::FFmpegAudioDecoderFactory>(
            audio_thread_.task_runner()));
  }
  // The text leg rides on the FFmpeg layer like the other decoders; a
  // host-injected factory keeps its place (there is no fallback for text:
  // without a factory SelectTrack(kText) reports kNotImplemented).
  if (!deps_->text_decoder_factory) {
    deps_->text_decoder_factory =
        base::MakeRefCounted<media::FFmpegTextDecoderFactory>();
  }
#endif
  media_log_ = base::MakeRefCounted<media::MediaLog>();
}

PlayerImpl::~PlayerImpl() {
  StopSync(config_.shutdown_timeout);
  event_hub_.Shutdown(config_.shutdown_timeout);
  media_thread_.Stop();
  video_thread_.Stop();
  audio_thread_.Stop();
}

void PlayerImpl::set_deps_observer_for_testing(FactoryDepsObserver observer) {
  base::AutoLock scoped(state_lock_);
  deps_observer_ = std::move(observer);
}

void PlayerImpl::Start() {
  PlayerState previous;
  {
    base::AutoLock scoped(state_lock_);
    previous = machine_.state();
    if (previous != PlayerState::kPrepared &&
        previous != PlayerState::kPaused &&
        previous != PlayerState::kCompleted) {
      LOG(ERROR) << "Player::Start in state " << GetPlayerStateName(previous)
                 << " is not legal";
      return;
    }
    machine_.TransitionTo(PlayerState::kStarted);
  }
  if (previous == PlayerState::kCompleted) {
    // Replay: restart from the beginning rather than unpausing at EOS.
    RestartFromBeginning();
    return;
  }
  if (const auto pipeline = GetPipeline()) {
    pipeline->Play();
  }
  event_hub_.PostStateChanged(previous, PlayerState::kStarted, GetMediaTime());
}

void PlayerImpl::Pause() {
  {
    base::AutoLock scoped(state_lock_);
    if (machine_.state() != PlayerState::kStarted) {
      LOG(ERROR) << "Player::Pause in state "
                 << GetPlayerStateName(machine_.state()) << " is not legal";
      return;
    }
    machine_.TransitionTo(PlayerState::kPaused);
  }
  if (const auto pipeline = GetPipeline()) {
    pipeline->Pause();
  }
  event_hub_.PostStateChanged(PlayerState::kStarted, PlayerState::kPaused,
                              GetMediaTime());
}

void PlayerImpl::SetVideoSurface(base::scoped_refptr<NativeDisplay> display) {
  {
    base::AutoLock scoped(state_lock_);
    display_ = display;
  }
  if (const auto factory = GetRendererFactory()) {
    factory->set_display(display);
  }
  if (const auto pipeline = GetPipeline()) {
    pipeline->SetOutputTarget(std::move(display));
  }
}

Result<int64_t> PlayerImpl::SeekTo(base::TimeDelta position, SeekMode mode,
                                   Player::SeekCB cb) {
  PlayerState current;
  {
    base::AutoLock scoped(state_lock_);
    current = machine_.state();
  }
  if (current != PlayerState::kStarted && current != PlayerState::kPaused &&
      current != PlayerState::kPrepared && current != PlayerState::kCompleted) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidState,
        std::string("SeekTo called in state ") + GetPlayerStateName(current),
        "seeking needs a prepared or playing pipeline",
        "wait for kPrepared before seeking"));
  }
  const auto pipeline = GetPipeline();
  if (!pipeline) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidState, "no pipeline to seek",
        "SeekTo was called before PrepareAsync", "call PrepareAsync first"));
  }
  const int64_t id = next_request_id_.fetch_add(1);
  {
    base::AutoLock scoped(seek_lock_);
    if (mode == SeekMode::kAccurate) {
      // M9: land on the requested position, not on the keyframe before it.
      // The drop window (Renderer::BeginAccurateSeek) is opened before the
      // keyframe seek so the new generation is already framed when decoding
      // restarts; SeekController owns the timeout and the completion.
      accurate_seek_targets_[id] = position;
    }
    if (cb) {
      pending_seeks_[id] = std::move(cb);
    }
  }
  // Posted in order: on the media sequence the wait bookkeeping (and the
  // supersede of any running wait) lands before the pipeline seek does.
  media_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](PlayerImpl* self, int64_t seek_id, base::TimeDelta target,
             bool accurate) {
            if (accurate) {
              self->BeginAccurateWaitOnMedia(seek_id, target);
            } else {
              self->EndAccurateWaitOnMedia();
            }
          },
          base::Unretained(this), id, position, mode == SeekMode::kAccurate));
  const base::TimeDelta requested = position;
  pipeline->Seek(requested,
                 base::BindOnce(&PlayerImpl::OnMediaSeekDone,
                                base::Unretained(this), id, requested));
  return id;
}

}  // namespace avbase
