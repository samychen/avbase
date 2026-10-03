// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_frame.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include "base/check.h"

namespace avbase::media {
namespace {

// Bytes per pixel of a plane, expressed as numerator/denominator over the
// luma sample count so that subsampled chroma planes work out.
struct PlaneLayout {
  int plane_count;
  // Height divisor and width-bytes multiplier per plane.
  int height_div[VideoFrame::kMaxPlanes];
  int width_bytes_num[VideoFrame::kMaxPlanes];
  int width_bytes_den[VideoFrame::kMaxPlanes];
};

PlaneLayout LayoutFor(VideoFormat format) {
  switch (format) {
  case VideoFormat::kI420:
  case VideoFormat::kYV12:
    return {3, {1, 2, 2, 0}, {1, 1, 1, 0}, {1, 2, 2, 0}};
  case VideoFormat::kNV12:
  case VideoFormat::kNV21:
    return {2, {1, 2, 0, 0}, {1, 2, 0, 0}, {1, 1, 0, 0}};
  case VideoFormat::kYUV420P10:
    return {3, {1, 2, 2, 0}, {2, 2, 2, 0}, {1, 2, 2, 0}};
  case VideoFormat::kP010:
    return {2, {1, 2, 0, 0}, {4, 4, 0, 0}, {1, 1, 0, 0}};
  case VideoFormat::kYUY2:
    return {1, {1, 0, 0, 0}, {2, 0, 0, 0}, {1, 0, 0, 0}};
  case VideoFormat::kARGB:
    return {1, {1, 0, 0, 0}, {4, 0, 0, 0}, {1, 0, 0, 0}};
  case VideoFormat::kRGB24:
    return {1, {1, 0, 0, 0}, {3, 0, 0, 0}, {1, 0, 0, 0}};
  case VideoFormat::kRGB565:
    return {1, {1, 0, 0, 0}, {2, 0, 0, 0}, {1, 0, 0, 0}};
  case VideoFormat::kUnknown:
    break;
  }
  return {0, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}};
}

}  // namespace

const char* GetVideoFormatName(VideoFormat format) {
  switch (format) {
  case VideoFormat::kUnknown:
    return "unknown";
  case VideoFormat::kI420:
    return "I420";
  case VideoFormat::kYV12:
    return "YV12";
  case VideoFormat::kNV12:
    return "NV12";
  case VideoFormat::kNV21:
    return "NV21";
  case VideoFormat::kYUY2:
    return "YUY2";
  case VideoFormat::kARGB:
    return "ARGB";
  case VideoFormat::kRGB24:
    return "RGB24";
  case VideoFormat::kRGB565:
    return "RGB565";
  case VideoFormat::kYUV420P10:
    return "YUV420P10";
  case VideoFormat::kP010:
    return "P010";
  }
  return "invalid";
}

int VideoFormatPlaneCount(VideoFormat format) {
  return LayoutFor(format).plane_count;
}

const char* GetNativeHandleKindName(NativeHandleKind kind) {
  switch (kind) {
  case NativeHandleKind::kNone:
    return "none";
  case NativeHandleKind::kVaapiSurface:
    return "vaapi-surface";
  case NativeHandleKind::kD3D11Texture:
    return "d3d11-texture";
  case NativeHandleKind::kCVPixelBuffer:
    return "cvpixelbuffer";
  }
  return "invalid";
}

VideoFrame::VideoFrame() = default;

VideoFrame::~VideoFrame() {
  std::free(allocation_);
  allocation_ = nullptr;
  if (release_cb_) {
    std::move(release_cb_).Run();
  }
}

base::scoped_refptr<VideoFrame> VideoFrame::ToI420() const {
  if (format_ == VideoFormat::kI420 && IsMappable()) {
    // Already owned I420 CPU memory: hand back another reference to the same
    // pixels rather than duplicating them.
    return base::scoped_refptr<VideoFrame>(const_cast<VideoFrame*>(this));
  }
  if (to_i420_cb_) {
    return to_i420_cb_.Run();
  }
  return nullptr;
}

// static
base::scoped_refptr<VideoFrame> VideoFrame::WrapNativeBuffer(
    NativeHandle handle, VideoFormat format, Size coded_size, Size natural_size,
    Rational sar, base::TimeDelta timestamp, base::TimeDelta duration,
    int32_t serial, base::OnceClosure release_cb,
    base::RepeatingCallback<base::scoped_refptr<VideoFrame>()> to_i420_cb) {
  DCHECK(handle.kind != NativeHandleKind::kNone)
      << "WrapNativeBuffer needs a typed handle; use CreateBlackFrame for "
         "CPU frames";
  DCHECK(release_cb) << "a native-buffer frame without a release callback "
                        "would leak its producer-side backing store";

  base::scoped_refptr<VideoFrame> frame(new VideoFrame());
  frame->format_ = format;
  frame->storage_type_ = StorageType::kStorageOpaque;
  frame->coded_size_ = coded_size;
  frame->natural_size_ = natural_size;
  frame->sar_ = sar;
  frame->timestamp_ = timestamp;
  frame->duration_ = duration;
  frame->serial_ = serial;
  frame->native_handle_ = handle;
  frame->opaque_handle_ = handle.id;
  frame->release_cb_ = std::move(release_cb);
  frame->to_i420_cb_ = std::move(to_i420_cb);
  return frame;
}

std::span<const uint8_t> VideoFrame::visible_data(Plane plane) const {
  if (!planes_[plane] || allocated_sizes_[plane] == 0) {
    return {};
  }
  return {planes_[plane], allocated_sizes_[plane]};
}

std::span<uint8_t> VideoFrame::mutable_data(Plane plane) {
  if (!planes_[plane] || allocated_sizes_[plane] == 0) {
    return {};
  }
  return {planes_[plane], allocated_sizes_[plane]};
}

std::string VideoFrame::AsDebugString() const {
  std::string out = GetVideoFormatName(format_);
  out += " " + std::to_string(coded_size_.width) + "x" +
         std::to_string(coded_size_.height);
  out += " ts=" + timestamp_.ToString();
  out += " dur=" + duration_.ToString();
  out += " serial=" + std::to_string(serial_);
  if (storage_type_ != StorageType::kStorageOwned) {
    out += " storage=non-cpu";
  }
  if (native_handle_.kind != NativeHandleKind::kNone) {
    out += " handle=";
    out += GetNativeHandleKindName(native_handle_.kind);
  }
  if (color_space_.IsSpecified()) {
    out += " cs=" + color_space_.AsDebugString();
  }
  return out;
}

// static
base::scoped_refptr<VideoFrame> VideoFrame::CreateBlackFrame(
    VideoFormat format, Size coded_size, Size natural_size, Rational sar,
    base::TimeDelta timestamp, base::TimeDelta duration, int32_t serial) {
  const PlaneLayout layout = LayoutFor(format);
  CHECK_GT(layout.plane_count, 0) << "CreateBlackFrame: unknown format";

  base::scoped_refptr<VideoFrame> frame(new VideoFrame());
  frame->format_ = format;
  frame->storage_type_ = VideoFrame::StorageType::kStorageOwned;
  frame->coded_size_ = coded_size;
  frame->natural_size_ = natural_size;
  frame->sar_ = sar;
  frame->timestamp_ = timestamp;
  frame->duration_ = duration;
  frame->serial_ = serial;

  // Single allocation, planes carved out of it. Rows are 64-byte aligned so
  // that GL PBO uploads and sws_scale do not hit misaligned stores.
  constexpr int kAlignment = 64;
  size_t total = 0;
  size_t offsets[VideoFrame::kMaxPlanes] = {0, 0, 0, 0};
  for (int p = 0; p < layout.plane_count; ++p) {
    const int stride = ((coded_size.width * layout.width_bytes_num[p] /
                         layout.width_bytes_den[p]) +
                        kAlignment - 1) /
                       kAlignment * kAlignment;
    const int rows =
        (coded_size.height + layout.height_div[p] - 1) / layout.height_div[p];
    // strides_/allocated_sizes_ are std::array, whose operator[] takes size_t;
    // the plane index is an int because it is compared against plane_count.
    const size_t plane = static_cast<size_t>(p);
    offsets[plane] = total;
    frame->strides_[plane] = stride;
    frame->allocated_sizes_[plane] =
        static_cast<size_t>(stride) * static_cast<size_t>(rows);
    total += frame->allocated_sizes_[plane] + kAlignment;
  }

  frame->allocation_ = std::calloc(1, total);
  CHECK(frame->allocation_)
      << "CreateBlackFrame: allocation of " << total << " bytes failed";
  auto* base_ptr = static_cast<uint8_t*>(frame->allocation_);
  for (int p = 0; p < layout.plane_count; ++p) {
    const size_t plane = static_cast<size_t>(p);
    frame->planes_[plane] = base_ptr + offsets[plane];
  }
  return frame;
}

}  // namespace avbase::media
