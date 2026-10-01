// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/media_resource.h` (BSD-3-Clause), reduced to
// the single hook avbase actually needs.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// (milestone M7/M8, docs/08 §2).
// Interface frozen so M7 and M8 can be written in parallel. Excluded from
// every CMake target on purpose; see media/base/pipeline_status.h for the
// DRAFT convention this project uses.
//
// Gaps to close before this file joins the build:
//   1. The only implementation is Demuxer itself (Demuxer : public
//      MediaResource), which needs the base added in media/base/demuxer.h --
//      a one-line change, but it touches a frozen M4 header, so it goes
//      through interface review.
//   2. RendererImpl::Initialize() is the only caller (M7).
//   3. Chromium's GetFirstDataSource()/byte-range loaders are intentionally
//      absent: decision D2 keeps FFmpeg types out of media/base, and avbase's
//      byte source is media::DataSource, reached through the Demuxer rather
//      than through the resource. If M9's DataSource decorators
//      (RetryDataSource / LiveDataSource) need a second access path, add it
//      here rather than widening Demuxer.

#ifndef AVBASE_MEDIA_BASE_MEDIA_RESOURCE_H_
#define AVBASE_MEDIA_BASE_MEDIA_RESOURCE_H_

#include "media/base/demuxer_stream.h"
#include "media/media_export.h"

namespace avbase::media {

// What a Renderer is given to read from.
//
// WHY AN ABSTRACTION AND NOT `Demuxer*`. Two reasons, both of which are
// already true in this codebase:
//
//   1. Testing. tests/support/synthetic_demuxer (docs/07 §5) generates
//      deterministic streams -- frame numbers encoded as pixel blocks, audio
//      pts encoded as sine frequency -- so that seek landing points and A/V
//      sync can be asserted without a real container. A Renderer that took a
//      Demuxer would still accept it, but a Renderer that takes a
//      MediaResource can also be handed a fake that returns a fixed stream
//      list, which is what the RendererImpl unit tests want.
//   2. Track switching. Player::SelectTrack() (M8) and Pipeline::AddVideoStream
//      change which streams the renderer consumes without re-opening the
//      source. Routing that through GetStream(type) keeps the change on the
//      media sequence and gives RendererClient::OnTracksChanged something to
//      report.
//
// Threading: called on the media sequence only. Implementations need no lock;
// SEQUENCE_CHECKER enforces it.
class AVBASE_MEDIA_EXPORT MediaResource {
 public:
  MediaResource(const MediaResource&) = delete;
  MediaResource& operator=(const MediaResource&) = delete;

  // Returns the stream of the given type, or nullptr when the source has none
  // (audio-only file, or video disabled by config.video.disabled). Returning
  // nullptr is not an error: RendererImpl must fall back to the external clock
  // when there is no audio stream, exactly as ffplay does.
  virtual DemuxerStream* GetStream(DemuxerStreamType type) = 0;

 protected:
  MediaResource() = default;
  virtual ~MediaResource() = default;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_MEDIA_RESOURCE_H_
