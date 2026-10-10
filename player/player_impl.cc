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

#include <algorithm>
#include <string>
#include <utility>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/default_tick_clock.h"
#include "media/base/media_constants.h"
#include "media/base/media_error.h"
#include "media/base/media_log.h"
#if AVBASE_ENABLE_FFMPEG
#include "media/ffmpeg/ffmpeg_audio_filter.h"
#include "media/ffmpeg/ffmpeg_decoder_factories.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"
#include "media/ffmpeg/ffmpeg_text_decoder.h"
#include "media/ffmpeg/ffmpeg_video_filter.h"
#include "media/ffmpeg/url_data_source.h"
#include "media/filters/retry_data_source.h"
#endif

#include "player/video_decoder_defaults.h"

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

Status
PlayerImpl::SetDataSource(const media::DataSourceDescriptor& descriptor) {
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
  options.probe_size = 0;                        // TEST
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

  // Installed at once, so a concurrent Reset() can find and stop it; the rest
  // of PrepareAsync uses this local handle instead of the member.
  auto pipeline = std::make_shared<media::PipelineImpl>();
  SetPipeline(pipeline);
  pipeline->SetTickClock(deps_->tick_clock.get());
  pipeline->SetClock(av_sync);
  // The pipeline is the only layer that learns the source is live (from the
  // demuxer), so the cue-age window travels here and the policy is decided
  // where the fact is.
  pipeline->SetLiveCueMaxAge(config_.subtitle.enabled
                                 ? config_.subtitle.live_cue_max_age
                                 : base::TimeDelta());
  media::DataSourceDescriptor source;
  {
    base::AutoLock scoped(state_lock_);
    source = source_;
  }
#if AVBASE_ENABLE_FFMPEG
  // Production retry wiring (docs/12 §2.5 GAP closed): http(s) URIs leave
  // FFmpeg's protocol layer and enter the DataSource bridge as an avio-backed
  // UrlDataSource wrapped in RetryDataSource. The protocol layer's own
  // reconnect only covers mid-stream errors; a reset during connect or a hard
  // reset mid-transfer surfaces as a read error HERE, and the retry
  // decorator is what turns it into a fresh connection. rtmp/rtsp/srt stay
  // on the protocol layer (streaming semantics the bridge does not model).
  if (source.kind == media::DataSourceDescriptor::Kind::kUri &&
      (source.uri.rfind("http://", 0) == 0 ||
       source.uri.rfind("https://", 0) == 0) &&
      config_.net.reconnect) {
    auto url_source =
        base::MakeRefCounted<media::ffmpeg::UrlDataSource>(source.uri);
    auto retry = base::MakeRefCounted<media::RetryDataSource>(
        std::move(url_source),
        media::RetryDataSource::Config{config_.net.reconnect_max_retries,
                                       config_.net.reconnect_delay});
    {
      base::AutoLock scoped(state_lock_);
      retry_source_ = retry;
    }
    source = media::DataSourceDescriptor::FromSource(std::move(retry));
    // Keep the URI for logs and MediaInfo; read it under state_lock_, which
    // guards source_ -- the hop above replaced |source| with a URI-less bridge.
    {
      base::AutoLock scoped(state_lock_);
      source.uri = source_.uri;
    }
  }
#endif
  pipeline->SetSource(std::move(source), std::move(options));

  media::DefaultRendererFactory::Deps factory_deps;
  factory_deps.audio_filter_graph = config_.audio.filter_graph;
  factory_deps.video_filter_graph = config_.video.filter_graph;
#if AVBASE_ENABLE_FFMPEG
  factory_deps.audio_filter_factory = [] {
    return std::unique_ptr<media::AudioFilterStage>(
        std::make_unique<media::FFmpegAudioFilter>());
  };
  factory_deps.video_filter_factory = [] {
    return std::unique_ptr<media::VideoFilterStage>(
        std::make_unique<media::FFmpegVideoFilter>());
  };
#endif
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
  // This is where config.video.decoder_preference stops being documentation.
  // It reaches VideoRendererImpl, which ranks the candidates against the real
  // stream config; before this wiring the field was honoured nowhere at
  // runtime and kHardwareOnly silently fell back to software (docs/12 §2.3).
  factory_deps.video_decoder_preference = config_.video.decoder_preference;
  factory_deps.video_hw_codecs = config_.video.hw_codecs;
  factory_deps.video_decoder_preference_set = true;
  auto renderer_factory =
      std::make_shared<media::DefaultRendererFactory>(std::move(factory_deps));
  {
    base::AutoLock scoped(state_lock_);
    renderer_factory->set_display(display_);
  }
  SetRendererFactory(renderer_factory);

  pipeline->Start(std::move(demuxer), renderer_factory.get(),
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
  const auto pipeline = GetPipeline();
  const media::MediaInfo info =
      pipeline ? pipeline->media_info() : media::MediaInfo();
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
