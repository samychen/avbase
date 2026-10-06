// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_audio_filter.h"

#include <cstring>
#include <string>

#include "base/logging.h"
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"

namespace avbase::media {
namespace {

AVSampleFormat ToAvSampleFormat(SampleFormat format) {
  switch (format) {
  case SampleFormat::kU8:
    return AV_SAMPLE_FMT_U8;
  case SampleFormat::kS16:
    return AV_SAMPLE_FMT_S16;
  case SampleFormat::kS32:
    return AV_SAMPLE_FMT_S32;
  case SampleFormat::kF32:
    return AV_SAMPLE_FMT_FLT;
  case SampleFormat::kS16P:
    return AV_SAMPLE_FMT_S16P;
  case SampleFormat::kS32P:
    return AV_SAMPLE_FMT_S32P;
  case SampleFormat::kF32P:
    return AV_SAMPLE_FMT_FLTP;
  default:
    return AV_SAMPLE_FMT_NONE;
  }
}

const char* ToChannelLayoutName(int channels) {
  switch (channels) {
  case 1:
    return "mono";
  case 2:
    return "stereo";
  default:
    return nullptr;
  }
}

int BytesPerSample(SampleFormat format) {
  switch (format) {
  case SampleFormat::kU8:
    return 1;
  case SampleFormat::kS16:
  case SampleFormat::kS16P:
    return 2;
  case SampleFormat::kS32:
  case SampleFormat::kF32:
  case SampleFormat::kF32P:
    return 4;
  default:
    return 0;
  }
}

bool IsPlanar(SampleFormat format) {
  return format == SampleFormat::kS16P || format == SampleFormat::kS32P ||
         format == SampleFormat::kF32P;
}

}  // namespace

struct FFmpegAudioFilter::Context {
  AVFilterGraph* graph = nullptr;
  AVFilterContext* src = nullptr;
  AVFilterContext* sink = nullptr;
  int sample_rate = 0;
  SampleFormat format = SampleFormat::kUnknown;
  int channels = 0;
  ChannelLayout layout = ChannelLayout::kNone;
  // Output timestamp bookkeeping: the graph works in 1/sample_rate seconds.
  int64_t next_pts_samples = 0;
  bool drained = false;
};

FFmpegAudioFilter::FFmpegAudioFilter() = default;

FFmpegAudioFilter::~FFmpegAudioFilter() {
  if (ctx_) {
    avfilter_graph_free(&ctx_->graph);
  }
}

bool FFmpegAudioFilter::Initialize(const std::string& graph, int sample_rate,
                                   int channels, int frames_per_hint) {
  // avbase's audio decode output is always planar float (fmt=f32p from the
  // decoder logs); the filter stage inherits that contract, and constrains
  // the sink to the same format so every output buffer maps straight back
  // onto an AudioBuffer without a conversion hop. Graphs that change formats
  // must append an aresample+aformat tail themselves.
  constexpr SampleFormat kFormat = SampleFormat::kF32P;
  const char* layout_name = ToChannelLayoutName(channels);
  if (!layout_name) {
    LOG(ERROR) << "audio filter: only mono/stereo streams are supported (got "
               << channels << " channels)";
    return false;
  }
  const AVFilter* buffersrc = avfilter_get_by_name("abuffer");
  const AVFilter* buffersink = avfilter_get_by_name("abuffersink");
  if (!buffersrc || !buffersink) {
    LOG(ERROR) << "audio filter: avfilter buffer endpoints missing from "
                  "this FFmpeg build";
    return false;
  }
  auto c = std::make_unique<Context>();
  c->graph = avfilter_graph_alloc();
  if (!c->graph) {
    return false;
  }
  c->sample_rate = sample_rate;
  c->channels = channels;
  c->format = kFormat;
  c->layout = channels == 1 ? ChannelLayout::kMono : ChannelLayout::kStereo;

  const std::string args =
      "time_base=1/" + std::to_string(sample_rate) +
      ":sample_rate=" + std::to_string(sample_rate) + ":sample_fmt=" +
      av_get_sample_fmt_name(ToAvSampleFormat(kFormat)) +
      ":channel_layout=" + layout_name;
  if (avfilter_graph_create_filter(&c->src, buffersrc, "in", args.c_str(),
                                   nullptr, c->graph) < 0) {
    LOG(ERROR) << "audio filter: abuffer create failed";
    avfilter_graph_free(&c->graph);
    return false;
  }
  if (avfilter_graph_create_filter(&c->sink, buffersink, "out", nullptr,
                                   nullptr, c->graph) < 0) {
    LOG(ERROR) << "audio filter: abuffersink create failed";
    avfilter_graph_free(&c->graph);
    return false;
  }
  // Constrain the output to the input format.
  const AVSampleFormat out_fmts[] = {ToAvSampleFormat(kFormat),
                                     AV_SAMPLE_FMT_NONE};
  const int64_t out_rates[] = {sample_rate, -1};
  if (av_opt_set_int_list(c->sink, "sample_fmts", out_fmts,
                          AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN) < 0 ||
      av_opt_set_int_list(c->sink, "sample_rates", out_rates, -1,
                          AV_OPT_SEARCH_CHILDREN) < 0) {
    LOG(ERROR) << "audio filter: sink format constraints failed";
    avfilter_graph_free(&c->graph);
    return false;
  }
  // Parse the user graph and wire it between the buffer endpoints: parse_ptr
  // does not auto-connect open pads to the endpoints, the InOut lists carry
  // the graph's free input/output pads.
  AVFilterInOut* user_in = nullptr;
  AVFilterInOut* user_out = nullptr;
  if (avfilter_graph_parse_ptr(c->graph, graph.c_str(), &user_in, &user_out,
                               nullptr) < 0) {
    LOG(ERROR) << "audio filter: graph \"" << graph << "\" failed to parse";
    avfilter_graph_free(&c->graph);
    return false;
  }
  bool linked = false;
  if (user_in && user_out) {
    linked = avfilter_link(c->src, 0, user_in->filter_ctx,
                           user_in->pad_idx) >= 0 &&
             avfilter_link(user_out->filter_ctx, user_out->pad_idx, c->sink,
                           0) >= 0;
  }
  avfilter_inout_free(&user_in);
  avfilter_inout_free(&user_out);
  if (!linked) {
    LOG(ERROR) << "audio filter: graph \"" << graph
               << "\" has no single open input and output pad";
    avfilter_graph_free(&c->graph);
    return false;
  }
  if (avfilter_graph_config(c->graph, nullptr) < 0) {
    LOG(ERROR) << "audio filter: graph \"" << graph
               << "\" failed to configure";
    avfilter_graph_free(&c->graph);
    return false;
  }
  (void)frames_per_hint;  // The graph allocates its own buffers.
  ctx_ = std::move(c);
  return true;
}

bool FFmpegAudioFilter::Process(
    base::scoped_refptr<AudioBuffer> in,
    std::vector<base::scoped_refptr<AudioBuffer>>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  const int bytes = BytesPerSample(ctx_->format);
  const bool planar = IsPlanar(ctx_->format);
  if (in && in->end_of_stream()) {
    // Drain: send EOF once, pull everything the graph still holds.
    if (!ctx_->drained) {
      av_buffersrc_add_frame_flags(ctx_->src, nullptr, 0);
      ctx_->drained = true;
    }
    while (true) {
      AVFrame* frame = av_frame_alloc();
      if (av_buffersink_get_frame(ctx_->sink, frame) < 0) {
        av_frame_free(&frame);
        break;
      }
      ctx_->next_pts_samples += frame->nb_samples;
      av_frame_free(&frame);
    }
    out->push_back(AudioBuffer::CreateEOSBuffer());
    return true;
  }
  if (!in || ctx_->drained) {
    return false;
  }

  // Push a frame VIEW over the AudioBuffer's planes. No KEEP_REF: the frame
  // is not refcounted (the data lives in the AudioBuffer), so buffersrc
  // copies at the default flags.
  {
    AVFrame* frame = av_frame_alloc();
    frame->format = ToAvSampleFormat(ctx_->format);
    frame->sample_rate = ctx_->sample_rate;
    frame->nb_samples = in->frame_count();
    av_channel_layout_default(&frame->ch_layout, ctx_->channels);
    frame->pts = ctx_->next_pts_samples;
    for (int ch = 0; ch < ctx_->channels; ++ch) {
      frame->data[ch] = const_cast<uint8_t*>(in->channel_data(ch).data());
      if (!planar) {
        break;  // Packed formats live in channel 0 only.
      }
    }
    if (av_buffersrc_add_frame_flags(ctx_->src, frame, 0) < 0) {
      av_frame_free(&frame);
      LOG(ERROR) << "audio filter: buffersrc rejected a frame";
      return false;
    }
    av_frame_free(&frame);  // The graph copied what it needs.
  }

  // Pull everything the graph makes available this round.
  while (true) {
    AVFrame* out_frame = av_frame_alloc();
    const int got = av_buffersink_get_frame(ctx_->sink, out_frame);
    if (got < 0) {
      av_frame_free(&out_frame);
      break;  // EAGAIN: come back with the next input.
    }
    const int samples = out_frame->nb_samples;
    const size_t plane = static_cast<size_t>(samples) * bytes;
    std::vector<uint8_t> data(
        planar ? plane * static_cast<size_t>(ctx_->channels)
               : plane * static_cast<size_t>(ctx_->channels));
    if (planar) {
      for (int ch = 0; ch < ctx_->channels; ++ch) {
        std::memcpy(data.data() + ch * plane, out_frame->data[ch], plane);
      }
    } else {
      std::memcpy(data.data(), out_frame->data[0], plane * ctx_->channels);
    }
    const base::TimeDelta ts = base::SecondsD(
        static_cast<double>(ctx_->next_pts_samples) / ctx_->sample_rate);
    ctx_->next_pts_samples += samples;
    auto buffer = AudioBuffer::Create(
        ctx_->format, ctx_->layout, ctx_->channels, ctx_->sample_rate, samples,
        ts, base::SecondsD(static_cast<double>(samples) / ctx_->sample_rate),
        0, std::move(data));
    av_frame_free(&out_frame);
    if (!buffer) {
      av_frame_free(&out_frame);
      return false;
    }
    out->push_back(std::move(buffer));
  }
  return true;
}

}  // namespace avbase::media
