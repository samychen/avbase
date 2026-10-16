// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_TESTS_SUPPORT_TEXT_FAKE_DEMUXER_H_
#define AVBASE_TESTS_SUPPORT_TEXT_FAKE_DEMUXER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/demuxer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_info.h"
#include "tests/support/fake_demuxer_stream.h"

namespace avbase::media::test {

// A Demuxer whose only streams are scripted text streams (FakeDemuxerStream),
// numbered from |first_index| -- deliberately NOT zero-based in the tests
// that use it, because two children answering the same LOCAL index is the
// case only the aggregate numbering can tell apart (docs/14 stage 4).
//
// Promoted out of composite_demuxer_unittest.cc when the pipeline-level
// suites needed a text-only source too: the Demuxer boilerplate below is the
// third copy that would otherwise exist, and a double that drifts from the
// interface is worse than no double. For the video/audio shapes see
// SyntheticDemuxer; for a fixed stream list handed straight to a renderer see
// FakeMediaResource (fake_demuxer_stream.h).
class TextFakeDemuxer : public Demuxer {
 public:
  explicit TextFakeDemuxer(int32_t first_index, size_t stream_count = 2);

  // Demuxer / MediaResource.
  DemuxerStream* GetStream(DemuxerStreamType type) override;
  std::vector<DemuxerStream*> GetStreams(DemuxerStreamType type) override;
  void SetActiveStream(DemuxerStreamType type, int stream_index) override;
  const MediaInfo& media_info() const override { return media_info_; }
  void Initialize(const DataSourceDescriptor&, const DemuxerOptions&, Host*,
                  base::scoped_refptr<base::SequencedTaskRunner>,
                  InitializeCB cb) override {
    std::move(cb).Run(Status());
  }
  void StartPlayingFrom(base::TimeDelta, SeekCB cb) override {
    std::move(cb).Run(Status(), base::TimeDelta());
  }
  void Flush(base::OnceClosure cb) override { std::move(cb).Run(); }
  void Reset(base::OnceClosure cb) override { std::move(cb).Run(); }
  void Stop() override {}
  base::TimeDelta GetStartTime() const override { return base::TimeDelta(); }
  bool IsLive() const override { return false; }
  bool IsSeekable() const override { return true; }
  DemuxerStats GetStats() const override { return DemuxerStats(); }
  const char* name() const override { return "TextFakeDemuxer"; }

  // Scripting and observation. The concrete type first: AppendBuffer is how a
  // test feeds the text leg, and casting at every call site is noise.
  FakeDemuxerStream* text_stream(size_t i) { return streams_[i].get(); }
  DemuxerStream* stream(size_t i) const { return streams_[i].get(); }
  DemuxerStreamType last_active_type() const { return last_active_type_; }
  int last_active_index() const { return last_active_index_; }
  int active_calls() const { return active_calls_; }

 private:
  std::vector<std::unique_ptr<FakeDemuxerStream>> streams_;
  MediaInfo media_info_;
  DemuxerStreamType last_active_type_{DemuxerStreamType::kUnknown};
  int last_active_index_{-1};
  int active_calls_{0};
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_TEXT_FAKE_DEMUXER_H_
