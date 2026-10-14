// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_RENDERERS_DEFAULT_RENDERER_FACTORY_H_
#define AVBASE_MEDIA_RENDERERS_DEFAULT_RENDERER_FACTORY_H_

#include <memory>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/tick_clock.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/renderer_factory.h"
#include "media/base/text_decoder.h"
#include "media/base/video_decoder_factory.h"
#include "media/base/video_renderer_sink.h"
#include "media/filters/audio_filter_stage.h"
#include "media/filters/decoder_selector.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/legacy/video_frame_compositor.h"
#include "media/filters/video_filter_stage.h"
#include "media/media_export.h"

namespace avbase::media {

// The production RendererFactory: assembles RendererImpl from the sink and
// decoder factories it was constructed with, and falls back to the null sinks
// when none were injected.
//
// THE SEAM (renderer_factory.h gap 4, resolved): player::Deps carries (a)
// VideoRendererSinkFactory/AudioRendererSinkFactory and Player::Impl carries
// the display from SetVideoSurface(). This class is *constructed from* those
// and answers RendererFactory::CreateVideoRendererSink() with what it was
// given -- so (c) is the internal seam and (a) stays the public one. A null
// sink factory means "no backend injected", and the null sinks are what make
// zero-configuration headless playback work (docs/10 rule E1).
//
// OWNERSHIP OF THE SEQUENCES: this class does not create threads. It receives
// the S3/S4 runners at construction from whoever owns the threads (Player::
// Impl), and passes them into every RendererImpl it builds -- RendererImpl
// borrows the runners and must be destroyed before they are.
class AVBASE_MEDIA_EXPORT DefaultRendererFactory final
    : public RendererFactory {
 public:
  struct Deps {
    // S3 / S4 in docs/04 §1. Borrowed; must outlive every renderer created.
    base::scoped_refptr<base::SequencedTaskRunner> video_task_runner;
    base::scoped_refptr<base::SequencedTaskRunner> audio_task_runner;
    const base::TickClock* tick_clock = nullptr;
    std::vector<base::scoped_refptr<VideoDecoderFactory>>
        video_decoder_factories;
    std::vector<base::scoped_refptr<AudioDecoderFactory>>
        audio_decoder_factories;
    base::scoped_refptr<TextDecoderFactory> text_decoder_factory;
    std::shared_ptr<VideoRendererSinkFactory> video_sink_factory;
    std::shared_ptr<AudioRendererSinkFactory> audio_sink_factory;
    VideoFrameCompositor::Thresholds compositor_thresholds;
    AvSyncController::Thresholds sync_thresholds;
    AvSyncController::MasterType sync_master{
        AvSyncController::MasterType::kAudio};
    int audio_frames_per_buffer = 1024;
    // config.audio.filter_graph ("af") plus the stage factory the host
    // injected. Forwarded through RendererImpl into AudioRendererImpl.
    std::string audio_filter_graph;
    AudioFilterStageFactory audio_filter_factory;
    std::string video_filter_graph;
    VideoFilterStageFactory video_filter_factory;
    // config.video.decoder_preference and config.video.hw_codecs, forwarded to
    // VideoRendererImpl so the ranking happens against the real stream config
    // (see the setter's comment for why it is not done here). Unset means the
    // injected factory order is authoritative -- a host that built its own
    // list has already decided, and re-ranking would overrule it.
    DecoderPreference video_decoder_preference{DecoderPreference::kAuto};
    HwCodecMask video_hw_codecs{static_cast<HwCodecMask>(HwCodecFlag::kAll)};
    bool video_decoder_preference_set = false;
    // config.video.disabled / config.audio.disabled ("vn"/"an"): the stream
    // is hidden from the renderer, exactly as if the container had none.
    bool video_disabled = false;
    bool audio_disabled = false;
    // config.video.max_fps ("max-fps") and config.video.max_frame_drop
    // ("framedrop"). Both default to 0 here, which is the compositor's own
    // default and means "no cap / no dropping" -- so a host that builds a
    // renderer without a Player keeps today's behaviour, and it is the
    // PlayerConfig default (31) that turns the cap on when it is plumbed
    // through. See VideoFrameCompositor::SetMaxFps for what the values mean.
    int video_max_fps = 0;
    int video_max_frame_drop = 0;
    // The shared clock controller, constructed by the facade per playback
    // (the facade and PipelineImpl::GetMediaTime() both need the same
    // instance to survive renderer teardown).
    std::shared_ptr<AvSyncController> av_sync;

    // Called once per video sink this factory creates, with that sink's scene
    // engine -- or null when the sink cannot composite, which is every sink
    // that is not rendering through GL, including the null sink the
    // zero-configuration host gets. See VideoRendererSink::scene_engine() on
    // why it is answered rather than inferred.
    //
    // An observer rather than a pointer because the sink does not exist when
    // this factory is built: it is created later, on whatever sequence the
    // renderer is started from, and this is the only moment anything outside
    // the pipeline can see it before it starts presenting.
    //
    // May be null, meaning nobody is listening.
    base::RepeatingCallback<void(GlSceneEngine*)> scene_observer;
  };

  explicit DefaultRendererFactory(Deps deps);
  DefaultRendererFactory(const DefaultRendererFactory&) = delete;
  DefaultRendererFactory& operator=(const DefaultRendererFactory&) = delete;
  ~DefaultRendererFactory() override;

  // The display the next CreateVideoRendererSink() should target. Set from
  // Player::SetVideoSurface() before PrepareAsync(); a live swap goes through
  // Renderer::SetOutputTarget instead of here.
  void set_display(base::scoped_refptr<NativeDisplay> display) {
    display_ = std::move(display);
  }

  // RendererFactory:
  std::unique_ptr<Renderer>
  CreateRenderer(RendererType type,
                 base::scoped_refptr<base::SequencedTaskRunner>
                     media_task_runner) override;
  std::unique_ptr<VideoRendererSink>
  CreateVideoRendererSink(base::scoped_refptr<NativeDisplay> display) override;
  base::scoped_refptr<AudioRendererSink> CreateAudioRendererSink() override;
  VideoDecoderFactory* GetVideoDecoderFactory() override;
  AudioDecoderFactory* GetAudioDecoderFactory() override;
  const char* name() const override { return "default"; }

 private:
  Deps deps_;
  base::scoped_refptr<NativeDisplay> display_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_RENDERERS_DEFAULT_RENDERER_FACTORY_H_
