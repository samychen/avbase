// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Aggregates N independent Demuxers into one MediaResource so that a
// multi-source pipeline (docs/14, stage 0) can own and address them by
// source_id. A single
// Demuxer is always source 0; wrapping one in a CompositeDemuxer behaves
// identically to using it directly, so the single-source path is unchanged.
//
// STATUS: IN THE BUILD (promoted by the stage-0 landing, docs/PROGRESS.md).
// Pure and portable -- no platform macros, no GL, no FFmpeg -- so it lives in
// media/base and obeys invariant C5.

#ifndef AVBASE_MEDIA_BASE_COMPOSITE_DEMUXER_H_
#define AVBASE_MEDIA_BASE_COMPOSITE_DEMUXER_H_

#include <memory>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/demuxer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_error.h"
#include "media/base/media_info.h"
#include "media/base/media_resource.h"
#include "media/media_export.h"

namespace avbase::media {

// One child of the aggregate, carrying the parameters needed to open it. Each
// child owns its own DataSourceDescriptor + DemuxerOptions because a composite
// source has no single open descriptor -- source 0's descriptor is not the
// other
// sources' descriptor.
struct CompositeDemuxerSource {
  std::unique_ptr<Demuxer> demuxer;
  DataSourceDescriptor descriptor;
  DemuxerOptions options;
};

// Aggregates N independent Demuxers behind the MediaResource interface.
//
// Lifecycle methods (Initialize/StartPlayingFrom/Flush/Reset/Stop) fan out to
// every child and only report completion once all children have answered.
// GetStream(source_id, type) routes to the child at |source_id|; the no-source
// overload answers source 0, so single-source callers see no change.
//
// CompositeDemuxer is also a Demuxer::Host: it forwards each child's host
// events (duration, buffering, errors) to the pipeline host it was given.
class AVBASE_MEDIA_EXPORT CompositeDemuxer : public Demuxer,
                                             public Demuxer::Host {
 public:
  using Source = CompositeDemuxerSource;

  explicit CompositeDemuxer(std::vector<Source> sources);
  ~CompositeDemuxer() override;

  // MediaResource (addressing axis).
  DemuxerStream* GetStream(DemuxerStreamType type) override {
    return GetStream(0, type);
  }
  DemuxerStream* GetStream(int source_id, DemuxerStreamType type) override;

  // Forwarded to source 0 so a single wrapped demuxer behaves exactly as it did
  // when used directly: multi-stream sources expose all their streams for track
  // selection, and the active-stream choice reaches the real demuxer.
  std::vector<DemuxerStream*> GetStreams(DemuxerStreamType type) override;
  void SetActiveStream(DemuxerStreamType type, int stream_index) override;

  // Demuxer lifecycle, fanned out to every child. Each child carries its own
  // open parameters in its Source bundle; the single |source|/|options| from
  // Demuxer::Initialize do not apply to a multi-source resource and are
  // ignored.
  void Initialize(const DataSourceDescriptor& source,
                  const DemuxerOptions& options, Host* host,
                  base::scoped_refptr<base::SequencedTaskRunner>
                      media_task_runner,
                  Demuxer::InitializeCB init_cb) override;
  void StartPlayingFrom(base::TimeDelta time, Demuxer::SeekCB cb) override;
  void Flush(base::OnceClosure flush_cb) override;
  void Reset(base::OnceClosure reset_cb) override;
  void Stop() override;

  const MediaInfo& media_info() const override;
  base::TimeDelta GetStartTime() const override;
  bool IsLive() const override;
  bool IsSeekable() const override;
  DemuxerStats GetStats() const override;
  const char* name() const override;

  // Aggregate inspection.
  size_t source_count() const { return sources_.size(); }
  Demuxer* source(int id) const;

  // Demuxer::Host: forward child events to the pipeline host.
  void SetDuration(base::TimeDelta duration) override;
  void OnBufferedTimeUpdate(base::TimeDelta buffered,
                            base::TimeDelta playback_time) override;
  void OnDemuxerError(MediaError error) override;

 private:
  struct Child {
    std::unique_ptr<Demuxer> demuxer;
    DataSourceDescriptor descriptor;
    DemuxerOptions options;
  };

  void OnChildInitialized(Status status);
  void OnChildSeeked(Status status, base::TimeDelta actual);
  void OnChildFlushed();
  void OnChildReset();

  std::vector<Child> sources_;
  base::raw_ptr<Host> host_ = nullptr;
  MediaInfo media_info_;

  // Initialize fan-out state.
  size_t pending_inits_ = 0;
  Status combined_init_ = Status();
  Demuxer::InitializeCB init_cb_;

  // Seek fan-out state.
  size_t pending_seeks_ = 0;
  Status seek_status_ = Status();
  base::TimeDelta seek_actual_;
  Demuxer::SeekCB seek_cb_;

  // Flush / Reset fan-out state.
  size_t pending_flushes_ = 0;
  base::OnceClosure flush_cb_;
  size_t pending_resets_ = 0;
  base::OnceClosure reset_cb_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CompositeDemuxer> weak_factory_{this};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_COMPOSITE_DEMUXER_H_
