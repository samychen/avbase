// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/pipeline_status.h` (BSD-3-Clause), with the
// browser-only codes (encryption, live trace, WebMedia* ) removed.
//
// STATUS: DRAFT — NOT YET IN THE BUILD (milestone M8, docs/08 §2).
// Interface frozen so that M7 (RendererImpl / VideoRendererImpl /
// AudioRendererImpl) and M8 (pipeline_impl + Player wiring) can be written in
// parallel against a fixed contract, the same way player/public/ was frozen
// ahead of its implementation. Excluded from every CMake target on purpose: a
// file that cannot compile must not be reachable from a build.
// tools/check_invariants.py exempts DRAFT files from the style and size rules
// but still enforces C20 (namespace balance) and C22 (layering), and lists
// them at the end of every run so they cannot be quietly forgotten.
//
// Gaps to close before this file joins the build:
//   1. Nothing consumes it yet; Pipeline::Start() and Renderer::Initialize()
//      are the first callers (pipeline.h, renderer.h).
//   2. PipelineStatusToMediaError() needs a .cc with the code -> MediaError
//      mapping, and every entry needs summary/detail/suggestion text that
//      satisfies docs/10 §4.3 (ErrorMessagesTest asserts it verbatim).
//   3. kAudioInitializationError / kVideoInitializationError are deliberately
//      coarse. ijkplayer reports the same coarseness through FFP_MSG_ERROR; if
//      M8 needs the decoder name in the event payload, extend the mapping
//      rather than the enum (Δ12 already carries the decoder name separately).

#ifndef IJKPP_MEDIA_BASE_PIPELINE_STATUS_H_
#define IJKPP_MEDIA_BASE_PIPELINE_STATUS_H_

#include "base/functional/callback.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace ijkpp::media {

// Outcome of an asynchronous pipeline stage. Ordered: anything other than
// kOk is a failure, and the ordering groups the failures by which stage
// produced them, so a range check beats a switch at most call sites.
enum class PipelineStatus {
  kOk = 0,

  // Demuxer stage.
  kDemuxerError,            // Opening or probing the source failed.
  kDemuxerInitializationError,
  kMissingDemuxerStreams,   // Container opened but selected no usable stream.

  // Decoder stage.
  kAudioInitializationError,
  kVideoInitializationError,
  kDecoderError,            // Fatal decode failure after a successful open.
  kVideoDecoderDoesNotSupportHardwareProtection,

  // Renderer / sink stage.
  kRendererError,
  kAudioRendererInitializationError,
  kVideoRendererInitializationError,
  kInitializationError,     // Generic stage failure, kept last-resort.

  // Whole-pipeline stage.
  kAborted,                 // Stopped or destroyed mid-initialisation.
  kFailedToCreatePipeline,  // Backend or factory could not provide a part.

  kMaxValue = kFailedToCreatePipeline,
};

IJKPP_MEDIA_EXPORT const char* PipelineStatusToString(PipelineStatus status);

// Every status has a MediaError equivalent, because the SDK surface reports
// MediaError and never PipelineStatus (player/public/error.h re-exports
// MediaError, not this header). The mapping is total: no status may fall
// through to a generic "playback failed", which is exactly the failure mode
// docs/10 §4 exists to prevent.
IJKPP_MEDIA_EXPORT MediaError PipelineStatusToMediaError(PipelineStatus status);

// Runs on the media sequence. Never run inline by the callee: an initialising
// pipeline holds locks that a synchronous callback would re-enter.
using PipelineStatusCallback = base::OnceCallback<void(PipelineStatus)>;

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_PIPELINE_STATUS_H_
