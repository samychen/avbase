// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/fake_sink_factories.h"

namespace ijkpp::media::test {

FakeVideoSink* FakeVideoSinkFactory::last_sink() const {
  std::scoped_lock scoped(lock_);
  return last_;
}

std::unique_ptr<VideoRendererSink> FakeVideoSinkFactory::Create(
    base::scoped_refptr<NativeDisplay> /*display*/) {
  // The display is deliberately ignored: the fake has nowhere to draw, and
  // SetOutputTarget() is how a live swap would reach it anyway.
  auto sink = std::make_unique<FakeVideoSink>();
  std::scoped_lock scoped(lock_);
  last_ = sink.get();
  return sink;
}

FakeAudioSink* FakeAudioSinkFactory::last_sink() const {
  std::scoped_lock scoped(lock_);
  return last_;
}

base::scoped_refptr<AudioRendererSink> FakeAudioSinkFactory::Create() {
  auto sink = base::MakeRefCounted<FakeAudioSink>();
  std::scoped_lock scoped(lock_);
  last_ = sink.get();
  return sink;
}

}  // namespace ijkpp::media::test
