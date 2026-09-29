// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/media_info.h"

namespace ijkpp::media {

int MediaInfo::FirstStreamOfKind(StreamKind kind) const {
  for (const StreamInfo& s : streams) {
    if (s.kind == kind) {
      return s.index;
    }
  }
  return -1;
}

const StreamInfo* MediaInfo::FindStream(int index) const {
  for (const StreamInfo& s : streams) {
    if (s.index == index) {
      return &s;
    }
  }
  return nullptr;
}

std::vector<const StreamInfo*> MediaInfo::StreamsOfKind(StreamKind kind) const {
  std::vector<const StreamInfo*> out;
  for (const StreamInfo& s : streams) {
    if (s.kind == kind) {
      out.push_back(&s);
    }
  }
  return out;
}

}  // namespace ijkpp::media
