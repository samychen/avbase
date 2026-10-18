// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PUBLIC_OPTION_REGISTRY_H_
#define AVBASE_PLAYER_PUBLIC_OPTION_REGISTRY_H_

#include <stdint.h>

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "player/public/error.h"
#include "player/public/player_config.h"
#include "player/public/player_export.h"

namespace avbase {

// Mirrors ijkplayer's IJKMP_OPT_CATEGORY_*.
enum class OptionCategory { kPlayer = 0, kFormat, kCodec, kScaler };

using OptionValue = std::variant<bool, int64_t, double, std::string>;

struct AVBASE_PLAYER_EXPORT OptionDescriptor {
  std::string key;
  OptionCategory category{OptionCategory::kPlayer};
  enum class Type {
    kBool,
    kInt,
    kInt64,
    kDouble,
    kString,
    kEnum
  } type{Type::kInt};
  OptionValue min;
  OptionValue max;
  OptionValue default_value;
  std::string config_field;  // e.g. "buffer.max_bytes"
  std::string doc;
};

// String-keyed configuration, kept for migrating existing setOption() call
// sites. Unlike ijkplayer, an unknown key or an out-of-range value is an
// error rather than a silent no-op (behaviour difference Δ2).
//
// The table is generated from player_config.h by tools/gen_options.py so the
// typed fields and the string keys can never drift apart (rule C13).
class AVBASE_PLAYER_EXPORT OptionRegistry {
 public:
  OptionRegistry(const OptionRegistry&) = delete;
  OptionRegistry& operator=(const OptionRegistry&) = delete;

  // Returns the process-wide OptionRegistry.
  // Thread safety: any thread; the instance is constructed once and then
  // read-only, so the read path needs no locking.
  // Errors: none (never fails). Side effects: none -- first-call construction
  // is deferred and idempotent.
  static const OptionRegistry& GetInstance();

  // Writes |value| into |config| for the option |key| of |category|.
  // Errors: kInvalidArgument for an unknown key (the message carries a
  // "did you mean" suggestion) or a value outside the option's range.
  // Side effects: mutates |config| only on success.
  Status SetInt(PlayerConfig* config, OptionCategory category,
                std::string_view key, int64_t value) const;
  // Double form of SetInt; same contract.
  // Errors: kInvalidArgument for an unknown key or out-of-range value.
  // Side effects: mutates |config| only on success.
  Status SetDouble(PlayerConfig* config, OptionCategory category,
                   std::string_view key, double value) const;
  // String form of SetInt; same contract.
  // Errors: kInvalidArgument for an unknown key or unparsable value.
  // Side effects: mutates |config| only on success.
  Status SetString(PlayerConfig* config, OptionCategory category,
                   std::string_view key, std::string_view value) const;
  // Typed-value form of SetInt; dispatches on the OptionValue's kind.
  // Errors: kInvalidArgument for an unknown key or a kind the option does
  // not accept.
  // Side effects: mutates |config| only on success.
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

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_OPTION_REGISTRY_H_
