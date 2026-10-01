// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Equivalent in role to Chromium's `media/base/audio_buffer_queue.h`
// (BSD-3-Clause): a frame-addressable queue over a list of decoded
// AudioBuffers, which is what a time-stretching renderer needs because WSOLA
// reads the same frames several times (once as the target block, once inside
// the search block) before consuming them. DELIBERATE DEVIATION: Chromium puts
// this in media/base. ijkpp puts it in media/filters because media/base is the
// frozen interface layer and its headers are reviewed as an API surface
// (docs/03 §11), whereas this class has exactly one consumer --
// AudioRendererAlgorithm. Promoting it to media/base later costs a move and an
// include change; demoting it after publishing it as API does not cost nothing.
// If a second consumer appears (AudioRendererImpl's own buffering, or the M9
// watermarks), that is the signal to move it.
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// NOT YET IN THE BUILD. Never compiled; see the gap list in
// audio_renderer_algorithm.h.

#ifndef IJKPP_MEDIA_FILTERS_AUDIO_FRAME_QUEUE_H_
#define IJKPP_MEDIA_FILTERS_AUDIO_FRAME_QUEUE_H_

#include <deque>
#include <optional>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_bus.h"
#include "media/media_export.h"

namespace ijkpp::media {

// A FIFO of AudioBuffers addressed by frame index rather than by buffer.
//
// Not thread-safe, and deliberately so: AudioRendererAlgorithm owns one and is
// itself single-sequence. Adding a lock here would put a mutex on the audio
// callback path, which AudioRendererSink::RenderCallback's 100 us budget
// (docs/07 §4) does not allow.
//
// WHY PEEK AND READ ARE SEPARATE. WSOLA needs to look at frames it has not
// consumed yet -- the target block and the whole search block are read before
// any of them are dropped -- and then to drop a computed prefix in one step. A
// queue that only offered "take the next N frames" would force the algorithm to
// buffer copies of what it had already taken, which is how ffplay ended up with
// both a packet queue and a frame queue.
class IJKPP_MEDIA_EXPORT AudioFrameQueue {
 public:
  AudioFrameQueue();
  AudioFrameQueue(const AudioFrameQueue&) = delete;
  AudioFrameQueue& operator=(const AudioFrameQueue&) = delete;
  ~AudioFrameQueue();

  // Takes ownership. An end-of-stream marker is ignored rather than queued: it
  // carries no audio, and letting one in would make frames() and
  // front_timestamp() lie to the buffering controller. Callers should filter
  // EOS out and signal it separately (AudioRendererImpl calls
  // AudioRendererAlgorithm::MarkEndOfStream); ignoring it here is the second
  // line of defence, not the first, and it is deliberately not a DCHECK because
  // this queue is drained from the audio thread -- see the note on the same
  // trade in AudioRendererAlgorithm::PeekAudioWithZeroPrepend.
  void Append(base::scoped_refptr<AudioBuffer> buffer);

  int frames() const { return frames_; }
  bool empty() const { return buffers_.empty(); }

  // Timestamp of the buffer at the front, or nullopt when the queue is empty.
  // Note this is the timestamp of the whole buffer, not of the first
  // unconsumed frame within it, so it lags by up to one buffer after a partial
  // read. Chromium has the same approximation; AudioTimestampHelper is what
  // makes it exact, and ijkpp does not have that class yet.
  std::optional<base::TimeDelta> FrontTimestamp() const;

  // Drops everything, releasing the buffers.
  void Clear();

  // Drops |n| frames from the front. Any index the caller holds relative to the
  // front must be shifted by the same amount.
  void SeekFrames(int n);

  // Copies |num_frames| starting |read_offset| frames from the front into
  // |dest| at |write_offset|, without consuming. Frames past the end are zero
  // filled, so a caller can never read samples left over from a previous peek.
  // Returns the number of frames actually available. |scratch| must hold at
  // least |num_frames| and have the same channel count as |dest|; it exists
  // because AudioBuffer::ReadFrames() always writes from frame 0 of its
  // destination, and ijkpp's AudioBus has no partial-offset copy (gap 6 in
  // audio_renderer_algorithm.h).
  int PeekFrames(int num_frames, int read_offset, int write_offset,
                 AudioBus* dest, AudioBus* scratch) const;

  // PeekFrames at offset 0 followed by SeekFrames of whatever was read.
  int ReadFrames(int num_frames, int write_offset, AudioBus* dest,
                 AudioBus* scratch);

 private:
  std::deque<base::scoped_refptr<AudioBuffer>> buffers_;
  int front_offset_{0};   // frames already consumed within buffers_.front()
  int frames_{0};         // cached total, so frames() stays O(1)
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_AUDIO_FRAME_QUEUE_H_
