// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Sink factories that hand out the manually-pumped fakes from
// fake_renderer_sinks.h. A pipeline-level test drives rendering itself
// (there is no device thread), so it must reach the *exact* sink instances
// the renderer assembled with -- and the factory is the only witness of
// that assembly, because Create() runs on the media sequence while the
// pipeline is starting.

#ifndef IJKPP_TESTS_SUPPORT_FAKE_SINK_FACTORIES_H_
#define IJKPP_TESTS_SUPPORT_FAKE_SINK_FACTORIES_H_

#include <memory>
#include <mutex>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/video_renderer_sink.h"
#include "tests/support/fake_renderer_sinks.h"

namespace ijkpp::media::test {

class FakeVideoSinkFactory final : public VideoRendererSinkFactory {
 public:
  FakeVideoSinkFactory() = default;

  // The sink created last, borrowed. Valid while the pipeline holds it --
  // which is exactly the window a test pumps it in.
  FakeVideoSink* last_sink() const;

  // VideoRendererSinkFactory.
  std::unique_ptr<VideoRendererSink> Create(
      base::scoped_refptr<NativeDisplay> display) override;
  const char* name() const override { return "FakeVideoSinkFactory"; }

 private:
  mutable std::mutex lock_;
  FakeVideoSink* last_ = nullptr;
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

}  // namespace ijkpp::media::test

#endif  // IJKPP_TESTS_SUPPORT_FAKE_SINK_FACTORIES_H_
