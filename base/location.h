// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/location.h` (BSD-3-Clause).

#ifndef IJKPP_BASE_LOCATION_H_
#define IJKPP_BASE_LOCATION_H_

#include <string>

namespace ijkpp::base {

// Capture site of a posted task. Carried all the way to the task queue so that
// a "task ran after its target died" or "queue backed up" diagnostic can name
// the poster instead of just the runner.
class Location {
 public:
  constexpr Location() = default;
  constexpr Location(const char* file, int line) : file_(file), line_(line) {}

  constexpr const char* file_name() const { return file_; }
  constexpr int line_number() const { return line_; }
  constexpr bool has_source_info() const { return file_ != nullptr; }

  std::string ToString() const {
    if (!has_source_info()) {
      return "<unknown location>";
    }
    return std::string(file_) + ":" + std::to_string(line_);
  }

 private:
  const char* file_{nullptr};
  int line_{0};
};

}  // namespace ijkpp::base

#define FROM_HERE ::ijkpp::base::Location(__FILE__, __LINE__)

#endif  // IJKPP_BASE_LOCATION_H_
