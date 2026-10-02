// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_MEDIA_FILTERS_LIVE_EDGE_POLICY_H_
#define IJKPP_MEDIA_FILTERS_LIVE_EDGE_POLICY_H_

#include "base/time/time.h"
#include "media/media_export.h"

namespace avbase::media {

// The live-edge chase policy (docs/12 §2.1, behaviour spec volume §3), as a
// pure decision core in the DecideNextFrame / BufferController pattern: no
// clock, no threads, exhaustively testable.
//
// WHEN CHASING APPLIES. Only for live sources (IsLive()) with a positive
// latency target (config.net.live_max_latency; zero means "never chase" --
// a VOD-style consumer of a live stream opts out explicitly). The playhead
// falls behind the live edge (the newest generated timestamp) whenever the
// decoder/renderer cannot keep up; past the target latency, playback shows
// stale content and the only remedy is to skip forward.
//
// WHAT THE CALLER DOES ON kChase. A self-initiated fast-forward to
// SuggestedTarget(): drop everything buffered (renderer flush + demuxer
// generation bump) and resume near the edge. The demuxer path is a skip,
// NOT a physical seek -- live containers cannot seek backward, and a
// forward skip is just "stop reading old bytes".
class AVBASE_MEDIA_EXPORT LiveEdgePolicy {
 public:
  // Pure decision. |behind| = live edge minus current media time.
  static bool ShouldChase(bool is_live, base::TimeDelta latency_hint,
                          base::TimeDelta behind);

  // Where to land after the skip: half the target behind the edge, so the
  // next comparison has headroom and the chase does not ping-pong.
  static base::TimeDelta SuggestedTarget(base::TimeDelta live_edge,
                                         base::TimeDelta latency_hint);

  LiveEdgePolicy() = delete;
};

}  // namespace avbase::media

#endif  // IJKPP_MEDIA_FILTERS_LIVE_EDGE_POLICY_H_
