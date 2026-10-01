// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The public API surface is FROZEN (milestone M8 gate in docs/08 §2); every
// method forwards to PlayerImpl (player/player_impl.cc), which owns the
// threads, the pipeline and the event hub. Methods that remain unwired return
// ErrorCode::kNotImplemented with an actionable message rather than silently
// doing nothing -- the M8 skeleton's contract, kept for the ones M9+ still
// owes (snapshots, track selection, reconnect).

#include "player/public/player.h"

#include "base/check.h"
#include "base/logging.h"
#include "player/player_impl.h"

namespace avbase {

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------
class Player::Impl {
 public:
  Impl(const PlayerConfig& config, std::unique_ptr<Deps> deps)
      : core(config, std::move(deps)) {}

  PlayerImpl core;
};

Player::Player() : impl_(std::make_unique<Impl>(PlayerConfig(), nullptr)) {
  GlobalInit();
}
Player::Player(const PlayerConfig& config)
    : impl_(std::make_unique<Impl>(config, nullptr)) {
  GlobalInit();
}
Player::Player(const PlayerConfig& config, std::unique_ptr<Deps> deps)
    : impl_(std::make_unique<Impl>(config, std::move(deps))) {
  GlobalInit();
}
Player::~Player() = default;

Status Player::SetDataSource(std::string_view uri) {
  return impl_->core.SetDataSource(uri);
}
Status Player::SetDataSource(const DataSourceDescriptor& descriptor) {
  return impl_->core.SetDataSource(descriptor);
}
Status Player::PrepareAsync() {
  return impl_->core.PrepareAsync();
}
Status Player::PrepareSync(base::TimeDelta timeout) {
  return impl_->core.PrepareSync(timeout);
}
void Player::Start() {
  impl_->core.Start();
}
void Player::Pause() {
  impl_->core.Pause();
}
void Player::Stop() {
  impl_->core.Stop();
}
void Player::StopSync(base::TimeDelta timeout) {
  impl_->core.StopSync(timeout);
}
void Player::Reset() {
  impl_->core.Reset();
}
void Player::RunUntilIdle() {
  // The Level-0 convenience loop. The real one needs the L2->L0 task-runner
  // upgrade (base/threading/message_pump_epoll, R2 debt); until then a caller
  // that drives its own loop -- or the SDL2 examples' poll loop -- is the way
  // to stay evented.
  LOG(WARNING) << "Player::RunUntilIdle is not wired to a message pump yet; "
                  "drive your own loop or wait on events";
}

Result<int64_t> Player::SeekTo(base::TimeDelta position, SeekMode mode,
                               SeekCB cb) {
  return impl_->core.SeekTo(position, mode, std::move(cb));
}
void Player::SeekTo(base::TimeDelta position) {
  impl_->core.SeekTo(position, SeekMode::kPreviousKeyframe, Player::SeekCB());
}
void Player::StepOnce() {
  LOG(ERROR) << MediaError(
                    ErrorCode::kNotImplemented, "StepOnce is not wired up yet",
                    "single-frame stepping needs the compositor's step mode "
                    "exposed through the pipeline, which lands with M9's "
                    "seek controller",
                    "pause and seek by one frame duration instead")
                    .ToString();
}
void Player::SetPlaybackRate(double rate) {
  impl_->core.SetPlaybackRate(rate);
}
void Player::SetVolume(double volume) {
  impl_->core.SetVolume(volume);
}
void Player::SetMuted(bool muted) {
  impl_->core.SetMuted(muted);
}
void Player::SetLoopCount(int count) {
  impl_->core.SetLoopCount(count);
}
Status Player::SelectTrack(media::DemuxerStreamType, int) {
  // Track switching needs sub-renderer re-initialisation (M9); reporting
  // success here would show a toggle that does nothing, which is worse.
  return base::unexpected(MediaError(
      ErrorCode::kNotImplemented, "track selection is not wired up yet",
      "switching tracks requires re-initialising the matching sub-renderer "
      "(milestone M9)",
      "select the stream via PlayerConfig before PrepareAsync "
      "(video.selected_stream / audio.selected_stream)"));
}
void Player::SetVideoSurface(base::scoped_refptr<NativeDisplay> display) {
  impl_->core.SetVideoSurface(std::move(display));
}
Status Player::TakeSnapshot(base::TimeDelta, std::string) {
  return base::unexpected(MediaError(
      ErrorCode::kNotImplemented, "TakeSnapshot is not wired up yet",
      "snapshots need a compositor frame grab exposed through the pipeline "
      "(post-M9)",
      "read the frame from a custom VideoRendererSink instead"));
}
void Player::ReconnectNow() {
  LOG(WARNING) << "Player::ReconnectNow lands with M9's RetryDataSource";
}

PlayerState Player::state() const {
  return impl_->core.state();
}
std::optional<MediaInfo> Player::media_info() const {
  return impl_->core.media_info();
}
base::TimeDelta Player::GetMediaTime() const {
  return impl_->core.GetMediaTime();
}
base::TimeDelta Player::GetBufferedTime() const {
  return impl_->core.GetBufferedTime();
}
base::TimeDelta Player::GetDuration() const {
  return impl_->core.GetDuration();
}
bool Player::is_live() const {
  return impl_->core.is_live();
}
bool Player::IsPlaying() const {
  return impl_->core.IsPlaying();
}
media::Size Player::video_natural_size() const {
  return impl_->core.video_natural_size();
}
media::Size Player::video_coded_size() const {
  return impl_->core.video_coded_size();
}
int Player::video_rotation() const {
  return impl_->core.video_rotation();
}
double Player::playback_rate() const {
  return impl_->core.playback_rate();
}
double Player::volume() const {
  return impl_->core.volume();
}
bool Player::muted() const {
  return impl_->core.muted();
}
PlaybackStats Player::GetPlaybackStats() const {
  return impl_->core.GetPlaybackStats();
}
std::string Player::DumpDiagnostics() const {
  return impl_->core.DumpDiagnostics();
}

void Player::SetEventHandler(EventHandler handler) {
  impl_->core.SetEventHandler(std::move(handler));
}

class Player::Subscription::Impl {
 public:
  Impl(PlayerImpl* player, int id) : player(player), id(id) {}
  base::raw_ptr<PlayerImpl> player;
  int id{0};
};

Player::Subscription Player::AddObserver(PlayerObserver* observer) {
  Subscription sub;
  sub.impl_ = std::make_unique<Subscription::Impl>(
      &impl_->core, impl_->core.AddObserver(observer));
  return sub;
}
Status Player::UpdateConfig(const PlayerConfig&) {
  return base::unexpected(MediaError(
      ErrorCode::kNotImplemented, "UpdateConfig is not wired up yet",
      "runtime reconfiguration needs the whitelist of hot fields defined "
      "against a running pipeline (M9's BufferController is the first "
      "consumer)",
      "build a new Player with PlayerBuilder::SetConfig instead"));
}
const PlayerConfig& Player::config() const {
  return impl_->core.config();
}

Player::Subscription::Subscription() = default;
Player::Subscription::Subscription(Subscription&&) noexcept = default;
Player::Subscription& Player::Subscription::operator=(Subscription&&) noexcept =
    default;
Player::Subscription::~Subscription() = default;
void Player::Subscription::Reset() {
  if (impl_ && impl_->player && impl_->id > 0) {
    impl_->player->RemoveObserver(impl_->id);
  }
  impl_.reset();
}
bool Player::Subscription::active() const {
  return impl_ != nullptr && impl_->id > 0;
}

// ---------------------------------------------------------------------------
// PlayerBuilder
// ---------------------------------------------------------------------------
class PlayerBuilder::Impl {
 public:
  PlayerConfig config;
  std::unique_ptr<Deps> deps;
  std::vector<MediaError> option_errors;
};

PlayerBuilder::PlayerBuilder() : impl_(std::make_unique<Impl>()) {}
PlayerBuilder::~PlayerBuilder() = default;

PlayerBuilder& PlayerBuilder::SetConfig(PlayerConfig config) {
  impl_->config = std::move(config);
  return *this;
}

Status PlayerBuilder::SetOption(OptionCategory category, std::string_view key,
                                const OptionValue& value) {
  const Status s =
      OptionRegistry::GetInstance().SetValue(&impl_->config, category, key, value);
  if (!s) {
    impl_->option_errors.push_back(s.error());
  }
  return s;
}

PlayerBuilder& PlayerBuilder::SetDeps(std::unique_ptr<Deps> deps) {
  impl_->deps = std::move(deps);
  return *this;
}

PlayerBuilder& PlayerBuilder::SetLogLevel(base::logging::LogSeverity severity) {
  base::logging::SetMinLogLevel(severity);
  return *this;
}

Result<std::unique_ptr<Player>> PlayerBuilder::Build() {
  // Report every configuration problem at once so the caller fixes them in a
  // single pass instead of one rebuild per error.
  std::vector<ConfigIssue> issues = ValidateConfig(impl_->config);
  for (const MediaError& e : impl_->option_errors) {
    issues.push_back(ConfigIssue{e.context(), e.summary(), e.suggestion()});
  }
  if (!issues.empty()) {
    std::string detail;
    for (size_t i = 0; i < issues.size(); ++i) {
      detail += "\n           [" + std::to_string(i + 1) + "] " +
                issues[i].field + " " + issues[i].problem;
    }
    return base::unexpected(MediaError(
        ErrorCode::kConfigInvalid,
        std::to_string(issues.size()) + " configuration problems found", detail,
        "fix the fields above, or start from PlayerConfig{} defaults and "
        "change only what you need"));
  }

  if (impl_->deps) {
    return std::make_unique<Player>(impl_->config, std::move(impl_->deps));
  }
  return std::make_unique<Player>(impl_->config);
}

}  // namespace avbase
