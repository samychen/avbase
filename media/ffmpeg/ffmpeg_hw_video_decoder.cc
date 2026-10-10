// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_hw_video_decoder.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/av_packet_storage.h"
#include "media/ffmpeg/color_space_bridge.h"
#include "media/ffmpeg/compat.h"
#include "media/ffmpeg/hw_frame_readback.h"

namespace avbase::media::ffmpeg {
namespace {

namespace ff = ::avbase::media::ffmpeg;

// Resolves |name| against FFmpeg's own device-type list by iteration, so no
// version-specific enum switch lands here. Returns AV_HWDEVICE_TYPE_NONE when
// the FFmpeg build does not know the name (which Initialize reports as an
// actionable status, and the fallback chain absorbs).
AVHWDeviceType DeviceTypeFromName(const std::string& name) {
  AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
  while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
    const char* type_name = av_hwdevice_get_type_name(type);
    if (type_name && name == type_name) {
      return type;
    }
  }
  return AV_HWDEVICE_TYPE_NONE;
}

media::VideoFormat LayoutHintFromSwFormat(AVPixelFormat sw_format) {
  switch (sw_format) {
  case AV_PIX_FMT_NV12:
    return media::VideoFormat::kNV12;
  case AV_PIX_FMT_P010LE:
    return media::VideoFormat::kP010;
  case AV_PIX_FMT_YUV420P:
    return media::VideoFormat::kI420;
  default:
    return media::VideoFormat::kNV12;
  }
}

// get_format callback. Runs inside avcodec during format negotiation; must
// return one of the offered formats. The wanted device type is read back from
// |codec_ctx->hw_device_ctx| itself, so this needs no access to the decoder's
// privates. Voting for a software format would silently degrade to software
// decode, which is NOT what this decoder promises -- the software tail of the
// fallback chain is the software decoder, with its own events -- so when no
// hw format is offered this returns AV_PIX_FMT_NONE and open/decode fails
// into the fallback chain instead.
AVPixelFormat HwGetFormatCb(AVCodecContext* codec_ctx,
                            const AVPixelFormat* formats) {
  AVHWDeviceType wanted = AV_HWDEVICE_TYPE_NONE;
  if (codec_ctx->hw_device_ctx) {
    auto* device =
        reinterpret_cast<AVHWDeviceContext*>(codec_ctx->hw_device_ctx->data);
    wanted = device->type;
  }
  for (const AVPixelFormat* fmt = formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
    for (int i = 0;; ++i) {
      const AVCodecHWConfig* hw_config =
          avcodec_get_hw_config(codec_ctx->codec, i);
      if (!hw_config) {
        break;
      }
      if (hw_config->pix_fmt == *fmt &&
          (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
          hw_config->device_type == wanted) {
        return *fmt;
      }
    }
  }
  return AV_PIX_FMT_NONE;
}

}  // namespace

struct FFmpegHwVideoDecoder::Context {
  ff::CodecCtxPtr codec_ctx;
  ff::FramePtr frame;
  AVBufferRef* device_ref{nullptr};  // av_hwdevice_ctx_create output.
  AVPixelFormat hw_pix_fmt{AV_PIX_FMT_NONE};
  bool device_ready{false};

  ~Context() {
    if (device_ref) {
      av_buffer_unref(&device_ref);
    }
  }
};

FFmpegHwVideoDecoder::FFmpegHwVideoDecoder(
    FFmpegHwDecoderSpec spec,
    base::scoped_refptr<base::SequencedTaskRunner> task_runner)
    : spec_(std::move(spec)),
      task_runner_(std::move(task_runner)),
      ctx_(std::make_unique<Context>()) {
  CHECK(task_runner_) << "FFmpegHwVideoDecoder needs a task runner so that "
                         "decode_cb is never run inline";
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

FFmpegHwVideoDecoder::~FFmpegHwVideoDecoder() = default;

bool FFmpegHwVideoDecoder::InitializeDevice() {
  if (ctx_->device_ready) {
    return true;
  }
  const AVHWDeviceType type = DeviceTypeFromName(spec_.device_type);
  if (type == AV_HWDEVICE_TYPE_NONE) {
    LOG(WARNING) << "FFmpegHwVideoDecoder: this FFmpeg build has no '"
                 << spec_.device_type << "' hw device type";
    return false;
  }
  AVBufferRef* device_ref = nullptr;
  const int ret = av_hwdevice_ctx_create(
      &device_ref, type,
      spec_.device_name.empty() ? nullptr : spec_.device_name.c_str(), nullptr,
      0);
  if (ret < 0) {
    LOG(WARNING) << "av_hwdevice_ctx_create(" << spec_.device_type
                 << "): " << ff::AvErrorString(ret);
    return false;
  }
  ctx_->device_ref = device_ref;
  ctx_->device_ready = true;
  return true;
}

bool FFmpegHwVideoDecoder::OpenCodec(const media::VideoDecoderConfig& config) {
  if (!InitializeDevice()) {
    return false;
  }
  const AVCodec* codec =
      avcodec_find_decoder_by_name(config.codec_name.c_str());
  if (!codec) {
    LOG(WARNING) << "FFmpegHwVideoDecoder: no decoder named \""
                 << config.codec_name << "\"";
    return false;
  }

  // The hwaccel table for this decoder: the first config that matches the
  // codec AND our device type fixes the hw pixel format get_format() will
  // vote for. A decoder with no matching config cannot use this device --
  // reporting that here is what moves the fallback chain to the next
  // candidate with a precise reason.
  bool have_hw_config = false;
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* hw_config = avcodec_get_hw_config(codec, i);
    if (!hw_config) {
      break;
    }
    if (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
        hw_config->device_type == DeviceTypeFromName(spec_.device_type)) {
      ctx_->hw_pix_fmt = hw_config->pix_fmt;
      have_hw_config = true;
      break;
    }
  }
  if (!have_hw_config) {
    LOG(WARNING) << "FFmpegHwVideoDecoder: codec " << config.codec_name
                 << " has no hwaccel config for device " << spec_.device_type;
    return false;
  }

  ff::CodecCtxPtr codec_ctx(avcodec_alloc_context3(codec));
  if (!codec_ctx) {
    return false;
  }
  codec_ctx->width = config.coded_size.width;
  codec_ctx->height = config.coded_size.height;
  // Same pkt_timebase contract as the software decoder: without it every
  // AVFrame::pts is in an undefined time base and A/V sync dies silently.
  codec_ctx->pkt_timebase =
      AVRational{config.time_base.num, config.time_base.den};
  if (codec_ctx->pkt_timebase.num <= 0 || codec_ctx->pkt_timebase.den <= 0) {
    codec_ctx->pkt_timebase = AVRational{1, AV_TIME_BASE};
  }
  codec_ctx->workaround_bugs = FF_BUG_AUTODETECT;
  // HW decoders: no frame threading of our own; the hwaccel serializes.
  if (!config.extra_data.empty()) {
    codec_ctx->extradata = static_cast<uint8_t*>(
        av_mallocz(config.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codec_ctx->extradata) {
      return false;
    }
    memcpy(codec_ctx->extradata, config.extra_data.data(),
           config.extra_data.size());
    codec_ctx->extradata_size = static_cast<int>(config.extra_data.size());
  }
  codec_ctx->hw_device_ctx = av_buffer_ref(ctx_->device_ref);
  if (!codec_ctx->hw_device_ctx) {
    return false;
  }
  codec_ctx->get_format = HwGetFormatCb;

  const int ret = avcodec_open2(codec_ctx.get(), codec, nullptr);
  if (ret < 0) {
    LOG(WARNING) << "avcodec_open2(" << config.codec_name << "/"
                 << spec_.device_type << "): " << ff::AvErrorString(ret);
    return false;
  }
  ctx_->codec_ctx = std::move(codec_ctx);
  ctx_->frame = ff::FramePtr(av_frame_alloc());
  return ctx_->frame != nullptr;
}

void FFmpegHwVideoDecoder::Initialize(const media::VideoDecoderConfig& config,
                                      bool /*low_delay*/,
                                      media::CdmContext* /*cdm*/,
                                      InitCB init_cb, const OutputCB& output_cb,
                                      const media::WaitingCB& waiting_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  output_cb_ = output_cb;
  waiting_cb_ = waiting_cb;
  config_ = config;
  ctx_ = std::make_unique<Context>();

  media::DecoderStatus status;
  if (config.codec == media::VideoCodec::kUnknown ||
      config.codec_name.empty()) {
    status = media::DecoderStatus(
        media::DecoderStatus::Codes::kUnsupportedCodec,
        "unknown video codec (codec_name=\"" + config.codec_name + "\")");
  } else if (config.coded_size.IsEmpty()) {
    status = media::DecoderStatus(
        media::DecoderStatus::Codes::kUnsupportedResolution,
        "coded_size is empty for codec " + config.codec_name);
  } else if (!OpenCodec(config)) {
    // OpenCodec already logged the precise cause (missing device, no hwaccel
    // config, open failure); the status is intentionally coarse because the
    // actionable detail is in the log line and the fallback chain only needs
    // "this candidate is out".
    status = media::DecoderStatus(
        media::DecoderStatus::Codes::kUnsupportedCodec,
        "hardware path \"" + spec_.display_name + "\" cannot decode " +
            config.codec_name + " on device " + spec_.device_type);
  }
  initialized_ = status.is_ok();
  if (init_cb) {
    task_runner_->PostTask(FROM_HERE,
                           base::BindOnce(std::move(init_cb), status));
  }
}

bool FFmpegHwVideoDecoder::DecodeAvailableFrames() {
  AVCodecContext* codec_ctx = ctx_->codec_ctx.get();
  AVFrame* frame = ctx_->frame.get();
  for (;;) {
    const int ret = avcodec_receive_frame(codec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }
    if (ret < 0) {
      LOG(ERROR) << "avcodec_receive_frame(" << spec_.display_name
                 << "): " << ff::AvErrorString(ret);
      return false;
    }

    if (frame->format != ctx_->hw_pix_fmt) {
      // The decoder fell out of the hw path mid-stream (driver reset to
      // software frames). This decoder must not impersonate the software
      // one: report the failure and let the fallback chain restart on the
      // software candidate.
      LOG(ERROR) << "FFmpegHwVideoDecoder: expected hw format "
                 << av_get_pix_fmt_name(ctx_->hw_pix_fmt) << ", got "
                 << av_get_pix_fmt_name(
                        static_cast<AVPixelFormat>(frame->format));
      av_frame_unref(frame);
      return false;
    }

    media::NativeHandle handle;
    handle.kind = spec_.handle_kind;
    switch (spec_.handle_kind) {
    case media::NativeHandleKind::kCVPixelBuffer:
    case media::NativeHandleKind::kVaapiSurface:
      // videotoolbox: CVPixelBufferRef; vaapi: VASurfaceID (uintptr).
      handle.id = frame->data[3];
      handle.subresource = 0;
      break;
    case media::NativeHandleKind::kD3D11Texture:
      // d3d11va: data[3] is the ID3D11Texture2D, data[4] carries the
      // subresource index as an intptr (FFmpeg's d3d11va hwaccel).
      handle.id = frame->data[3];
      handle.subresource =
          static_cast<int>(reinterpret_cast<intptr_t>(frame->data[4]));
      break;
    case media::NativeHandleKind::kNone:
      av_frame_unref(frame);
      return false;
    }

    // The surface's software layout comes from the frames context, not from
    // the opaque hw pixel format.
    AVPixelFormat sw_format = AV_PIX_FMT_NV12;
    if (frame->hw_frames_ctx) {
      auto* frames_ctx =
          reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
      sw_format = frames_ctx->sw_format;
    }

    media::Rational sar{1, 1};
    if (frame->sample_aspect_ratio.num > 0 &&
        frame->sample_aspect_ratio.den > 0) {
      sar = media::Rational{frame->sample_aspect_ratio.num,
                            frame->sample_aspect_ratio.den};
    } else if (config_.sar.num > 0) {
      sar = config_.sar;
    }
    const base::TimeDelta timestamp =
        ff::ToTimeDelta(frame->pts, codec_ctx->pkt_timebase);
    const base::TimeDelta duration =
        ff::ToTimeDelta(frame->duration, codec_ctx->pkt_timebase);
    const media::VideoColorSpace cs = ColorSpaceFromAvFrame(frame, config_);
    // Captured before the frame is unref'd: av_frame_unref zeroes these.
    const media::Size size{frame->width, frame->height};

    // Two independent references to the hw frame: one dies with the display
    // frame (release_cb), one backs the readback path (to_i420_cb). The
    // readback may run more than once, so its reference must be copyable:
    // shared_ptr supplies the copying, the deleter supplies the unref.
    std::shared_ptr<AVFrame> readback_frame(av_frame_clone(frame),
                                            [](AVFrame* f) {
                                              if (f) {
                                                av_frame_free(&f);
                                              }
                                            });
    AVFrame* held_for_release = av_frame_clone(frame);
    av_frame_unref(frame);
    if (!readback_frame || !held_for_release) {
      // M7: a partial clone pair must not leak the surviving half
      // (readback_frame releases itself through its shared_ptr deleter).
      if (held_for_release) {
        av_frame_free(&held_for_release);
      }
      return false;
    }
    auto out = media::VideoFrame::WrapNativeBuffer(
        handle, LayoutHintFromSwFormat(sw_format), size, size, sar, timestamp,
        duration, current_serial_,
        base::BindOnce(
            [](AVFrame* f) {
              if (f) {
                av_frame_free(&f);
              }
            },
            held_for_release),
        base::BindRepeating(
            [](std::shared_ptr<AVFrame> hw_frame, media::Rational sar,
               base::TimeDelta timestamp, base::TimeDelta duration,
               int32_t serial, media::VideoColorSpace cs)
                -> base::scoped_refptr<media::VideoFrame> {
              return ff::MapHwFrameToI420(hw_frame.get(), sar, timestamp,
                                          duration, serial, cs);
            },
            readback_frame, sar, timestamp, duration, current_serial_, cs));
    if (!out) {
      // M7: WrapNativeBuffer failed, so the release closure bound to
      // |held_for_release| is destroyed WITHOUT running -- free the cloned
      // reference here or it leaks on every wrap failure.
      av_frame_free(&held_for_release);
      return false;
    }
    out->set_color_space(cs);

    ++frames_decoded_;
    if (output_cb_) {
      output_cb_.Run(std::move(out));
    }
  }
}

void FFmpegHwVideoDecoder::Decode(
    base::scoped_refptr<media::DecoderBuffer> buffer, DecodeCB decode_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!decode_cb) {
    return;
  }
  if (!initialized_ || !ctx_->codec_ctx) {
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            std::move(decode_cb),
            media::DecoderStatus(media::DecoderStatus::Codes::kNotInitialized,
                                 "Initialize() did not succeed")));
    return;
  }
  pending_decode_cbs_.push_back(std::move(decode_cb));

  if (!buffer) {
    RunOneDecodeCallback(media::DecoderStatus(
        media::DecoderStatus::Codes::kDecodeError, "null buffer"));
    return;
  }

  if (buffer->IsEndOfStream()) {
    avcodec_send_packet(ctx_->codec_ctx.get(), nullptr);
    if (!DecodeAvailableFrames()) {
      RunOneDecodeCallback(media::DecoderStatus(
          media::DecoderStatus::Codes::kDecodeError, "hw flush failed"));
      return;
    }
    // All remaining requests are answered, mirroring the software decoder.
    while (!pending_decode_cbs_.empty()) {
      DecodeCB cb = std::move(pending_decode_cbs_.front());
      pending_decode_cbs_.pop_front();
      task_runner_->PostTask(
          FROM_HERE, base::BindOnce(std::move(cb), media::DecoderStatus()));
    }
    return;
  }

  auto* storage = buffer->storage_as<ff::AvPacketStorage>();
  if (!storage || !storage->raw()) {
    RunOneDecodeCallback(media::DecoderStatus(
        media::DecoderStatus::Codes::kDecodeError,
        "buffer storage is not an AVPacket; a custom Demuxer must produce "
        "AvPacketStorage for FFmpegHwVideoDecoder"));
    return;
  }

  current_serial_ = buffer->serial();
  const int send_ret =
      avcodec_send_packet(ctx_->codec_ctx.get(), storage->raw());
  if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
    RunOneDecodeCallback(ff::ToDecoderStatus(send_ret, "avcodec_send_packet"));
    return;
  }
  if (!DecodeAvailableFrames()) {
    RunOneDecodeCallback(
        media::DecoderStatus(media::DecoderStatus::Codes::kDecodeError,
                             "avcodec_receive_frame failed on the hw path"));
    return;
  }
  RunOneDecodeCallback(media::DecoderStatus());
}

void FFmpegHwVideoDecoder::Reset(base::OnceClosure closure) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (ctx_->codec_ctx) {
    avcodec_flush_buffers(ctx_->codec_ctx.get());
  }
  // Every outstanding request is answered before |closure|, per the contract
  // (same as the software decoder's Reset()).
  while (!pending_decode_cbs_.empty()) {
    DecodeCB cb = std::move(pending_decode_cbs_.front());
    pending_decode_cbs_.pop_front();
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            std::move(cb),
            media::DecoderStatus(media::DecoderStatus::Codes::kDecodingAborted,
                                 "Reset()")));
  }
  if (closure) {
    task_runner_->PostTask(FROM_HERE, std::move(closure));
  }
}

int FFmpegHwVideoDecoder::GetMaxDecodeRequests() const {
  return 1;
}

void FFmpegHwVideoDecoder::RunOneDecodeCallback(media::DecoderStatus status) {
  if (pending_decode_cbs_.empty()) {
    return;
  }
  DecodeCB cb = std::move(pending_decode_cbs_.front());
  pending_decode_cbs_.pop_front();
  task_runner_->PostTask(FROM_HERE, base::BindOnce(std::move(cb), status));
}

}  // namespace avbase::media::ffmpeg
