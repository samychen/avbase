// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/audio_convert.h"

#include <algorithm>

#include "base/check.h"
#include "base/logging.h"

namespace avbase::media::ffmpeg {

namespace {

// Allocates an output frame of |nb_samples| in the converter's output
// geometry. Returns null on allocation failure (a real OOM, not a resampler
// hiccup -- MakeFrame itself CHECKs, so this only guards get_buffer).
FramePtr MakeOutFrame(int nb_samples, int rate, int channels,
                      AVSampleFormat fmt) {
  FramePtr f = MakeFrame();
  f->format = fmt;
  f->sample_rate = rate;
  av_channel_layout_default(&f->ch_layout, channels);
  f->nb_samples = nb_samples;
  if (av_frame_get_buffer(f.get(), 0) < 0) {
    LOG(ERROR) << "audio_convert: output frame allocation failed";
    return FramePtr();
  }
  return f;
}

}  // namespace

uint64_t AudioConverter::LayoutMask(int channels) {
  AVChannelLayout layout{};
  av_channel_layout_default(&layout, channels);
  const uint64_t mask = layout.u.mask;
  av_channel_layout_uninit(&layout);
  return mask;
}

AudioConverter::AudioConverter() = default;
AudioConverter::~AudioConverter() = default;

bool AudioConverter::Configure(int in_rate, int in_channels,
                               AVSampleFormat in_fmt, int out_rate,
                               int out_channels, AVSampleFormat out_fmt) {
  if (swr_ && in_rate == in_rate_ && in_channels == in_channels_ &&
      in_fmt == in_fmt_ && out_rate == out_rate_ &&
      out_channels == out_channels_ && out_fmt == out_fmt_) {
    return true;  // Unchanged geometry: reuse the existing context.
  }

  const uint64_t in_mask = LayoutMask(in_channels);
  const uint64_t out_mask = LayoutMask(out_channels);
  SwrPtr ctx =
      MakeSwrContext(out_fmt, out_mask, out_rate, in_fmt, in_mask, in_rate);
  if (!ctx) {
    LOG(ERROR) << "audio_convert: cannot create resampler " << in_rate << "Hz/"
               << in_channels << "ch -> " << out_rate << "Hz/" << out_channels
               << "ch";
    swr_.reset();
    return false;
  }
  if (swr_init(ctx.get()) < 0) {
    LOG(ERROR) << "audio_convert: swr_init failed";
    return false;
  }

  swr_ = std::move(ctx);
  in_rate_ = in_rate;
  in_channels_ = in_channels;
  in_fmt_ = in_fmt;
  out_rate_ = out_rate;
  out_channels_ = out_channels;
  out_fmt_ = out_fmt;
  return true;
}

void AudioConverter::Reset() {
  swr_.reset();
  in_rate_ = in_channels_ = out_rate_ = out_channels_ = 0;
  in_fmt_ = out_fmt_ = AV_SAMPLE_FMT_NONE;
}

std::vector<FramePtr> AudioConverter::Push(const AVFrame& src) {
  std::vector<FramePtr> out;
  if (!swr_) {
    return out;
  }
  // Output sample count this input can yield: the input plus whatever the
  // resampler is still holding (asked in INPUT-rate units -- asking in the
  // output rate makes the capacity too small and swr_convert() writes past
  // the end of the frame).
  const int64_t delay = swr_get_delay(swr_.get(), in_rate_);
  const int capacity = static_cast<int>(
      av_rescale_rnd(delay + src.nb_samples, out_rate_, in_rate_, AV_ROUND_UP));
  if (capacity <= 0) {
    return out;
  }
  FramePtr f = MakeOutFrame(capacity, out_rate_, out_channels_, out_fmt_);
  if (!f) {
    return out;
  }
  const int produced =
      swr_convert(swr_.get(), f->data, capacity,
                  const_cast<const uint8_t**>(src.data), src.nb_samples);
  if (produced < 0) {
    LOG(ERROR) << "audio_convert: resampling failed";
    return out;
  }
  f->nb_samples = produced;
  out.push_back(std::move(f));
  return out;
}

std::vector<FramePtr> AudioConverter::Flush() {
  std::vector<FramePtr> out;
  if (!swr_) {
    return out;
  }
  // Chunk size only bounds per-iteration work; the loop drains fully
  // regardless of its value. ~40ms at the output rate.
  const int chunk = std::max(1024, out_rate_ / 25);
  for (;;) {
    FramePtr f = MakeOutFrame(chunk, out_rate_, out_channels_, out_fmt_);
    if (!f) {
      break;
    }
    const int produced = swr_convert(swr_.get(), f->data, chunk, nullptr, 0);
    if (produced <= 0) {
      break;
    }
    f->nb_samples = produced;
    out.push_back(std::move(f));
  }
  return out;
}

}  // namespace avbase::media::ffmpeg
