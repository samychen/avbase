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
  impl_deps.audio_factories = deps_.audio_decoder_factories;
  impl_deps.video_sink = CreateVideoRendererSink(display_);
  impl_deps.audio_sink = CreateAudioRendererSink();
  impl_deps.compositor_thresholds = deps_.compositor_thresholds;
  impl_deps.sync_thresholds = deps_.sync_thresholds;
  impl_deps.sync_master = deps_.sync_master;
  impl_deps.audio_frames_per_buffer = deps_.audio_frames_per_buffer;
  impl_deps.video_disabled = deps_.video_disabled;
  impl_deps.audio_disabled = deps_.audio_disabled;
  impl_deps.av_sync = deps_.av_sync;
  return std::make_unique<RendererImpl>(std::move(impl_deps));
}

std::unique_ptr<VideoRendererSink> DefaultRendererFactory::
    CreateVideoRendererSink(base::scoped_refptr<NativeDisplay> display) {
  if (deps_.video_sink_factory) {
    return deps_.video_sink_factory->Create(std::move(display));
  }
  auto sink = std::make_unique<NullVideoSink>();
  sink->SetOutputTarget(std::move(display));
  return sink;
}

base::scoped_refptr<AudioRendererSink> DefaultRendererFactory::
    CreateAudioRendererSink() {
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
