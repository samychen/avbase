// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/composite_demuxer.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/demuxer_stream.h"

namespace avbase::media {
namespace {

// Everything that can be selected. Enough here to bound how high a child's
// own numbering can go, which is all the stride's definition needs.
constexpr DemuxerStreamType kSelectableTypes[] = {DemuxerStreamType::kVideo,
                                                  DemuxerStreamType::kAudio,
                                                  DemuxerStreamType::kText};
}  // namespace

CompositeDemuxer::CompositeDemuxer(std::vector<Source> sources) {
  sources_.reserve(sources.size());
  for (auto& s : sources) {
    sources_.push_back(Child{std::move(s.demuxer), std::move(s.descriptor),
                             std::move(s.options)});
  }
  media_info_ =
      sources_.empty() ? MediaInfo() : sources_[0].demuxer->media_info();
}

CompositeDemuxer::~CompositeDemuxer() = default;

DemuxerStream* CompositeDemuxer::GetStream(int source_id,
                                           DemuxerStreamType type) {
  if (source_id < 0 || source_id >= static_cast<int>(sources_.size())) {
    return nullptr;
  }
  return sources_[source_id].demuxer->GetStream(type);
}

std::vector<DemuxerStream*>
CompositeDemuxer::GetStreams(DemuxerStreamType type) {
  if (sources_.empty()) {
    return {};
  }
  return sources_[0].demuxer->GetStreams(type);
}

void CompositeDemuxer::SetActiveStream(DemuxerStreamType type,
                                       int stream_index) {
  const DemuxerStream* target = FindStream(type, stream_index);
  if (!target) {
    // Not ours -- including "no such index in any child". Silently routing it
    // is not an option: whichever source owns that number is the only one that
    // can route packets to it, so a misrouted index here would show up as
    // "the track switched and nothing arrived", with no error anywhere.
    LOG(WARNING) << "CompositeDemuxer: no " << GetDemuxerStreamTypeName(type)
                 << " stream " << stream_index
                 << " is among this resource's sources";
    return;
  }
  const int stride = IndexStride();
  const int source_id = stream_index / stride;
  sources_[static_cast<size_t>(source_id)].demuxer->SetActiveStream(
      type, stream_index - source_id * stride);
}

DemuxerStream* CompositeDemuxer::FindStream(DemuxerStreamType type,
                                            int stream_index) {
  const int stride = IndexStride();
  if (stream_index < 0 || stride <= 0) {
    return nullptr;
  }
  const int source_id = stream_index / stride;
  if (source_id >= static_cast<int>(sources_.size())) {
    return nullptr;
  }
  // Translated at the boundary: the child sees its OWN index, because that is
  // what its packets carry and what its routing predicate compares.
  return sources_[source_id].demuxer->FindStream(type, stream_index -
                                                           source_id * stride);
}

int CompositeDemuxer::IndexStride() const {
  // Derived, not a constant: a fixed stride would be a promise about how many
  // streams a container can carry, and breaking it would produce a collision
  // between two sources' streams -- exactly the silent ambiguity this numbering
  // exists to remove. max+1 cannot collide by construction.
  int max_child_index = -1;
  for (const auto& child : sources_) {
    for (DemuxerStreamType type : kSelectableTypes) {
      for (DemuxerStream* stream : child.demuxer->GetStreams(type)) {
        max_child_index = std::max(max_child_index, stream->stream_index());
      }
    }
  }
  return max_child_index + 1;
}

MediaInfo CompositeDemuxer::MergeMediaInfo() const {
  const int stride = IndexStride();
  MediaInfo merged = sources_[0].demuxer->media_info();
  merged.streams.clear();
  for (size_t i = 0; i < sources_.size(); ++i) {
    const int offset = static_cast<int>(i) * stride;
    for (StreamInfo info : sources_[i].demuxer->media_info().streams) {
      info.index = info.index + offset;
      // Which source this stream belongs to, for a host offering "source 1's
      // subtitles": the presence of a second source is invisible otherwise,
      // since two sources' identical-looking track lists differ only by this.
      info.metadata["avbase:source"] = std::to_string(i);
      merged.streams.push_back(std::move(info));
    }
  }
  return merged;
}

Demuxer* CompositeDemuxer::source(int id) const {
  if (id < 0 || id >= static_cast<int>(sources_.size())) {
    return nullptr;
  }
  return sources_[id].demuxer.get();
}

void CompositeDemuxer::Initialize(
    const DataSourceDescriptor& /*source*/, const DemuxerOptions& /*options*/,
    Host* host,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    Demuxer::InitializeCB init_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  host_ = host;
  init_cb_ = std::move(init_cb);
  combined_init_ = Status();
  pending_inits_ = sources_.size();
  if (pending_inits_ == 0) {
    if (init_cb_) {
      std::move(init_cb_).Run(Status());
    }
    return;
  }
  for (auto& child : sources_) {
    child.demuxer->Initialize(
        child.descriptor, child.options, this, media_task_runner,
        base::BindOnce(&CompositeDemuxer::OnChildInitialized,
                       weak_factory_.GetWeakPtr()));
  }
}

void CompositeDemuxer::OnChildInitialized(Status status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // First error wins; an ok child after an error does not clear it.
  if (!status.has_value() && combined_init_.has_value()) {
    combined_init_ = status;
  }
  if (--pending_inits_ == 0 && init_cb_) {
    std::move(init_cb_).Run(combined_init_);
  }
}

void CompositeDemuxer::StartPlayingFrom(base::TimeDelta time,
                                        Demuxer::SeekCB cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  seek_cb_ = std::move(cb);
  seek_status_ = Status();
  seek_actual_ = base::TimeDelta();
  seek_actuals_.assign(sources_.size(), base::TimeDelta());
  pending_seeks_ = sources_.size();
  if (pending_seeks_ == 0) {
    if (seek_cb_) {
      std::move(seek_cb_).Run(Status(), base::TimeDelta());
    }
    return;
  }
  for (size_t i = 0; i < sources_.size(); ++i) {
    sources_[i].demuxer->StartPlayingFrom(
        time, base::BindOnce(&CompositeDemuxer::OnChildSeeked,
                             weak_factory_.GetWeakPtr(), static_cast<int>(i)));
  }
}

void CompositeDemuxer::OnChildSeeked(int source_id, Status status,
                                     base::TimeDelta actual) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK_GE(source_id, 0);
  DCHECK_LT(static_cast<size_t>(source_id), seek_actuals_.size());
  if (!status.has_value() && seek_status_.has_value()) {
    seek_status_ = status;
  }
  seek_actual_ = std::max(seek_actual_, actual);
  // Recorded whatever the child returned: the AGGREGATE above is what the
  // callback sees, and a failed seek's landing is not a position anything
  // should align to -- the pipeline applies the offsets only when the
  // aggregate status is ok (stage 2). Recording it anyway keeps the vector
  // the complete answer to "where did each child land", which is what a
  // diagnostic needs after a partial failure.
  seek_actuals_[static_cast<size_t>(source_id)] = actual;
  if (--pending_seeks_ == 0 && seek_cb_) {
    std::move(seek_cb_).Run(seek_status_, seek_actual_);
  }
}

void CompositeDemuxer::Flush(base::OnceClosure flush_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  flush_cb_ = std::move(flush_cb);
  pending_flushes_ = sources_.size();
  if (pending_flushes_ == 0) {
    if (flush_cb_) {
      std::move(flush_cb_).Run();
    }
    return;
  }
  for (auto& child : sources_) {
    child.demuxer->Flush(base::BindOnce(&CompositeDemuxer::OnChildFlushed,
                                        weak_factory_.GetWeakPtr()));
  }
}

void CompositeDemuxer::OnChildFlushed() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (--pending_flushes_ == 0 && flush_cb_) {
    std::move(flush_cb_).Run();
  }
}

void CompositeDemuxer::Reset(base::OnceClosure reset_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  reset_cb_ = std::move(reset_cb);
  pending_resets_ = sources_.size();
  if (pending_resets_ == 0) {
    if (reset_cb_) {
      std::move(reset_cb_).Run();
    }
    return;
  }
  for (auto& child : sources_) {
    child.demuxer->Reset(base::BindOnce(&CompositeDemuxer::OnChildReset,
                                        weak_factory_.GetWeakPtr()));
  }
}

void CompositeDemuxer::OnChildReset() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (--pending_resets_ == 0 && reset_cb_) {
    std::move(reset_cb_).Run();
  }
}

void CompositeDemuxer::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (auto& child : sources_) {
    child.demuxer->Stop();
  }
}

const MediaInfo& CompositeDemuxer::media_info() const {
  // Computed on demand: child 0's MediaResource reports its real (post-
  // Initialize) stream configs and duration here, so the aggregate mirrors a
  // single demuxer for source 0 instead of returning the MediaInfo cached at
  // construction. Called from OnDemuxerInitialized (once) and through the
  // pipeline's own snapshot from there -- never per packet -- so composing
  // the merged answer here costs nothing that matters.
  if (sources_.empty()) {
    return media_info_;
  }
  if (sources_.size() == 1) {
    return sources_[0].demuxer->media_info();
  }
  media_info_ = MergeMediaInfo();
  return media_info_;
}

base::TimeDelta CompositeDemuxer::GetStartTime() const {
  return sources_.empty() ? base::TimeDelta()
                          : sources_[0].demuxer->GetStartTime();
}

bool CompositeDemuxer::IsLive() const {
  for (auto& child : sources_) {
    if (child.demuxer->IsLive()) {
      return true;
    }
  }
  return false;
}

bool CompositeDemuxer::IsSeekable() const {
  for (auto& child : sources_) {
    if (!child.demuxer->IsSeekable()) {
      return false;
    }
  }
  return !sources_.empty();
}

DemuxerStats CompositeDemuxer::GetStats() const {
  DemuxerStats stats;
  for (auto& child : sources_) {
    const DemuxerStats child_stats = child.demuxer->GetStats();
    stats.bytes_read += child_stats.bytes_read;
    stats.physical_position += child_stats.physical_position;
    stats.tcp_speed_bytes_per_sec += child_stats.tcp_speed_bytes_per_sec;
    stats.packets_demuxed += child_stats.packets_demuxed;
    stats.seek_count += child_stats.seek_count;
    stats.interrupt_count += child_stats.interrupt_count;
  }
  return stats;
}

const char* CompositeDemuxer::name() const {
  return "CompositeDemuxer";
}

void CompositeDemuxer::SetDuration(base::TimeDelta duration) {
  // Report the longest source duration to the pipeline host.
  if (duration > media_info_.duration) {
    media_info_.duration = duration;
    if (host_) {
      host_->SetDuration(duration);
    }
  }
}

void CompositeDemuxer::OnBufferedTimeUpdate(base::TimeDelta buffered,
                                            base::TimeDelta playback_time) {
  if (host_) {
    host_->OnBufferedTimeUpdate(buffered, playback_time);
  }
}

void CompositeDemuxer::OnDemuxerError(MediaError error) {
  if (host_) {
    host_->OnDemuxerError(error);
  }
}

}  // namespace avbase::media
