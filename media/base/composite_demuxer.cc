// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/composite_demuxer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"

namespace avbase::media {

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
  if (!sources_.empty()) {
    sources_[0].demuxer->SetActiveStream(type, stream_index);
  }
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
  // single demuxer exactly for source 0 instead of returning the default
  // MediaInfo cached at construction.
  if (sources_.empty()) {
    return media_info_;
  }
  return sources_[0].demuxer->media_info();
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
