// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/pipeline_controller.h` (BSD-3-Clause), which
// is likewise a state-machine wrapper that owns a Pipeline and forwards to it.
//
// STATUS: DRAFT — NOT YET IN THE BUILD (milestone M8, docs/08 §2).
// Interface frozen so M7 and M8 can be written in parallel. Excluded from
// every CMake target on purpose; see media/base/pipeline_status.h for the
// DRAFT convention this project uses.
//
// Gaps to close before this file joins the build:
//   1. Chromium's PipelineController is a concrete class holding a
//      std::unique_ptr<Pipeline>. Declaring it abstract here is a deliberate
//      deviation: the concrete state machine lives in
//      media/filters/pipeline_impl.cc next to the Pipeline it drives, so that
//      the transition table and the teardown order (docs/03 §10.1) are in one
//      translation unit instead of two. If M8 finds the two genuinely
//      separable, make this concrete and move the table up.
//   2. player/state_machine.cc (M8) owns the *SDK* state machine (PlayerState,
//      12 states, frozen in player/public/player.h). The two must not duplicate
//      each other: this one governs the media graph, that one governs what a
//      caller is allowed to ask for. StateMachineTest (M8 DoD) exhaustively
//      asserts the player one; this one needs the equivalent for the six
//      states below, and that test does not exist yet.
//   3. Every Pipeline method needs a forwarding declaration here. They are
//      omitted from this draft on purpose rather than written speculatively:
//      adding 20 one-line forwarders before pipeline_impl.cc exists would
//      freeze names that the implementation has not yet earned.
//
// .cc owed by this header -- media/base/pipeline_controller.cc:
//   PipelineControllerStateToString(PipelineController::State)
//   PipelineController::PipelineController()   -- out-of-line `= default`
//   PipelineController::~PipelineController()
// The destructor must be the thing that drives the state to kDestroying and
// joins every sequence, in the fixed order at docs/03 §10.1. That ordering is
// mitigation 1 for risk R5 (stop/destructor deadlock), so it belongs in the
// .cc next to the transition table rather than in a header.

#ifndef IJKPP_MEDIA_BASE_PIPELINE_CONTROLLER_H_
#define IJKPP_MEDIA_BASE_PIPELINE_CONTROLLER_H_

#include "media/base/pipeline.h"
#include "media/media_export.h"

namespace ijkpp::media {

// Owns a Pipeline and makes its lifecycle explicit.
//
// WHY A WRAPPER AND NOT FLAGS IN PipelineImpl. The failure this prevents is
// the one ijkplayer has: control methods that are callable in any order from
// any thread, where "what happens if you Seek() during Stop()" is answered by
// whatever the code path happens to do. Here every transition is named, the
// illegal ones are DCHECKed in debug builds and answered with kInvalidState in
// release, and a callback arriving after teardown is dropped by construction
// (WeakPtr, docs/04 §3) rather than by luck.
//
// Legal transitions:
//
//   kCreated    --Start()-->  kStarting   --graph ready-->  kReady
//   kReady      --Stop()-->   kStopping   --sequences joined--> kStopped
//   kStopped    --Start()-->  kStarting   (restart after a full stop)
//   any         --~PipelineController()--> kDestroying --> gone
//
// kDestroying exists so that a callback which fires during destruction can be
// recognised and dropped instead of being delivered to a half-destroyed owner.
// That is the "lost wakeup" class of bug the sanitiser pass already caught once
// in Stop() (docs/PROGRESS, second round).
class IJKPP_MEDIA_EXPORT PipelineController {
 public:
  enum class State {
    kCreated = 0,
    kStarting,
    kReady,
    kStopping,
    kStopped,
    kDestroying,
  };

  PipelineController(const PipelineController&) = delete;
  PipelineController& operator=(const PipelineController&) = delete;
  virtual ~PipelineController();

  virtual State state() const = 0;
  virtual bool IsRunning() const = 0;   // state() == kReady

  // kCreated/kStopped -> kStarting. Returns immediately; readiness is observed
  // through Pipeline::Client.
  virtual void Start() = 0;

  // kReady/kStarting -> kStopping. Non-blocking (Δ1). Calling it twice, or
  // calling it in kStopped, is a no-op rather than an error, because the SDK
  // facade legitimately calls Stop() from both ~Player() and Player::Stop().
  virtual void Stop() = 0;

 protected:
  PipelineController();
};

IJKPP_MEDIA_EXPORT const char* PipelineControllerStateToString(
    PipelineController::State state);

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_PIPELINE_CONTROLLER_H_
