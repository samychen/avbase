// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/renderers/default_renderer_factory.h"

#include <utility>

#include "media/filters/audio_renderer_impl.h"
#include "media/filters/null_audio_sink.h"
#include "media/filters/null_video_sink.h"
#include "media/filters/renderer_impl.h"
#include "media/filters/video_renderer_impl.h"

namespace avbase::media {

DefaultRendererFactory::DefaultRendererFactory(Deps deps)
    : deps_(std::move(deps)) {
  CHECK(deps_.video_task_runner);
  CHECK(deps_.audio_task_runner);
  CHECK(deps_.tick_clock);
}

DefaultRendererFactory::~DefaultRendererFactory() = default;

std::unique_ptr<Renderer> DefaultRendererFactory::CreateRenderer(
    RendererType type,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner) {
  // nullptr means "not mine", which lets a chained factory fall through -- the
  // same shape DecoderSelector uses for decoders.
  if (type != RendererType::kRendererImpl) {
    return nullptr;
  }
  RendererImpl::Deps impl_deps;
  impl_deps.media_task_runner = std::move(media_task_runner);
  impl_deps.video_task_runner = deps_.video_task_runner;
  impl_deps.audio_task_runner = deps_.audio_task_runner;
  impl_deps.tick_clock = deps_.tick_clock;
  impl_deps.video_factories = deps_.video_decoder_factories;
  // The preference is passed through untouched; VideoRendererImpl applies it
  // once the stream's config is known.
  impl_deps.video_decoder_preference = deps_.video_decoder_preference;
  impl_deps.video_hw_codecs = deps_.video_hw_codecs;
  impl_deps.video_decoder_preference_set = deps_.video_decoder_preference_set;
  impl_deps.audio_factories = deps_.audio_decoder_factories;
  impl_deps.text_decoder_factory = deps_.text_decoder_factory;
  impl_deps.video_sink = CreateVideoRendererSink(display_);
  impl_deps.audio_sink = CreateAudioRendererSink();
  // Docs/14 stage 3: forward the secondary-leg sink factory, if the host set
  // one. Null means the secondary legs fall back to NullVideoSink in
  // RendererImpl -- the decode chains still run, the frames are just not
  // composited until a host wires this up.
  impl_deps.extra_video_sink_factory = deps_.extra_video_sink_factory;
  impl_deps.compositor_thresholds = deps_.compositor_thresholds;
  impl_deps.sync_thresholds = deps_.sync_thresholds;
  impl_deps.sync_master = deps_.sync_master;
  impl_deps.audio_frames_per_buffer = deps_.audio_frames_per_buffer;
  impl_deps.audio_filter_graph = deps_.audio_filter_graph;
  impl_deps.audio_filter_factory = deps_.audio_filter_factory;
  impl_deps.video_filter_graph = deps_.video_filter_graph;
  impl_deps.video_filter_factory = deps_.video_filter_factory;
  impl_deps.video_disabled = deps_.video_disabled;
  impl_deps.audio_disabled = deps_.audio_disabled;
  impl_deps.video_max_fps = deps_.video_max_fps;
  impl_deps.video_max_frame_drop = deps_.video_max_frame_drop;
  impl_deps.av_sync = deps_.av_sync;
  return std::make_unique<RendererImpl>(std::move(impl_deps));
}

std::unique_ptr<VideoRendererSink>
DefaultRendererFactory::CreateVideoRendererSink(
    base::scoped_refptr<NativeDisplay> display) {
  std::unique_ptr<VideoRendererSink> sink;
  if (deps_.disable_video_output) {
    // "nodisp": present nowhere, and say so in the stats. The display is
    // dropped on the floor rather than forwarded, and the sink is built in
    // always-discard mode: NullVideoSink treats a valid target as "shown", so
    // without both halves a later Player::SetVideoSurface() would flip the
    // flag back and count every discarded frame as presented -- the one number
    // this sink exists to report honestly.
    sink = std::make_unique<NullVideoSink>(/*always_discard=*/true);
    sink->SetOutputTarget(nullptr);
  } else if (deps_.video_sink_factory) {
    sink = deps_.video_sink_factory->Create(std::move(display));
  } else {
    sink = std::make_unique<NullVideoSink>();
    sink->SetOutputTarget(std::move(display));
  }
  if (!deps_.scene_observer.is_null()) {
    // Asked of the SINK, not guessed from which factory we were handed: a null
    // sink and a frame-capturing one are both VideoRendererSinks and neither
    // has a scene, and RTTI is off so there is no safe way to find that out
    // from here.
    deps_.scene_observer.Run(sink->scene_engine());
  }
  return sink;
}

base::scoped_refptr<AudioRendererSink>
DefaultRendererFactory::CreateAudioRendererSink() {
  if (deps_.audio_sink_factory) {
    return deps_.audio_sink_factory->Create();
  }
  return base::MakeRefCounted<NullAudioSink>();
}

VideoDecoderFactory* DefaultRendererFactory::GetVideoDecoderFactory() {
  return deps_.video_decoder_factories.empty()
             ? nullptr
             : deps_.video_decoder_factories.front().get();
}

AudioDecoderFactory* DefaultRendererFactory::GetAudioDecoderFactory() {
  return deps_.audio_decoder_factories.empty()
             ? nullptr
             : deps_.audio_decoder_factories.front().get();
}

}  // namespace avbase::media
