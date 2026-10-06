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
    // From kIdle/kInitialized there is no pipeline to stop; walk the table to
    // kStopped so the published sequence is still well-formed.
    if (previous == PlayerState::kIdle ||
        previous == PlayerState::kInitialized) {
      previous = PlayerState::kStopped;
    } else {
      machine_.TransitionTo(PlayerState::kStopping);
    }
  }
  if (pipeline_) {
    pipeline_->Stop();
  }
  {
    base::AutoLock scoped(state_lock_);
    machine_.TransitionTo(PlayerState::kStopping);
    machine_.TransitionTo(PlayerState::kStopped);
  }
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
  const base::TimeTicks deadline = deps_->tick_clock->NowTicks() + timeout;
  while (pipeline_ && pipeline_->IsRunning()) {
    if (deps_->tick_clock->NowTicks() >= deadline) {
      LOG(ERROR) << "avbase: StopSync timed out after "
                 << timeout.InMillisecondsF()
                 << "ms with the pipeline still running (state "
                 << (pipeline_->IsRunning() ? "running" : "stopped")
                 << "); detaching (delta 15: leak a thread, never hang). "
                    "The renderer or demuxer did not finish tearing down, so "
                    "~PipelineImpl will run against live state -- expect its "
                    "shutdown DCHECK to fire in a debug build.";
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void PlayerImpl::Reset() {
  StopSync(config_.shutdown_timeout);
  pipeline_.reset();
  renderer_factory_.reset();
  {
    base::AutoLock scoped(state_lock_);
    machine_.ForceReset();
    source_set_ = false;
    source_ = media::DataSourceDescriptor();
    prepared_handled_ = false;
  }
  prepared_event_.Reset();
  event_hub_.PostStateChanged(PlayerState::kStopped, PlayerState::kIdle,
                              base::TimeDelta());
}

}  // namespace avbase
