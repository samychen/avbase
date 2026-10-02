// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Replaces ijkplayer's read_thread() in ff_ffplay.c. Structure follows
// Chromium's media::FFmpegDemuxer; the threading model deliberately does not —
// see docs/04 §2.1 (decision D3) for why the demux loop runs on its own
// blocking thread instead of on the media sequence.


#ifndef AVBASE_MEDIA_FILTERS_FFMPEG_DEMUXER_H_
#define AVBASE_MEDIA_FILTERS_FFMPEG_DEMUXER_H_

#include <stdint.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "media/base/data_source.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/decoder_buffer_queue.h"
#include "media/base/decoder_config.h"
#include "media/base/demuxer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_log.h"
#include "media/filters/ffmpeg_glue.h"
#include "media/media_export.h"

namespace avbase::media {

// One elementary stream. Owns the DecoderBufferQueue the demux thread fills.
class AVBASE_MEDIA_EXPORT FFmpegDemuxerStream final : public DemuxerStream {
 public:
  FFmpegDemuxerStream(DemuxerStreamType type, int32_t index,
                      base::scoped_refptr<MediaLog> media_log,
                      base::scoped_refptr<base::SequencedTaskRunner> media_runner,
                      const VideoDecoderConfig& video_config,
                      const AudioDecoderConfig& audio_config,
                      StreamLiveness liveness);
  FFmpegDemuxerStream(const FFmpegDemuxerStream&) = delete;
  FFmpegDemuxerStream& operator=(const FFmpegDemuxerStream&) = delete;
  ~FFmpegDemuxerStream() override;

  // DemuxerStream:
  void Read(uint32_t count, ReadCB read_cb) override;
  const AudioDecoderConfig& audio_decoder_config() const override { return audio_config_; }
  const VideoDecoderConfig& video_decoder_config() const override { return video_config_; }
  DemuxerStreamType type() const override { return type_; }
  int32_t stream_index() const override { return index_; }
  StreamLiveness liveness() const override { return liveness_; }
  bool SupportsConfigChanges() const override { return false; }
  int32_t serial() const override { return queue_->serial(); }
  size_t buffered_buffers() const override { return queue_->size(); }
  size_t buffered_bytes() const override { return queue_->bytes(); }
  base::TimeDelta buffered_duration() const override { return queue_->buffered_duration(); }

  // ---- Called on the demux thread -----------------------------------------
  // Enqueues a buffer and, if a Read() is waiting, posts its reply to the media
  // sequence. This is what keeps the media sequence free of blocking calls: the
  // producer does the fulfilling, the consumer only ever receives callbacks.
  bool EnqueueFromDemuxThread(base::scoped_refptr<DecoderBuffer> buffer);
  void NotifyEosFromDemuxThread();
  void FlushFromDemuxThread(base::OnceClosure done);
  void AbortFromDemuxThread();

 private:
  // Satisfies a pending Read() if the queue can provide data now.
  // Called with |lock_| held; posts to |media_runner_| and never blocks.
  // The *Locked suffix is the contract: the caller already holds |lock_|.
  void FulfilPendingReadLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  const DemuxerStreamType type_;
  const int32_t index_;
  const base::scoped_refptr<MediaLog> media_log_;
  const base::scoped_refptr<base::SequencedTaskRunner> media_runner_;
  const VideoDecoderConfig video_config_;
  const AudioDecoderConfig audio_config_;
  const StreamLiveness liveness_;
  const std::unique_ptr<DecoderBufferQueue> queue_;

  // Guards only the pending-read slot; buffer storage has its own lock.
  mutable base::Lock lock_;
  uint32_t pending_count_ GUARDED_BY(lock_){0};
  ReadCB pending_read_cb_ GUARDED_BY(lock_);
  bool aborted_ GUARDED_BY(lock_){false};
};

class AVBASE_MEDIA_EXPORT FFmpegDemuxer final : public Demuxer {
 public:
  explicit FFmpegDemuxer(base::scoped_refptr<MediaLog> media_log);
  FFmpegDemuxer(const FFmpegDemuxer&) = delete;
  FFmpegDemuxer& operator=(const FFmpegDemuxer&) = delete;
  ~FFmpegDemuxer() override;

  // Demuxer:
  void Initialize(const DataSourceDescriptor& source,
                  const DemuxerOptions& options,
                  Host* host,
                  base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
                  InitializeCB init_cb) override;
  void StartPlayingFrom(base::TimeDelta time, SeekCB cb) override;
  void Flush(base::OnceClosure flush_cb) override;
  void Reset(base::OnceClosure reset_cb) override;
  void Stop() override;
  DemuxerStream* GetStream(DemuxerStreamType type) override;
  std::vector<DemuxerStream*> GetStreams(DemuxerStreamType type) override;
  void SetActiveStream(DemuxerStreamType type, int stream_index) override;
  bool IsActiveRoutingTarget(int index, DemuxerStreamType type) const;
  const MediaInfo& media_info() const override { return media_info_; }
  base::TimeDelta GetStartTime() const override { return start_time_; }
  bool IsLive() const override { return media_info_.is_live; }
  bool IsSeekable() const override { return media_info_.seekable; }
  DemuxerStats GetStats() const override;
  const char* name() const override { return "FFmpegDemuxer"; }

 private:
  // One physical seek request, handed to the demux thread through an atomic
  // swap slot so that a burst of seeks collapses to the latest one.
  struct SeekRequest {
    base::TimeDelta target;
    bool any_frame{false};
    int64_t request_id{0};
  };

  // ---- demux thread ("avbase-demux") ----
  Status OpenOnDemuxThread(const DataSourceDescriptor& source,
                           const DemuxerOptions& options);
  // Builds the AVDictionary that avformat_open_input receives. Split out of
  // OpenOnDemuxThread so the option precedence (structured config first, then
  // the verbatim passthrough map) is reviewable in one place.
  static std::map<std::string, std::string> BuildOpenOptions(
      const DemuxerOptions& options, const std::string& uri);
  void DemuxLoop();
  // One iteration of the read/backpressure step. Returns false when the loop
  // should exit.
  // void* rather than AVFormatContext*/AVPacket*: no libav* type may appear in
  // a media/ header (invariant C4). The .cc casts via its local Ctx() helper.
  bool ReadAndRouteOnePacket(void* ctx, void* packet);
  void HandleSeekRequestOnDemuxThread(const SeekRequest& request);
  void BuildMediaInfo();
  // Fills one StreamInfo and creates the matching FFmpegDemuxerStream.
  void AddStream(void* av_stream, uint32_t index, StreamLiveness liveness);
  // Runs and erases the SeekCB registered for |request_id|.
  void CompleteSeek(int64_t request_id, Status status, base::TimeDelta actual);

  // ---- media sequence ----
  void OnOpened(Status status, InitializeCB init_cb);
  void OnSeekedOnDemuxThread(Status status, base::TimeDelta actual,
                             int64_t request_id, SeekCB cb);
  // Stage reporting goes through MediaLog only; player/ translates log events
  // into PlayerEvents. media/ must not know about player::StageReachedPayload
  // (invariant C22).
  void EmitStage(MediaLogEvent::Type stage, base::TimeDelta elapsed);

  base::scoped_refptr<MediaLog> media_log_;
  base::raw_ptr<Host> host_{nullptr};
  base::scoped_refptr<base::SequencedTaskRunner> media_runner_;

  std::unique_ptr<base::Thread> demux_thread_;
  // Only the demux thread touches this while the loop runs; ~FFmpegDemuxer
  // releases it after joining that thread (docs/04 §7 R7). Kept as void* in the
  // header so no libav* type appears outside platform/ffmpeg and this .cc.
  void* format_ctx_raw_{nullptr};
  // The DataSource→AVIOContext bridge (platform/ffmpeg/data_source_io.h),
  // non-null when the source is a memory buffer or a host DataSource. Same
  // void* convention as format_ctx_raw_: the type lives in platform/, this
  // header must not name it (media/ must not depend on platform/).
  void* data_source_io_raw_{nullptr};
  // Keeps the byte source alive for as long as the AVIOContext references
  // it. Null for URI sources (FFmpeg's protocol layer opens those itself).
  base::scoped_refptr<media::DataSource> data_source_;

  MediaInfo media_info_;
  base::TimeDelta start_time_;
  mutable base::Lock stats_lock_;
  DemuxerStats stats_ GUARDED_BY(stats_lock_);

  std::vector<std::unique_ptr<FFmpegDemuxerStream>> streams_;
  // The actively-consumed stream per type (Phase 4 track switching). -1 until
  // Initialize() selects the first stream of each type.
  std::atomic<int> active_video_{-1};
  std::atomic<int> active_audio_{-1};

  // Cross-thread control. Exactly three writers of interrupt_flag_, all
  // documented in the .cc — replacing ijkplayer's abort_request, which had to
  // be signalled from five places and hung release() whenever one was missed.
  std::atomic<SeekRequest*> pending_seek_{nullptr};
  // Seek callbacks keyed by request id. A newer seek supersedes an older one,
  // and the superseded callback is answered with kAborted rather than being
  // dropped silently — ijkplayer gave callers no way to match a completion to
  // the request that produced it (docs/05 table 5).
  mutable base::Lock seek_cb_lock_;
  std::map<int64_t, SeekCB> pending_seek_cbs_ GUARDED_BY(seek_cb_lock_);
  base::AtomicFlag interrupt_flag_;
  base::AtomicFlag stop_flag_;
  base::AtomicFlag opened_ok_;
  base::WaitableEvent resume_event_;
  std::atomic<int64_t> next_request_id_{1};
  std::atomic<int64_t> bytes_read_{0};
  std::atomic<uint64_t> packets_demuxed_{0};
  std::atomic<uint64_t> seek_count_{0};
  std::atomic<uint64_t> interrupt_count_{0};
  base::TimeTicks prepare_started_at_;

  SEQUENCE_CHECKER(media_sequence_checker_);
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_FFMPEG_DEMUXER_H_
