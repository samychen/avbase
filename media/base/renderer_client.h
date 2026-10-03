// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/renderer_client.h` (BSD-3-Clause). The
// browser-only hooks are dropped: RequestOverlayInfo / GetOverlayTaskRunner
// (surface-layer compositing), OnAudioOutputDeviceChanged's device-permission
// UI path, and OnWaiting's key-system variants.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// (milestone M7, docs/08 §2).
// Interface frozen so M7 and M8 can be written in parallel. Excluded from
// every CMake target on purpose; see media/base/pipeline_status.h for the
// DRAFT convention this project uses.
//
// Gaps to close before this file joins the build:
//   1. The only implementation is RendererImpl's owner (PipelineImpl, M8) and
//      tests/support/mock_renderer_client (docs/07 §2.2, not written yet).
//   2. OnAudioOutputDeviceChanged is declared but has no caller until
//      SwitchableAudioRendererSink exists (media/audio/, M7). If M7 ships
//      without device switching, delete the method rather than leave a
//      callback nobody fires -- an interface with dead hooks is how SDK users
//      lose trust in the rest of it.
//   3. PipelineStatistics must be reconciled with player/public/
//      playback_stats.h (109 lines, frozen at M8): the mapping is docs/05
//      table 6, and acceptance criterion A12 requires every legacy
//      FFP_PROP_* to appear somewhere in the pair. That reconciliation is a
//      M8 task and may add fields here.
//
// .cc owed by this header -- media/base/renderer_client.cc:
//   BufferingStateToString(BufferingState)
//   OutputDeviceStatusToString(OutputDeviceStatus)
// RendererClient itself owes nothing: every member is pure virtual, the
// destructor is inline-defaulted, and the copy operations are deleted. That
// matches media/base/video_renderer_sink.h, whose factory is likewise
// header-only.

#ifndef AVBASE_MEDIA_BASE_RENDERER_CLIENT_H_
#define AVBASE_MEDIA_BASE_RENDERER_CLIENT_H_

#include <stdint.h>

#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/decoder_config.h"
#include "media/base/media_error.h"
#include "media/base/media_types.h"
#include "media/base/timed_text.h"
#include "media/base/waiting.h"
#include "media/media_export.h"

namespace avbase::media {

// Buffering state, reported through OnBufferingStateChange. Replaces the
// int-valued FFP_MSG_BUFFERING_START / _END pair plus a separate
// FFP_MSG_BUFFERING_UPDATE, so a consumer cannot observe END before START.
enum class BufferingState {
  kHaveNothing = 0,  // No data at all; playback cannot start.
  kHaveMetadata,     // Probed, no media data buffered yet.
  kHaveEnough,       // Enough buffered to play; resumes rendering.
  kHaveFuture,       // Some data, will stall soon (drives the kBuffering
                     // warning before the stall, not after it).
};

AVBASE_MEDIA_EXPORT const char* BufferingStateToString(BufferingState state);

// Audio output device lifecycle, for SwitchableAudioRendererSink.
enum class OutputDeviceStatus {
  kOk = 0,
  kFailed,         // The device could not be opened.
  kNotAuthorized,  // Permission denied (sandbox / policy).
  kNotFound,       // The requested device id does not exist.
};

AVBASE_MEDIA_EXPORT const char* OutputDeviceStatusToString(OutputDeviceStatus);

// Counters the renderer reports upward. Mirrors Chromium's
// PipelineStatistics minus the browser-only fields. Everything here is
// thread-safe to read because every field is written on one sequence and read
// through a snapshot copy.
//
// The ijkplayer equivalents are the FFP_PROP_INT64_* family (docs/05 table 6):
// twenty separate ijkmp_get_property_int64() calls become one struct return,
// so a stats consumer can no longer observe a half-updated picture.
struct AVBASE_MEDIA_EXPORT PipelineStatistics {
  // Demuxer.
  int64_t audio_bytes{0};
  int64_t video_bytes{0};
  int64_t text_bytes{0};
  int64_t total_bytes_read{0};
  base::TimeDelta buffered_time;  // How far ahead of the play head.
  base::TimeDelta estimated_playback_time;

  // Video decode / render.
  uint64_t video_frames_decoded{0};
  uint64_t video_frames_dropped{0};  // Late, fps-capped, or stale serial.
  uint64_t video_frames_presented{0};
  uint64_t video_frames_repeated{0};  // Held because no new frame was due.
  uint64_t video_decode_error_count{0};
  uint64_t video_keyframe_count{0};

  // Audio decode / render.
  uint64_t audio_bytes_decoded{0};
  uint64_t audio_frames_decoded{0};
  uint64_t audio_decode_error_count{0};
  uint64_t audio_underruns{0};
  uint64_t audio_glitches{0};
  base::TimeDelta audio_glitch_duration;

  // A/V sync. Steady-state |av_diff| above 40 ms is the trigger signal for
  // risk R1 in docs/08 §5.1: stop and do a sync-specific pass.
  double avg_av_diff_ms{0.0};
  double max_av_diff_ms{0.0};
  uint64_t seek_count{0};
  base::TimeDelta last_seek_duration;
};

// The upward channel from a Renderer to whoever owns it.
//
// Threading: every method is called on the media sequence unless the comment
// says otherwise. Implementations must not block and must not call back into
// Renderer methods synchronously -- doing so deadlocks on the pipeline's
// sequence, which is why tests/unit/player/callback_reentry (M8 DoD) calls
// Stop/Reset/SeekTo from inside each event handler.
//
// Lifetime: the Renderer holds a raw pointer, never an owning one. The client
// must outlive the renderer; PipelineImpl guarantees that by destroying the
// renderer before itself, in the fixed order documented at docs/03 §10.1.
class AVBASE_MEDIA_EXPORT RendererClient {
 public:
  RendererClient(const RendererClient&) = delete;
  RendererClient& operator=(const RendererClient&) = delete;

  // Fatal. After this returns the renderer stops on its own; the owner must
  // not call Flush/StartPlayingFrom again. |error| always carries the
  // three-part summary/detail/suggestion required by docs/10 §4.
  virtual void OnError(MediaError error) = 0;

  // End of stream reached and the last frame has been presented. Fires once
  // per playback, not once per loop iteration when loop_count > 1.
  virtual void OnEnded() = 0;

  // |memory_usage| is the renderer's estimate of buffered bytes, used by
  // BufferController (M9) to drive the three-tier high water mark.
  virtual void OnBufferingStateChange(BufferingState state,
                                      base::TimeDelta memory_usage) = 0;

  // Stall reason, so "why is it stuck" is answerable from the log instead of
  // from a stack trace. Mirrors media/base/waiting.h.
  virtual void OnWaiting(WaitingReason reason) = 0;

  // Duration became known or changed. Live streams report kNoTimestamp-derived
  // values through MediaInfo::duration_estimate instead (Δ7).
  virtual void OnDurationChange(base::TimeDelta duration) = 0;

  // Periodic counters. Interval is config.stats_interval (default 1 s).
  virtual void OnStatisticsUpdate(const PipelineStatistics& stats) = 0;

  // Mid-stream codec configuration change (resolution or profile change).
  // The renderer has already re-initialised its decoder when this fires; the
  // owner only needs to tell the UI, which becomes EventType::
  // kVideoSizeChanged (Δ8 merges the legacy SAR and SIZE messages into it).
  virtual void OnVideoConfigChange(const VideoDecoderConfig& config) = 0;

  // Text leg (Phase 4.2): one decoded subtitle cue, base renders nothing.
  // Default no-op so clients that do not care never see it.
  virtual void OnTimedText(const TimedTextCue& cue) {}

  // Audio output device switched, or the attempt failed. No caller until
  // media/audio/ exists (M7); see gap 2 in the file header.
  virtual void OnAudioOutputDeviceChanged(const std::string& device_id,
                                          bool is_default,
                                          OutputDeviceStatus status) = 0;

  // Where the renderer may post work that must not run on the media sequence.
  // Returning nullptr means "no such sequence", and the renderer must then
  // avoid the work rather than run it inline.
  virtual base::scoped_refptr<base::SequencedTaskRunner>
  GetOverlayTaskRunner() = 0;

 protected:
  RendererClient() = default;
  // Protected and non-virtual-deleted on purpose: the client is owned by the
  // pipeline, never by the renderer, so deleting through a RendererClient*
  // would be a layering violation.
  virtual ~RendererClient() = default;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_RENDERER_CLIENT_H_
