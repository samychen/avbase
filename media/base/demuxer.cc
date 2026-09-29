// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/demuxer.h"

namespace ijkpp::media {

const char* GetStreamLivenessName(StreamLiveness liveness) {
  switch (liveness) {
    case StreamLiveness::kUnknown:  return "unknown";
    case StreamLiveness::kRecorded: return "recorded";
    case StreamLiveness::kLive:     return "live";
  }
  return "invalid";
}

// static
const char* DemuxerStream::GetStatusName(Status status) {
  switch (status) {
    case Status::kOk:            return "ok";
    case Status::kAborted:       return "aborted";
    case Status::kConfigChanged: return "config-changed";
    case Status::kError:         return "error";
  }
  return "invalid";
}

StreamLiveness DemuxerStream::liveness() const {
  return StreamLiveness::kUnknown;
}

Demuxer::Demuxer() = default;
Demuxer::~Demuxer() = default;

}  // namespace ijkpp::media
