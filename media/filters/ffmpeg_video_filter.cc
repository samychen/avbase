// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_video_filter.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "base/logging.h"
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"
#include "platform/ffmpeg/video_convert.h"

namespace avbase::media {

struct FFmpegVideoFilter::Context {
  AVFilterGraph* graph = nullptr;
  AVFilterContext* src = nullptr;
  AVFilterContext* sink = nullptr;
  VideoFormat format = VideoFormat::kUnknown;
  Size coded_size{0, 0};
};

FFmpegVideoFilter::FFmpegVideoFilter() = default;

FFmpegVideoFilter::~FFmpegVideoFilter() {
  if (ctx_) {
    avfilter_graph_free(&ctx_->graph);
  }
}

bool FFmpegVideoFilter::Initialize(const std::string& graph, VideoFormat format,
                                   const Size& coded_size) {
  const AVPixelFormat pix =
      platform::ffmpeg::AvPixelFormatFromVideoFormat(format);
  if (pix == AV_PIX_FMT_NONE || coded_size.width <= 0 ||
      coded_size.height <= 0) {
    LOG(ERROR) << "video filter: unsupported frame format "
               << static_cast<int>(format);
    return false;
  }
  const AVFilter* buffer = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");
  if (!buffer || !buffersink) {
    LOG(ERROR) << "video filter: avfilter buffer endpoints missing from "
                  "this FFmpeg build";
    return false;
  }
  auto c = std::make_unique<Context>();
  c->graph = avfilter_graph_alloc();
  if (!c->graph) {
    return false;
  }
  c->format = format;
  c->coded_size = coded_size;

  const AVRational sar = {1, 1};
  const AVRational frame_rate = {30, 1};
  const std::string args = "video_size=" + std::to_string(coded_size.width) +
                           "x" + std::to_string(coded_size.height) +
                           ":pix_fmt=" + std::to_string(static_cast<int>(pix)) +
                           ":time_base=1/90000"
                           ":pixel_aspect=" +
                           std::to_string(sar.num) + "/" +
                           std::to_string(sar.den) +
                           ":frame_rate=" + std::to_string(frame_rate.num) +
                           "/" + std::to_string(frame_rate.den);
  if (avfilter_graph_create_filter(&c->src, buffer, "in", args.c_str(), nullptr,
                                   c->graph) < 0) {
    LOG(ERROR) << "video filter: buffer create failed";
    avfilter_graph_free(&c->graph);
    return false;
  }
  if (avfilter_graph_create_filter(&c->sink, buffersink, "out", nullptr,
                                   nullptr, c->graph) < 0) {
    LOG(ERROR) << "video filter: buffersink create failed";
    avfilter_graph_free(&c->graph);
    return false;
  }
  // Pin the output to the input format: the stage's contract is
  // same-format-in, same-format-out, so the compositor sees no surprises.
  const AVPixelFormat out_pixs[] = {pix, AV_PIX_FMT_NONE};
  if (av_opt_set_int_list(c->sink, "pix_fmts", out_pixs, AV_PIX_FMT_NONE,
                          AV_OPT_SEARCH_CHILDREN) < 0) {
    LOG(ERROR) << "video filter: sink pix_fmt constraint failed";
    avfilter_graph_free(&c->graph);
    return false;
  }

  AVFilterInOut* user_in = nullptr;
  AVFilterInOut* user_out = nullptr;
  if (avfilter_graph_parse_ptr(c->graph, graph.c_str(), &user_in, &user_out,
                               nullptr) < 0) {
    LOG(ERROR) << "video filter: graph \"" << graph << "\" failed to parse";
    avfilter_graph_free(&c->graph);
    return false;
  }
  bool linked = false;
  if (user_in && user_out) {
    linked = avfilter_link(c->src, 0, user_in->filter_ctx,
                           static_cast<unsigned>(user_in->pad_idx)) >= 0 &&
             avfilter_link(user_out->filter_ctx,
                           static_cast<unsigned>(user_out->pad_idx), c->sink,
                           0) >= 0;
  }
  avfilter_inout_free(&user_in);
  avfilter_inout_free(&user_out);
  if (!linked) {
    LOG(ERROR) << "video filter: graph \"" << graph
               << "\" has no single open input and output pad";
    avfilter_graph_free(&c->graph);
    return false;
  }
  if (avfilter_graph_config(c->graph, nullptr) < 0) {
    LOG(ERROR) << "video filter: graph \"" << graph << "\" failed to configure";
    avfilter_graph_free(&c->graph);
    return false;
  }
  ctx_ = std::move(c);
  return true;
}

const Size& FFmpegVideoFilter::CodedSize() const {
  return ctx_->coded_size;
}

bool FFmpegVideoFilter::Process(base::scoped_refptr<VideoFrame> in,
                                base::scoped_refptr<VideoFrame>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  *out = nullptr;
  if (!in) {
    return true;
  }
  if (in->format() != ctx_->format || in->coded_size() != ctx_->coded_size) {
    LOG(ERROR) << "video filter: frame geometry moved under the filter";
    return false;
  }

  // Push a view over the frame's planes (not refcounted; buffersrc copies).
  AVFrame* frame = av_frame_alloc();
  frame->format = platform::ffmpeg::AvPixelFormatFromVideoFormat(ctx_->format);
  frame->width = ctx_->coded_size.width;
  frame->height = ctx_->coded_size.height;
  frame->pts = in->timestamp().InMicroseconds();
  const int planes = media::VideoFormatPlaneCount(ctx_->format);
  for (int p = 0; p < planes; ++p) {
    const auto plane = static_cast<VideoFrame::Plane>(p);
    frame->data[p] = const_cast<uint8_t*>(in->visible_data(plane).data());
    frame->linesize[p] = in->stride(plane);
  }
  if (av_buffersrc_add_frame_flags(ctx_->src, frame, 0) < 0) {
    av_frame_free(&frame);
    LOG(ERROR) << "video filter: buffersrc rejected a frame";
    return false;
  }
  av_frame_free(&frame);

  AVFrame* out_frame = av_frame_alloc();
  const int got = av_buffersink_get_frame(ctx_->sink, out_frame);
  if (got < 0) {
    av_frame_free(&out_frame);
    return true;  // EAGAIN: no output this round.
  }

  auto result = VideoFrame::CreateBlackFrame(
      ctx_->format, ctx_->coded_size, ctx_->coded_size, Rational{1, 1},
      in->timestamp(), in->duration(), in->serial());
  if (result) {
    const int out_planes = VideoFormatPlaneCount(ctx_->format);
    for (int p = 0; p < out_planes; ++p) {
      const auto plane = static_cast<VideoFrame::Plane>(p);
      const auto dst_span = result->mutable_data(plane);
      const int rows =
          p == 0 ? ctx_->coded_size.height : ctx_->coded_size.height / 2;
      for (int y = 0; y < rows; ++y) {
        std::memcpy(
            dst_span.data() + y * result->stride(plane),
            out_frame->data[p] + y * out_frame->linesize[p],
            static_cast<size_t>(out_frame->linesize[p] < result->stride(plane)
                                    ? out_frame->linesize[p]
                                    : result->stride(plane)));
      }
    }
    result->set_color_space(in->color_space());
    *out = std::move(result);
  }
  av_frame_free(&out_frame);
  return true;
}

}  // namespace avbase::media
