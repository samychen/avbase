// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// ALGORITHM PROVENANCE
// --------------------
// The demux loop's shape (read -> route -> watermark wait -> handle seek) and
// the seek flags follow ijkplayer's read_thread() in ff_ffplay.c (LGPL-2.1).
// The class structure follows Chromium's media::FFmpegDemuxer. The threading
// model differs from both on purpose; see docs/04 §2.1 (decision D3).

#include "media/filters/ffmpeg_demuxer.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ptr_util.h"
#include "media/base/media_constants.h"
#include "platform/ffmpeg/av_includes.h"
extern "C" {
#include <libavutil/display.h>
}
#include "platform/ffmpeg/av_packet_storage.h"
#include "platform/ffmpeg/compat.h"
#include "platform/ffmpeg/log_bridge.h"

namespace ijkpp::media {
namespace {

namespace ff = ::ijkpp::platform::ffmpeg;

AVFormatContext* Ctx(void* raw) { return static_cast<AVFormatContext*>(raw); }

VideoDecoderConfig MakeVideoConfig(const AVStream* stream) {
  VideoDecoderConfig config;
  const AVCodecParameters* par = stream->codecpar;
  const AVCodec* codec = avcodec_find_decoder(par->codec_id);
  config.codec_name = codec ? codec->name : "unknown";
  config.codec = VideoCodecFromName(config.codec_name);
  config.coded_size = Size{par->width, par->height};
  if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
    config.sar = Rational{par->sample_aspect_ratio.num,
                          par->sample_aspect_ratio.den};
    // natural_size is the SAR-applied display size. Rounding matches FFmpeg's
    // av_reduce so a 720x576 anamorphic stream reports 1024x576.
    const int64_t w = static_cast<int64_t>(par->width) * config.sar.num;
    const int64_t d = config.sar.den;
    config.natural_size = Size{static_cast<int>((w + d / 2) / d), par->height};
  } else {
    config.natural_size = config.coded_size;
  }
  config.frame_rate =
      Rational{stream->avg_frame_rate.num, stream->avg_frame_rate.den};
  // The decoder cannot interpret AVFrame::pts without this (see
  // VideoDecoderConfig::time_base).
  config.time_base = Rational{stream->time_base.num, stream->time_base.den};
  config.avg_frame_rate = config.frame_rate;
  config.bit_rate = par->bit_rate > 0 ? par->bit_rate : 0;
  config.profile = par->profile >= 0 ? std::to_string(par->profile) : "";
  config.level = par->level >= 0 ? std::to_string(par->level) : "";
  if (par->extradata && par->extradata_size > 0) {
    config.extra_data.assign(
        par->extradata,
        par->extradata + static_cast<size_t>(par->extradata_size));
  }
  // Rotation lives in a display-matrix side-data entry, not in the codec
  // parameters. ijkplayer reads it in three separate places.
  size_t matrix_size = 0;
  const uint8_t* matrix = ijkpp_stream_side_data(
      stream, AV_PKT_DATA_DISPLAYMATRIX, &matrix_size);
  if (matrix && matrix_size >= 9 * sizeof(int32_t)) {
    const double rotation =
        av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    if (!std::isnan(rotation)) {
      int degrees = static_cast<int>(-rotation) % 360;
      if (degrees < 0) {
        degrees += 360;
      }
      config.rotation = degrees;
    }
  }
  return config;
}

AudioDecoderConfig MakeAudioConfig(const AVStream* stream) {
  AudioDecoderConfig config;
  const AVCodecParameters* par = stream->codecpar;
  const AVCodec* codec = avcodec_find_decoder(par->codec_id);
  config.codec_name = codec ? codec->name : "unknown";
  config.codec = AudioCodecFromName(config.codec_name);
  config.sample_rate = par->sample_rate;
  config.channels = ff::ChannelCount(par);
  switch (config.channels) {
    case 1: config.channel_layout = ChannelLayout::kMono;    break;
    case 2: config.channel_layout = ChannelLayout::kStereo;  break;
    case 6: config.channel_layout = ChannelLayout::k5_1;     break;
    case 8: config.channel_layout = ChannelLayout::k7_1;     break;
    default:
      config.channel_layout = ff::ChannelLayoutMask(par)
                                  ? ChannelLayout::kDiscrete
                                  : ChannelLayout::kNone;
      break;
  }
  switch (par->format) {
    case AV_SAMPLE_FMT_U8:   config.sample_format = SampleFormat::kU8;   break;
    case AV_SAMPLE_FMT_S16:  config.sample_format = SampleFormat::kS16;  break;
    case AV_SAMPLE_FMT_S32:  config.sample_format = SampleFormat::kS32;  break;
    case AV_SAMPLE_FMT_FLT:  config.sample_format = SampleFormat::kF32;  break;
    case AV_SAMPLE_FMT_S16P: config.sample_format = SampleFormat::kS16P; break;
    case AV_SAMPLE_FMT_S32P: config.sample_format = SampleFormat::kS32P; break;
    case AV_SAMPLE_FMT_FLTP: config.sample_format = SampleFormat::kF32P; break;
    default: config.sample_format = SampleFormat::kUnknown; break;
  }
  config.bit_rate = par->bit_rate > 0 ? par->bit_rate : 0;
  config.codec_delay_frames = par->initial_padding;
  config.seek_preroll_frames = par->seek_preroll;
  if (par->extradata && par->extradata_size > 0) {
    config.extra_data.assign(
        par->extradata,
        par->extradata + static_cast<size_t>(par->extradata_size));
  }
  return config;
}

StreamKind ToStreamKind(AVMediaType type) {
  switch (type) {
    case AVMEDIA_TYPE_VIDEO:    return StreamKind::kVideo;
    case AVMEDIA_TYPE_AUDIO:    return StreamKind::kAudio;
    case AVMEDIA_TYPE_SUBTITLE: return StreamKind::kText;
    default:                    return StreamKind::kUnknown;
  }
}

DemuxerStreamType ToDemuxerStreamType(AVMediaType type) {
  switch (type) {
    case AVMEDIA_TYPE_VIDEO:    return DemuxerStreamType::kVideo;
    case AVMEDIA_TYPE_AUDIO:    return DemuxerStreamType::kAudio;
    case AVMEDIA_TYPE_SUBTITLE: return DemuxerStreamType::kText;
    default:                    return DemuxerStreamType::kUnknown;
  }
}

// Answers a ReadCB with an empty result. Always posted, never run inline.
void PostEmptyRead(const base::scoped_refptr<base::SequencedTaskRunner>& runner,
                   DemuxerStream::ReadCB cb, DemuxerStream::Status status) {
  runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](DemuxerStream::ReadCB c, DemuxerStream::Status s) {
            std::move(c).Run(s, DemuxerStream::DecoderBufferVector{});
          },
          std::move(cb), status));
}

void PostRead(const base::scoped_refptr<base::SequencedTaskRunner>& runner,
              DemuxerStream::ReadCB cb, DemuxerStream::Status status,
              DemuxerStream::DecoderBufferVector buffers) {
  runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](DemuxerStream::ReadCB c, DemuxerStream::Status s,
             DemuxerStream::DecoderBufferVector b) {
            std::move(c).Run(s, std::move(b));
          },
          std::move(cb), status, std::move(buffers)));
}

}  // namespace

// ---------------------------------------------------------------------------
// FFmpegDemuxerStream
// ---------------------------------------------------------------------------

FFmpegDemuxerStream::FFmpegDemuxerStream(
    DemuxerStreamType type, int32_t index,
    base::scoped_refptr<MediaLog> media_log,
    base::scoped_refptr<base::SequencedTaskRunner> media_runner,
    const VideoDecoderConfig& video_config,
    const AudioDecoderConfig& audio_config, StreamLiveness liveness)
    : type_(type),
      index_(index),
      media_log_(std::move(media_log)),
      media_runner_(std::move(media_runner)),
      video_config_(video_config),
      audio_config_(audio_config),
      liveness_(liveness),
      queue_(std::make_unique<DecoderBufferQueue>(
          std::string(GetDemuxerStreamTypeName(type)) + "#" +
              std::to_string(index),
          /*max_buffers=*/256, kDefaultMaxBufferBytes / 2, media_log_)) {}

FFmpegDemuxerStream::~FFmpegDemuxerStream() { queue_->Abort(); }

void FFmpegDemuxerStream::Read(uint32_t count, ReadCB read_cb) {
  if (!read_cb) {
    return;
  }
  if (count == 0) {
    count = 1;
  }
  base::AutoLock scoped(lock_);
  if (aborted_) {
    PostEmptyRead(media_runner_, std::move(read_cb), Status::kAborted);
    return;
  }
  if (pending_read_cb_) {
    // One outstanding Read per stream, matching Chromium: issuing a second
    // before the first is answered is a caller bug, not something to queue.
    LOG(ERROR) << "DemuxerStream::Read() called while a read is pending";
    PostEmptyRead(media_runner_, std::move(read_cb), Status::kAborted);
    return;
  }

  // Fast path: data is already buffered. Still posted, never run inline.
  DecoderBufferVector batch;
  const DecoderBufferQueue::PopStatus status = queue_->PopUpTo(count, &batch);
  if (!batch.empty()) {
    PostRead(media_runner_, std::move(read_cb), Status::kOk, std::move(batch));
    return;
  }
  if (status == DecoderBufferQueue::PopStatus::kAborted) {
    PostEmptyRead(media_runner_, std::move(read_cb), Status::kAborted);
    return;
  }
  pending_count_ = count;
  pending_read_cb_ = std::move(read_cb);
  // Fulfilled by the demux thread from EnqueueFromDemuxThread(). This is what
  // keeps the media sequence free of blocking calls (docs/04 §2.1).
}

bool FFmpegDemuxerStream::EnqueueFromDemuxThread(
    base::scoped_refptr<DecoderBuffer> buffer) {
  if (!queue_->TryPush(buffer)) {
    return false;   // At a watermark; the caller backs off.
  }
  base::AutoLock scoped(lock_);
  FulfilPendingReadLocked();
  return true;
}

void FFmpegDemuxerStream::NotifyEosFromDemuxThread() {
  queue_->MarkEndOfStream();
  base::AutoLock scoped(lock_);
  FulfilPendingReadLocked();
}

void FFmpegDemuxerStream::FulfilPendingReadLocked() {
  if (!pending_read_cb_) {
    return;
  }
  DecoderBufferVector batch;
  queue_->PopUpTo(pending_count_, &batch);
  if (batch.empty() && !queue_->end_of_stream() && !queue_->aborted()) {
    return;   // Nothing yet; keep the request pending.
  }
  ReadCB cb = std::move(pending_read_cb_);
  pending_count_ = 0;
  const Status status = queue_->aborted() ? Status::kAborted : Status::kOk;
  PostRead(media_runner_, std::move(cb), status, std::move(batch));
}

void FFmpegDemuxerStream::FlushFromDemuxThread(base::OnceClosure done) {
  queue_->Flush();
  {
    base::AutoLock scoped(lock_);
    if (pending_read_cb_) {
      // Chromium's DemuxerStream contract: a read pending across a Flush is
      // answered with kAborted, never left dangling.
      ReadCB cb = std::move(pending_read_cb_);
      pending_count_ = 0;
      PostEmptyRead(media_runner_, std::move(cb), Status::kAborted);
    }
  }
  if (done) {
    std::move(done).Run();
  }
}

void FFmpegDemuxerStream::AbortFromDemuxThread() {
  queue_->Abort();
  base::AutoLock scoped(lock_);
  aborted_ = true;
  if (pending_read_cb_) {
    ReadCB cb = std::move(pending_read_cb_);
    pending_count_ = 0;
    PostEmptyRead(media_runner_, std::move(cb), Status::kAborted);
  }
}

// ---------------------------------------------------------------------------
// FFmpegDemuxer
// ---------------------------------------------------------------------------

FFmpegDemuxer::FFmpegDemuxer(base::scoped_refptr<MediaLog> media_log)
    : media_log_(std::move(media_log)),
      resume_event_(base::WaitableEvent::ResetPolicy::kAutomaticReset,
                    base::WaitableEvent::InitialState::kNotSignaled) {
  DETACH_FROM_SEQUENCE(media_sequence_checker_);
}

FFmpegDemuxer::~FFmpegDemuxer() {
  Stop();
  // The demux thread has been joined, so nothing can touch the format context
  // any more and it is safe to release it here.
  if (format_ctx_raw_) {
    AVFormatContext* ctx = Ctx(format_ctx_raw_);
    avformat_close_input(&ctx);
    format_ctx_raw_ = nullptr;
  }
  delete pending_seek_.exchange(nullptr);
}

void FFmpegDemuxer::Initialize(
    const DataSourceDescriptor& source, const DemuxerOptions& options, Host* host,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    InitializeCB init_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(media_sequence_checker_);
  CHECK(media_task_runner) << "Initialize() needs a media task runner";
  host_ = host;
  media_runner_ = std::move(media_task_runner);
  prepare_started_at_ = base::TimeTicks::Now();
  ffmpeg::InitializeFFmpeg();
  interrupt_flag_.Reset();
  stop_flag_.Reset();

  demux_thread_ = std::make_unique<base::Thread>("ijkpp-demux");
  base::Thread::Options thread_options;
  thread_options.name = "ijkpp-demux";
  // Demuxing is I/O bound and tolerates latency; it must not compete with the
  // audio render thread for CPU.
  thread_options.priority = base::ThreadPriority::kBackground;
  if (!demux_thread_->StartWithOptions(thread_options)) {
    if (init_cb) {
      std::move(init_cb).Run(
          Err(ErrorCode::kNotImplemented, "failed to start the demux thread",
              "the OS refused to create a thread",
              "check the process thread limit (ulimit -u)"));
    }
    return;
  }

  // Opening can block on network I/O, so it runs on the demux thread and the
  // result is posted back to the media sequence.
  demux_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](FFmpegDemuxer* self, DataSourceDescriptor src,
             DemuxerOptions opts, InitializeCB cb) {
            Status status = self->OpenOnDemuxThread(src, opts);
            self->media_runner_->PostTask(
                FROM_HERE,
                base::BindOnce(&FFmpegDemuxer::OnOpened,
                               base::Unretained(self), std::move(status),
                               std::move(cb)));
          },
          base::Unretained(this), source, options, std::move(init_cb)));
}

// static
namespace {

// reconnect* / user_agent / headers are options of the network protocol
// handlers, not of the container demuxers; handing them to avformat for a
// plain file leaves them unconsumed, which the Δ2 reporting then shows as a
// spurious failure detail. Gate them on the URI scheme.
bool IsNetworkUri(const std::string& uri) {
  static const char* kSchemes[] = {"http://", "https://", "rtmp://",
                                   "rtmps://", "rtsp://",  "srt://",
                                   "mms://",   "udp://",   "tcp://"};
  for (const char* scheme : kSchemes) {
    if (uri.rfind(scheme, 0) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::map<std::string, std::string> FFmpegDemuxer::BuildOpenOptions(
    const DemuxerOptions& options, const std::string& uri) {
  // Start from the verbatim passthrough map so a caller can override anything,
  // then apply the structured fields on top. Structured config wins because it
  // is type-checked; a caller who needs to override a structured field can set
  // the field itself rather than smuggling it through extra_options.
  std::map<std::string, std::string> out = options.extra_options;
  if (options.probe_size > 0) {
    out["probesize"] = std::to_string(options.probe_size);
  }
  if (!options.analyze_duration.is_zero()) {
    out["analyzeduration"] =
        std::to_string(options.analyze_duration.InMicroseconds());
  }
  if (options.io_timeout > base::TimeDelta()) {
    out["rw_timeout"] = std::to_string(options.io_timeout.InMicroseconds());
    out["timeout"] = std::to_string(options.io_timeout.InMilliseconds());
  }
  // Network-only options: see IsNetworkUri() above.
  if (IsNetworkUri(uri)) {
    if (options.reconnect) {
      out["reconnect"] = "1";
      out["reconnect_streamed"] = "1";
      out["reconnect_delay_max"] = std::to_string(
          std::max<int64_t>(1, options.reconnect_delay.InSeconds()));
    }
    if (!options.user_agent.empty()) {
      out["user_agent"] = options.user_agent;
    }
    if (!options.headers.empty()) {
      std::string joined;
      for (const auto& [key, value] : options.headers) {
        joined += key + ": " + value + "\r\n";
      }
      out["headers"] = joined;
    }
    if (options.dns_cache_clear) {
      out["dns_cache_clear"] = "1";
    }
  }
  return out;
}

Status FFmpegDemuxer::OpenOnDemuxThread(const DataSourceDescriptor& source,
                                        const DemuxerOptions& options) {
  // Runs on the demux thread. This is the ONLY sequence that may touch
  // |format_ctx_raw_| while the loop is running.
  const base::TimeTicks open_started = base::TimeTicks::Now();

  if (source.kind != DataSourceDescriptor::Kind::kUri &&
      source.kind != DataSourceDescriptor::Kind::kFileDescriptor) {
    return Err(ErrorCode::kNotImplemented,
               "this DataSource kind is not wired up yet",
               "kind = " + std::to_string(static_cast<int>(source.kind)),
               "use DataSourceDescriptor::FromUri() for now; memory and custom "
               "sources land with the DataSource-backed AVIOContext path");
  }
  const bool is_fd = source.kind == DataSourceDescriptor::Kind::kFileDescriptor;
  const std::string uri = is_fd ? ("fd:" + std::to_string(source.fd)) : source.uri;
  if (uri.empty()) {
    return Err(ErrorCode::kInvalidArgument, "empty media URI", {},
               "pass a non-empty path or URL to SetDataSource()");
  }

  AVFormatContext* ctx = avformat_alloc_context();
  if (!ctx) {
    return Err(ErrorCode::kOutOfMemory, "avformat_alloc_context failed", {},
               "reduce config.buffer.max_bytes or check available memory");
  }
  ctx->interrupt_callback.callback = [](void* opaque) -> int {
    auto* self = static_cast<FFmpegDemuxer*>(opaque);
    return (self->interrupt_flag_.IsSet() || self->stop_flag_.IsSet()) ? 1 : 0;
  };
  ctx->interrupt_callback.opaque = this;

  ff::DictPtr dict = ff::ToAvDict(BuildOpenOptions(options, uri));
  const AVInputFormat* forced = options.forced_format.empty()
                                    ? nullptr
                                    : av_find_input_format(
                                          options.forced_format.c_str());

  AVDictionary* raw_dict = dict.release();
  const int ret = avformat_open_input(&ctx, uri.c_str(), forced, &raw_dict);
  dict.reset(raw_dict);
  if (ret < 0) {
    // avformat_open_input frees |ctx| and nulls the local on failure, so clear
    // our own pointer too: leaving it set would make ~FFmpegDemuxer call
    // avformat_close_input on freed memory.
    format_ctx_raw_ = nullptr;
    // Report the options FFmpeg did not consume, so a typo'd extra_options key
    // is visible instead of silently ignored (behaviour difference Δ2).
    const std::vector<std::string> unused = ff::UnconsumedOptions(raw_dict);
    std::string detail = "uri = \"" + uri + "\"";
    if (!unused.empty()) {
      detail += "\n           unrecognised options:";
      for (const std::string& key : unused) {
        detail += " " + key;
      }
    }
    return base::unexpected(ff::ToMediaError(
        ret, "FFmpegDemuxer::Open", detail,
        "verify the URI is reachable (try `ffprobe \"" + uri + "\"). If the "
        "container cannot be auto-detected, set config.demux.forced_format."));
  }
  format_ctx_raw_ = ctx;
  EmitStage(MediaLogEvent::Type::kOpenInput,
            base::TimeTicks::Now() - open_started);

  ctx->flags |= AVFMT_FLAG_GENPTS;
  if (options.find_stream_info) {
    const base::TimeTicks probe_started = base::TimeTicks::Now();
    const int info_ret = avformat_find_stream_info(ctx, nullptr);
    if (info_ret < 0 && info_ret != AVERROR_EOF) {
      return base::unexpected(ff::ToMediaError(
          info_ret, "FFmpegDemuxer::Open (find_stream_info)",
          "uri = \"" + uri + "\"",
          "the file may be truncated; raise config.demux.probe_size, or set "
          "config.demux.find_stream_info = false to skip probing"));
    }
    EmitStage(MediaLogEvent::Type::kFindStreamInfo,
              base::TimeTicks::Now() - probe_started);
  }

  BuildMediaInfo();
  opened_ok_.Set();
  if (media_log_) {
    media_log_->AddEvent(MediaLogEvent::Level::kInfo,
                         MediaLogEvent::Type::kOpenInput,
                         {{"format", media_info_.format_name},
                          {"streams", std::to_string(media_info_.streams.size())},
                          {"duration_ms",
                           std::to_string(media_info_.duration.InMilliseconds())}},
                         "opened " + media_info_.format_name);
  }
  return OkStatus();
}

void FFmpegDemuxer::AddStream(void* av_stream_raw, uint32_t index,
                              StreamLiveness liveness) {
  AVStream* av_stream = static_cast<AVStream*>(av_stream_raw);
  const DemuxerStreamType type =
      ToDemuxerStreamType(av_stream->codecpar->codec_type);
  if (type == DemuxerStreamType::kUnknown) {
    return;   // Data and attachment streams are not playable.
  }
  VideoDecoderConfig video_config;
  AudioDecoderConfig audio_config;
  if (type == DemuxerStreamType::kVideo) {
    video_config = MakeVideoConfig(av_stream);
  } else if (type == DemuxerStreamType::kAudio) {
    audio_config = MakeAudioConfig(av_stream);
  }

  StreamInfo info;
  info.index = static_cast<int>(index);
  info.kind = ToStreamKind(av_stream->codecpar->codec_type);
  const AVCodec* codec = avcodec_find_decoder(av_stream->codecpar->codec_id);
  info.codec_name = codec ? codec->name : "unknown";
  info.duration =
      (av_stream->duration > 0 && av_stream->time_base.den > 0)
          ? ff::ToTimeDelta(av_stream->duration, av_stream->time_base)
          : media_info_.duration;
  info.bit_rate = av_stream->codecpar->bit_rate > 0
                      ? av_stream->codecpar->bit_rate
                      : 0;
  if (type == DemuxerStreamType::kVideo) {
    info.coded_size = video_config.coded_size;
    info.natural_size = video_config.natural_size;
    info.sar = video_config.sar;
    info.frame_rate = video_config.frame_rate;
    info.avg_frame_rate = video_config.avg_frame_rate;
    info.rotation = video_config.rotation;
    const char* pix = av_get_pix_fmt_name(
        static_cast<AVPixelFormat>(av_stream->codecpar->format));
    info.pixel_format = pix ? pix : "";
  } else if (type == DemuxerStreamType::kAudio) {
    info.sample_rate = audio_config.sample_rate;
    info.channels = audio_config.channels;
    info.channel_layout = GetChannelLayoutName(audio_config.channel_layout);
    info.sample_format = GetSampleFormatName(audio_config.sample_format);
  }
  for (const char* key : {"language", "title"}) {
    AVDictionaryEntry* entry =
        av_dict_get(av_stream->metadata, key, nullptr, 0);
    if (entry && entry->value) {
      if (std::string_view(key) == "language") info.language = entry->value;
      else                                     info.title = entry->value;
    }
  }
  info.metadata = ff::FromAvDict(av_stream->metadata);
  media_info_.streams.push_back(std::move(info));

  streams_.push_back(std::make_unique<FFmpegDemuxerStream>(
      type, static_cast<int32_t>(index), media_log_, media_runner_,
      video_config, audio_config, liveness));
}

void FFmpegDemuxer::BuildMediaInfo() {
  AVFormatContext* ctx = Ctx(format_ctx_raw_);
  media_info_ = MediaInfo{};
  media_info_.uri = ctx->url ? ctx->url : "";
  media_info_.format_name =
      (ctx->iformat && ctx->iformat->name) ? ctx->iformat->name : "unknown";
  media_info_.duration =
      ctx->duration > 0 ? base::Microseconds(ctx->duration) : base::TimeDelta();
  media_info_.duration_is_estimate =
      ctx->duration_estimation_method == AVFMT_DURATION_FROM_STREAM ||
      ctx->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE;
  media_info_.is_live = media_info_.duration <= base::TimeDelta() ||
                        (ctx->iformat && (ctx->iformat->flags & AVFMT_NOFILE));
  media_info_.seekable = !media_info_.is_live && ctx->pb && ctx->pb->seekable;
  media_info_.bit_rate = ctx->bit_rate > 0 ? ctx->bit_rate : 0;
  media_info_.file_size = ctx->pb ? avio_size(ctx->pb) : -1;
  if (ctx->start_time != AV_NOPTS_VALUE) {
    start_time_ = base::Microseconds(ctx->start_time);
    media_info_.start_time = start_time_;
  }
  media_info_.metadata = ff::FromAvDict(ctx->metadata);

  streams_.clear();
  const StreamLiveness liveness = media_info_.is_live ? StreamLiveness::kLive
                                                      : StreamLiveness::kRecorded;
  for (uint32_t i = 0; i < ctx->nb_streams; ++i) {
    AddStream(ctx->streams[i], i, liveness);
  }
}

void FFmpegDemuxer::OnOpened(Status status, InitializeCB init_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(media_sequence_checker_);
  if (status) {
    if (host_) {
      host_->SetDuration(media_info_.duration);
    }
    // Start filling the queues immediately so the first frame is available as
    // soon as a decoder asks for it.
    demux_thread_->task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&FFmpegDemuxer::DemuxLoop,
                                  base::Unretained(this)));
  } else if (host_) {
    host_->OnDemuxerError(status.error());
  }
  if (init_cb) {
    std::move(init_cb).Run(std::move(status));
  }
}

bool FFmpegDemuxer::ReadAndRouteOnePacket(void* ctx_raw, void* packet_raw) {
  AVFormatContext* ctx = Ctx(ctx_raw);
  AVPacket* packet = static_cast<AVPacket*>(packet_raw);
  const int ret = av_read_frame(ctx, packet);
  if (ret < 0) {
    if (ret == AVERROR_EOF || avio_feof(ctx->pb)) {
      for (auto& stream : streams_) {
        stream->NotifyEosFromDemuxThread();
      }
    } else if (!stop_flag_.IsSet() && !interrupt_flag_.IsSet() && host_ &&
               media_runner_) {
      media_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(&Demuxer::Host::OnDemuxerError, base::Unretained(host_.get()),
                         ff::ToMediaError(ret, "FFmpegDemuxer::DemuxLoop",
                                          "uri = \"" + media_info_.uri + "\"",
                                          "for a network source raise "
                                          "config.net.reconnect_max_retries, or "
                                          "call ReconnectNow()")));
    }
    return false;   // Exit the loop.
  }

  const int index = packet->stream_index;
  const size_t slot = static_cast<size_t>(index);
  if (index < 0 || slot >= streams_.size() ||
      !streams_[slot] || streams_[slot]->stream_index() != index) {
    av_packet_unref(packet);   // Unselected stream; drop it.
    return true;
  }
  FFmpegDemuxerStream* stream = streams_[slot].get();

  // Zero-copy: the AVPacket is ref'd into the storage, not memcpy'd.
  auto storage = std::make_unique<ff::AvPacketStorage>();
  if (!storage->AddRef(packet)) {
    av_packet_unref(packet);
    return true;
  }
  auto buffer = DecoderBuffer::FromStorage(std::move(storage), stream->type(),
                                           index);
  AVStream* av_stream = ctx->streams[index];
  buffer->set_timestamp(ff::ToTimeDelta(packet->pts, av_stream->time_base));
  buffer->set_decode_timestamp(ff::ToTimeDelta(packet->dts, av_stream->time_base));
  buffer->set_duration(
      ff::ToTimeDelta(ff::PacketDuration(packet), av_stream->time_base));
  buffer->set_offset(packet->pos);
  buffer->set_keyframe((packet->flags & AV_PKT_FLAG_KEY) != 0);
  buffer->set_serial(stream->serial());
  const size_t payload_size = buffer->data_size();
  av_packet_unref(packet);

  bytes_read_.fetch_add(static_cast<int64_t>(payload_size),
                        std::memory_order_relaxed);
  packets_demuxed_.fetch_add(1, std::memory_order_relaxed);

  // Backpressure: if the consumer's queue is full, wait rather than growing
  // without bound. resume_event_ has exactly three signallers — Flush, Stop and
  // a seek — which is what replaced ijkplayer's continue_read_thread that had
  // to be signalled from five places (docs/01 病灶 10).
  while (!stream->EnqueueFromDemuxThread(buffer) && !stop_flag_.IsSet()) {
    resume_event_.TimedWait(base::Milliseconds(10));
    if (SeekRequest* pending = pending_seek_.exchange(nullptr)) {
      HandleSeekRequestOnDemuxThread(*pending);
      delete pending;
      break;
    }
  }
  return true;
}

void FFmpegDemuxer::DemuxLoop() {
  // Runs on "ijkpp-demux": the only sequence allowed to block on I/O
  // (docs/04 §2.1, decision D3).
  AVFormatContext* ctx = Ctx(format_ctx_raw_);
  if (!ctx) {
    return;
  }
  ff::PacketPtr packet(av_packet_alloc());
  if (!packet) {
    if (host_ && media_runner_) {
      media_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(&Demuxer::Host::OnDemuxerError, base::Unretained(host_.get()),
                         MediaError(ErrorCode::kOutOfMemory,
                                    "av_packet_alloc failed", {},
                                    "reduce config.buffer.max_bytes")));
    }
    return;
  }
  while (!stop_flag_.IsSet()) {
    // Seek requests take priority over reading.
    if (SeekRequest* request = pending_seek_.exchange(nullptr)) {
      HandleSeekRequestOnDemuxThread(*request);
      delete request;
      continue;
    }
    if (!ReadAndRouteOnePacket(ctx, packet.get())) {
      break;
    }
  }
}

void FFmpegDemuxer::HandleSeekRequestOnDemuxThread(const SeekRequest& request) {
  AVFormatContext* ctx = Ctx(format_ctx_raw_);
  if (!ctx) {
    CompleteSeek(request.request_id,
                 Err(ErrorCode::kInvalidState, "demuxer is not open", {},
                     "call PrepareAsync() first"),
                 request.target);
    return;
  }
  interrupt_flag_.Reset();
  const base::TimeTicks seek_started = base::TimeTicks::Now();

  // Prefer the video stream as the seek reference: seeking against audio in a
  // file whose audio has sparse index entries lands far from the request.
  int seek_index = -1;
  for (const auto& stream : streams_) {
    if (stream->type() == DemuxerStreamType::kVideo) {
      seek_index = stream->stream_index();
      break;
    }
  }
  if (seek_index < 0 && !streams_.empty()) {
    seek_index = streams_[0]->stream_index();
  }

  AVRational time_base{1, AV_TIME_BASE};
  if (seek_index >= 0) {
    time_base = ctx->streams[seek_index]->time_base;
  }
  const int64_t target_ts = ff::FromTimeDelta(request.target + start_time_,
                                              time_base);
  const int flags = request.any_frame ? 0 : AVSEEK_FLAG_BACKWARD;
  const int ret = av_seek_frame(ctx, seek_index, target_ts, flags);
  seek_count_.fetch_add(1, std::memory_order_relaxed);

  if (ret < 0) {
    CompleteSeek(request.request_id,
                 base::unexpected(ff::ToMediaError(
                     ret, "FFmpegDemuxer::Seek",
                     "target = " + std::to_string(request.target.InMilliseconds()) +
                         " ms",
                     "this source may not be seekable; check "
                     "media_info().seekable and is_live()")),
                 request.target);
    return;
  }

  // Bump every stream's generation and drop what is already queued: buffers
  // from before the seek must never reach a decoder (docs/04 §4 rule R1).
  for (auto& stream : streams_) {
    stream->FlushFromDemuxThread(base::DoNothing());
  }

  const base::TimeDelta load_duration = base::TimeTicks::Now() - seek_started;
  {
    base::AutoLock scoped(stats_lock_);
    stats_.last_seek_duration = load_duration;
  }
  interrupt_flag_.Reset();
  resume_event_.Signal();

  // Report the position actually reached. av_seek_frame with BACKWARD lands at
  // or before the request; reading the first packet afterwards would give the
  // exact value, but that would couple seeking to decoding, so the request
  // target is reported and the accurate-seek layer refines it (docs/04 §4.1).
  CompleteSeek(request.request_id, OkStatus(), request.target);
}

void FFmpegDemuxer::CompleteSeek(int64_t request_id, Status status,
                                 base::TimeDelta actual) {
  SeekCB cb;
  {
    base::AutoLock scoped(seek_cb_lock_);
    auto it = pending_seek_cbs_.find(request_id);
    if (it == pending_seek_cbs_.end()) {
      return;   // Superseded by a newer seek; its callback was already answered.
    }
    cb = std::move(it->second);
    pending_seek_cbs_.erase(it);
  }
  if (media_log_) {
    media_log_->AddEvent(
        MediaLogEvent::Level::kInfo, MediaLogEvent::Type::kSeekCompleted,
        {{"request_id", std::to_string(request_id)},
         {"target_ms", std::to_string(actual.InMilliseconds())},
         {"ok", status ? "true" : "false"}},
        "seek completed");
  }
  if (cb && media_runner_) {
    media_runner_->PostTask(
        FROM_HERE,
        base::BindOnce([](SeekCB c, Status s, base::TimeDelta a) {
          std::move(c).Run(std::move(s), a);
        }, std::move(cb), std::move(status), actual));
  }
}

void FFmpegDemuxer::StartPlayingFrom(base::TimeDelta time, SeekCB cb) {
  if (!opened_ok_.IsSet() || !demux_thread_) {
    if (cb) {
      std::move(cb).Run(Err(ErrorCode::kInvalidState,
                            "cannot seek before Initialize() succeeded",
                            "state = not opened",
                            "call PrepareAsync() and wait for kPrepared first"),
                        time);
    }
    return;
  }
  if (!media_info_.seekable) {
    if (cb) {
      std::move(cb).Run(
          Err(ErrorCode::kMediaUnseekable, "this stream is not seekable",
              "uri = \"" + media_info_.uri + "\"\n           is_live = " +
                  (media_info_.is_live ? "true" : "false"),
              "live and non-seekable streams cannot seek; check is_live(). "
              "For VOD-over-HLS the playlist must contain #EXT-X-ENDLIST."),
          time);
    }
    return;
  }

  const int64_t request_id = next_request_id_.fetch_add(1);
  {
    base::AutoLock scoped(seek_cb_lock_);
    // Answer every superseded request rather than dropping it silently, so a
    // caller doing rapid seeks never waits forever on a stale id.
    for (auto& [id, pending_cb] : pending_seek_cbs_) {
      if (pending_cb) {
        media_runner_->PostTask(
            FROM_HERE,
            base::BindOnce([](SeekCB c, Status s, base::TimeDelta a) {
              std::move(c).Run(std::move(s), a);
            }, std::move(pending_cb),
            Err(ErrorCode::kAborted, "seek superseded by a newer request",
                "request_id = " + std::to_string(id),
                "this is expected during rapid seeking; match completions to "
                "requests by request_id"),
            time));
      }
    }
    pending_seek_cbs_.clear();
    if (cb) {
      pending_seek_cbs_[request_id] = std::move(cb);
    }
  }

  // Publish the request. A burst collapses to the latest one.
  delete pending_seek_.exchange(new SeekRequest{time, false, request_id});

  // Interrupt a possibly-blocking av_read_frame so the seek is picked up
  // promptly instead of after the current read finishes.
  interrupt_flag_.Set();
  interrupt_count_.fetch_add(1, std::memory_order_relaxed);
  resume_event_.Signal();
}

void FFmpegDemuxer::Flush(base::OnceClosure flush_cb) {
  if (!demux_thread_) {
    if (flush_cb) {
      std::move(flush_cb).Run();
    }
    return;
  }
  // Performed on the demux thread so it cannot race with av_read_frame.
  demux_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](FFmpegDemuxer* self, base::OnceClosure done) {
            for (auto& stream : self->streams_) {
              stream->FlushFromDemuxThread(base::DoNothing());
            }
            self->resume_event_.Signal();
            if (done) {
              std::move(done).Run();
            }
          },
          base::Unretained(this), std::move(flush_cb)));
}

void FFmpegDemuxer::Reset(base::OnceClosure reset_cb) {
  Flush(std::move(reset_cb));
}

void FFmpegDemuxer::Stop() {
  stop_flag_.Set();
  // The three and only three writers of interrupt_flag_, Stop() being one:
  // StartPlayingFrom (to break a read so a seek is picked up) and here.
  // ijkplayer's abort_request had to be signalled from five places and hung
  // release() whenever one was missed (docs/01 病灶 11).
  interrupt_flag_.Set();
  resume_event_.Signal();

  if (demux_thread_) {
    for (auto& stream : streams_) {
      stream->AbortFromDemuxThread();
    }
    demux_thread_->Stop();
    demux_thread_.reset();
  }
  base::AutoLock scoped(seek_cb_lock_);
  pending_seek_cbs_.clear();   // Dropping these is correct: the owner is going.
}

DemuxerStream* FFmpegDemuxer::GetStream(DemuxerStreamType type) {
  for (auto& stream : streams_) {
    if (stream->type() == type) {
      return stream.get();
    }
  }
  return nullptr;
}

DemuxerStats FFmpegDemuxer::GetStats() const {
  base::AutoLock scoped(stats_lock_);
  DemuxerStats stats = stats_;
  stats.bytes_read = bytes_read_.load(std::memory_order_relaxed);
  stats.packets_demuxed = packets_demuxed_.load(std::memory_order_relaxed);
  stats.seek_count = seek_count_.load(std::memory_order_relaxed);
  stats.interrupt_count = interrupt_count_.load(std::memory_order_relaxed);
  return stats;
}

void FFmpegDemuxer::EmitStage(MediaLogEvent::Type stage,
                              base::TimeDelta elapsed) {
  if (!media_log_) {
    return;
  }
  media_log_->AddEvent(
      MediaLogEvent::Level::kInfo, stage,
      {{"elapsed_ms", std::to_string(elapsed.InMilliseconds())},
       {"since_prepare_ms",
        std::to_string((base::TimeTicks::Now() - prepare_started_at_)
                           .InMilliseconds())}},
      GetMediaLogEventTypeName(stage));
}

}  // namespace ijkpp::media
