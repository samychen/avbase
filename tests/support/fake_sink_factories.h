// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Sink factories that hand out the manually-pumped fakes from
// fake_renderer_sinks.h. A pipeline-level test drives rendering itself
// (there is no device thread), so it must reach the *exact* sink instances
// the renderer assembled with -- and the factory is the only witness of
// that assembly, because Create() runs on the media sequence while the
// pipeline is starting.

#ifndef AVBASE_TESTS_SUPPORT_FAKE_SINK_FACTORIES_H_
#define AVBASE_TESTS_SUPPORT_FAKE_SINK_FACTORIES_H_

#include <memory>
#include <mutex>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/video_renderer_sink.h"
#include "tests/support/fake_renderer_sinks.h"

namespace avbase::media::test {

class FakeVideoSinkFactory final : public VideoRendererSinkFactory {
 public:
  FakeVideoSinkFactory() = default;

  // The sink created last, borrowed. Valid while the pipeline holds it --
  // which is exactly the window a test pumps it in.
  FakeVideoSink* last_sink() const;
  // EVERY sink this factory created, in creation order, borrowed under the
  // same rule. The multi-source suites need the full list (docs/14 stage 1):
  // one sink per secondary video leg, and "the host sink went to source 0"
  // is only assertable against all of them -- last_sink() alone cannot tell
  // the first leg's sink from the second's.
  std::vector<FakeVideoSink*> sinks() const;

  // VideoRendererSinkFactory.
  std::unique_ptr<VideoRendererSink>
  Create(base::scoped_refptr<NativeDisplay> display) override;
  const char* name() const override { return "FakeVideoSinkFactory"; }

 private:
  mutable std::mutex lock_;
  FakeVideoSink* last_ = nullptr;
  std::vector<FakeVideoSink*> all_;
};

class FakeAudioSinkFactory final : public AudioRendererSinkFactory {
 public:
  FakeAudioSinkFactory() = default;

  // Same borrowing rule as the video side.
  FakeAudioSink* last_sink() const;

  // AudioRendererSinkFactory.
  base::scoped_refptr<AudioRendererSink> Create() override;
  const char* name() const override { return "FakeAudioSinkFactory"; }

 private:
  mutable std::mutex lock_;
  FakeAudioSink* last_ = nullptr;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_FAKE_SINK_FACTORIES_H_
