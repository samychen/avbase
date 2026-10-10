// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PlayerImpl's stopping half: Stop(), StopSync() and Reset(). player_impl.cc
// keeps construction, source selection, prepare and transport; the split is a
// line-count one (C1), and the seam is "the player being driven" versus
// "the player being torn down". The StopSync comment is load-bearing -- it
// records a real teardown-ordering race the corpus run caught -- so it moved
// here verbatim rather than being compressed to fit the other file.

#include "player/player_impl.h"

#include <chrono>
#include <thread>

#include "base/logging.h"
#include "base/functional/bind.h"
#include "base/location.h"

namespace avbase {

void PlayerImpl::Stop() {
  PlayerState previous;
  {
    base::AutoLock scoped(state_lock_);
    previous = machine_.state();
    if (previous == PlayerState::kStopping ||
        previous == PlayerState::kStopped) {
      return;
    }
    // M3: kIdle/kInitialized now have a legal kStopping edge (state_machine
    // .cc), so the machine REALLY reaches kStopped below. The old code
    // overwrote |previous| with kStopped here because the table refused the
    // transitions -- the machine stayed in kIdle while observers received a
    // fictional kStopped -> kStopped event.
    machine_.TransitionTo(PlayerState::kStopping);
  }
  const std::shared_ptr<media::PipelineImpl> pipeline = GetPipeline();
  if (pipeline) {
    pipeline->Stop();
  }
  {
    base::AutoLock scoped(state_lock_);
    machine_.TransitionTo(PlayerState::kStopping);
    machine_.TransitionTo(PlayerState::kStopped);
  }
  // |previous| is the true pre-Stop state (kIdle, kInitialized, kPreparing,
  // kPrepared, ...): the published sequence starts where the player actually
  // was, not at kStopped.
  event_hub_.PostStateChanged(previous, PlayerState::kStopped, GetMediaTime());
}

void PlayerImpl::StopSync(base::TimeDelta timeout) {
  Stop();
  if (!media_thread_.IsRunning()) {
    return;
  }
  // Wait for the PIPELINE to stop, not for the media queue to drain.
  //
  // The previous version posted a barrier task and waited for that, which
  // looks equivalent and is not. DoStop() completes ASYNCHRONOUSLY: it sets
  // kStopping, hands the renderer a Flush, and RETURNS -- FinishStop() (which
  // is what actually reaches kStopped) only runs once the flush has completed
  // and hopped back to S1. So the barrier fires while the state is still
  // kStopping, StopSync returns, and ~PipelineImpl then runs against a
  // pipeline that has not finished tearing down.
  //
  // That is not a theoretical ordering. The corpus run hit it on 14 of 29
  // generated samples, always AFTER a successful playback, and the symptom was
  // ~PipelineImpl's DCHECK seeing kReady/kStopping. In a release build the
  // DCHECK is compiled out, so the race is silent there -- the teardown
  // ordering is simply not sound, and only the debug build was reporting it.
  //
  // IsRunning() is the predicate that means "safe to destroy" -- pipeline_impl
  // .cc says so on IsRunning() itself, including that kStopping does NOT count
  // as safe. So poll that, bounded, exactly as the test teardowns do.
  //
  // Each round re-snapshots through GetPipeline(): Reset() (from another
  // thread) may clear or replace the member while this loop runs, and that must
  // end the wait rather than be read through a stale pointer.
  const base::TimeTicks deadline = deps_->tick_clock->NowTicks() + timeout;
  for (;;) {
    const std::shared_ptr<media::PipelineImpl> pipeline = GetPipeline();
    if (!pipeline || !pipeline->IsRunning()) {
      return;
    }
    if (deps_->tick_clock->NowTicks() >= deadline) {
      LOG(ERROR)
          << "avbase: StopSync timed out after " << timeout.InMillisecondsF()
          << "ms with the pipeline still running; detaching (delta 15: "
             "leak a thread, never hang). The renderer or demuxer did "
             "not finish tearing down, so ~PipelineImpl will run against "
             "live state -- expect its shutdown DCHECK to fire in a "
             "debug build.";
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void PlayerImpl::Reset() {
  StopSync(config_.shutdown_timeout);
  {
    base::AutoLock scoped(pipeline_lock_);
    // pipeline_ first: it holds a raw RendererFactory* (Start's argument), so
    // the factory must outlive it. A reader that snapshotted the pipeline just
    // before this point keeps that object alive past the reset, but StopSync
    // above has already stopped it, so it no longer reaches into the factory.
    pipeline_.reset();
    renderer_factory_.reset();
  }
  // M4: in-flight seeks used to dangle forever -- StopSync stopped the
  // pipeline, so OnMediaSeekDone could never fire, yet the user callbacks
  // stayed parked in pending_seeks_. Answer every one of them with kAborted
  // and drop the seek bookkeeping before the machine resets.
  {
    base::AutoLock scoped(seek_lock_);
    std::map<int64_t, Player::SeekCB> pending =
        std::move(pending_seeks_);
    pending_seeks_.clear();
    accurate_seek_targets_.clear();
    for (auto& entry : pending) {
      event_hub_.PostClosure(base::BindOnce(
          std::move(entry.second),
          base::unexpected(MediaError(
              ErrorCode::kAborted, "player reset while the seek was pending",
              "request_id = " + std::to_string(entry.first),
              "expected across Reset(); re-issue the seek on the new "
              "source"))));
    }
  }
  // The two controllers are media-sequence-bound; rewind them where they live
  // instead of tripping their sequence checkers from the caller's thread.
  // OnSeekCompleted is BufferController's own "start fresh" primitive (it
  // closes a still-open cycle and rewinds the tier to kFirst).
  media_thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](PlayerImpl* self) {
                       self->accurate_seek_.End();
                       self->buffer_controller_.OnSeekCompleted();
                     },
                     base::Unretained(this)));
  {
    base::AutoLock scoped(state_lock_);
    machine_.ForceReset();
    source_set_ = false;
    source_ = media::DataSourceDescriptor();
    prepared_handled_ = false;
    // M4: the retry decorator belongs to the OLD source; keeping it alive
    // past Reset() let ReconnectNow() reach a data source that is no longer
    // wired to anything.
    retry_source_.reset();
  }
  prepared_event_.Reset();
  event_hub_.PostStateChanged(PlayerState::kStopped, PlayerState::kIdle,
                              base::TimeDelta());
}

}  // namespace avbase
