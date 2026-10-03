// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Runtime track selection and the routing predicates it needs, split out of
// ffmpeg_demuxer.cc when that file crossed its C1 line ceiling. Same seam and
// same reason as media/filters/pipeline_track_select.cc: "which stream of a
// type is active" is a routing question, while everything left in
// ffmpeg_demuxer.cc is the AVFormatContext lifecycle (open, demux loop, seek,
// close). Keeping them apart means the routing rules can be read -- and
// changed -- without reading the demux loop.
//
// WHAT MOVED HERE, and why it is a real boundary rather than a line count:
//   * GetStream / GetStreams -- enumeration by type.
//   * IsActiveRoutingTarget -- the per-packet predicate the demux loop calls
//     for every packet it routes. It is the read side of the three
//     active_*_ atomics.
//   * SetActiveStream -- the write side, and the only place those atomics are
//     stored.
//   * FFmpegDemuxerStream::EnqueueTextFromDemuxThread -- the text leg's
//     enqueue, which differs from the A/V one on purpose: the text queue is
//     permanently bounded and drops its oldest packet (TryPushDropOldest)
//     rather than applying back-pressure, because a subtitle packet arrives
//     once and either a late subscriber gets it or it is stale. That rule
//     belongs next to the routing code it is part of, not 800 lines away in
//     the middle of the A/V path.
//
// Nothing here touches the demux thread's own state beyond the atomics and
// streams_, both of which the contract already shares with the demux loop:
// the loop reads the atomics per packet, and SetActiveStream only stores.

#include "media/filters/ffmpeg_demuxer.h"

#include <utility>
#include <vector>

#include "base/synchronization/lock.h"
#include "media/base/demuxer_stream.h"

namespace avbase::media {

DemuxerStream* FFmpegDemuxer::GetStream(DemuxerStreamType type) {
  for (auto& stream : streams_) {
    if (stream->type() == type) {
      return stream.get();
    }
  }
  return nullptr;
}

std::vector<DemuxerStream*> FFmpegDemuxer::GetStreams(DemuxerStreamType type) {
  std::vector<DemuxerStream*> out;
  out.reserve(streams_.size());
  for (auto& stream : streams_) {
    if (stream->type() == type) {
      out.push_back(stream.get());
    }
  }
  return out;
}

bool FFmpegDemuxer::IsActiveRoutingTarget(int index,
                                          DemuxerStreamType type) const {
  int active = -1;
  if (type == DemuxerStreamType::kVideo) {
    active = active_video_.load(std::memory_order_relaxed);
  } else if (type == DemuxerStreamType::kAudio) {
    active = active_audio_.load(std::memory_order_relaxed);
  } else if (type == DemuxerStreamType::kText) {
    active = active_text_.load(std::memory_order_relaxed);
  }
  return index == active;
}

void FFmpegDemuxer::SetActiveStream(DemuxerStreamType type, int stream_index) {
  // Relaxed is enough: the demux thread re-reads the value per packet.
  if (type == DemuxerStreamType::kVideo) {
    active_video_.store(stream_index, std::memory_order_relaxed);
  } else if (type == DemuxerStreamType::kAudio) {
    active_audio_.store(stream_index, std::memory_order_relaxed);
  } else if (type == DemuxerStreamType::kText) {
    active_text_.store(stream_index, std::memory_order_relaxed);
  }
  // WAKE THE DEMUX LOOP, and this is the whole reason a track switch used to
  // hang the video leg.
  //
  // The loop's backpressure wait (ReadAndRouteOnePacket) blocks until the
  // target stream's queue has room, and it is signalled by exactly three
  // things: Flush, Stop, and a seek. A track switch was none of them, so a
  // switch that retired a stream whose queue was already full left the demux
  // thread parked in that wait FOREVER -- the queue could never drain again,
  // because the renderer that used to drain it had just been replaced, and
  // nothing was going to signal. Symptom from the outside: the new renderer
  // arms its decode pump, issues one Read, and never gets a buffer, so it
  // presents nothing for the rest of playback.
  //
  // The old comment here claimed a packet routed to the just-retired stream
  // "is harmless (it lands in a queue whose consumer is draining or gone)".
  // The "or gone" case is precisely the one that wedges, and it is the case a
  // track switch creates by definition.
  resume_event_.Signal();
}

bool FFmpegDemuxerStream::EnqueueTextFromDemuxThread(
    base::scoped_refptr<DecoderBuffer> buffer) {
  if (!queue_->TryPushDropOldest(std::move(buffer))) {
    return false;  // Aborted/closed: the caller drops the packet.
  }
  base::AutoLock scoped(lock_);
  FulfilPendingReadLocked();
  return true;
}

}  // namespace avbase::media
