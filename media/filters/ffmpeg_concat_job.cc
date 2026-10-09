// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_concat_job.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "media/filters/encoded_packet.h"
#include "media/filters/ffmpeg_encode_muxer.h"
#include "media/filters/ffmpeg_transcode_job.h"
#include "media/filters/ffmpeg_transcode_streams.h"  // RescaleTimestamps
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::platform::ffmpeg;

// RAII for AVBSFContext.
struct BsfCtxDeleter {
  void operator()(AVBSFContext* p) const {
    if (p) {
      av_bsf_free(&p);
    }
  }
};
using BsfCtxPtr = std::unique_ptr<AVBSFContext, BsfCtxDeleter>;

// Determine whether a codec requires annexb format for raw-stream
// concatenation (H.264/HEVC in MP4 length-prefixed → annexb NAL).
const char* BsfNameForCodec(AVCodecID codec_id) {
  switch (codec_id) {
  case AV_CODEC_ID_H264:
    return "h264_mp4toannexb";
  case AV_CODEC_ID_HEVC:
    return "hevc_mp4toannexb";
  default:
    return nullptr;
  }
}

// Probe one input: open it, read stream info, and close. Returns the stream
// parameters and duration. This is how the concat decides whether the fast
// path (copy) is possible.
struct ProbeResult {
  Status status = OkStatus();
  int audio_stream_index = -1;
  int video_stream_index = -1;
  AVCodecID audio_codec_id = AV_CODEC_ID_NONE;
  AVCodecID video_codec_id = AV_CODEC_ID_NONE;
  int audio_sample_rate = 0;
  int audio_channels = 0;
  int video_width = 0;
  int video_height = 0;
  AVRational video_time_base = {0, 1};
  AVRational audio_time_base = {0, 1};
  std::vector<uint8_t> audio_extradata;
  std::vector<uint8_t> video_extradata;
  int64_t duration_us = 0;
};

ProbeResult ProbeInput(const std::string& uri) {
  ProbeResult r;
  AVFormatContext* raw = nullptr;
  if (avformat_open_input(&raw, uri.c_str(), nullptr, nullptr) < 0 || !raw) {
    r.status =
        Err(ErrorCode::kSourceOpenFailed, "concat: cannot open input", uri, {});
    return r;
  }
  ff::FormatCtxPtr ctx(raw);
  if (avformat_find_stream_info(ctx.get(), nullptr) < 0) {
    r.status = Err(ErrorCode::kSourceOpenFailed, "concat: cannot probe input",
                   uri, {});
    return r;
  }
  r.duration_us = ctx->duration;
  for (unsigned i = 0; i < ctx->nb_streams; ++i) {
    AVStream* s = ctx->streams[i];
    const AVCodecParameters* cp = s->codecpar;
    if (cp->codec_type == AVMEDIA_TYPE_AUDIO && r.audio_stream_index < 0) {
      r.audio_stream_index = static_cast<int>(i);
      r.audio_codec_id = cp->codec_id;
      r.audio_sample_rate = cp->sample_rate;
      r.audio_channels = cp->ch_layout.nb_channels;
      r.audio_time_base = s->time_base;
      if (cp->extradata && cp->extradata_size > 0) {
        r.audio_extradata.assign(cp->extradata,
                                 cp->extradata + cp->extradata_size);
      }
    } else if (cp->codec_type == AVMEDIA_TYPE_VIDEO &&
               r.video_stream_index < 0) {
      r.video_stream_index = static_cast<int>(i);
      r.video_codec_id = cp->codec_id;
      r.video_width = cp->width;
      r.video_height = cp->height;
      r.video_time_base = s->time_base;
      if (cp->extradata && cp->extradata_size > 0) {
        r.video_extradata.assign(cp->extradata,
                                 cp->extradata + cp->extradata_size);
      }
    }
  }
  return r;
}

// Check whether two probe results have compatible codec parameters (so the
// fast/copy path is viable).
bool CodecsCompatible(const ProbeResult& a, const ProbeResult& b) {
  // Audio.
  if (a.audio_stream_index >= 0 && b.audio_stream_index >= 0) {
    if (a.audio_codec_id != b.audio_codec_id ||
        a.audio_sample_rate != b.audio_sample_rate ||
        a.audio_channels != b.audio_channels) {
      return false;
    }
  }
  // Video.
  if (a.video_stream_index >= 0 && b.video_stream_index >= 0) {
    if (a.video_codec_id != b.video_codec_id ||
        a.video_width != b.video_width || a.video_height != b.video_height) {
      return false;
    }
  }
  return true;
}

// --- Timestamp rebase ------------------------------------------------
// Why this exists (measured, not assumed): a segment read back from a
// container does NOT start at 0 whenever the codec has priming samples. An
// AAC MP4 written by our own muxer reads back as
//   pkt[0] pts=-1024 dts=-1024, ..., max_pts=14336  (16 packets)
// The obvious rebase for the next segment is "offset = previous max_pts +
// 1" = 14337, which puts that segment's first packet at -1024 + 14337 =
// 13313 — *behind* the previous segment's last dts of 14336. FFmpeg rejects
// it ("non monotonically increasing dts to muxer in stream 0: 14336 >=
// 13313") and drops the packet. Because WritePacket()'s result was ignored,
// every seam silently lost one frame and the test still measured a duration
// that merely looked close enough (619ms where 2x320ms wanted ~683ms).
//
// So each segment is *normalized onto the running output timeline* instead:
// its first packet lands exactly where the previous segment ended (0 for the
// very first), and intra-segment deltas are preserved. That also fixes the
// related defect that the output timeline used to start at -1024.
struct StreamRebase {
  bool seen_any = false;     // Anything written yet (across all segments).
  bool seg_started = false;  // This segment's first packet already placed.
  int64_t seg_offset = 0;
  int64_t next_ts = 0;  // Where the output timeline continues.
};

// Rebases one packet onto the continuous output timeline and advances
// |next_ts| past its end so the next segment butts against this one.
// Returns false when the packet carries no usable timestamp at all, i.e.
// there is nothing the timeline could say about it.
bool RebasePacket(StreamRebase* st, int64_t* pts, int64_t* dts, int64_t dur) {
  const int64_t base = *dts != AV_NOPTS_VALUE
                           ? *dts
                           : (*pts != AV_NOPTS_VALUE ? *pts : AV_NOPTS_VALUE);
  if (base == AV_NOPTS_VALUE) {
    return false;
  }
  if (!st->seg_started) {
    st->seg_offset = (st->seen_any ? st->next_ts : 0) - base;
    st->seg_started = true;
  }
  if (*pts != AV_NOPTS_VALUE) {
    *pts += st->seg_offset;
  }
  if (*dts != AV_NOPTS_VALUE) {
    *dts += st->seg_offset;
  }
  st->seen_any = true;
  const int64_t end = base + st->seg_offset + (dur > 0 ? dur : 0);
  if (end > st->next_ts) {
    st->next_ts = end;
  }
  return true;
}

}  // namespace

Status Concat(const ConcatParams& params) {
  if (params.inputs.empty()) {
    return Err(ErrorCode::kInvalidArgument, "concat: no inputs", {}, {});
  }
  if (params.inputs.size() == 1) {
    // Single input: just remux/transcode.
    TranscodeParams tp;
    tp.output_path = params.output_path;
    tp.muxer_options = params.muxer_options;
    tp.audio_codec.codec = "copy";
    tp.video_codec.codec = "copy";
    return Transcode(params.inputs[0], tp);
  }

  // --- Probe all inputs ---
  std::vector<ProbeResult> probes;
  probes.reserve(params.inputs.size());
  for (const auto& uri : params.inputs) {
    auto pr = ProbeInput(uri);
    if (!pr.status) {
      return pr.status;
    }
    probes.push_back(std::move(pr));
  }

  // --- Determine fast/slow path per segment ---
  const ProbeResult& ref = probes[0];
  bool all_compatible = true;
  for (size_t i = 1; i < probes.size(); ++i) {
    if (!CodecsCompatible(ref, probes[i])) {
      all_compatible = false;
      break;
    }
  }
  const bool can_fast_path = all_compatible && !params.force_reencode;

  // --- Open output muxer ---
  FFmpegEncodeMuxer muxer;
  if (!muxer.Open(params.output_path, params.muxer_options)) {
    return Err(ErrorCode::kSourceOpenFailed, "concat: cannot open output",
               params.output_path, {});
  }

  // Add output streams based on the reference (first input).
  int out_audio_idx = -1;
  int out_video_idx = -1;
  if (ref.audio_stream_index >= 0) {
    FFmpegEncodeMuxer::AudioStreamParams as;
    as.sample_rate = ref.audio_sample_rate;
    as.channels = ref.audio_channels;
    const AVCodec* enc = avcodec_find_encoder(ref.audio_codec_id);
    as.codec_name = enc ? enc->name : "aac";
    as.extradata = ref.audio_extradata;
    out_audio_idx = muxer.AddAudioStream(as);
    if (out_audio_idx < 0) {
      return Err(ErrorCode::kInvalidState, "concat: cannot add audio stream",
                 {}, {});
    }
  }
  if (ref.video_stream_index >= 0) {
    FFmpegEncodeMuxer::VideoStreamParams vs;
    const AVCodec* enc = avcodec_find_encoder(ref.video_codec_id);
    vs.codec_name = enc ? enc->name : "libx264";
    vs.width = ref.video_width;
    vs.height = ref.video_height;
    vs.time_base_num = ref.video_time_base.num;
    vs.time_base_den = ref.video_time_base.den;
    vs.extradata = ref.video_extradata;
    out_video_idx = muxer.AddVideoStream(vs);
    if (out_video_idx < 0) {
      return Err(ErrorCode::kInvalidState, "concat: cannot add video stream",
                 {}, {});
    }
  }

  StreamRebase audio_rb;
  StreamRebase video_rb;

  // The output stream's timestamp units: audio is declared 1/sample_rate and
  // video with the reference segment's timebase.
  AVRational audio_dst_tb{1, ref.audio_sample_rate > 0 ? ref.audio_sample_rate
                                                       : 48000};
  const AVRational video_dst_tb = ref.video_time_base;

  // Emits one packet onto |idx| with its timestamps rebased onto the
  // continuous output timeline. Every write is checked: an unchecked
  // WritePacket is exactly how the seam bug described above stayed green —
  // the muxer rejected the packet, nobody read the bool, and the output came
  // out one frame short per seam with no error surfaced anywhere.
  // |src_tb| is the segment container's timebase and |dst_tb| is the output
  // stream's. They are the same for mp4 audio (1/sample_rate) and differ for
  // every other container, so rebasing onto a continuous timeline is not
  // enough on its own — the units have to match too.
  auto emit = [&muxer](int idx, StreamRebase* st, const AVPacket* src,
                       AVRational src_tb, AVRational dst_tb) -> bool {
    int64_t pts = src->pts;
    int64_t dts = src->dts;
    if (!RebasePacket(st, &pts, &dts, src->duration)) {
      return true;  // No usable timestamp to place; nothing written.
    }
    EncodedPacket ep;
    ep.data.assign(src->data, src->data + static_cast<size_t>(src->size));
    ep.pts = pts;
    ep.dts = dts;
    ep.duration = src->duration;
    ep.flags = src->flags;
    RescaleTimestamps(&ep, src_tb.num, src_tb.den, dst_tb.num, dst_tb.den);
    if (!muxer.WritePacket(idx, ep)) {
      LOG(ERROR) << "concat: muxer rejected packet stream=" << idx
                 << " pts=" << pts << " dts=" << dts;
      return false;
    }
    return true;
  };

  // Bitstream filter for video (auto-insert for H.264/HEVC).
  BsfCtxPtr video_bsf;
  const char* bsf_name = ref.video_stream_index >= 0
                             ? BsfNameForCodec(ref.video_codec_id)
                             : nullptr;

  // --- Concatenation loop ---
  ff::PacketPtr pkt(av_packet_alloc());
  ff::PacketPtr bsf_out(av_packet_alloc());

  for (size_t seg = 0; seg < params.inputs.size(); ++seg) {
    const auto& uri = params.inputs[seg];
    const bool slow_path = !can_fast_path && seg > 0;

    if (slow_path) {
      // Re-encode this segment to a temp file, then read it back.
      //
      // Audio is re-encoded rather than re-muxed: a segment whose sample
      // rate differs from the reference's cannot be copied into the output
      // track without the audio coming out at the wrong speed — copying
      // 44100 packets into a track declared 48000 makes them play 8.9%
      // fast. Video is still copied, because a resolution mismatch would
      // need a scaled re-encode this job does not yet have a scale stage
      // for; it is registered as a known gap rather than faked.
      const std::string tmp_path =
          params.output_path + ".seg" + std::to_string(seg) + ".tmp.mp4";
      TranscodeParams tp;
      tp.output_path = tmp_path;
      tp.muxer_options = params.muxer_options;
      tp.audio_codec.codec = "aac";
      tp.audio_codec.sample_rate = ref.audio_sample_rate;
      tp.audio_codec.channels = ref.audio_channels;
      tp.video_codec.codec = "copy";
      Status st = Transcode(uri, tp);
      if (!st) {
        return st;
      }
      // Now read the temp file and mux into output.
      AVFormatContext* raw = nullptr;
      if (avformat_open_input(&raw, tmp_path.c_str(), nullptr, nullptr) < 0) {
        std::remove(tmp_path.c_str());
        return Err(ErrorCode::kSourceOpenFailed,
                   "concat: cannot reopen transcoded segment", tmp_path, {});
      }
      ff::FormatCtxPtr seg_ctx(raw);
      avformat_find_stream_info(seg_ctx.get(), nullptr);

      int seg_audio = -1, seg_video = -1;
      for (unsigned i = 0; i < seg_ctx->nb_streams; ++i) {
        const AVMediaType t = seg_ctx->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_AUDIO && seg_audio < 0) {
          seg_audio = static_cast<int>(i);
        } else if (t == AVMEDIA_TYPE_VIDEO && seg_video < 0) {
          seg_video = static_cast<int>(i);
        }
      }

      const AVRational seg_a_tb =
          seg_audio >= 0
              ? seg_ctx->streams[static_cast<unsigned>(seg_audio)]->time_base
              : AVRational{0, 1};
      const AVRational seg_v_tb =
          seg_video >= 0
              ? seg_ctx->streams[static_cast<unsigned>(seg_video)]->time_base
              : AVRational{0, 1};
      bool ok = true;
      while (av_read_frame(seg_ctx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == seg_audio && out_audio_idx >= 0) {
          ok = emit(out_audio_idx, &audio_rb, pkt.get(), seg_a_tb,
                    audio_dst_tb);
        } else if (pkt->stream_index == seg_video && out_video_idx >= 0) {
          ok = emit(out_video_idx, &video_rb, pkt.get(), seg_v_tb,
                    video_dst_tb);
        }
        av_packet_unref(pkt.get());
        if (!ok) {
          std::remove(tmp_path.c_str());
          return Err(ErrorCode::kInvalidState,
                     "concat: writing a re-encoded segment failed", uri,
                     "the packets could not be placed on a continuous "
                     "timeline");
        }
      }
      // The next segment starts a timeline of its own (see StreamRebase).
      audio_rb.seg_started = false;
      video_rb.seg_started = false;
      std::remove(tmp_path.c_str());
      continue;
    }

    // --- Fast path: open input, copy packets with timestamp rebase ---
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, uri.c_str(), nullptr, nullptr) < 0) {
      return Err(ErrorCode::kSourceOpenFailed, "concat: cannot open segment",
                 uri, {});
    }
    ff::FormatCtxPtr seg_ctx(raw);
    avformat_find_stream_info(seg_ctx.get(), nullptr);

    // Find audio/video streams in this segment.
    int seg_audio = -1, seg_video = -1;
    for (unsigned i = 0; i < seg_ctx->nb_streams; ++i) {
      const AVMediaType t = seg_ctx->streams[i]->codecpar->codec_type;
      if (t == AVMEDIA_TYPE_AUDIO && seg_audio < 0) {
        seg_audio = static_cast<int>(i);
      } else if (t == AVMEDIA_TYPE_VIDEO && seg_video < 0) {
        seg_video = static_cast<int>(i);
      }
    }

    // Set up bsf for video if needed (first segment only).
    if (seg == 0 && bsf_name && seg_video >= 0) {
      const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
      if (bsf) {
        AVBSFContext* raw_bsf = nullptr;
        if (av_bsf_alloc(bsf, &raw_bsf) == 0) {
          AVStream* vs = seg_ctx->streams[static_cast<unsigned>(seg_video)];
          avcodec_parameters_copy(raw_bsf->par_in, vs->codecpar);
          raw_bsf->time_base_in = vs->time_base;
          if (av_bsf_init(raw_bsf) == 0) {
            video_bsf.reset(raw_bsf);
          } else {
            av_bsf_free(&raw_bsf);
          }
        }
      }
    }

    const AVRational seg_a_tb =
        seg_audio >= 0
            ? seg_ctx->streams[static_cast<unsigned>(seg_audio)]->time_base
            : AVRational{0, 1};
    const AVRational seg_v_tb =
        seg_video >= 0
            ? seg_ctx->streams[static_cast<unsigned>(seg_video)]->time_base
            : AVRational{0, 1};
    bool ok = true;
    while (av_read_frame(seg_ctx.get(), pkt.get()) >= 0) {
      if (pkt->stream_index == seg_audio && out_audio_idx >= 0) {
        ok = emit(out_audio_idx, &audio_rb, pkt.get(), seg_a_tb, audio_dst_tb);
      } else if (pkt->stream_index == seg_video && out_video_idx >= 0) {
        // Apply bsf if configured.
        if (video_bsf) {
          av_packet_unref(bsf_out.get());
          if (av_bsf_send_packet(video_bsf.get(), pkt.get()) == 0) {
            while (av_bsf_receive_packet(video_bsf.get(), bsf_out.get()) == 0) {
              // The filter rescales to its own output timebase on the way
              // through, so that — not the segment's — is the source here.
              ok = emit(out_video_idx, &video_rb, bsf_out.get(),
                        video_bsf->time_base_out, video_dst_tb);
              av_packet_unref(bsf_out.get());
              if (!ok) {
                break;
              }
            }
          }
        } else {
          ok = emit(out_video_idx, &video_rb, pkt.get(), seg_v_tb,
                    video_dst_tb);
        }
      }
      av_packet_unref(pkt.get());
      if (!ok) {
        return Err(ErrorCode::kInvalidState,
                   "concat: writing a copied segment failed", uri,
                   "the packets could not be placed on a continuous "
                   "timeline");
      }
    }

    // Each segment carries its own timeline — usually starting at a negative
    // priming dts — so the next one is normalized onto this one's end.
    audio_rb.seg_started = false;
    video_rb.seg_started = false;
  }

  if (!muxer.Finish()) {
    return Err(ErrorCode::kInvalidState, "concat: muxer finish failed",
               params.output_path, {});
  }
  return OkStatus();
}

}  // namespace avbase::media
