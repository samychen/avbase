// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `media/base/video_frame.h` (BSD-3-Clause).
//
// Replaces the two parallel hierarchies ijkplayer maintains for the same
// concept: `AVFrame` (software pixels) and `SDL_VoutOverlay` (hardware
// surfaces). Chromium never needed the second one because VideoFrame carries
// a StorageType; see docs/02 §9 D2.

#ifndef AVBASE_MEDIA_BASE_VIDEO_FRAME_H_
#define AVBASE_MEDIA_BASE_VIDEO_FRAME_H_

#include <stdint.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/media_export.h"

namespace avbase::media {

struct AVBASE_MEDIA_EXPORT Rational {
  int num{1};
  int den{1};
  constexpr bool is_zero() const noexcept { return num == 0; }
  constexpr double ToDouble() const noexcept {
    return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
  }
  constexpr friend bool operator==(const Rational&, const Rational&) = default;
};

struct AVBASE_MEDIA_EXPORT Size {
  int width{0};
  int height{0};
  constexpr int GetArea() const noexcept { return width * height; }
  constexpr bool IsEmpty() const noexcept { return width <= 0 || height <= 0; }
  constexpr friend bool operator==(const Size&, const Size&) = default;
};

enum class VideoFormat {
  kUnknown = 0,
  kI420,
  kYV12,
  kNV12,
  kNV21,
  kYUY2,
  kARGB,
  kRGB24,
  kRGB565,
  kYUV420P10,
  kP010,
};

AVBASE_MEDIA_EXPORT const char* GetVideoFormatName(VideoFormat format);
AVBASE_MEDIA_EXPORT int VideoFormatPlaneCount(VideoFormat format);

// A decoded video frame.
//
// All instances are reference-counted and must be held through
// scoped_refptr<VideoFrame>. Passing a frame to a sink therefore costs one
// atomic increment, never a pixel copy.
class AVBASE_MEDIA_EXPORT VideoFrame
    : public base::RefCountedThreadSafe<VideoFrame> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  enum class StorageType {
    kStorageOwned = 0,       // CPU memory allocated by avbase; planes readable.
    kStorageDmaBufs,         // Linux dmabuf; zero-copy into EGL, planes unreadable.
    kStorageGpuMemoryBuffer,
    kStorageOpaque,          // Platform-private handle (MediaCodec Surface,
                             // CVPixelBuffer). planes() returns an empty span.
  };

  enum Plane { kYPlane = 0, kUPlane = 1, kVPlane = 2, kAPlane = 3, kMaxPlanes = 4 };

  VideoFormat format() const { return format_; }
  StorageType storage_type() const { return storage_type_; }
  Size coded_size() const { return coded_size_; }
  Size natural_size() const { return natural_size_; }
  Rational sar() const { return sar_; }
  int rotation() const { return rotation_; }

  base::TimeDelta timestamp() const { return timestamp_; }
  base::TimeDelta duration() const { return duration_; }
  base::TimeTicks reference_time() const { return reference_time_; }
  int32_t serial() const { return serial_; }
  int64_t frame_index() const { return frame_index_; }

  // CPU access. Returns an empty span for kStorageOpaque / kStorageDmaBufs.
  std::span<const uint8_t> visible_data(Plane plane) const;
  // Producer-side write access. Only the code that is filling a freshly
  // allocated frame (a decoder or a converter) may call this; once the frame is
  // handed to a sink it is treated as immutable, which is what lets the same
  // buffer be read concurrently by the compositor and the sink without a lock.
  std::span<uint8_t> mutable_data(Plane plane);
  int32_t stride(Plane plane) const { return strides_[plane]; }
  size_t allocated_size(Plane plane) const { return allocated_sizes_[plane]; }

  bool IsMappable() const { return storage_type_ == StorageType::kStorageOwned; }

  // Test/CLI helper: builds a zero-filled owned frame. Production decoders go
  // through VideoFramePool instead.
  static base::scoped_refptr<VideoFrame> CreateBlackFrame(
      VideoFormat format, Size coded_size, Size natural_size, Rational sar,
      base::TimeDelta timestamp, base::TimeDelta duration, int32_t serial);

  // Adapter escape hatch for platform code (FFmpeg / dmabuf). See
  // DecoderBuffer::storage_as() for the rationale; no RTTI involved.
  const void* opaque_handle() const { return opaque_handle_; }

  std::string AsDebugString() const;

 private:
  friend class base::RefCountedThreadSafe<VideoFrame>;
  friend class VideoFramePool;
  ~VideoFrame();

  VideoFrame();

  VideoFormat format_{VideoFormat::kUnknown};
  StorageType storage_type_{StorageType::kStorageOwned};
  Size coded_size_;
  Size natural_size_;
  Rational sar_{1, 1};
  int rotation_{0};
  base::TimeDelta timestamp_;
  base::TimeDelta duration_;
  base::TimeTicks reference_time_;
  int32_t serial_{0};
  int64_t frame_index_{-1};
  std::array<uint8_t*, kMaxPlanes> planes_{nullptr, nullptr, nullptr, nullptr};
  std::array<int32_t, kMaxPlanes> strides_{0, 0, 0, 0};
  std::array<size_t, kMaxPlanes> allocated_sizes_{0, 0, 0, 0};
  const void* opaque_handle_{nullptr};
  void* allocation_{nullptr};   // Owned backing store when storage is owned.
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_VIDEO_FRAME_H_
