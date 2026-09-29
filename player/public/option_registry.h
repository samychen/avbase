// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_PLAYER_PUBLIC_OPTION_REGISTRY_H_
#define IJKPP_PLAYER_PUBLIC_OPTION_REGISTRY_H_

#include <stdint.h>

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "player/public/error.h"
#include "player/public/player_config.h"
#include "player/public/player_export.h"

namespace ijkpp {

// Mirrors ijkplayer's IJKMP_OPT_CATEGORY_*.
enum class OptionCategory { kPlayer = 0, kFormat, kCodec, kScaler };

using OptionValue = std::variant<bool, int64_t, double, std::string>;

struct IJKPP_PLAYER_EXPORT OptionDescriptor {
  std::string key;
  OptionCategory category{OptionCategory::kPlayer};
  enum class Type { kBool, kInt, kInt64, kDouble, kString, kEnum } type{Type::kInt};
  OptionValue min;
  OptionValue max;
  OptionValue default_value;
  std::string config_field;   // e.g. "buffer.max_bytes"
  std::string doc;
};

// String-keyed configuration, kept for migrating existing setOption() call
// sites. Unlike ijkplayer, an unknown key or an out-of-range value is an
// error rather than a silent no-op (behaviour difference Δ2).
//
// The table is generated from player_config.h by tools/gen_options.py so the
// typed fields and the string keys can never drift apart (rule C13).
class IJKPP_PLAYER_EXPORT OptionRegistry {
 public:
  OptionRegistry(const OptionRegistry&) = delete;
  OptionRegistry& operator=(const OptionRegistry&) = delete;

  static const OptionRegistry& GetInstance();

  Status SetInt(PlayerConfig* config, OptionCategory category,
                std::string_view key, int64_t value) const;
  Status SetDouble(PlayerConfig* config, OptionCategory category,
                   std::string_view key, double value) const;
  Status SetString(PlayerConfig* config, OptionCategory category,
                   std::string_view key, std::string_view value) const;
  Status SetValue(PlayerConfig* config, OptionCategory category,
                  std::string_view key, const OptionValue& value) const;

  // Inverse of the setters: exports a config as string key/value pairs, for
  // logging and for diffing two players' configurations.
  std::map<std::string, std::string> Dump(const PlayerConfig& config,
                                          OptionCategory category) const;

  // Full machine-readable description; also the source for docs/API options.
  std::vector<OptionDescriptor> Describe() const;

  // "did you mean ..." helper used by the kInvalidArgument error message.
  std::string SuggestNearestKey(std::string_view key) const;

 private:
  OptionRegistry();
  ~OptionRegistry();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ijkpp

#endif  // IJKPP_PLAYER_PUBLIC_OPTION_REGISTRY_H_
