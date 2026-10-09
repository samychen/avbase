// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_transcode_job.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "media/filters/ffmpeg_transcode_streams.h"
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::platform::ffmpeg;

// AVIO interrupt callback: a non-zero return aborts the blocking read in
// progress. This is what makes Cancel() prompt on a stalled source -- without
// it the token would only be noticed once av_read_frame returned on its own.
int InterruptTranscode(void* opaque) {
  const auto* token = static_cast<const TranscodeCancelToken*>(opaque);
  return (token && token->IsCancelled()) ? 1 : 0;
}

Status CancelledStatus(const std::string& input_uri) {
  return Err(ErrorCode::kCancelled, "transcode cancelled", input_uri,
             "the caller cancelled the job; any partial output is left in "
             "place");
}

}  // namespace

// The main transcode loop: read → decode → encode → write, paced by nothing
// (there is no clock offline). Per-stream open/encoder setup lives in
// ffmpeg_transcode_streams.cc; this file owns the pump.
Status Transcode(const std::string& input_uri, const TranscodeParams& params,
                 TranscodeProgressCB progress_cb,
                 const TranscodeCancelToken* cancel) {
  const auto cancelled = [cancel]() {
    return cancel && cancel->IsCancelled();
  };

  // --- Open input ---
  // Allocate the context here rather than letting avformat_open_input do it,
  // so the interrupt callback is installed BEFORE the open: opening a URL can
  // block, and that is exactly the moment a cancel has to be able to land.
  AVFormatContext* raw_in = avformat_alloc_context();
  if (!raw_in) {
    return Err(ErrorCode::kOutOfMemory, "cannot allocate an input context",
               input_uri, "out of memory");
  }
  if (cancel) {
    raw_in->interrupt_callback.callback = &InterruptTranscode;
    // Read-only on our side; FFmpeg just wants a void* to hand back.
    raw_in->interrupt_callback.opaque =
        const_cast<TranscodeCancelToken*>(cancel);
  }
  if (avformat_open_input(&raw_in, input_uri.c_str(), nullptr, nullptr) < 0 ||
      !raw_in) {
    // Most failure paths free the context (and null the pointer), but not
    // every one does -- close it if it survived.
    if (raw_in) {
      avformat_close_input(&raw_in);
    }
    if (cancelled()) {
      return CancelledStatus(input_uri);
    }
    return Err(ErrorCode::kSourceOpenFailed, "cannot open input", input_uri,
               "check the source file exists and is readable");
  }
  ff::FormatCtxPtr in_ctx(raw_in);
  if (avformat_find_stream_info(in_ctx.get(), nullptr) < 0) {
    return Err(ErrorCode::kSourceOpenFailed, "cannot probe input", input_uri,
               "the file may be corrupt or unsupported");
  }

  // Total duration for progress.
  const double total_duration =
      in_ctx->duration > 0
          ? static_cast<double>(in_ctx->duration) / AV_TIME_BASE
          : 0.0;

  // --- Seek if trim ---
  if (params.start_time_seconds > 0.0) {
    const int64_t seek_target =
        static_cast<int64_t>(params.start_time_seconds * AV_TIME_BASE);
    if (av_seek_frame(in_ctx.get(), -1, seek_target, AVSEEK_FLAG_BACKWARD) <
        0) {
      LOG(WARNING) << "transcode: seek to " << params.start_time_seconds
                   << "s failed, " << "transcoding from start";
    }
  }

  // --- Find input streams ---
  int audio_stream_idx = -1;
  int video_stream_idx = -1;
  for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
    const AVMediaType type = in_ctx->streams[i]->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_AUDIO && audio_stream_idx < 0) {
      audio_stream_idx = static_cast<int>(i);
    } else if (type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0) {
      video_stream_idx = static_cast<int>(i);
    }
  }

  // --- Decide which streams are active ---
  const bool want_audio =
      !params.audio_codec.codec.empty() && audio_stream_idx >= 0;
  const bool want_video =
      !params.video_codec.codec.empty() && video_stream_idx >= 0;

  if (!want_audio && !want_video) {
    return Err(ErrorCode::kInvalidArgument, "no streams to transcode",
               "input has no audio/video or output specs are empty",
               "check the input file and codec settings");
  }

  // --- Initialize output muxer ---
  FFmpegEncodeMuxer muxer;
  if (!muxer.Open(params.output_path, params.muxer_options)) {
    return Err(ErrorCode::kSourceOpenFailed, "cannot open output",
               params.output_path, "check the extension is supported");
  }

  AudioState audio;
  if (want_audio) {
    const Status st = PrepareAudioStream(in_ctx.get(), audio_stream_idx, params,
                                         &muxer, &audio);
    if (!st) {
      return st;
    }
  }

  VideoState video;
  if (want_video) {
    const Status st = PrepareVideoStream(in_ctx.get(), video_stream_idx, params,
                                         &muxer, &video);
    if (!st) {
      return st;
    }
  }

  // Trim start (-ss) has to move the OUTPUT timeline to zero, not merely
  // stop writing earlier frames. Before this existed, trimming 200ms off a
  // 426ms file produced output whose reported duration looked correct
  // (234ms) while the audio actually began 192ms in — two thirds of dead
  // air that no duration-based check can see. The re-encode path needs the
  // same treatment because the AAC encoder's own timeline starts at its
  // priming sample (-1024).
  bool audio_seen = false;
  bool video_seen = false;
  int64_t audio_origin = 0;
  int64_t video_origin = 0;

  // Every packet written by this job goes through |emit|: it shifts onto
  // the output timeline and it CHECKS the muxer's result. An unchecked
  // WritePacket is how a lost frame stays invisible — the same defect cost
  // concat_job.cc one frame per seam.
  auto emit = [&muxer](int idx, const EncodedPacket& src, bool* seen,
                       int64_t* origin) -> bool {
    TimelineOrigin(seen, origin, src.pts);
    EncodedPacket ep = src;
    ep.pts = ShiftTs(ep.pts, *origin);
    ep.dts = ShiftTs(ep.dts, *origin);
    if (!muxer.WritePacket(idx, ep)) {
      LOG(ERROR) << "transcode: muxer rejected packet stream=" << idx
                 << " pts=" << ep.pts << " dts=" << ep.dts;
      return false;
    }
    return true;
  };
  auto write_audio = [&](const EncodedPacket& ep) {
    return emit(audio.muxer_stream_index, ep, &audio_seen, &audio_origin);
  };
  auto write_video = [&](const EncodedPacket& ep) {
    return emit(video.muxer_stream_index, ep, &video_seen, &video_origin);
  };
  auto write_failed = [&params](const char* what) {
    return Err(ErrorCode::kInvalidState,
               std::string("transcode: writing ") + what + " failed",
               params.output_path,
               "the muxer rejected the packet; check the output container");
  };

  // --- Transcode loop ---
  ff::PacketPtr pkt(av_packet_alloc());
  ff::FramePtr frame(av_frame_alloc());
  int64_t processed_us = 0;
  const int64_t trim_end_us =
      params.duration_seconds > 0.0
          ? static_cast<int64_t>(
                (params.start_time_seconds + params.duration_seconds) *
                AV_TIME_BASE)
          : INT64_MAX;

  while (!cancelled() && av_read_frame(in_ctx.get(), pkt.get()) >= 0) {
    const int idx = pkt->stream_index;
    AVStream* stream = in_ctx->streams[static_cast<unsigned>(idx)];

    // Check trim end.
    const int64_t pkt_us =
        pkt->pts != AV_NOPTS_VALUE
            ? av_rescale_q(pkt->pts, stream->time_base, {1, AV_TIME_BASE})
            : processed_us;
    if (pkt_us > trim_end_us) {
      av_packet_unref(pkt.get());
      break;
    }
    processed_us = std::max(processed_us, pkt_us);

    if (idx == audio_stream_idx && want_audio) {
      if (audio.copy) {
        // Stream copy: re-timestamp and write directly. The muxer rejects
        // anything whose dts runs backwards, hence |emit|'s check.
        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + static_cast<size_t>(pkt->size));
        ep.pts = pkt->pts;
        ep.dts = pkt->dts;
        ep.duration = pkt->duration;
        ep.flags = pkt->flags;
        // The container's units are not the muxer's units: mp4 audio uses
        // 1/48000, mkv 1/1000, adts 1/28224000. Without this the same
        // payload comes out 48× too short or 589× too long.
        RescaleTimestamps(&ep, audio.in_tb_num, audio.in_tb_den,
                          audio.out_tb_num, audio.out_tb_den);
        if (!write_audio(ep)) {
          return write_failed("an audio copy packet");
        }
      } else {
        // Decode → encode.
        if (avcodec_send_packet(audio.decoder.get(), pkt.get()) < 0) {
          LOG(WARNING) << "transcode: audio send_packet failed, skipping";
          av_packet_unref(pkt.get());
          continue;
        }
        while (true) {
          const int ret =
              avcodec_receive_frame(audio.decoder.get(), frame.get());
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
          }
          if (ret < 0) {
            LOG(WARNING) << "transcode: audio decode failed";
            break;
          }
          // Resample into the encoder's geometry when the two disagree.
          // This yields zero or more whole codec frames, so the loop is over
          // the buffers and not over decoded frames.
          for (auto& buf : FramesForEncoder(&audio, frame.get(),
                                            audio.encoder.frame_size())) {
            std::vector<EncodedPacket> out_packets;
            if (audio.encoder.Encode(std::move(buf), &out_packets)) {
              for (const auto& ep : out_packets) {
                if (!write_audio(ep)) {
                  return write_failed("a re-encoded audio packet");
                }
              }
            }
          }
          av_frame_unref(frame.get());
        }
      }
    } else if (idx == video_stream_idx && want_video) {
      if (video.copy) {
        EncodedPacket ep;
        ep.data.assign(pkt->data, pkt->data + static_cast<size_t>(pkt->size));
        ep.pts = pkt->pts;
        ep.dts = pkt->dts;
        ep.duration = pkt->duration;
        ep.flags = pkt->flags;
        RescaleTimestamps(&ep, video.in_tb_num, video.in_tb_den,
                          video.out_tb_num, video.out_tb_den);
        if (!write_video(ep)) {
          return write_failed("a video copy packet");
        }
      } else {
        if (avcodec_send_packet(video.decoder.get(), pkt.get()) < 0) {
          LOG(WARNING) << "transcode: video send_packet failed, skipping";
          av_packet_unref(pkt.get());
          continue;
        }
        while (true) {
          const int ret =
              avcodec_receive_frame(video.decoder.get(), frame.get());
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
          }
          if (ret < 0) {
            LOG(WARNING) << "transcode: video decode failed";
            break;
          }
          // Convert to VideoFrame (I420).
          auto vf =
              AvFrameToVideoFrame(frame.get(), video.in_width, video.in_height);
          if (vf) {
            std::vector<EncodedPacket> out_packets;
            if (video.encoder.Encode(std::move(vf), &out_packets)) {
              for (const auto& ep : out_packets) {
                if (!write_video(ep)) {
                  return write_failed("a re-encoded video packet");
                }
              }
            }
          }
          av_frame_unref(frame.get());
        }
      }
    }
    av_packet_unref(pkt.get());

    // Progress.
    if (progress_cb && total_duration > 0.0) {
      const int pct = std::min(
          100, static_cast<int>(100.0 * static_cast<double>(processed_us) /
                                (total_duration * AV_TIME_BASE)));
      progress_cb(pct);
    }
  }

  // A cancel that interrupted a blocking read lands here as an ordinary
  // av_read_frame() failure -- do not mistake it for end of input.
  if (cancelled()) {
    return CancelledStatus(input_uri);
  }

  // --- Flush decoders and encoders ---
  if (want_audio && !audio.copy) {
    if (cancelled()) {
      return CancelledStatus(input_uri);
    }
    avcodec_send_packet(audio.decoder.get(), nullptr);
    while (avcodec_receive_frame(audio.decoder.get(), frame.get()) >= 0) {
      auto buf = AvFrameToAudioBuffer(frame.get(), audio.in_sample_rate,
                                      audio.in_channels);
      if (buf) {
        std::vector<EncodedPacket> out_packets;
        if (audio.encoder.Encode(std::move(buf), &out_packets)) {
          for (const auto& ep : out_packets) {
            if (!write_audio(ep)) {
              return write_failed("a trailing audio packet");
            }
          }
        }
      }
      av_frame_unref(frame.get());
    }
    // Drain the resampler before the encoder: everything it is still
    // holding belongs on the timeline ahead of the encoder's own delay.
    for (auto& buf : FlushResampler(&audio, audio.encoder.frame_size())) {
      std::vector<EncodedPacket> tail_packets;
      if (audio.encoder.Encode(std::move(buf), &tail_packets)) {
        for (const auto& ep : tail_packets) {
          if (!write_audio(ep)) {
            return write_failed("a resampler tail packet");
          }
        }
      }
    }
    std::vector<EncodedPacket> flushed;
    audio.encoder.Flush(&flushed);
    for (const auto& ep : flushed) {
      if (!write_audio(ep)) {
        return write_failed("a flushed audio packet");
      }
    }
  }

  if (want_video && !video.copy) {
    if (cancelled()) {
      return CancelledStatus(input_uri);
    }
    avcodec_send_packet(video.decoder.get(), nullptr);
    while (avcodec_receive_frame(video.decoder.get(), frame.get()) >= 0) {
      auto vf =
          AvFrameToVideoFrame(frame.get(), video.in_width, video.in_height);
      if (vf) {
        std::vector<EncodedPacket> out_packets;
        if (video.encoder.Encode(std::move(vf), &out_packets)) {
          for (const auto& ep : out_packets) {
            if (!write_video(ep)) {
              return write_failed("a trailing video packet");
            }
          }
        }
      }
      av_frame_unref(frame.get());
    }
    std::vector<EncodedPacket> flushed;
    video.encoder.Flush(&flushed);
    for (const auto& ep : flushed) {
      if (!write_video(ep)) {
        return write_failed("a flushed video packet");
      }
    }
  }

  // --- Finish ---
  if (!muxer.Finish()) {
    return Err(ErrorCode::kInvalidState, "muxer finish failed",
               params.output_path, {});
  }

  // Final progress.
  if (progress_cb) {
    progress_cb(100);
  }

  return OkStatus();
}

}  // namespace avbase::media
