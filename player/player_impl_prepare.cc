// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PlayerImpl's preparing half: SetDataSource, CreateDemuxer,
// PrepareAsync/PrepareSync and the ready snapshot (OnPipelineReady).
// player_impl.cc keeps construction and the transport controls; the split is
// a line-count one (C1), and the seam is "the player being given a source
// and prepared" versus "the player being driven once prepared".
//
// The split was made to open a seam, not for its own sake: asserting what
// PrepareAsync hands the renderer factory (tests/unit/player/
// player_deps_wiring_unittest.cc) needed room, and player_impl.cc sat one
// line under its 500-line ceiling. The comments inside PrepareAsync are
// load-bearing (they record which config field became behaviour in which
// round, and why), so they moved here verbatim rather than being compressed
// to fit.

#include "player/player_impl.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/media_error.h"
#include "media/base/media_log.h"
#if AVBASE_ENABLE_FFMPEG
#include "media/ffmpeg/ffmpeg_audio_filter.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"
#include "media/ffmpeg/ffmpeg_video_filter.h"
#include "media/ffmpeg/url_data_source.h"
#endif
#include "player/scene_internal.h"

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

namespace {

// Production retry wiring (docs/12 §2.5 GAP closed): http(s) URIs leave
// FFmpeg's protocol layer and enter the DataSource bridge as an avio-backed
// UrlDataSource wrapped in RetryDataSource. The protocol layer's own reconnect
// only covers mid-stream errors; a reset during connect or a hard reset
// mid-transfer surfaces as a read error HERE, and the retry decorator is what
// turns it into a fresh connection. rtmp/rtsp/srt stay on the protocol layer
// (streaming semantics the bridge does not model). |out_retry| is filled only
// when |track| is set -- the main source keeps its decorator named for
// diagnostics / ReconnectNow, while secondary sources keep theirs alive inside
// the wrapped descriptor's |custom| and need no separate handle.
media::DataSourceDescriptor
WrapRetry(const media::DataSourceDescriptor& descriptor, const NetConfig& net,
          bool track, base::scoped_refptr<media::RetryDataSource>* out_retry) {
#if AVBASE_ENABLE_FFMPEG
  if (descriptor.kind == media::DataSourceDescriptor::Kind::kUri &&
      (descriptor.uri.starts_with("http://") ||
       descriptor.uri.starts_with("https://")) &&
      net.reconnect) {
    auto url_source =
        base::MakeRefCounted<media::ffmpeg::UrlDataSource>(descriptor.uri);
    auto retry = base::MakeRefCounted<media::RetryDataSource>(
        std::move(url_source),
        media::RetryDataSource::Config{net.reconnect_max_retries,
                                       net.reconnect_delay});
    // Named BEFORE the descriptor takes ownership, never after: moving
    // |retry| into FromSource() below leaves it null, and a ReconnectNow()
    // that holds an empty handle fails silently -- which is what an ordering
    // this subtle deserves a warning about. (Found by clang-tidy's
    // bugprone-use-after-move; no test had noticed.)
    if (track && out_retry) {
      *out_retry = retry;
    }
    // Keep the URI for logs and MediaInfo: the hop below replaces the
    // descriptor with a URI-less bridge, and source_.uri is guarded by
    // state_lock_, so the copy arrives from |descriptor| instead.
    auto wrapped = media::DataSourceDescriptor::FromSource(std::move(retry));
    wrapped.uri = descriptor.uri;
    return wrapped;
  }
  return descriptor;
#else
  (void)net;
  (void)track;
  (void)out_retry;
  return descriptor;
#endif
}

}  // namespace

std::unique_ptr<media::Demuxer> PlayerImpl::CreateDemuxer() {
  base::AutoLock scoped(state_lock_);
  if (!source_set_) {
    return nullptr;
  }
  return CreateDemuxerFor(source_);
}

std::unique_ptr<media::Demuxer>
PlayerImpl::CreateDemuxerFor(const media::DataSourceDescriptor& descriptor) {
#if AVBASE_ENABLE_FFMPEG
  // A descriptor that names nothing cannot be opened, and discovering that
  // INSIDE the demuxer means one secondary source takes the whole prepare down
  // with "failed to open ''" -- which does not say which source, or why a
  // source was there at all. Refused here instead, with the same test
  // SetDataSource() applies to the player's own source, so the two agree.
  if (descriptor.kind == media::DataSourceDescriptor::Kind::kUri &&
      descriptor.uri.empty()) {
    return nullptr;
  }
  // Every descriptor kind the demuxer can now serve: URI and fd through
  // FFmpeg's protocol layer, memory and custom sources through the
  // DataSource→AVIOContext bridge. Unsupported kinds fail inside the
  // demuxer with an actionable error rather than here.
  //
  // The descriptor is deliberately NOT handed to the constructor: it travels
  // with the demuxer to Initialize() (one per source, paired in
  // PipelineImpl::DoStart), which is where an open actually happens.
  //
  // BufferConfig -> BufferQueueLimits (invariant C22: media/ never sees
  // PlayerConfig). infbuf makes the byte cap unlimited, max-buffer-size is that
  // cap, and both are whole-player budgets the demuxer divides across streams.
  // min-frames is NOT carried here: the gate that reads it is in PipelineImpl.
  media::BufferQueueLimits limits;
  limits.max_buffers = 256;  // count cap; a byte cap or infbuf governs memory
  limits.max_bytes = config_.buffer.unlimited ? 0 : config_.buffer.max_bytes;
  return std::make_unique<media::FFmpegDemuxer>(media_log_, limits);
#else
  (void)descriptor;
  return nullptr;
#endif
}

std::shared_ptr<SceneSource>
PlayerImpl::AddSource(const media::DataSourceDescriptor& descriptor,
                      std::string name, int z) {
  base::AutoLock scoped(state_lock_);
  // Stage 3 sources are demuxers built at PrepareAsync, so they must be
  // registered before it runs. Allowed from kIdle (no source yet) or
  // kInitialized (SetDataSource done); anything later is a kInvalidState,
  // which is also what makes "register then re-register after a Reset" the
  // only supported shape.
  const PlayerState s = machine_.state();
  if (s != PlayerState::kIdle && s != PlayerState::kInitialized) {
    return nullptr;
  }
  // The scene creates the layer handle; the player keeps a parallel record so
  // PrepareAsync can build the demuxer and the secondary sink factory in the
  // same source-id order.
  std::shared_ptr<SceneSource> source = scene_.AddSource(std::move(name), z);
  if (!source) {
    return nullptr;
  }
  SecondarySource entry;
  entry.descriptor = descriptor;
  entry.source = source;
  entry.z = z;
  secondary_sources_.push_back(std::move(entry));
  return source;
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

  // docs/14 stage 3: build the descriptor + demuxer list. Index 0 is this
  // player's own source (SetDataSource); indices 1..N are the secondary
  // sources registered via AddSource(), in registration order -- so each
  // secondary's demuxer index is also its source id. With none registered
  // this collapses to the single-source path exactly as before.
  std::vector<media::DataSourceDescriptor> descriptors;
  std::vector<std::unique_ptr<media::Demuxer>> demuxers;
  std::vector<std::shared_ptr<SceneSource>> secondary_scene_sources;
  {
    base::AutoLock scoped(state_lock_);
    descriptors.push_back(source_);
    for (const auto& entry : secondary_sources_) {
      descriptors.push_back(entry.descriptor);
      secondary_scene_sources.push_back(entry.source);
    }
  }
  // Production retry wiring (docs/12 §2.5): wrap http(s) URIs in a
  // RetryDataSource. The main source keeps its decorator named for
  // diagnostics / ReconnectNow; secondary sources keep theirs alive inside
  // the wrapped descriptor's |custom|. retry_source_ is shared with
  // ReconnectNow(), so writing it stays inside state_lock_ exactly as it was
  // before the descriptor list existed.
  {
    base::AutoLock scoped(state_lock_);
    descriptors[0] =
        WrapRetry(descriptors[0], config_.net, /*track=*/true, &retry_source_);
  }
  for (size_t i = 1; i < descriptors.size(); ++i) {
    descriptors[i] =
        WrapRetry(descriptors[i], config_.net, /*track=*/false, nullptr);
  }
  // One demuxer per descriptor, same order: the main reads source_, each
  // secondary from its descriptor.
  demuxers.push_back(CreateDemuxer());
  for (size_t i = 1; i < descriptors.size(); ++i) {
    demuxers.push_back(CreateDemuxerFor(descriptors[i]));
  }
  {
    bool missing = demuxers.empty();
    for (const auto& d : demuxers) {
      if (!d) {
        missing = true;
        break;
      }
    }
    if (missing) {
      {
        base::AutoLock scoped(state_lock_);
        machine_.TransitionTo(PlayerState::kError);
      }
      // Outside the lock, as it was: an event handler is allowed to call back
      // into the player, and PostError runs them synchronously.
      event_hub_.PostError(
          MediaError(ErrorCode::kNotImplemented,
                     "no demuxer is available for this data source",
                     "this build has AVBASE_ENABLE_FFMPEG off, or a source "
                     "kind needs the DataSource bridge (M9)",
                     "build with -DAVBASE_ENABLE_FFMPEG=ON and pass a URI, or "
                     "wait for the M9 DataSource AVIOContext bridge"),
          base::TimeDelta());
      return OkStatus();
    }
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
  pipeline->SetSource(std::move(descriptors), options);

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
  // docs/14 stage 3: the secondary legs (source id >= 1) get a sink that
  // publishes into their SceneSource, which the host sink's engine composites.
  // Built from the scene sources PrepareAsync collected above; null here means
  // every secondary leg renders into a NullVideoSink (stage 1 policy) -- the
  // decode chains still run, the frames are just not shown.
  if (!secondary_scene_sources.empty()) {
    factory_deps.extra_video_sink_factory =
        CreateSecondarySinkFactory(std::move(secondary_scene_sources));
  }
  factory_deps.audio_sink_factory = deps_->audio_sink_factory;
  factory_deps.av_sync = std::move(av_sync);
  factory_deps.video_disabled = config_.video.disabled;
  factory_deps.audio_disabled = config_.audio.disabled;
  // "nodisp", and why it is not video_disabled: this keeps the video leg
  // decoding (av sync, cadence and the sink stats stay meaningful) and drops
  // only the picture. The factory makes the swap, so an injected backend sink
  // cannot reintroduce a window the host asked not to have.
  factory_deps.disable_video_output = config_.render.disable_video_output;
  // "max-fps"/"framedrop": the setters had no caller in the tree until now, so
  // the cap never reached the compositor. The default (31) is what turns it on.
  factory_deps.video_max_fps = config_.video.max_fps;
  factory_deps.video_max_frame_drop = config_.video.max_frame_drop;
  // This is where config.video.decoder_preference stops being documentation.
  // It reaches VideoRendererImpl, which ranks the candidates against the real
  // stream config; before this wiring the field was honoured nowhere at
  // runtime and kHardwareOnly silently fell back to software (docs/12 §2.3).
  factory_deps.video_decoder_preference = config_.video.decoder_preference;
  factory_deps.video_hw_codecs = config_.video.hw_codecs;
  factory_deps.video_decoder_preference_set = true;
  // How this scene gets plumbed into whichever sink this factory builds. Why a
  // callback rather than a pointer, and how the old engine gets released
  // without anybody remembering to do it, are on Deps::scene_observer.
  factory_deps.scene_observer =
      base::BindRepeating(&Scene::Bind, base::Unretained(&scene_));
  // The §5c seam, and why it sits HERE rather than anywhere later: this is
  // the last moment the assembled Deps exist as a whole, before the move
  // into the factory scatters them into the renderers. Snapshotted under
  // state_lock_, run outside it -- see set_deps_observer_for_testing.
  FactoryDepsObserver deps_observer;
  {
    base::AutoLock scoped(state_lock_);
    deps_observer = deps_observer_;
  }
  if (deps_observer) {
    deps_observer(factory_deps);
  }
  auto renderer_factory =
      std::make_shared<media::DefaultRendererFactory>(std::move(factory_deps));
  {
    base::AutoLock scoped(state_lock_);
    renderer_factory->set_display(display_);
  }
  SetRendererFactory(renderer_factory);

  // Arm the pre-buffer gate with the initial tier before playback can begin, so
  // the first kHaveEnough is held until the buffer reaches the first high-water
  // mark (or packet floor), matching ijkplayer's initial packet-buffering wait.
  SyncBufferingGate();
  pipeline->Start(std::move(demuxers), renderer_factory.get(),
                  media::RendererType::kRendererImpl,
                  media_thread_.task_runner(), this);
  return OkStatus();
}

Status PlayerImpl::PrepareSync(base::TimeDelta timeout) {
  // Not const -- `return prepare;` should move, and a const local copies.
  Status prepare = PrepareAsync();
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
  // OnPipelineReady signals prepared_event_ only AFTER its autostart, so with
  // the default start_on_prepared=true this read is kStarted, not kPrepared.
  // Accept all three anyway: start_on_prepared=false leaves kPrepared, and a
  // caller may also have raced ahead and started us ourselves. The three are
  // exactly the states that mean "ready"; anything else is reported below.
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
  // BEFORE the signal, deliberately, so that "PrepareSync() returned" and
  // "the autostart has happened" are the same statement rather than two events
  // a caller has to hope land in order. Start() is cheap and non-blocking here
  // (a state_lock_ transition plus a PostTask), so there is nothing to gain
  // from signalling first.
  //
  // Honest caveat, because the first version of this comment overclaimed: the
  // old order was NOT reliably observable as a bug. Signal() holds the
  // WaitableEvent mutex and the waiter needs that same mutex, so the autostart
  // had already won in 20/20 measured runs even with the signal first
  // (see tests/unit/player/prepare_autostart_unittest.cc). The reorder is
  // therefore a correctness-by-construction change, not a visible-bug fix --
  // which is exactly why the old comment could sit here claiming the opposite
  // of what the code did. The ordering that DOES matter for consumers is the
  // two PostStateChanged calls above: kPreparing->kPrepared is published
  // before any kStarted, so an event consumer never sees a start before the
  // prepare it belongs to.
  if (config_.start_on_prepared) {
    Start();
  }
  prepared_event_.Signal();
}

}  // namespace avbase
