// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The bodies behind the Player facade. What is deliberately NOT here yet:
// accurate seek and the seek controller (M9), track switching (M9, needs
// sub-renderer re-initialisation), snapshots, and the C ABI. Every one of
// those is rejected with kNotImplemented naming its milestone rather than
// silently doing nothing -- the M8 skeleton's contract, kept.

#include "player/player_impl.h"

#include "player/video_decoder_defaults.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/default_tick_clock.h"
#include "avbase/BuildConfig.h"
#include "media/base/media_constants.h"
#include "media/base/media_error.h"
#include "media/base/media_log.h"
#if AVBASE_ENABLE_FFMPEG
#include "media/filters/ffmpeg_decoder_factories.h"
#include "media/filters/ffmpeg_text_decoder.h"
#include "media/filters/ffmpeg_demuxer.h"
#endif
namespace avbase {
namespace {

media::AvSyncController::MasterType ToMediaMaster(SyncMasterType type) {
  switch (type) {
    case SyncMasterType::kAudio:
      return media::AvSyncController::MasterType::kAudio;
    case SyncMasterType::kVideo:
      return media::AvSyncController::MasterType::kVideo;
    case SyncMasterType::kExternal:
      return media::AvSyncController::MasterType::kExternal;
  }
  return media::AvSyncController::MasterType::kAudio;
}

Status ErrState(std::string_view api, PlayerState state) {
  return base::unexpected(MediaError(
      ErrorCode::kInvalidState,
      std::string(api) + " called in state " + GetPlayerStateName(state),
      "the call was rejected by the player state machine",
      "call the method in a state where it is legal, or Reset() first; see "
      "the state table in player/state_machine.h"));
}

}  // namespace

PlayerImpl::PlayerImpl(const PlayerConfig& config, std::unique_ptr<Deps> deps)
    : config_(config),
      deps_(deps ? std::move(deps)
                 : std::make_unique<Deps>(Deps::CreateDefault())),
      media_thread_("avbase-media"),
      video_thread_("avbase-video"),
      audio_thread_("avbase-audio") {
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
    deps_->video_decoder_factories.push_back(
        base::MakeRefCounted<media::FFmpegVideoDecoderFactory>(
            video_thread_.task_runner()));
  }
  if (deps_->audio_decoder_factories.empty()) {
    deps_->audio_decoder_factories.push_back(
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

std::unique_ptr<media::Demuxer> PlayerImpl::CreateDemuxer() {
#if AVBASE_ENABLE_FFMPEG
  bool has_source = false;
  {
    base::AutoLock scoped(state_lock_);
    has_source = source_set_;
  }
  if (has_source) {
    // Every descriptor kind the demuxer can now serve: URI and fd through
    // FFmpeg's protocol layer, memory and custom sources through the
    // DataSource→AVIOContext bridge. Unsupported kinds fail inside the
    // demuxer with an actionable error rather than here.
    return std::make_unique<media::FFmpegDemuxer>(media_log_);
  }
#endif
  return nullptr;
}

Status PlayerImpl::SetDataSource(std::string_view uri) {
  return SetDataSource(media::DataSourceDescriptor::FromUri(uri));
}

Status PlayerImpl::SetDataSource(
    const media::DataSourceDescriptor& descriptor) {
  if (descriptor.kind == media::DataSourceDescriptor::Kind::kUri &&
      descriptor.uri.empty()) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidArgument, "the data source URI is empty",
        "SetDataSource received an empty URI",
        "pass the file path or URL as the argument to SetDataSource"));
  }
  base::AutoLock scoped(state_lock_);
  if (machine_.state() != PlayerState::kIdle) {
    return ErrState("SetDataSource", machine_.state());
  }
  source_ = descriptor;
  source_set_ = true;
  machine_.TransitionTo(PlayerState::kInitialized);
  event_hub_.PostStateChanged(PlayerState::kIdle, machine_.state(),
                              base::TimeDelta());
  return OkStatus();
}

Status PlayerImpl::PrepareAsync() {
  {
    base::AutoLock scoped(state_lock_);
    if (machine_.state() != PlayerState::kInitialized || !source_set_) {
      return ErrState("PrepareAsync", machine_.state());
    }
    machine_.TransitionTo(PlayerState::kPreparing);
    // Same lock as the state it describes: the hop arrives on S1, not here.
    prepared_handled_ = false;
  }
  event_hub_.PostStateChanged(PlayerState::kInitialized,
                              PlayerState::kPreparing, base::TimeDelta());

  media::DemuxerOptions options;
  options.probe_size = 0;                   // TEST
  options.analyze_duration = base::TimeDelta();  // TEST
  options.find_stream_info = config_.demux.find_stream_info;
  options.forced_format = config_.demux.forced_format;
  options.io_timeout = config_.demux.timeout;
  options.reconnect = config_.net.reconnect;
  options.reconnect_max_retries = config_.net.reconnect_max_retries;
  options.reconnect_delay = config_.net.reconnect_delay;
  options.user_agent = config_.net.user_agent;
  options.headers = config_.net.headers;
  options.extra_options = config_.extra_format_options;

  auto demuxer = CreateDemuxer();
  if (!demuxer) {
    {
      base::AutoLock scoped(state_lock_);
      machine_.TransitionTo(PlayerState::kError);
    }
    event_hub_.PostError(
        MediaError(ErrorCode::kNotImplemented,
                   "no demuxer is available for this data source",
                   "this build has AVBASE_ENABLE_FFMPEG off, or the source "
                   "kind needs the DataSource bridge (M9)",
                   "build with -DAVBASE_ENABLE_FFMPEG=ON and pass a URI, or "
                   "wait for the M9 DataSource AVIOContext bridge"),
        base::TimeDelta());
    return OkStatus();
  }

  auto av_sync = std::make_shared<media::AvSyncController>(
      ToMediaMaster(config_.sync_master), deps_->tick_clock.get(),
      media::AvSyncController::Thresholds());

  pipeline_ = std::make_unique<media::PipelineImpl>();
  pipeline_->SetTickClock(deps_->tick_clock.get());
  pipeline_->SetClock(av_sync);
  media::DataSourceDescriptor source;
  {
    base::AutoLock scoped(state_lock_);
    source = source_;
  }
  pipeline_->SetSource(std::move(source), std::move(options));

  media::DefaultRendererFactory::Deps factory_deps;
  factory_deps.video_task_runner = video_thread_.task_runner();
  factory_deps.audio_task_runner = audio_thread_.task_runner();
  factory_deps.tick_clock = deps_->tick_clock.get();
  factory_deps.video_decoder_factories = deps_->video_decoder_factories;
  factory_deps.audio_decoder_factories = deps_->audio_decoder_factories;
  factory_deps.text_decoder_factory = deps_->text_decoder_factory;
  factory_deps.video_sink_factory = deps_->video_sink_factory;
  factory_deps.audio_sink_factory = deps_->audio_sink_factory;
  factory_deps.av_sync = std::move(av_sync);
  factory_deps.video_disabled = config_.video.disabled;
  factory_deps.audio_disabled = config_.audio.disabled;
  renderer_factory_ =
      std::make_unique<media::DefaultRendererFactory>(std::move(factory_deps));
  {
    base::AutoLock scoped(state_lock_);
    renderer_factory_->set_display(display_);
  }

  pipeline_->Start(std::move(demuxer), renderer_factory_.get(),
                   media::RendererType::kRendererImpl,
                   media_thread_.task_runner(), this);
  return OkStatus();
}

Status PlayerImpl::PrepareSync(base::TimeDelta timeout) {
  const Status prepare = PrepareAsync();
  if (!prepare) {
    return prepare;
  }
  // Both the ready path and the error path signal; whichever comes first
  // decides the answer. Bounded by |timeout| per the frozen contract.
  if (!prepared_event_.TimedWait(timeout)) {
    return base::unexpected(MediaError(
        ErrorCode::kTimeout, "PrepareSync timed out",
        "the pipeline was still preparing after " +
            std::to_string(timeout.InMillisecondsF()) + "ms",
        "raise the timeout, or use PrepareAsync and wait for kPrepared"));
  }
  prepared_event_.Reset();
  // start_on_prepared (the default) means Start() already ran by the time
  // the event fired, so kStarted is a successful answer too.
  const PlayerState s = state();
  if (s == PlayerState::kPrepared || s == PlayerState::kStarted ||
      s == PlayerState::kCompleted) {
    return OkStatus();
  }
  if (s == PlayerState::kError) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidState, "preparing the source failed",
        "the pipeline reported an error before it was ready; the kError event "
        "carries the three-part detail",
        "inspect the error event, or run avbase-inspect probe <url> on the "
        "source"));
  }
  return base::unexpected(MediaError(
      ErrorCode::kInvalidState, "prepare ended in an unexpected state",
      std::string("final state: ") + GetPlayerStateName(s),
      "inspect the event stream; this is an avbase bug, please report it with "
      "DumpDiagnostics() output"));
}

void PlayerImpl::OnPipelineReady() {
  // Snapshot everything the frozen query surface promises, then publish
  // kPrepared once. Runs on the media sequence; the state lock is brief and
  // never held while anything else is taken.
  {
    base::AutoLock scoped(state_lock_);
    if (prepared_handled_) {
      return;
    }
    prepared_handled_ = true;
    machine_.TransitionTo(PlayerState::kPrepared);
  }
  const media::MediaInfo info =
      pipeline_ ? pipeline_->media_info() : media::MediaInfo();
  {
    base::AutoLock scoped(snapshot_lock_);
    media_info_ = info;
    is_live_ = info.is_live;
    if (const media::StreamInfo* v = info.FindStream(
            info.FirstStreamOfKind(media::StreamKind::kVideo))) {
      video_coded_size_ = v->coded_size;
      video_natural_size_ = v->natural_size;
      video_rotation_ = v->rotation;
    }
  }
  PreparedPayload payload;
  payload.media_info = info;
  event_hub_.Post(EventType::kPrepared, std::move(payload), GetMediaTime());
  event_hub_.PostStateChanged(PlayerState::kPreparing, PlayerState::kPrepared,
                              GetMediaTime());
  prepared_event_.Signal();
  if (config_.start_on_prepared) {
    Start();
  }
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
  if (previous == PlayerState::kCompleted && pipeline_) {
    // Replay: restart from the beginning rather than unpausing at EOS.
    pipeline_->Seek(base::TimeDelta(),
                    base::BindOnce([](PlayerImpl* self) {
                      if (self->pipeline_) {
                        self->pipeline_->Play();
                      }
                    }, base::Unretained(this)));
    return;
  }
  if (pipeline_) {
    pipeline_->Play();
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
  if (pipeline_) {
    pipeline_->Pause();
  }
  event_hub_.PostStateChanged(PlayerState::kStarted, PlayerState::kPaused,
                              GetMediaTime());
}

void PlayerImpl::Stop() {
  PlayerState previous;
  {
    base::AutoLock scoped(state_lock_);
    previous = machine_.state();
    if (previous == PlayerState::kStopping ||
        previous == PlayerState::kStopped) {
      return;
    }
    // From kIdle/kInitialized there is no pipeline to stop; walk the table to
    // kStopped so the published sequence is still well-formed.
    if (previous == PlayerState::kIdle ||
        previous == PlayerState::kInitialized) {
      previous = PlayerState::kStopped;
    } else {
      machine_.TransitionTo(PlayerState::kStopping);
    }
  }
  if (pipeline_) {
    pipeline_->Stop();
  }
  {
    base::AutoLock scoped(state_lock_);
    machine_.TransitionTo(PlayerState::kStopping);
    machine_.TransitionTo(PlayerState::kStopped);
  }
  event_hub_.PostStateChanged(previous, PlayerState::kStopped,
                              GetMediaTime());
}

void PlayerImpl::StopSync(base::TimeDelta timeout) {
  Stop();
  if (!media_thread_.IsRunning()) {
    return;
  }
  // Wait (bounded) for the media sequence to have executed the Stop posted
  // above, which is when the renderer and the demux thread are gone. On
  // expiry: detach and log rather than hang the caller (Δ15).
  base::WaitableEvent done;
  media_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce([](base::WaitableEvent* e) { e->Signal(); }, &done));
  if (!done.TimedWait(timeout)) {
    LOG(ERROR) << "avbase: StopSync timed out after "
               << timeout.InMillisecondsF()
               << "ms; detaching (delta 15: leak a thread, never hang)";
  }
}

void PlayerImpl::Reset() {
  StopSync(config_.shutdown_timeout);
  pipeline_.reset();
  renderer_factory_.reset();
  {
    base::AutoLock scoped(state_lock_);
    machine_.ForceReset();
    source_set_ = false;
    source_ = media::DataSourceDescriptor();
    prepared_handled_ = false;
  }
  prepared_event_.Reset();
  event_hub_.PostStateChanged(PlayerState::kStopped, PlayerState::kIdle,
                              base::TimeDelta());
}

void PlayerImpl::SetVideoSurface(base::scoped_refptr<NativeDisplay> display) {
  {
    base::AutoLock scoped(state_lock_);
    display_ = display;
  }
  if (renderer_factory_) {
    renderer_factory_->set_display(display);
  }
  if (pipeline_) {
    pipeline_->SetOutputTarget(std::move(display));
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
      current != PlayerState::kPrepared &&
      current != PlayerState::kCompleted) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidState,
        std::string("SeekTo called in state ") + GetPlayerStateName(current),
        "seeking needs a prepared or playing pipeline",
        "wait for kPrepared before seeking"));
  }
  if (!pipeline_) {
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
  pipeline_->Seek(requested, base::BindOnce(&PlayerImpl::OnMediaSeekDone,
                                            base::Unretained(this), id,
                                            requested));
  return id;
}

}  // namespace avbase
