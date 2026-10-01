// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/demuxer.h` (BSD-3-Clause), minus the browser
// specific parts (MediaResource, mojo, byte-range loaders).

#ifndef AVBASE_MEDIA_BASE_DEMUXER_H_
#define AVBASE_MEDIA_BASE_DEMUXER_H_

#include <stdint.h>

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_log.h"
#include "media/base/media_error.h"
#include "media/base/media_info.h"
#include "media/base/media_resource.h"
#include "media/media_export.h"

namespace avbase::media {

// Everything a Demuxer needs from the player's configuration, expressed in
// media-layer terms. player::PlayerConfig is mapped into this by PlayerImpl, so
// media/ never has to know that PlayerConfig exists (invariant C22).
struct AVBASE_MEDIA_EXPORT DemuxerOptions {
  int64_t probe_size{5 * 1024 * 1024};
  base::TimeDelta analyze_duration{base::Seconds(5)};
  bool find_stream_info{true};
  std::string forced_format;
  base::TimeDelta io_timeout;
  // Network behaviour.
  bool reconnect{true};
  int reconnect_max_retries{3};
  base::TimeDelta reconnect_delay{base::Milliseconds(100)};
  bool dns_cache_clear{false};
  std::string user_agent;
  std::map<std::string, std::string> headers;
  // Verbatim AVDictionary passthrough (PlayerConfig::extra_format_options).
  std::map<std::string, std::string> extra_options;
};

// Statistics a demuxer can report without blocking.
struct AVBASE_MEDIA_EXPORT DemuxerStats {
  int64_t bytes_read{0};
  int64_t physical_position{0};
  int64_t tcp_speed_bytes_per_sec{0};
  uint64_t packets_demuxed{0};
  uint64_t seek_count{0};
  base::TimeDelta last_seek_duration;
  uint64_t interrupt_count{0};
};

class AVBASE_MEDIA_EXPORT Demuxer : public MediaResource {
 public:
  // Receives duration updates (live streams) and buffering progress.
  class Host {
   public:
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    virtual void SetDuration(base::TimeDelta duration) = 0;
    virtual void OnBufferedTimeUpdate(base::TimeDelta buffered,
                                      base::TimeDelta playback_time) = 0;
    virtual void OnDemuxerError(MediaError error) = 0;
   protected:
    Host() = default;
    virtual ~Host() = default;
  };

  using InitializeCB = base::OnceCallback<void(Status)>;
  using SeekCB = base::OnceCallback<void(Status, base::TimeDelta actual)>;

  Demuxer(const Demuxer&) = delete;
  Demuxer& operator=(const Demuxer&) = delete;
  virtual ~Demuxer();

  // Opens the source and probes stream information. |init_cb| runs on
  // |media_task_runner| and is never run inline.
  virtual void Initialize(const DataSourceDescriptor& source,
                          const DemuxerOptions& options,
                          Host* host,
                          base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
                          InitializeCB init_cb) = 0;

  // Physical seek. |cb| reports the position actually reached, which for a
  // keyframe seek is at or before the request.
  virtual void StartPlayingFrom(base::TimeDelta time, SeekCB cb) = 0;
  virtual void Flush(base::OnceClosure flush_cb) = 0;
  virtual void Reset(base::OnceClosure reset_cb) = 0;
  // Non-blocking request to stop; after it returns, in-flight blocking I/O is
  // interrupted and no further callbacks run.
  virtual void Stop() = 0;

  // MediaResource's stream accessor, re-declared pure so that every demuxer
  // must answer it. |override| (not just |virtual|) is what the style rule
  // asks for, and the base class is the reason it is an override at all.
  DemuxerStream* GetStream(DemuxerStreamType type) override = 0;
  virtual const MediaInfo& media_info() const = 0;
  virtual base::TimeDelta GetStartTime() const = 0;
  virtual bool IsLive() const = 0;
  virtual bool IsSeekable() const = 0;
  virtual DemuxerStats GetStats() const = 0;
  virtual const char* name() const = 0;

 protected:
  Demuxer();
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_DEMUXER_H_
