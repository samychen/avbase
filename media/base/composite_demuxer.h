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

  // Deliberately still source 0's answer. RendererImpl::IsPrimaryStream()
  // uses this to ask "is it one of SOURCE 0's streams" -- that is what keeps a
  // secondary source's video or audio stream out of the primary leg's track
  // switch. Widening this list would silently widen that guard, so the
  // all-sources question is a different call (FindStream below).
  std::vector<DemuxerStream*> GetStreams(DemuxerStreamType type) override;
  void SetActiveStream(DemuxerStreamType type, int stream_index) override;

  // ---- The aggregate index space (docs/14 stage 4) ----------------------
  //
  // Each child numbers its streams from its own container order, so source 1's
  // text stream #2 and source 0's are the same integer. Two children, one
  // index space: the aggregate numbers its streams
  //
  //     aggregate_index = source_id * stride + child_index
  //
  // where |stride| is one more than the largest container index any child
  // reports. Source 0's offset is therefore 0 and its indices are literally
  // unchanged -- which is what makes the single-source path identical to
  // handing the child over unwrapped, and what makes the two spaces provably
  // disjoint rather than probably so.
  //
  // This is why every stream has TWO numbers: the child answers its own, and
  // only the aggregate boundary translates. Nothing below it changes -- the
  // demux loop compares the stream index the CONTAINER wrote on the packet,
  // so a child still receives its own numbering.
  DemuxerStream* FindStream(DemuxerStreamType type, int stream_index) override;

  // The source a stream index belongs to, under the same aggregate numbering as
  // FindStream. Answers index / stride (see FindStream); an out-of-range index
  // falls back to 0 so a caller that already failed FindStream gets a harmless
  // source rather than an invalid one.
  int SourceIdForIndex(DemuxerStreamType type, int stream_index) override;

  // Demuxer lifecycle, fanned out to every child. Each child carries its own
  // open parameters in its Source bundle; the single |source|/|options| from
  // Demuxer::Initialize do not apply to a multi-source resource and are
  // ignored.
  void
  Initialize(const DataSourceDescriptor& source, const DemuxerOptions& options,
             Host* host,
             base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
             Demuxer::InitializeCB init_cb) override;
  void StartPlayingFrom(base::TimeDelta time, Demuxer::SeekCB cb) override;
  void Flush(base::OnceClosure flush_cb) override;
  void Reset(base::OnceClosure reset_cb) override;
  void Stop() override;

  // Every source's streams, numbered in the aggregate space above. Source 0's
  // scalars describe source 0's container -- duration included: an aggregate
  // has no single format_name, bit_rate or duration, and inventing one would
  // be worse than saying whose it is. (SetDuration() separately reports the
  // LONGEST child's duration upstream, for the playback clock; that number is
  // deliberately not the one this snapshot carries.)
  // Source-major order, so FirstStreamOfKind still answers a source-0 stream.
  const MediaInfo& media_info() const override;
  base::TimeDelta GetStartTime() const override;
  bool IsLive() const override;
  bool IsSeekable() const override;
  DemuxerStats GetStats() const override;
  const char* name() const override;
  size_t GetBufferedPacketCount() const override;

  // Aggregate inspection.
  // MediaResource's addressing contract (media/base/media_resource.h): an
  // aggregate is the one resource whose source_count() exceeds 1, and it must
  // route GetStream(source_id, type) to match -- both were added together in
  // docs/14 stage 0/1.
  int source_count() const override {
    return static_cast<int>(sources_.size());
  }
  Demuxer* source(int id) const;

  // Where the last StartPlayingFrom landed EACH child, indexed by source id;
  // empty before the first seek. The pipeline derives the per-source playback
  // offsets (docs/14 stage 2, §7.2) from these: a secondary source's seek
  // lands where ITS keyframe geometry says, and the difference against source
  // 0's landing is what maps the two timelines onto one master moment. The
  // aggregate landing stays available through the SeekCB.
  const std::vector<base::TimeDelta>& seek_actuals() const {
    return seek_actuals_;
  }

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
  // docs/14 stage 4: the aggregate index space, and the merged MediaInfo built
  // in it. Both are derived from the children rather than cached in one, so
  // a child that grows its stream list at Initialize is not answering from
  // a stale answer taken at construction -- GetStreams is a live query.
  int IndexStride() const;
  // The ONE place an aggregate index is taken apart into (source_id,
  // child_index). Every public entry point that answers "which stream is
  // index N" must go through here: the disjointness of the two numbering
  // spaces (FindStream's contract above) is a property of this arithmetic,
  // and three open-coded copies of it would be three chances to drift --
  // which is the silent ambiguity the aggregate index exists to remove.
  // Returns false when the index names no stream in any source.
  bool DecodeIndex(int stream_index, int* source_id, int* child_index) const;
  MediaInfo MergeMediaInfo() const;
  // |source_id| is the child's index, bound FIRST by StartPlayingFrom
  // (base::BindOnce passes pre-bound arguments ahead of the callback's), so
  // the landing point is recorded against the child that reported it.
  void OnChildSeeked(int source_id, Status status, base::TimeDelta actual);
  void OnChildFlushed();
  void OnChildReset();

  std::vector<Child> sources_;
  base::raw_ptr<Host> host_ = nullptr;
  // mutable: media_info() is const and the multi-source answer is composed on
  // demand (path above for why it cannot be cached once at construction).
  // Carries the MERGED snapshot only -- never a running maximum: the longest
  // child duration SetDuration() reports upstream lives in
  // |reported_duration_| below, because this member is overwritten wholesale
  // by the next media_info() call and a high-water kept here would be
  // silently reset by it.
  mutable MediaInfo media_info_;
  // The longest duration any child has reported (Demuxer::Host::SetDuration).
  // Deliberately NOT media_info_.duration: the snapshot above is source 0's
  // scalars by design (see the media_info() comment), and the header there
  // promises the two numbers stay separate.
  base::TimeDelta reported_duration_;

  // Initialize fan-out state.
  size_t pending_inits_ = 0;
  Status combined_init_ = Status();
  Demuxer::InitializeCB init_cb_;

  // Seek fan-out state.
  size_t pending_seeks_ = 0;
  Status seek_status_ = Status();
  base::TimeDelta seek_actual_;
  // Where EACH child's seek landed, indexed by source id (docs/14 stage 2).
  // The aggregate |seek_actual_| above is the max of these and answers the
  // pipeline's own resume question; the per-child values answer a different
  // one -- "which moment of source i is on screen when source 0 shows moment
  // T" -- which is what the playback offset is derived from. Sized by
  // StartPlayingFrom, so a reader that arrives before any seek sees an empty
  // vector rather than stale landings.
  std::vector<base::TimeDelta> seek_actuals_;
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
