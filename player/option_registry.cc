// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Milestone M1 will replace the hand-written table below with the output of
// tools/gen_options.py, generated from player_config.h so the two can never
// drift (invariant C13). The shape of the API is final; only the table's
// provenance changes.

#include "player/public/option_registry.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace avbase {
namespace {

// Edit distance used for the "did you mean" hint. ijkplayer silently ignores
// an unknown key, which is why a typo'd option can ship unnoticed (Δ2).
int Levenshtein(std::string_view a, std::string_view b) {
  if (a.empty())
    return static_cast<int>(b.size());
  if (b.empty())
    return static_cast<int>(a.size());
  std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
    prev[j] = static_cast<int>(j);
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = static_cast<int>(i);
    for (size_t j = 1; j <= b.size(); ++j) {
      const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    prev.swap(cur);
  }
  return prev[b.size()];
}

base::unexpected<MediaError> UnknownKey(std::string_view key,
                                        std::string_view suggestion) {
  std::string detail("key = \"");
  detail.append(key);
  detail.push_back('"');
  if (!suggestion.empty()) {
    detail.append("\n           did you mean \"");
    detail.append(suggestion);
    detail.append("\"?");
  }
  return Err(ErrorCode::kInvalidArgument, "unknown option key", detail,
             "call OptionRegistry::GetInstance().Describe() for the full list, "
             "or use the typed PlayerConfig field instead");
}

base::unexpected<MediaError> OutOfRange(std::string_view key,
                                        const std::string& got,
                                        const std::string& allowed) {
  std::string detail("key = \"");
  detail.append(key);
  detail.append("\"   value = ").append(got);
  detail.append("\n           allowed = ").append(allowed);
  return Err(ErrorCode::kInvalidArgument, "option value out of range", detail,
             "clamp the value, or remove the override and use the default");
}

base::unexpected<MediaError> TypeMismatch(std::string_view key,
                                          const std::string& expected) {
  std::string detail("key = \"");
  detail.append(key);
  detail.append("\" expects ").append(expected);
  return Err(ErrorCode::kInvalidArgument, "option type mismatch", detail,
             "pass the value with the matching type");
}

}  // namespace

// Definition of the Pimpl declared in option_registry.h.
struct OptionRegistry::Impl {
  struct Entry {
    std::string key;
    OptionCategory category;
    OptionDescriptor::Type type;
    std::string config_field;
    std::function<Status(PlayerConfig*, const OptionValue&)> setter;
    std::function<OptionValue(const PlayerConfig&)> getter;
    std::string doc;
  };
  std::vector<Entry> entries;
};

using Type = OptionDescriptor::Type;

OptionRegistry::OptionRegistry() : impl_(std::make_unique<Impl>()) {
  auto& e = impl_->entries;
  const auto P = OptionCategory::kPlayer;

  e.push_back(
      {"packet-buffering", P, Type::kBool, "buffer.enabled",
       [](PlayerConfig* c, const OptionValue& v) -> Status {
         const bool* b = std::get_if<bool>(&v);
         if (!b)
           return TypeMismatch("packet-buffering", "a boolean");
         c->buffer.enabled = *b;
         return OkStatus();
       },
       [](const PlayerConfig& c) -> OptionValue { return c.buffer.enabled; },
       "pause output until enough packets have been read after stalling"});

  e.push_back({"max-buffer-size", P, Type::kInt64, "buffer.max_bytes",
               [](PlayerConfig* c, const OptionValue& v) -> Status {
                 const int64_t* i = std::get_if<int64_t>(&v);
                 if (!i)
                   return TypeMismatch("max-buffer-size", "an integer");
                 if (*i < 0)
                   return OutOfRange("max-buffer-size", std::to_string(*i),
                                     ">= 0");
                 c->buffer.max_bytes = static_cast<size_t>(*i);
                 return OkStatus();
               },
               [](const PlayerConfig& c) -> OptionValue {
                 return static_cast<int64_t>(c.buffer.max_bytes);
               },
               "max buffer size that should be pre-read, in bytes"});

  e.push_back({"min-frames", P, Type::kInt64, "buffer.min_frames",
               [](PlayerConfig* c, const OptionValue& v) -> Status {
                 const int64_t* i = std::get_if<int64_t>(&v);
                 if (!i)
                   return TypeMismatch("min-frames", "an integer");
                 if (*i < 2 || *i > 50)
                   return OutOfRange("min-frames", std::to_string(*i),
                                     "[2, 50]");
                 c->buffer.min_frames = static_cast<size_t>(*i);
                 return OkStatus();
               },
               [](const PlayerConfig& c) -> OptionValue {
                 return static_cast<int64_t>(c.buffer.min_frames);
               },
               "minimal frames to stop pre-reading"});

  e.push_back(
      {"infbuf", P, Type::kBool, "buffer.unlimited",
       [](PlayerConfig* c, const OptionValue& v) -> Status {
         const bool* b = std::get_if<bool>(&v);
         if (!b)
           return TypeMismatch("infbuf", "a boolean");
         c->buffer.unlimited = *b;
         return OkStatus();
       },
       [](const PlayerConfig& c) -> OptionValue { return c.buffer.unlimited; },
       "do not limit the input buffer size (useful with realtime streams)"});

  e.push_back({"framedrop", P, Type::kInt64, "video.max_frame_drop",
               [](PlayerConfig* c, const OptionValue& v) -> Status {
                 const int64_t* i = std::get_if<int64_t>(&v);
                 if (!i)
                   return TypeMismatch("framedrop", "an integer");
                 if (*i < -1 || *i > 120)
                   return OutOfRange("framedrop", std::to_string(*i),
                                     "[-1, 120]");
                 c->video.max_frame_drop = static_cast<int>(*i);
                 return OkStatus();
               },
               [](const PlayerConfig& c) -> OptionValue {
                 return static_cast<int64_t>(c.video.max_frame_drop);
               },
               "drop frames when the CPU is too slow"});

  e.push_back({"max-fps", P, Type::kInt64, "video.max_fps",
               [](PlayerConfig* c, const OptionValue& v) -> Status {
                 const int64_t* i = std::get_if<int64_t>(&v);
                 if (!i)
                   return TypeMismatch("max-fps", "an integer");
                 if (*i < -1 || *i > 121)
                   return OutOfRange("max-fps", std::to_string(*i),
                                     "[-1, 121]");
                 c->video.max_fps = static_cast<int>(*i);
                 return OkStatus();
               },
               [](const PlayerConfig& c) -> OptionValue {
                 return static_cast<int64_t>(c.video.max_fps);
               },
               "drop frames from video whose fps is greater than max-fps"});

  e.push_back({"nodisp", P, Type::kBool, "render.disable_video_output",
               [](PlayerConfig* c, const OptionValue& v) -> Status {
                 const bool* b = std::get_if<bool>(&v);
                 if (!b)
                   return TypeMismatch("nodisp", "a boolean");
                 c->render.disable_video_output = *b;
                 return OkStatus();
               },
               [](const PlayerConfig& c) -> OptionValue {
                 return c.render.disable_video_output;
               },
               "disable graphical display"});

  e.push_back(
      {"start-on-prepared", P, Type::kBool, "start_on_prepared",
       [](PlayerConfig* c, const OptionValue& v) -> Status {
         const bool* b = std::get_if<bool>(&v);
         if (!b)
           return TypeMismatch("start-on-prepared", "a boolean");
         c->start_on_prepared = *b;
         return OkStatus();
       },
       [](const PlayerConfig& c) -> OptionValue { return c.start_on_prepared; },
       "automatically start playing on prepared"});

  e.push_back(
      {"enable-accurate-seek", P, Type::kBool, "seek.accurate",
       [](PlayerConfig* c, const OptionValue& v) -> Status {
         const bool* b = std::get_if<bool>(&v);
         if (!b)
           return TypeMismatch("enable-accurate-seek", "a boolean");
         c->seek.accurate = *b;
         return OkStatus();
       },
       [](const PlayerConfig& c) -> OptionValue { return c.seek.accurate; },
       "decode from the previous keyframe and drop until the target is "
       "reached"});

  // NOTE: this hand-written table covers a representative subset. The full
  // 60+ key table from docs/05 §4 is generated by tools/gen_options.py at M1.
}

OptionRegistry::~OptionRegistry() = default;

// static
const OptionRegistry& OptionRegistry::GetInstance() {
  static const OptionRegistry* instance = new OptionRegistry();
  return *instance;
}

Status OptionRegistry::SetValue(PlayerConfig* config, OptionCategory category,
                                std::string_view key,
                                const OptionValue& value) const {
  if (!config) {
    return Err(ErrorCode::kInvalidArgument, "config must not be null", {},
               "pass a valid PlayerConfig pointer");
  }
  for (const Impl::Entry& entry : impl_->entries) {
    if (entry.category == category && entry.key == key) {
      return entry.setter(config, value);
    }
  }
  return UnknownKey(key, SuggestNearestKey(key));
}

Status OptionRegistry::SetInt(PlayerConfig* config, OptionCategory category,
                              std::string_view key, int64_t value) const {
  return SetValue(config, category, key, OptionValue{value});
}

Status OptionRegistry::SetDouble(PlayerConfig* config, OptionCategory category,
                                 std::string_view key, double value) const {
  return SetValue(config, category, key, OptionValue{value});
}

Status OptionRegistry::SetString(PlayerConfig* config, OptionCategory category,
                                 std::string_view key,
                                 std::string_view value) const {
  // FORMAT / CODEC / SWS categories accept arbitrary keys and are forwarded to
  // FFmpeg verbatim, so an unknown key there is not an error.
  if (category != OptionCategory::kPlayer) {
    std::map<std::string, std::string>* target = nullptr;
    switch (category) {
    case OptionCategory::kFormat:
      target = &config->extra_format_options;
      break;
    case OptionCategory::kCodec:
      target = &config->extra_codec_options;
      break;
    case OptionCategory::kScaler:
      target = &config->extra_scaler_options;
      break;
    case OptionCategory::kPlayer:
      break;
    }
    if (target) {
      (*target)[std::string(key)] = std::string(value);
      return OkStatus();
    }
  }
  return SetValue(config, category, key, OptionValue{std::string(value)});
}

std::map<std::string, std::string>
OptionRegistry::Dump(const PlayerConfig& config,
                     OptionCategory category) const {
  std::map<std::string, std::string> out;
  for (const Impl::Entry& entry : impl_->entries) {
    if (entry.category != category)
      continue;
    const OptionValue v = entry.getter(config);
    std::visit(
        [&out, &entry](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, bool>) {
            out[entry.key] = value ? "1" : "0";
          } else if constexpr (std::is_same_v<T, std::string>) {
            out[entry.key] = value;
          } else {
            out[entry.key] = std::to_string(value);
          }
        },
        v);
  }
  return out;
}

std::vector<OptionDescriptor> OptionRegistry::Describe() const {
  std::vector<OptionDescriptor> out;
  out.reserve(impl_->entries.size());
  for (const Impl::Entry& entry : impl_->entries) {
    OptionDescriptor d;
    d.key = entry.key;
    d.category = entry.category;
    d.type = entry.type;
    d.config_field = entry.config_field;
    d.doc = entry.doc;
    out.push_back(std::move(d));
  }
  return out;
}

std::string OptionRegistry::SuggestNearestKey(std::string_view key) const {
  std::string best;
  int best_distance = static_cast<int>(key.size()) + 1;
  for (const Impl::Entry& entry : impl_->entries) {
    const int d = Levenshtein(key, entry.key);
    if (d < best_distance) {
      best_distance = d;
      best = entry.key;
    }
  }
  // Only suggest when the candidate is plausibly a typo, not a different word.
  return best_distance <= 3 ? best : std::string();
}

}  // namespace avbase
