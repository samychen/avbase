// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_FFMPEG_HW_VIDEO_DECODER_H_
#define AVBASE_MEDIA_FFMPEG_FFMPEG_HW_VIDEO_DECODER_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/decoder_buffer.h"
#include "media/base/media_types.h"
#include "media/base/video_decoder.h"
#include "media/base/video_frame.h"

namespace avbase::media::ffmpeg {

// What a platform factory asks the generic FFmpeg hw path to do. The strings
// and enums here are libav-free on purpose: the per-platform factory targets
// (platform/videotoolbox, platform/vaapi, platform/d3d11) include this header
// and they must not see a libav type (invariant C8).
struct FFmpegHwDecoderSpec {
  // FFmpeg's AVHWDeviceType name, e.g. "videotoolbox", "vaapi", "d3d11va",
  // "cuda". Resolved through av_hwdevice_iterate_types so no libav enum
  // appears in this header.
  std::string device_type;
  // Optional device node / adapter name passed to av_hwdevice_ctx_create
  // (e.g. "/dev/dri/renderD128"); empty means the FFmpeg default.
  std::string device_name;
  // The typed GPU handle the decoder's frames will carry.
  media::NativeHandleKind handle_kind{media::NativeHandleKind::kNone};
  // Codecs this device is known to decode. CreateVideoDecoder returns nullptr
  // for anything else, which is what makes the fallback chain move on.
  std::vector<media::VideoCodec> codecs;
  // Which VideoDecoderType the capability/selection layer sees (kVideoToolbox,
  // kVaapiVideoDecoder, kNvDec, ...).
  media::VideoDecoderType decoder_type{media::VideoDecoderType::kUnknown};
  std::string display_name;  // For decoder events and status details.
};

// Hardware video decoding through libavcodec's hwaccel API:
// av_hwdevice_ctx_create() for the device, avcodec_get_hw_config() + a
// get_format callback to select the hw pixel format, and decoded frames that
// stay on the GPU as VideoFrame::WrapNativeBuffer() frames (avbase §6.2).
// Pixels are read back ONLY when a consumer explicitly calls ToI420(), which
// runs av_hwframe_transfer_data behind the frame's readback callback.
//
// Fallback contract: every failure here is an Initialize/Decode status, never
// an exception and never a crash, so the existing DecoderStream chain
// (hardware first, this device's software tail last) absorbs a missing
// device, an unsupported profile or a driver reset.
class FFmpegHwVideoDecoder final : public media::VideoDecoder {
 public:
  FFmpegHwVideoDecoder(
      FFmpegHwDecoderSpec spec,
      base::scoped_refptr<base::SequencedTaskRunner> task_runner);
  FFmpegHwVideoDecoder(const FFmpegHwVideoDecoder&) = delete;
  FFmpegHwVideoDecoder& operator=(const FFmpegHwVideoDecoder&) = delete;
  ~FFmpegHwVideoDecoder() override;

  // media::VideoDecoder:
  void Initialize(const media::VideoDecoderConfig& config, bool low_delay,
                  media::CdmContext* cdm_context, InitCB init_cb,
                  const OutputCB& output_cb,
                  const media::WaitingCB& waiting_cb) override;
  void Decode(base::scoped_refptr<media::DecoderBuffer> buffer,
              DecodeCB decode_cb) override;
  void Reset(base::OnceClosure closure) override;
  int GetMaxDecodeRequests() const override;
  media::VideoDecoderType GetDecoderType() const override {
    return spec_.decoder_type;
  }
  const char* name() const override { return spec_.display_name.c_str(); }

 private:
  struct Context;

  bool InitializeDevice();
  bool OpenCodec(const media::VideoDecoderConfig& config);
  bool DecodeAvailableFrames();
  void RunOneDecodeCallback(media::DecoderStatus status);

  FFmpegHwDecoderSpec spec_;
  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  std::unique_ptr<Context> ctx_;
  OutputCB output_cb_;
  media::WaitingCB waiting_cb_;
  std::deque<DecodeCB> pending_decode_cbs_;
  media::VideoDecoderConfig config_;
  bool initialized_{false};
  int32_t current_serial_{0};
  uint64_t frames_decoded_{0};

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_FFMPEG_HW_VIDEO_DECODER_H_
