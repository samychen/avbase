// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The audio half of ffmpeg_transcode_streams: turning decoded AVFrames into
// encoder-sized AudioBuffers. This holds AudioState's destructor, the frame
// conversion (AvFrameToAudioBuffer), and the Swr resampler + AvAudioFifo
// framing (ScratchFrame / DrainWholeFrames / FramesForEncoder /
// FlushResampler). The Swr resampling itself now lives in
// media/ffmpeg/audio_convert.h (AudioConverter), mirroring the video side's
// VideoConverter: this TU only configures the resampler lazily and chops its
// variable-length output into encoder-sized whole frames. Split from
// ffmpeg_transcode_streams.cc as a line-count seam (C1): the audio resampling
// is a self-contained concern, and pulling it out keeps the stream-preparation
// helpers (PrepareAudioStream / PrepareVideoStream) and the timestamp rebase
// in the parent TU.

#include "media/transcode/ffmpeg_transcode_streams.h"

#include <cstring>
#include <vector>

#include "base/check.h"
#include "base/logging.h"
#include "media/ffmpeg/audio_convert.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media {
namespace {

// (Re)allocates |*slot| as a planar-float scratch frame of |nb_samples|.
AVFrame* ScratchFrame(AvFramePtr* slot, int nb_samples, int rate,
                      int channels) {
  if (!*slot) {
    // This TU keeps its own AvFramePtr (transcode_streams.h stays free of
    // libav types), so ff::MakeFrame's type doesn't fit here -- the checked
    // allocation does: CHECK, never null (M10's deeper-fix convention).
    AVFrame* raw = av_frame_alloc();
    CHECK(raw) << "avbase.transcode: av_frame_alloc failed (OOM)";
    *slot = AvFramePtr(raw);
  }
  AVFrame* f = slot->get();
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

void DrainWholeFrames(AudioState* st, int frame_size,
                      std::vector<base::scoped_refptr<AudioBuffer>>* out) {
  while (av_audio_fifo_size(st->fifo) >= frame_size) {
    AVFrame* f = ScratchFrame(&st->framed, frame_size, st->out_sample_rate,
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
  if (fifo) {
    av_audio_fifo_free(fifo);
    fifo = nullptr;
  }
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
  // Lazily create and configure the resampler (media/ffmpeg/audio_convert.h),
  // replacing the old inline SwrContext setup. It is only built when decoder
  // and encoder disagree on rate/channels -- matching the prior EnsureResampler
  // guard, but now reusing the ff::MakeSwrContext factory (Round46 convention).
  if (!st->resampler) {
    st->resampler = std::make_unique<ffmpeg::AudioConverter>();
  }
  if (!st->resampler->Configure(st->in_sample_rate, st->in_channels,
                                static_cast<AVSampleFormat>(frame->format),
                                st->out_sample_rate, st->out_channels,
                                AV_SAMPLE_FMT_FLTP)) {
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
  // Push() may return zero, one, or several output frames; each is already in
  // the output geometry (FLTP / out_rate / out_channels), so it goes straight
  // into the FIFO.
  auto converted = st->resampler->Push(*frame);
  for (auto& f : converted) {
    if (av_audio_fifo_write(st->fifo, reinterpret_cast<void**>(f->data),
                            f->nb_samples) < f->nb_samples) {
      LOG(ERROR) << "transcode: resampler FIFO overflow";
      return out;
    }
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
  if (st->resampler) {
    // Flush() feeds the resampler a null input and returns the trailing
    // frames; they join the FIFO so DrainWholeFrames can emit whole frames.
    auto tail = st->resampler->Flush();
    for (auto& f : tail) {
      av_audio_fifo_write(st->fifo, reinterpret_cast<void**>(f->data),
                          f->nb_samples);
    }
  }
  DrainWholeFrames(st, frame_size, &out);
  // Whatever is left is the tail; the encoder accepts a short LAST frame.
  const int left = av_audio_fifo_size(st->fifo);
  if (left > 0) {
    AVFrame* f = ScratchFrame(&st->framed, frame_size, st->out_sample_rate,
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
