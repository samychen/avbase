// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/render_perf_stats.h"

#include <cstdio>

namespace avbase::media {
namespace {

// One-eighth weighting, i.e. a window of about eight frames. Responsive enough
// that a regression moves the number within a tenth of a second at 60 fps,
// damped enough that one outlier frame does not decide what the reader sees.
constexpr double kEwmaAlpha = 0.125;

std::string Ms(double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", value);
  return buf;
}

}  // namespace

const char* UploadPathName(UploadPath path) {
  switch (path) {
  case UploadPath::kNone:
    return "none";
  case UploadPath::kCpuSync:
    return "cpu_sync";
  case UploadPath::kPbo:
    return "pbo";
  case UploadPath::kHwZeroCopy:
    return "hw_zero_copy";
  }
  return "unknown";
}

const char* VideoFilterStateName(VideoFilterState state) {
  switch (state) {
  case VideoFilterState::kOff:
    return "off";
  case VideoFilterState::kRunning:
    return "running";
  case VideoFilterState::kRefusedHardwareFrame:
    return "refused_hardware_frame";
  case VideoFilterState::kRefusedNoFactory:
    return "refused_no_factory";
  case VideoFilterState::kRefusedGraphFailed:
    return "refused_graph_failed";
  }
  return "unknown";
}

void DurationEwma::Add(double ms) {
  ++samples_;
  if (ms > max_ms_) {
    max_ms_ = ms;
  }
  // The first sample seeds the average instead of blending toward it from
  // zero. An EMA started at 0 needs about eight frames to reach the truth, and
  // those are exactly the frames a reader watches when a video starts, so the
  // naive form would report a suspiciously cheap first second.
  average_ms_ =
      samples_ == 1 ? ms : average_ms_ + (ms - average_ms_) * kEwmaAlpha;
}

void DurationEwma::Reset() {
  average_ms_ = 0.0;
  max_ms_ = 0.0;
  samples_ = 0;
}

std::string RenderPerfStats::ToJson() const {
  std::string out = "{";
  out += "\"upload_ms\":" + Ms(upload.average_ms()) + ",";
  out += "\"compose_ms\":" + Ms(compose.average_ms()) + ",";
  out += "\"blit_ms\":" + Ms(blit.average_ms()) + ",";
  out += "\"present_ms\":" + Ms(present.average_ms()) + ",";
  out += "\"max_present_ms\":" + Ms(present.max_ms()) + ",";
  out +=
      "\"upload_path\":\"" + std::string(UploadPathName(upload_path)) + "\",";
  out += "\"filter_state\":\"" +
         std::string(VideoFilterStateName(filter_state)) + "\"";
  out += "}";
  return out;
}

}  // namespace avbase::media
