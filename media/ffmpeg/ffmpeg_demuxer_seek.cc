// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The seek path, split out of ffmpeg_demuxer.cc when the fix for
// the EOF-restart fix took that file over its C1 line ceiling. Same seam and
// reasoning as ffmpeg_demuxer_track_select.cc: everything left in
// ffmpeg_demuxer.cc is the AVFormatContext lifecycle, and a seek is the one
// lifecycle step that is a REQUEST (publish, execute, answer) rather than a
// loop. Keeping the three functions together means the whole answer path can
// be read without reading the demux loop, which is where the bug hid.
//
// WHAT MOVED HERE:
//   * StartPlayingFrom      -- media sequence: validate, publish, restart.
//   * HandleSeekRequestOnDemuxThread -- demux thread: av_seek_frame + flush.
//   * CompleteSeek          -- answer the registered callback exactly once.
//
// The one thing that is NOT a pure move is StartPlayingFrom restarting the
// demux loop: the loop returns at EOF, so the seek that lands after playback
// finished is the one that has to post it again. That handshake is documented
// where both halves live -- StartPlayingFrom below and the exit block of
// DemuxLoop in ffmpeg_demuxer.cc -- and it is locked on both sides.
//
// Nothing here owns anything: the AVFormatContext is the demuxer's, the request
// slot and the callback map are members, and the loop lock guards a bool.

#include "media/ffmpeg/ffmpeg_demuxer.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "media/base/media_log.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::media::ffmpeg;

AVFormatContext* Ctx(void* raw) {
  return static_cast<AVFormatContext*>(raw);
}

}  // namespace

void FFmpegDemuxer::HandleSeekRequestOnDemuxThread(const SeekRequest& request) {
  AVFormatContext* ctx = Ctx(format_ctx_raw_);
  if (!ctx) {
    CompleteSeek(request.request_id,
                 Err(ErrorCode::kInvalidState, "demuxer is not open", {},
                     "call PrepareAsync() first"),
                 request.target);
    return;
  }
  interrupt_flag_.Reset();
  const base::TimeTicks seek_started = base::TimeTicks::Now();

  // Prefer the video stream as the seek reference: seeking against audio in a
  // file whose audio has sparse index entries lands far from the request.
  int seek_index = -1;
  for (const auto& stream : streams_) {
    if (stream->type() == DemuxerStreamType::kVideo) {
      seek_index = stream->stream_index();
      break;
    }
  }
  if (seek_index < 0 && !streams_.empty()) {
    seek_index = streams_[0]->stream_index();
  }

  AVRational time_base{1, AV_TIME_BASE};
  if (seek_index >= 0) {
    time_base = ctx->streams[seek_index]->time_base;
  }
  const int64_t target_ts =
      ff::FromTimeDelta(request.target + start_time_, time_base);
  const int flags = request.any_frame ? 0 : AVSEEK_FLAG_BACKWARD;
  const int ret = av_seek_frame(ctx, seek_index, target_ts, flags);
  seek_count_.fetch_add(1, std::memory_order_relaxed);

  if (ret < 0) {
    CompleteSeek(
        request.request_id,
        base::unexpected(ff::ToMediaError(
            ret, "FFmpegDemuxer::Seek",
            "target = " + std::to_string(request.target.InMilliseconds()) +
                " ms",
            "this source may not be seekable; check "
            "media_info().seekable and is_live()")),
        request.target);
    return;
  }

  // Bump every stream's generation and drop what is already queued: buffers
  // from before the seek must never reach a decoder (docs/04 §4 rule R1).
  for (auto& stream : streams_) {
    stream->FlushFromDemuxThread(base::DoNothing());
  }

  const base::TimeDelta load_duration = base::TimeTicks::Now() - seek_started;
  {
    base::AutoLock scoped(stats_lock_);
    stats_.last_seek_duration = load_duration;
  }
  interrupt_flag_.Reset();
  resume_event_.Signal();

  // Report the position actually reached. av_seek_frame with BACKWARD lands at
  // or before the request; reading the first packet afterwards would give the
  // exact value, but that would couple seeking to decoding, so the request
  // target is reported and the accurate-seek layer refines it (docs/04 §4.1).
  CompleteSeek(request.request_id, OkStatus(), request.target);
}

void FFmpegDemuxer::CompleteSeek(int64_t request_id, Status status,
                                 base::TimeDelta actual) {
  SeekCB cb;
  {
    base::AutoLock scoped(seek_cb_lock_);
    auto it = pending_seek_cbs_.find(request_id);
    if (it == pending_seek_cbs_.end()) {
      return;  // Superseded by a newer seek; its callback was already answered.
    }
    cb = std::move(it->second);
    pending_seek_cbs_.erase(it);
  }
  if (media_log_) {
    media_log_->AddEvent(
        MediaLogEvent::Level::kInfo, MediaLogEvent::Type::kSeekCompleted,
        {{"request_id", std::to_string(request_id)},
         {"target_ms", std::to_string(actual.InMilliseconds())},
         {"ok", status ? "true" : "false"}},
        "seek completed");
  }
  if (cb && media_runner_) {
    media_runner_->PostTask(FROM_HERE,
                            base::BindOnce(
                                [](SeekCB c, Status s, base::TimeDelta a) {
                                  std::move(c).Run(std::move(s), a);
                                },
                                std::move(cb), std::move(status), actual));
  }
}

void FFmpegDemuxer::StartPlayingFrom(base::TimeDelta time, SeekCB cb) {
  if (!opened_ok_.IsSet() || !demux_thread_) {
    if (cb) {
      std::move(cb).Run(Err(ErrorCode::kInvalidState,
                            "cannot seek before Initialize() succeeded",
                            "state = not opened",
                            "call PrepareAsync() and wait for kPrepared first"),
                        time);
    }
    return;
  }
  if (!media_info_.seekable) {
    if (cb) {
      std::move(cb).Run(
          Err(ErrorCode::kMediaUnseekable, "this stream is not seekable",
              "uri = \"" + media_info_.uri + "\"\n           is_live = " +
                  (media_info_.is_live ? "true" : "false"),
              "live and non-seekable streams cannot seek; check is_live(). "
              "For VOD-over-HLS the playlist must contain #EXT-X-ENDLIST."),
          time);
    }
    return;
  }

  const int64_t request_id = next_request_id_.fetch_add(1);
  {
    base::AutoLock scoped(seek_cb_lock_);
    // Answer every superseded request rather than dropping it silently, so a
    // caller doing rapid seeks never waits forever on a stale id.
    for (auto& [id, pending_cb] : pending_seek_cbs_) {
      if (pending_cb) {
        media_runner_->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](SeekCB c, Status s, base::TimeDelta a) {
                  std::move(c).Run(std::move(s), a);
                },
                std::move(pending_cb),
                Err(ErrorCode::kAborted, "seek superseded by a newer request",
                    "request_id = " + std::to_string(id),
                    "this is expected during rapid seeking; match completions "
                    "to "
                    "requests by request_id"),
                time));
      }
    }
    pending_seek_cbs_.clear();
    if (cb) {
      pending_seek_cbs_[request_id] = std::move(cb);
    }
  }

  // Publish the request. A burst collapses to the latest one.
  delete pending_seek_.exchange(new SeekRequest{time, false, request_id});

  // Then restart the loop if it is gone -- a finished source is exactly the
  // state a seek lands in after playback ran to the end. Ownership rule: the
  // side that moves the state to kRunning posts the task, and that move is a
  // single compare-and-set under loop_lock_, so exactly one loop ever owns
  // this request. Do not "simplify" this into an unguarded load of a bool --
  // that is the bug this replaced (docs/04 §2.1, "EOF 与 seek 的握手").
  bool restart_loop = false;
  {
    base::AutoLock scoped(loop_lock_);
    if (loop_state_ == LoopState::kGone && demux_thread_) {
      loop_state_ = LoopState::kRunning;
      restart_loop = true;
    }
  }
  if (restart_loop) {
    demux_thread_->task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(&FFmpegDemuxer::DemuxLoop, base::Unretained(this)));
  }

  // Interrupt a possibly-blocking av_read_frame so the seek is picked up
  // promptly instead of after the current read finishes.
  interrupt_flag_.Set();
  interrupt_count_.fetch_add(1, std::memory_order_relaxed);
  resume_event_.Signal();
}
}  // namespace avbase::media
