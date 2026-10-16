// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's text leg (Phase 4.2): the live-edge policy
// (SetSourceLiveness / IsCueStale) and the cue pump (PumpText /
// OnTextRead). Split from renderer_impl_controls.cc as the line-count seam
// that file's allowlist entry named ("the better fix is a
// renderer_impl_text.cc split"): the control surface kept growing, and the
// text leg is the one part of it that is a self-contained state machine --
// a generation counter, at most one outstanding Read, a staleness policy --
// rather than a setter that fans out to the sub-renderers. What stays
// behind is exactly the fan-out surface plus the track switches.

#include "media/filters/renderer_impl.h"

#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"

namespace avbase::media {

// The reasoning lives at the declaration in renderer_impl.h.
void RendererImpl::SetSourceLiveness(bool is_live,
                                     base::TimeDelta max_cue_age) {
  source_is_live_ = is_live;
  // Clamped: a negative window would make every cue stale, so a misconfigured
  // value must disable the policy rather than drop everything.
  max_cue_age_ =
      max_cue_age > base::TimeDelta() ? max_cue_age : base::TimeDelta();
  if (source_is_live_ && max_cue_age_ > base::TimeDelta()) {
    LOG(INFO) << "avbase.text: live source, cues older than "
              << max_cue_age_.InMilliseconds() << "ms will be dropped";
  }
}

bool RendererImpl::IsCueStale(const TimedTextCue& cue) const {
  if (!source_is_live_ || max_cue_age_ <= base::TimeDelta()) {
    return false;  // Recorded content: a seek lands on a cue on purpose.
  }
  const AvSyncController::Snapshot now = av_sync_->GetSnapshot();
  if (!now.master_valid) {
    // No clock yet. Dropping on an unknown clock would discard every cue of a
    // stream that is merely starting up; showing a slightly late one is the
    // lesser error, and the window is generous by construction.
    return false;
  }
  return now.master - cue.pts > max_cue_age_;
}

void RendererImpl::PumpText(int source_id) {
  if (source_id < 0 || static_cast<size_t>(source_id) >= text_legs_.size()) {
    return;
  }
  TextLeg& leg = text_legs_[static_cast<size_t>(source_id)];
  if (!leg.stream || !leg.decoder || leg.read_outstanding || leg.ended) {
    return;
  }
  leg.read_outstanding = true;
  const int generation = leg.generation;
  leg.stream->Read(1, base::BindOnce(&RendererImpl::OnTextRead,
                                     weak_factory_.GetWeakPtr(), source_id,
                                     generation));
}

void RendererImpl::OnTextRead(int source_id, int generation,
                              DemuxerStream::Status status,
                              DemuxerStream::DecoderBufferVector buffers) {
  if (source_id < 0 || static_cast<size_t>(source_id) >= text_legs_.size()) {
    return;
  }
  TextLeg& leg = text_legs_[static_cast<size_t>(source_id)];
  leg.read_outstanding = false;
  if (generation != leg.generation) {
    return;  // Reply from a leg that was switched away or flushed.
  }
  if (status == DemuxerStream::Status::kAborted) {
    return;  // A flush invalidates the leg; the next PumpText re-arms.
  }
  if (status != DemuxerStream::Status::kOk) {
    // H2: a non-aborted read failure is terminal for the text leg (PumpText
    // stops re-arming), so report it instead of letting subtitles die
    // silently. kAborted returned above is a flush, not an error.
    const std::string name = DemuxerStream::GetStatusName(status);
    LOG(ERROR) << "avbase.text: read failed (" << name << ")";
    ReportError(MediaError(ErrorCode::kDecodeFailed, "text read failed", name,
                           "check the text track, or disable subtitles"));
    return;
  }
  for (auto& buffer : buffers) {
    if (buffer->IsEndOfStream()) {
      leg.ended = true;
      return;
    }
    if (leg.stream && buffer->serial() < leg.stream->serial()) {
      continue;  // Pre-seek generation; docs/04 §4 R1.
    }
    std::vector<TimedTextCue> cues;
    if (const Status decode = leg.decoder->Decode(*buffer, &cues); !decode) {
      LOG(ERROR) << "avbase.text: decode failed: " << decode.error().ToString();
      continue;
    }
    if (client_) {
      for (TimedTextCue& cue : cues) {
        if (IsCueStale(cue)) {
          // Dropped, not clamped forward. Shifting a stale cue onto the
          // present would display a line whose text belongs to a moment the
          // viewer has already watched past, which is worse than no subtitle.
          ++stale_cues_dropped_;
          continue;
        }
        // Tag the cue with the source it was decoded from so a multi-source
        // host can route it to the right subtitle surface (docs/14 stage 4).
        cue.source_id = source_id;
        client_->OnTimedText(cue);
      }
    }
  }
  PumpText(source_id);
}

}  // namespace avbase::media
