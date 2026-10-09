// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The audio half of ffmpeg_transcode_streams: turning decoded AVFrames into
// encoder-sized AudioBuffers. This holds AudioState's destructor, the frame
// conversion (AvFrameToAudioBuffer), and the resampler + AvAudioFifo framing
// (ScratchFrame / EnsureResampler / DrainWholeFrames / FramesForEncoder /
// FlushResampler). Split from ffmpeg_transcode_streams.cc as a line-count
// seam (C1): the audio resampling is a self-contained concern, and pulling it
// out keeps the stream-preparation helpers (PrepareAudioStream /
// PrepareVideoStream) and the timestamp rebase in the parent TU.

#include "media/filters/ffmpeg_transcode_streams.h"

#include <cstring>
#include <vector>

#include "base/logging.h"
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"

namespace avbase::media {
namespace {

// (Re)allocates |slot| as a planar-float scratch frame of |nb_samples|.
AVFrame* ScratchFrame(AvFramePtr& slot, int nb_samples, int rate,
                      int channels) {
  if (!slot) {
    slot = AvFramePtr(av_frame_alloc());
  }
  AVFrame* f = slot.get();
  av_frame_unref(f);
  f->format = AV_SAMPLE_FMT_FLTP;
  f->sample_rate = rate;
  av_channel_layout_default(&f->ch_layout, channels);
  f->nb_samples = nb_samples;
  if (av_frame_get_buffer(f, 0) < 0) {
    LOG(ERROR) << "transcode: scratch frame allocation failed";
    return nullptr;
  }
  return f;
}

bool EnsureResampler(AudioState* st, AVFrame* frame) {
  if (st->swr) {
    return true;
  }
  AVChannelLayout in_layout{};
  AVChannelLayout out_layout{};
  av_channel_layout_default(&in_layout, st->in_channels);
  av_channel_layout_default(&out_layout, st->out_channels);
  SwrContext* s = nullptr;
  if (swr_alloc_set_opts2(&s, &out_layout, AV_SAMPLE_FMT_FLTP,
                          st->out_sample_rate, &in_layout,
                          static_cast<AVSampleFormat>(frame->format),
                          st->in_sample_rate, 0, nullptr) < 0 ||
      !s || swr_init(s) < 0) {
    if (s) {
      swr_free(&s);
    }
    LOG(ERROR) << "transcode: cannot create resampler " << st->in_sample_rate
               << "Hz/" << st->in_channels << "ch -> " << st->out_sample_rate
               << "Hz/" << st->out_channels << "ch";
    return false;
  }
  st->swr = s;
  return true;
}

void DrainWholeFrames(AudioState* st, int frame_size,
                      std::vector<base::scoped_refptr<AudioBuffer>>* out) {
  while (av_audio_fifo_size(st->fifo) >= frame_size) {
    AVFrame* f = ScratchFrame(st->framed, frame_size, st->out_sample_rate,
                              st->out_channels);
    if (!f) {
      return;
    }
    if (av_audio_fifo_read(st->fifo, reinterpret_cast<void**>(f->data),
                           frame_size) < frame_size) {
      return;
    }
    auto buf = AvFrameToAudioBuffer(f, st->out_sample_rate, st->out_channels);
    if (buf) {
      out->push_back(std::move(buf));
    }
  }
}

}  // namespace

AudioState::~AudioState() {
  if (swr) {
    swr_free(&swr);
  }
  if (fifo) {
    av_audio_fifo_free(fifo);
    fifo = nullptr;
  }
  resampled.reset();
  framed.reset();
}

base::scoped_refptr<AudioBuffer>
AvFrameToAudioBuffer(AVFrame* frame, int sample_rate, int channels) {
  const int nb_samples = frame->nb_samples;
  if (nb_samples <= 0 || sample_rate <= 0 || channels <= 0) {
    return nullptr;
  }
  // Determine if the frame is planar.
  const bool planar =
      av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format));
  // Build interleaved float data.
  std::vector<float> float_data(static_cast<size_t>(nb_samples) *
                                static_cast<size_t>(channels));
  const float* src_channels[AV_NUM_DATA_POINTERS] = {};
  for (int ch = 0; ch < channels && ch < AV_NUM_DATA_POINTERS; ++ch) {
    src_channels[ch] = reinterpret_cast<const float*>(planar ? frame->data[ch]
                                                             : frame->data[0]);
  }
  const size_t channels_sz = static_cast<size_t>(channels);
  const size_t nb_samples_sz = static_cast<size_t>(nb_samples);
  if (planar) {
    // Source is planar float — de-interleave into interleaved.
    for (int ch = 0; ch < channels; ++ch) {
      const size_t ch_sz = static_cast<size_t>(ch);
      for (int s = 0; s < nb_samples; ++s) {
        float_data[static_cast<size_t>(s) * channels_sz + ch_sz] =
            src_channels[ch][s];
      }
    }
  } else {
    // Source is packed float — already interleaved.
    std::memcpy(float_data.data(), frame->data[0],
                float_data.size() * sizeof(float));
  }
  // Build planar float data (F32P) for AudioBuffer.
  std::vector<uint8_t> planar_data(nb_samples_sz * channels_sz * sizeof(float));
  auto* planes = reinterpret_cast<float*>(planar_data.data());
  for (int ch = 0; ch < channels; ++ch) {
    const size_t ch_sz = static_cast<size_t>(ch);
    for (int s = 0; s < nb_samples; ++s) {
      planes[ch_sz * nb_samples_sz + static_cast<size_t>(s)] =
          float_data[static_cast<size_t>(s) * channels_sz + ch_sz];
    }
  }
  const ChannelLayout layout =
      channels == 1 ? ChannelLayout::kMono : ChannelLayout::kStereo;
  const base::TimeDelta timestamp =
      base::SecondsD(static_cast<double>(frame->pts) / sample_rate);
  const base::TimeDelta dur =
      base::SecondsD(static_cast<double>(nb_samples) / sample_rate);
  // Encode the channel count in the data layout. AudioBuffer::Create takes
  // the raw planar data.
  return AudioBuffer::Create(SampleFormat::kF32P, layout, channels, sample_rate,
                             nb_samples, timestamp, dur, 0,
                             std::move(planar_data));
}

std::vector<base::scoped_refptr<AudioBuffer>> FramesForEncoder(  // NOLINT
    AudioState* st, AVFrame* frame, int frame_size) {
  std::vector<base::scoped_refptr<AudioBuffer>> out;
  const bool needs_conversion = st->in_sample_rate != st->out_sample_rate ||
                                st->in_channels != st->out_channels;
  if (!needs_conversion || frame_size <= 0) {
    auto buf =
        AvFrameToAudioBuffer(frame, st->out_sample_rate, st->out_channels);
    if (buf) {
      out.push_back(std::move(buf));
    }
    return out;
  }
  if (st->in_sample_rate <= 0 || st->out_sample_rate <= 0 ||
      st->in_channels <= 0 || st->out_channels <= 0 || frame->nb_samples <= 0) {
    return out;
  }
  if (!EnsureResampler(st, frame)) {
    return out;
  }
  if (!st->fifo) {
    st->fifo =
        av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, st->out_channels, frame_size);
    if (!st->fifo) {
      LOG(ERROR) << "transcode: resampler FIFO allocation failed";
      return out;
    }
  }

  // How many output samples this input can turn into: the frame plus
  // whatever the resampler is still holding. The delay must be asked for in
  // INPUT-rate units — passing the output rate makes |capacity| too small
  // and swr_convert() writes past the end of the frame.
  const int64_t delay = swr_get_delay(st->swr, st->in_sample_rate);
  const int capacity = static_cast<int>(
      av_rescale_rnd(delay + frame->nb_samples, st->out_sample_rate,
                     st->in_sample_rate, AV_ROUND_UP));
  if (capacity <= 0) {
    return out;
  }
  AVFrame* scratch = ScratchFrame(st->resampled, capacity, st->out_sample_rate,
                                  st->out_channels);
  if (!scratch) {
    return out;
  }
  const int produced =
      swr_convert(st->swr, scratch->data, capacity,
                  const_cast<const uint8_t**>(frame->data), frame->nb_samples);
  if (produced < 0) {
    LOG(ERROR) << "transcode: resampling failed";
    return out;
  }
  if (produced > 0 &&
      av_audio_fifo_write(st->fifo, reinterpret_cast<void**>(scratch->data),
                          produced) < produced) {
    LOG(ERROR) << "transcode: resampler FIFO overflow";
    return out;
  }
  DrainWholeFrames(st, frame_size, &out);
  return out;
}

std::vector<base::scoped_refptr<AudioBuffer>> FlushResampler(AudioState* st,
                                                             int frame_size) {
  std::vector<base::scoped_refptr<AudioBuffer>> out;
  if (!st->fifo || frame_size <= 0) {
    return out;
  }
  if (st->swr) {
    // Push a null input to make the resampler give up what it is holding.
    for (;;) {
      AVFrame* scratch = ScratchFrame(st->resampled, frame_size,
                                      st->out_sample_rate, st->out_channels);
      if (!scratch) {
        break;
      }
      const int produced =
          swr_convert(st->swr, scratch->data, frame_size, nullptr, 0);
      if (produced <= 0) {
        break;
      }
      av_audio_fifo_write(st->fifo, reinterpret_cast<void**>(scratch->data),
                          produced);
    }
  }
  DrainWholeFrames(st, frame_size, &out);
  // Whatever is left is the tail; the encoder accepts a short LAST frame.
  const int left = av_audio_fifo_size(st->fifo);
  if (left > 0) {
    AVFrame* f = ScratchFrame(st->framed, frame_size, st->out_sample_rate,
                              st->out_channels);
    if (f) {
      const int got =
          av_audio_fifo_read(st->fifo, reinterpret_cast<void**>(f->data), left);
      if (got > 0) {
        f->nb_samples = got;
        auto buf =
            AvFrameToAudioBuffer(f, st->out_sample_rate, st->out_channels);
        if (buf) {
          out.push_back(std::move(buf));
        }
      }
    }
  }
  return out;
}

}  // namespace avbase::media
