// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round). Never compiled.
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.

#include "media/filters/audio_frame_queue.h"

#include <algorithm>
#include <utility>

#include "base/check.h"

namespace ijkpp::media {

AudioFrameQueue::AudioFrameQueue() = default;
AudioFrameQueue::~AudioFrameQueue() = default;

void AudioFrameQueue::Append(base::scoped_refptr<AudioBuffer> buffer) {
  if (!buffer) {
    return;
  }
  DCHECK(!buffer->end_of_stream());
  if (buffer->end_of_stream()) {
    return;
  }
  frames_ += buffer->frame_count();
  buffers_.push_back(std::move(buffer));
}

std::optional<base::TimeDelta> AudioFrameQueue::FrontTimestamp() const {
  if (buffers_.empty()) {
    return std::nullopt;
  }
  return buffers_.front()->timestamp();
}

void AudioFrameQueue::Clear() {
  buffers_.clear();
  front_offset_ = 0;
  frames_ = 0;
}

void AudioFrameQueue::SeekFrames(int n) {
  int remaining = n;
  while (remaining > 0 && !buffers_.empty()) {
    const int available = buffers_.front()->frame_count() - front_offset_;
    if (remaining >= available) {
      remaining -= available;
      frames_ -= available;
      buffers_.pop_front();
      front_offset_ = 0;
    } else {
      front_offset_ += remaining;
      frames_ -= remaining;
      remaining = 0;
    }
  }
  DCHECK_GE(frames_, 0);
}

int AudioFrameQueue::PeekFrames(int num_frames, int read_offset,
                                int write_offset, AudioBus* dest,
                                AudioBus* scratch) const {
  if (!dest || !scratch || num_frames <= 0) {
    return 0;
  }
  DCHECK_LE(num_frames, scratch->frames());
  DCHECK_EQ(dest->channels(), scratch->channels());
  int remaining = num_frames;
  int dst = write_offset;
  int skip = read_offset;
  for (size_t i = 0; i < buffers_.size() && remaining > 0; ++i) {
    const AudioBuffer* buffer = buffers_[i].get();
    int offset = (i == 0) ? front_offset_ : 0;
    int available = buffer->frame_count() - offset;
    if (skip >= available) {
      skip -= available;
      continue;
    }
    offset += skip;
    available -= skip;
    skip = 0;
    const int chunk = std::min(available, remaining);
    // ReadFrames() converts whatever sample format the decoder produced into
    // float planar. That is the single place the conversion happens, which is
    // the same reason ffplay's swr_convert was moved out of the audio callback
    // (Δ13): the device callback must copy, not convert.
    const int converted = buffer->ReadFrames(chunk, offset, scratch);
    if (converted < chunk) {
      // ReadFrames can legitimately return fewer frames than asked (a truncated
      // buffer, or a format it cannot convert). Counting `chunk` anyway would
      // report frames that were never produced, which is how a decoder bug
      // turns into an A/V-sync bug several layers up. Zero the rest and stop.
      for (int c = 0; c < dest->channels(); ++c) {
        float* out = dest->channel(c) + dst;
        for (int i = converted; i < chunk; ++i) {
          out[i] = 0.0f;
        }
      }
      dst += converted;
      remaining -= converted;
      break;
    }
    for (int c = 0; c < dest->channels(); ++c) {
      const float* src = scratch->channel(c);
      float* out = dest->channel(c) + dst;
      for (int n = 0; n < chunk; ++n) {
        out[n] = src[n];
      }
    }
    dst += chunk;
    remaining -= chunk;
  }
  // Zero the tail rather than leaving whatever was there. A stale tail is
  // inaudible most of the time and undebuggable when it is not.
  for (int c = 0; c < dest->channels(); ++c) {
    float* out = dest->channel(c) + dst;
    for (int n = 0; n < remaining; ++n) {
      out[n] = 0.0f;
    }
  }
  return num_frames - remaining;
}

int AudioFrameQueue::ReadFrames(int num_frames, int write_offset,
                               AudioBus* dest, AudioBus* scratch) {
  const int got = PeekFrames(num_frames, 0, write_offset, dest, scratch);
  SeekFrames(got);
  return got;
}

}  // namespace ijkpp::media
