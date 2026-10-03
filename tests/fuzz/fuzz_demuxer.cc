// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// libFuzzer target: structured fuzzing of the demux input path (Phase 4.5 /
// design goal G8). Every input is a byte blob that becomes a MemoryDataSource
// and is pushed through the real pipeline front door: FFmpegDemuxer
// Initialize (container probe), a bounded number of Read()s on whatever
// streams opened, and a full Stop. The fuzzer therefore exercises the
// AVIOContext bridge, avformat probing, and the stream queue machinery --
// the components a malicious or corrupt remote source talks to first.
//
// Deliberately NOT a throughput target: each input uses the same threads a
// playback would, and every wait is time-bounded so a hung input is a missed
// finding for the soak run, never a wedged fuzzer. Corpus seeds are real
// containers (tests/fuzz/seeds/); libFuzzer's mutations corrupt headers,
// truncate moov atoms and splice across formats -- the corpus grows into the
// repo so regressions re-run.
//
// LIFETIME, the part this target exists to stress: MemoryDataSource does not
// copy its bytes, and libFuzzer invalidates the input buffer on return. The
// byte copy therefore lives on the heap and is destroyed by the same task
// that destroys the demuxer, on the media thread, FIFO after everything the
// input posted.

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "media/base/data_source.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_error.h"
#include "media/base/media_log.h"
#include "media/filters/ffmpeg_demuxer.h"
#include "media/filters/ffmpeg_glue.h"

#if !AVBASE_ENABLE_FFMPEG
#error "fuzz_demuxer requires AVBASE_ENABLE_FFMPEG"
#endif

namespace {

using namespace std::chrono_literals;  // NOLINT -- a fuzz target.

using avbase::media::Demuxer;
using avbase::media::DemuxerStream;
using avbase::media::DemuxerStreamType;

// Per-input budgets. The fuzzer's job is finding crashes, not measuring
// speed; the soak/corpus runs care about never wedging.
constexpr auto kInitTimeout = 10s;
constexpr auto kReadTimeout = 5s;
constexpr auto kDrainTimeout = 5s;
constexpr int kMaxReadRounds = 8;

// Demuxer::Host that records what arrived and never blocks.
class FuzzHost : public Demuxer::Host {
 public:
  void SetDuration(avbase::base::TimeDelta duration) override {
    std::scoped_lock scoped(lock_);
    duration_ = duration;
  }
  void OnBufferedTimeUpdate(avbase::base::TimeDelta buffered,
                            avbase::base::TimeDelta playback_time) override {
    (void)buffered;
    (void)playback_time;
  }
  void OnDemuxerError(avbase::media::MediaError error) override {
    std::scoped_lock scoped(lock_);
    error_ = std::move(error);
  }

 private:
  mutable std::mutex lock_;
  avbase::base::TimeDelta duration_;
  avbase::media::MediaError error_;
};

// Shared across inputs: FFmpeg global init runs once, the media thread (the
// demuxer's runner) lives as long as the process.
struct DemuxHarness {
  DemuxHarness() : media_thread_("fuzz-media") {
    avbase::media::ffmpeg::InitializeFFmpeg();
    media_thread_.Start();
  }

  avbase::base::Thread media_thread_;
};

// Waits for |event| up to |timeout|; false means "give up on this input".
bool WaitFor(avbase::base::WaitableEvent& event,
             std::chrono::milliseconds timeout) {
  return event.TimedWait(avbase::base::Milliseconds(static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count())));
}

// Destroys the demuxer and its byte buffer ON the media thread, FIFO after
// every task the input posted. |bytes| backs the MemoryDataSource; both die
// together.
void RetireOnMediaThread(avbase::base::Thread& media_thread,
                         std::unique_ptr<avbase::media::FFmpegDemuxer> demuxer,
                         std::unique_ptr<std::vector<uint8_t>> bytes) {
  media_thread.task_runner()->PostTask(
      FROM_HERE,
      avbase::base::BindOnce([](std::unique_ptr<avbase::media::FFmpegDemuxer> d,
                                std::unique_ptr<std::vector<uint8_t>> b) {},
                             std::move(demuxer), std::move(bytes)));
  avbase::base::WaitableEvent drained;
  media_thread.task_runner()->PostTask(
      FROM_HERE,
      avbase::base::BindOnce(
          [](avbase::base::WaitableEvent* e) { e->Signal(); }, &drained));
  WaitFor(drained, kDrainTimeout);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 8 || size > (4u << 20)) {
    return 0;  // Too small to open; too large to be worth fuzzing here.
  }
  // Deliberately leaked: a static DemuxHarness would be destroyed at process
  // exit, and its Thread joins after the logging subsystem's static mutex is
  // already gone (Stop() logs -- abort caught exactly that). Process-lifetime
  // infra in a fuzzer is the textbook intentional leak.
  static DemuxHarness* harness = new DemuxHarness();

  // The copy that outlives the input buffer.
  auto bytes = std::make_unique<std::vector<uint8_t>>(data, data + size);
  auto source = avbase::base::MakeRefCounted<avbase::media::MemoryDataSource>(
      bytes->data(), bytes->size());
  auto descriptor = avbase::media::DataSourceDescriptor::FromSource(source);

  FuzzHost host;
  auto demuxer = std::make_unique<avbase::media::FFmpegDemuxer>(
      avbase::base::MakeRefCounted<avbase::media::MediaLog>());

  // Heap-held: a callback may fire long after this input gave up waiting,
  // and the event must outlive the stack frame (the retire task below keeps
  // the demuxer alive exactly that long).
  auto init_done = std::make_shared<avbase::base::WaitableEvent>();
  harness->media_thread_.task_runner()->PostTask(
      FROM_HERE,
      avbase::base::BindOnce(
          &Demuxer::Initialize, avbase::base::Unretained(demuxer.get()),
          descriptor, avbase::media::DemuxerOptions{}, &host,
          harness->media_thread_.task_runner(),
          avbase::base::BindOnce(
              [](std::shared_ptr<avbase::base::WaitableEvent> done,
                 avbase::media::Status status) {
                (void)status;  // A failed open is a finding too.
                done->Signal();
              },
              init_done)));
  const bool opened = WaitFor(*init_done, kInitTimeout);
  if (opened) {
    // Read a bounded number of buffers from whatever streams exist. A hung
    // read gives up on the input; the retire below still runs.
    for (int round = 0; round < kMaxReadRounds; ++round) {
      DemuxerStream* stream =
          demuxer->GetStream(round % 2 == 0 ? DemuxerStreamType::kAudio
                                            : DemuxerStreamType::kVideo);
      if (!stream) {
        continue;
      }
      auto read_done = std::make_shared<avbase::base::WaitableEvent>();
      stream->Read(
          4, avbase::base::BindOnce(
                 [](std::shared_ptr<avbase::base::WaitableEvent> done,
                    DemuxerStream::Status,
                    std::vector<avbase::base::scoped_refptr<
                        avbase::media::DecoderBuffer>>) { done->Signal(); },
                 read_done));
      if (!WaitFor(*read_done, kReadTimeout)) {
        break;
      }
    }
  }

  // Full teardown every input: the Stop path is exactly what a playback
  // aborts through, and it is where lifecycle bugs surface. Stop() is
  // non-blocking and posted like every other entry point; the retire task
  // FIFO-guarantees it ran before the demuxer and its bytes go away.
  harness->media_thread_.task_runner()->PostTask(
      FROM_HERE, avbase::base::BindOnce(
                     &Demuxer::Stop, avbase::base::Unretained(demuxer.get())));
  RetireOnMediaThread(harness->media_thread_, std::move(demuxer),
                      std::move(bytes));
  return 0;
}
