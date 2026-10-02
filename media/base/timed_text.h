// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_TIMED_TEXT_H_
#define AVBASE_MEDIA_BASE_TIMED_TEXT_H_

#include <string>

#include "base/time/time.h"
#include "media/media_export.h"

namespace avbase::media {

// One decoded subtitle cue. The base's contract for text tracks is "text +
// time only" (avbase upgrade plan §Phase 4.2): layout and rendering belong to
// the host, which is why both a plain string and the raw markup are carried.
struct AVBASE_MEDIA_EXPORT TimedTextCue {
  base::TimeDelta pts;
  base::TimeDelta duration;
  std::string text;    // Plain text, all cue lines joined with '\n'.
  std::string ass;     // Raw ASS markup (ASS-type rects), empty for plain.
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_TIMED_TEXT_H_
