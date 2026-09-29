// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The public API surface is FROZEN as of this commit (milestone M8 gate in
// docs/08 §2); the bodies below are the M8 implementation skeleton. Every
// method that is not yet wired to the pipeline returns ErrorCode::kNotImplemented
// with an actionable message rather than silently doing nothing, so an early
// adopter sees exactly which milestone is missing.

#include "player/public/player.h"

#include "base/check.h"
#include "base/logging.h"

namespace ijkpp {
namespace {

base::unexpected<MediaError> NotImplemented(std::string_view api,
                                            std::string_view milestone) {
  return Err(ErrorCode::kNotImplemented,
             std::string(api) + " is not wired up yet",
             "this Player was built from the M8 API skeleton; the pipeline it "
             "would drive lands in " + std::string(milestone),
             "build against a release at or after " + std::string(milestone) +
                 ", or check the milestone table in docs/08 §1");
}

}  // namespace

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------
class Player::Impl {
 public:
  explicit Impl(PlayerConfig config) : config_(std::move(config)) {}
  PlayerConfig config_;
  PlayerState state_{PlayerState::kIdle};
};

Player::Player() : impl_(std::make_unique<Impl>(PlayerConfig{})) { GlobalInit(); }
Player::Player(const PlayerConfig& config)
    : impl_(std::make_unique<Impl>(config)) {
  GlobalInit();
  const std::vector<ConfigIssue> issues = ValidateConfig(config);
  for (const ConfigIssue& issue : issues) {
    LOG(ERROR) << "PlayerConfig problem: " << issue.field << " " << issue.problem
               << " — " << issue.suggestion;
  }
}
Player::Player(const PlayerConfig& config, std::unique_ptr<Deps> deps)
    : impl_(std::make_unique<Impl>(config)) {
  GlobalInit();
  deps_ = std::move(deps);
}
Player::~Player() = default;

Status Player::SetDataSource(std::string_view uri) {
  return SetDataSource(DataSourceDescriptor::FromUri(uri));
}
Status Player::SetDataSource(const DataSourceDescriptor&) {
  return NotImplemented("Player::SetDataSource", "M4");
}
Status Player::PrepareAsync() { return NotImplemented("Player::PrepareAsync", "M8"); }
Status Player::PrepareSync(base::TimeDelta) {
  return NotImplemented("Player::PrepareSync", "M8");
}
void Player::Start() { LOG(ERROR) << NotImplemented("Player::Start", "M8").value().ToString(); }
void Player::Pause() { LOG(ERROR) << NotImplemented("Player::Pause", "M8").value().ToString(); }
void Player::Stop() { LOG(ERROR) << NotImplemented("Player::Stop", "M8").value().ToString(); }
void Player::StopSync(base::TimeDelta) {}
void Player::Reset() {}
void Player::RunUntilIdle() {}

Result<int64_t> Player::SeekTo(base::TimeDelta, SeekMode, SeekCB) {
  return base::unexpected(MediaError(
      ErrorCode::kNotImplemented, "Player::SeekTo is not wired up yet",
      "the pipeline lands in M8", "see docs/08 §1"));
}
void Player::SeekTo(base::TimeDelta) {}
void Player::StepOnce() {}
void Player::SetPlaybackRate(double) {}
void Player::SetVolume(double) {}
void Player::SetMuted(bool) {}
void Player::SetLoopCount(int) {}
Status Player::SelectTrack(media::DemuxerStreamType, int) {
  return NotImplemented("Player::SelectTrack", "M8");
}
void Player::SetVideoSurface(base::scoped_refptr<NativeDisplay>) {}
Status Player::TakeSnapshot(base::TimeDelta, std::string) {
  return NotImplemented("Player::TakeSnapshot", "M8");
}
void Player::ReconnectNow() {}

PlayerState Player::state() const { return impl_->state_; }
std::optional<MediaInfo> Player::media_info() const { return std::nullopt; }
base::TimeDelta Player::GetMediaTime() const { return base::TimeDelta(); }
base::TimeDelta Player::GetBufferedTime() const { return base::TimeDelta(); }
base::TimeDelta Player::GetDuration() const { return base::TimeDelta(); }
bool Player::is_live() const { return false; }
bool Player::IsPlaying() const { return false; }
media::Size Player::video_natural_size() const { return {}; }
media::Size Player::video_coded_size() const { return {}; }
int Player::video_rotation() const { return 0; }
double Player::playback_rate() const { return 1.0; }
double Player::volume() const { return 1.0; }
bool Player::muted() const { return false; }
PlaybackStats Player::GetPlaybackStats() const { return {}; }

std::string Player::DumpDiagnostics() const {
  return std::string("{\"state\":\"") + GetPlayerStateName(impl_->state_) +
         "\",\"note\":\"M8 API skeleton; pipeline not yet wired\"}";
}

void Player::SetEventHandler(EventHandler) {}
Player::Subscription Player::AddObserver(PlayerObserver*) { return Subscription(); }
Status Player::UpdateConfig(const PlayerConfig&) {
  return NotImplemented("Player::UpdateConfig", "M8");
}
const PlayerConfig& Player::config() const { return impl_->config_; }

class Player::Subscription::Impl {};
Player::Subscription::Subscription() = default;
Player::Subscription::Subscription(Subscription&&) noexcept = default;
Player::Subscription& Player::Subscription::operator=(Subscription&&) noexcept = default;
Player::Subscription::~Subscription() = default;
void Player::Subscription::Reset() {}
bool Player::Subscription::active() const { return false; }

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
      detail += "\n           [" + std::to_string(i + 1) + "] " + issues[i].field +
                " " + issues[i].problem;
    }
    return base::unexpected(MediaError(
        ErrorCode::kConfigInvalid,
        std::to_string(issues.size()) + " configuration problems found", detail,
        "fix the fields above, or start from PlayerConfig{} defaults and change "
        "only what you need"));
  }

  if (impl_->deps) {
    return std::make_unique<Player>(impl_->config, std::move(impl_->deps));
  }
  return std::make_unique<Player>(impl_->config);
}

}  // namespace ijkpp
