// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/pipeline.h` (BSD-3-Clause). Removed: the
// MediaResource-based Start() overload set that Chromium accumulated for MSE,
// the CDM/key-system plumbing, text-track client routing into blink, and
// SetLatencyHint's optional wrapper (avbase always has a value or zero).
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// (milestone M8, docs/08 §2).
// Interface frozen so M7 and M8 can be written in parallel. Excluded from
// every CMake target on purpose; see media/base/pipeline_status.h for the
// DRAFT convention this project uses.
//
// Gaps to close before this file joins the build:
//   1. media/filters/pipeline_impl.cc (M8) is the only implementation. It owns
//      the media sequence, the Demuxer, the Renderer and the demux thread's
//      shutdown, in the fixed destruction order at docs/03 §10.1 -- that order
//      is the primary defence against risk R5 (stop/destructor deadlock, the
//      defect listed as #11 in docs/01 §2).
//   2. CLOSED (round 100). Statistics duplicated PipelineStatistics
//      (renderer_client.h); both were deleted rather than collapsed, because
//      the accessor that returned the duplicate had no callers. See gap 5.
//   3. Seek() reports completion through a closure, while Player::SeekTo()
//      (frozen at M8) reports through Result<int64_t> plus a SeekCB and adds
//      SeekMode::kAccurate. SeekController (player/, M9) owns accurate-seek
//      framing and retries; this interface deliberately stays keyframe-only so
//      that the media layer has no notion of "drop frames until the target".
//   4. Add/Remove{Video,Audio,Text}Stream are declared for track switching
//      (Player::SelectTrack). Until M8 wires them, an implementation should
//      return without effect and LOG(WARNING) -- not DCHECK, because the SDK
//      facade may legitimately call them before the pipeline is ready.
//   5. CLOSED (round 100). Statistics and PipelineStatistics overlapped, and
//      the two drifted exactly as predicted: Statistics gained nothing after
//      M8, while PipelineStatistics grew three fields (sink drops, submit
//      failures, measured refresh) that the copy never learned about. Since
//      Pipeline::GetStatistics() had no caller anywhere in the tree -- the
//      host reads statistics through RendererClient::OnStatisticsUpdate, which
//      PUSHES a PipelineStatistics -- both the struct and the accessor are
//      gone. There is now one statistics shape in the media layer, and the
//      player owns the snapshot the facade reads.
//
// .cc owed by this header -- media/base/pipeline.cc:
//   Pipeline::Pipeline()      -- out-of-line `= default`, same convention as
//   Pipeline::~Pipeline()        media/base/demuxer.cc
// Pipeline::Client owes nothing (all pure virtual, dtor inline-defaulted).

#ifndef AVBASE_MEDIA_BASE_PIPELINE_H_
#define AVBASE_MEDIA_BASE_PIPELINE_H_

#include <memory>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/demuxer.h"
#include "media/base/media_error.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer.h"
#include "media/base/renderer_client.h"
#include "media/base/renderer_factory.h"
#include "media/base/timed_text.h"
#include "media/base/waiting.h"
#include "media/media_export.h"

namespace avbase::media {

class NativeDisplay;

// Owns and drives one playback: demuxer in, renderer in the middle, sinks out.
//
// WHAT IT REPLACES. ijkplayer has no equivalent object. The closest thing is
// ff_ffplay.c's read_thread() plus the VideoState it mutates: control flow,
// buffering policy, clock ownership and teardown are all interleaved in one
// function, which is why stop() can hang (docs/01 defect #11). Pipeline is the
// seam that separates "what the media graph does" from "what the SDK promises".
//
// THREADING. Everything runs on |media_task_runner|, the "media" sequence in
// docs/04 §1. The exceptions are the three const getters below, which are
// documented individually as thread-safe because Player answers them from the
// caller's thread. Stop() is non-blocking by contract (Δ1): it returns once the
// stop has been *requested*, and Player::StopSync(timeout) is what waits, with
// config.shutdown_timeout bounding the wait and detach-plus-LOG(ERROR) as the
// escape hatch (Δ15: leak a thread rather than hang the caller).
class AVBASE_MEDIA_EXPORT Pipeline {
 public:
  // Upward channel to the owner. Same threading rules as RendererClient; the
  // two are separate interfaces because the pipeline reports pipeline-level
  // facts (duration, statistics) while the renderer reports rendering-level
  // ones, and PipelineImpl implements RendererClient by forwarding.
  class Client {
   public:
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    virtual void OnError(MediaError error) = 0;
    virtual void OnEnded() = 0;
    virtual void OnDurationChange(base::TimeDelta duration) = 0;
    virtual void OnBufferingStateChange(BufferingState state,
                                        base::TimeDelta memory_usage) = 0;
    virtual void OnWaiting(WaitingReason reason) = 0;
    virtual void OnStatisticsUpdate(const PipelineStatistics& stats) = 0;
    virtual void OnVideoConfigChange(const VideoDecoderConfig& config) = 0;
    virtual void OnTimedText(const media::TimedTextCue& /*cue*/) {}

   protected:
    Client() = default;
    virtual ~Client() = default;
  };

  // NO Statistics STRUCT HERE (round 100). Pipeline-level counters live in the
  // player, not here: the media layer PUSHES PipelineStatistics through
  // Client::OnStatisticsUpdate, and PlayerImpl keeps the latest one for
  // Player::GetPlaybackStats(). A pull-style accessor on this interface was
  // declared in the DRAFT and never called by anything; see gap 5.
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;
  virtual ~Pipeline();

  // Builds the graph and starts the media sequence. Takes ownership of
  // |demuxer|. |renderer_factory| and |client| are borrowed and must outlive
  // the pipeline. Completion is reported through Client::OnError or by the
  // first Client::OnDurationChange/OnBufferingStateChange -- there is no
  // status callback here because PipelineController owns the kStarting ->
  // kReady transition and needs to observe it either way.
  virtual void
  Start(std::vector<std::unique_ptr<Demuxer>> sources,
        RendererFactory* renderer_factory, RendererType renderer_type,
        base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
        Client* client) = 0;

  // Non-blocking (Δ1). Interrupts in-flight blocking I/O through the demuxer's
  // interrupt_callback (docs/04 §2.1) and requests teardown of every sequence.
  // After it returns, no new callbacks will be *started*; ones already running
  // finish, which is why Player::StopSync() exists.
  virtual void Stop() = 0;

  // ---- Added when M8 wired playback (docs/PROGRESS.md, playback round) ----
  // Chromium's media::Pipeline has Play()/Pause(); the DRAFT header dropped
  // them and no implementation existed to object. With PipelineImpl landing,
  // they are restored: Play() demuxes and renders from the current position,
  // Pause() gates the sinks and freezes the clocks. Both are no-ops before
  // Start() completes.
  virtual void Play() {}
  virtual void Pause() {}
  // Runtime surface swap, backing Player::SetVideoSurface() on a live
  // pipeline. No-op before the renderer exists.
  virtual void SetOutputTarget(base::scoped_refptr<NativeDisplay> display);
  // Requests the currently-presented frame for Player::TakeSnapshot; the
  // callback runs on the pipeline's media sequence. Default: reports
  // kNotImplemented (the no-op pipelines need no override).
  virtual void TakeSnapshot(base::TimeDelta at,
                            Renderer::SnapshotFrameCallback callback);

  virtual bool IsRunning() const = 0;

  virtual void SetVolume(float volume) = 0;       // 0.0 .. 1.0
  virtual void SetPlaybackRate(double rate) = 0;  // 0.25 .. 4.0
  // Live latency target; zero means "not live".
  virtual void SetLatencyHint(base::TimeDelta hint) = 0;
  virtual void SetPreservesPitch(bool preserves_pitch) = 0;

  // ---- Thread-safe getters ------------------------------------------------
  // Safe from any thread because they read through AvSyncController's seqlock
  // (Δ14) and through atomics, never through a mutex the media sequence holds.
  // A getter that blocked on the media sequence would be able to hang the
  // caller's UI thread, which is the failure mode this class exists to remove.
  virtual base::TimeDelta GetMediaTime() = 0;
  virtual base::TimeDelta GetBufferedTime() const = 0;
  virtual base::TimeDelta GetDuration() const = 0;

  // Keyframe seek, clamped to [0, duration]. |seeked_cb| runs on the media
  // sequence once the demuxer and every renderer have flushed to the new
  // serial. Accurate seek is layered above this by player/seek_controller.
  virtual void Seek(base::TimeDelta time, base::OnceClosure seeked_cb) = 0;

  // Runtime audio-track switch (Phase 4). |stream_index| is the CONTAINER
  // stream index -- the same value MediaInfo::streams reports and
  // DemuxerStream::stream_index() carries. The request is accepted on the
  // caller's thread; |cb| runs on the media sequence (never inline) once the
  // sub-renderer handover finished, kOk meaning playback continues on the new
  // track. A failed switch keeps the current one playing and reports the
  // reason through Client::OnError; this default answers for pipelines that
  // do not support switching at all.
  // Video-track switch (docs/12 4.1). Same contract as SelectAudioTrack: the
  // closure means "the attempt finished and the pipeline is consistent", not
  // "the new track is on screen". A failed switch reports through the client
  // and still runs the closure, so a caller waiting on it is never stuck.
  virtual void SelectVideoTrack(int stream_index, PipelineStatusCallback cb) {
    (void)stream_index;
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
  }

  virtual void SelectAudioTrack(int /*stream_index*/,
                                PipelineStatusCallback cb) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
  }

  // Subtitle-track switch, same contract as SelectAudioTrack. Cues surface
  // through Client::OnTimedText as they display.
  virtual void SelectTextTrack(int /*stream_index*/,
                               PipelineStatusCallback cb) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
  }

  virtual bool CanSeekForward() const = 0;
  virtual bool CanSeekBackward() const = 0;

  // Currently selected stream indices; -1 when that track type is absent or
  // disabled. Feeds MediaInfo and Player::SelectTrack().
  virtual int GetAudioStreamId() const = 0;
  virtual int GetVideoStreamId() const = 0;
  virtual int GetTextStreamId() const = 0;

  // Add/Remove{Video,Audio,Text}Stream describe a shape avbase does not have.
  //
  // They came from Chromium's MediaResource, where one resource can carry
  // several streams of the same type and `id` selects among them. avbase's
  // Demuxer answers `GetStream(DemuxerStreamType)` -- ONE stream per type, no
  // index -- so there is no "second video stream" for an id to name. A second
  // picture is a second URL, which is a second Demuxer, not a second id.
  //
  // Track switching went the other way: SelectXxxTrack(stream_index) above is
  // wired and is what Player::SelectTrack() actually calls. These six were
  // declared for it (gap 4 at the top of this file) and were never wired --
  // grepping AddVideoStream finds the declaration, the no-op override and
  // nothing else. So they are not "not implemented yet"; they name an axis
  // that does not exist here, and every implementation rejects them.
  //
  // They stay declared because this interface is frozen (M8) and because a
  // caller that reaches them deserves a WARNING rather than a DCHECK: the
  // facade may call before the pipeline is ready.
  virtual void AddVideoStream(int id) = 0;
  virtual void RemoveVideoStream(int id) = 0;
  virtual void AddAudioStream(int id) = 0;
  virtual void RemoveAudioStream(int id) = 0;
  virtual void AddTextStream(int id) = 0;
  virtual void RemoveTextStream(int id) = 0;

 protected:
  Pipeline();
};

// Stage 0 (docs/14): Start() takes a list of N demuxers. The common
// single-source pipeline wraps its one demuxer with this so callers don't spell
// out the vector.
// Found via ADL on the unique_ptr<Demuxer> argument, so callers can write
// MakeSingleSource(std::move(demuxer)) without qualification.
inline std::vector<std::unique_ptr<Demuxer>>
MakeSingleSource(std::unique_ptr<Demuxer> demuxer) {
  std::vector<std::unique_ptr<Demuxer>> sources;
  sources.push_back(std::move(demuxer));
  return sources;
}

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_PIPELINE_H_
