// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's sub-renderer construction and wiring (docs/14 stage 1):
// one VideoRendererImpl per source that carries a video stream, the sink
// assignment policy, and the ended / frame-presented / error hooks. Stage 2
// adds the per-source side of the clock offset: the per-leg accessor and
// SetSourceOffset. Split from renderer_impl.cc as a line-count seam (C1).
// The seam is "what was built": Initialize() discovers the sources, this
// file builds a renderer for each and wires it, and the rest of the class
// drives what exists -- Flush, StartPlayingFrom and the track switches
// never construct a leg except through SwitchVideoRenderer, which re-enters
// WireVideoCallbacks.

#include "media/filters/renderer_impl.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "media/filters/null_video_sink.h"

namespace avbase::media {

void RendererImpl::ApplyVideoConfig(VideoRendererImpl* v) {
  if (!deps_.video_filter_graph.empty()) {
    v->set_filter_graph(deps_.video_filter_graph, deps_.video_filter_factory);
  }
  // Forwarded, not applied here: the ranking needs the stream's config, which
  // only exists once Initialize() runs on S3 (see
  // VideoRendererImpl::set_decoder_preference).
  if (deps_.video_decoder_preference_set) {
    v->set_decoder_preference(deps_.video_decoder_preference,
                              deps_.video_hw_codecs);
  }
  // config.video.max_fps / config.video.max_frame_drop ("max-fps" /
  // "framedrop"). 0 disables each, which is also the compositor's default.
  v->SetMaxFps(deps_.video_max_fps);
  v->SetMaxFrameDrop(deps_.video_max_frame_drop);
}

void RendererImpl::CreateSubRenderers(
    const std::vector<SourceStreams>& sources) {
  // clear+resize, not assign(count, nullptr): vector<unique_ptr> has no
  // copyable value to fill with.
  video_renderers_.clear();
  video_renderers_.resize(sources.size());
  video_initialized_.assign(sources.size(), true);
  video_ended_.assign(sources.size(), true);

  // The host sink goes to the FIRST source that has a video leg. With source 0
  // carrying video -- every single-source pipeline, and the common
  // multi-source shape -- that is source 0, exactly as before. When source 0
  // is audio-only and a later source carries the picture, the host sink still
  // lands on the one leg that composites.
  bool host_sink_used = false;

  for (const SourceStreams& s : sources) {
    if (!s.video) {
      continue;
    }
    const size_t i = static_cast<size_t>(s.source_id);
    video_renderers_[i] = std::make_unique<VideoRendererImpl>(
        deps_.video_task_runner, deps_.video_factories, deps_.tick_clock,
        deps_.compositor_thresholds);
    ApplyVideoConfig(video_renderers_[i].get());
    video_initialized_[i] = false;
    video_ended_[i] = false;
    if (i == 0) {
      // Recorded next to the renderer, for the same reason audio_stream_ is
      // (media/base/media_resource.h: identity for "no switch needed" and for
      // a handover to re-anchor against). Source 0 only: the handover
      // machinery below switches source 0's alternates.
      video_stream_ = s.video;
    }
  }
  if (has_audio_) {
    audio_stream_ = sources[0].audio;
    audio_params_ = MakeAudioParameters(audio_stream_->audio_decoder_config(),
                                        deps_.audio_frames_per_buffer);
    audio_ = std::make_unique<AudioRendererImpl>(
        deps_.audio_task_runner, deps_.audio_factories, av_sync_.get());
    if (!deps_.audio_filter_graph.empty()) {
      audio_->set_filter_graph(deps_.audio_filter_graph,
                               deps_.audio_filter_factory);
    }
  }

  // Initialise the sub-renderers on their own sequences. Both callbacks come
  // back to S1, where MaybeReportInitialized() decides when the pipeline may be
  // told that startup finished.
  for (size_t i = 0; i < video_renderers_.size(); ++i) {
    VideoRendererImpl* v = video_renderers_[i].get();
    if (!v) {
      continue;
    }
    std::unique_ptr<VideoRendererSink> sink;
    if (!host_sink_used && deps_.video_sink) {
      // The host's sink: source 0's in every single-source pipeline.
      sink = std::move(deps_.video_sink);
      host_sink_used = true;
    } else if (deps_.extra_video_sink_factory) {
      // A host that opted into multi-source output (none yet -- that is stage
      // 3's scene compositor).
      sink = deps_.extra_video_sink_factory->Create(nullptr);
    } else {
      // Stage-1 policy (§4.1): the decode chain runs, the frames are decoded
      // and presented into a sink that drops them, because compositing stays
      // on source 0's layer until stage 3. Choosing NullVideoSink HERE rather
      // than skipping the leg is what makes "two decode chains" observable:
      // the frames exist, they are just not shown.
      sink = std::make_unique<NullVideoSink>();
    }
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&VideoRendererImpl::Initialize, base::Unretained(v),
                       sources[i].video, std::move(sink),
                       base::BindOnce(&RendererImpl::PostVideoInitialized,
                                      base::Unretained(this),
                                      static_cast<int>(i))));
  }
  if (audio_) {
    deps_.audio_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&AudioRendererImpl::Initialize,
                       base::Unretained(audio_.get()), audio_stream_,
                       audio_params_, deps_.audio_sink,
                       base::BindOnce(&RendererImpl::PostAudioInitialized,
                                      base::Unretained(this))));
  }
  // The ended callbacks fire on the sub-renderers' own sequences (S3/S4); the
  // decision "everything has drained" belongs to S1, so each callback only
  // posts a hop. CheckForEnded() then reads the two thread-safe buffered
  // getters (the compositor's lock and the audio handoff lock) instead of the
  // sub-renderers' plain bools, which would be a data race from S1.
  for (size_t i = 0; i < video_renderers_.size(); ++i) {
    if (video_renderers_[i]) {
      WireVideoCallbacks(static_cast<int>(i));
    }
  }
  if (audio_) {
    WireAudioCallbacks();
  }
}

size_t RendererImpl::video_renderer_count() const {
  size_t count = 0;
  for (const auto& v : video_renderers_) {
    if (v) {
      ++count;
    }
  }
  return count;
}

VideoRendererImpl* RendererImpl::video_renderer(int source_index) const {
  // The index IS the source id (video_renderers_ keeps a null entry for a
  // video-less source precisely so this holds), so an out-of-range index is
  // a caller bug rather than a legitimate "no leg here" -- answered null
  // either way, without reading past the vector.
  if (source_index < 0 ||
      static_cast<size_t>(source_index) >= video_renderers_.size()) {
    return nullptr;
  }
  return video_renderers_[static_cast<size_t>(source_index)].get();
}

void RendererImpl::SetSourceOffset(int source_id, base::TimeDelta offset) {
  // Stated to the clock controller, not applied here: the clock push
  // (renderer_impl_reporting.cc) consumes it, pacing each leg against the
  // master clock minus that source's offset. Source 0 is refused there (the
  // master timeline cannot be shifted against itself) and an unregistered id
  // is ignored -- Initialize registered exactly the sources the resource has.
  //
  // Lives in this file rather than with the other Set* controls because it is
  // a statement about a SOURCE, not about playback: the pipeline derives it
  // from the per-source seek results (docs/14 §7.2), and nothing a viewer or
  // a host can do mid-playback changes it.
  if (av_sync_) {
    av_sync_->SetSourceOffset(source_id, offset);
  }
}

// The callback wiring shared by both construction sites of each sub-renderer
// (initial start above and the mid-flight track switch in
// renderer_impl_controls.cc) -- duplicated wiring is how a new callback gets
// forgotten on one of the two paths. The ended callbacks stay Unretained
// because they only re-post weak-bound tasks (the class-level "task outlives
// its target" closure, see PostVideoEnded in renderer_impl.cc); the H2 error
// callbacks are weak themselves -- ReportError runs directly on the
// sub-renderer's decode sequence with no intermediate hop to cancel a call
// racing teardown, so the binding itself has to be the weak link.
//
// |source_index| picks which of the three video callbacks are wired. The
// frame-presented hook is source 0's alone: it feeds the master video clock
// and the accurate-seek window, both of which are source-0 concepts until
// stage 2 (docs/14 §4.1: "合成仍走源 0 主层"). A secondary leg's frames still
// reach its sink -- they just do not move the clock.
void RendererImpl::WireVideoCallbacks(int source_index) {
  VideoRendererImpl* v =
      video_renderers_[static_cast<size_t>(source_index)].get();
  DCHECK(v);
  v->set_ended_cb(base::BindRepeating(&RendererImpl::PostVideoEnded,
                                      base::Unretained(this), source_index));
  if (source_index == 0) {
    v->set_frame_presented_cb(base::BindRepeating(
        &RendererImpl::OnVideoFramePresented, base::Unretained(this)));
  }
  v->SetErrorCB(base::BindRepeating(&RendererImpl::ReportError,
                                    weak_factory_.GetWeakPtr()));
}

void RendererImpl::WireAudioCallbacks() {
  audio_->set_ended_cb(base::BindRepeating(&RendererImpl::PostAudioEnded,
                                           base::Unretained(this)));
  audio_->SetErrorCB(base::BindRepeating(&RendererImpl::ReportError,
                                         weak_factory_.GetWeakPtr()));
}

}  // namespace avbase::media
