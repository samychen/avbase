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

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/video_color_space.h"
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

// The strong-typed GPU handle carried by a non-CPU frame (avbase §6.2, the
// zero-copy contract). |kind| says what |id| points at, so a consumer never
// casts blind:
//
//   kVaapiSurface    id = VASurfaceID (as uintptr),  imported to EGLImage.
//   kD3D11Texture    id = ID3D11Texture2D*, subresource = SRV slice.
//   kCVPixelBuffer   id = CVPixelBufferRef,           importable to MTLTexture
//                    (or IOSurface-backed GL texture).
//
// |id| stays owned by the frame's producer-side backing (typically an FFmpeg
// hw frame); it is only valid while this VideoFrame is alive. media/base
// never names those platform types directly (invariant C4) -- the kind tag is
// the contract, the platform layers do the casting.
enum class NativeHandleKind {
  kNone = 0,
  kVaapiSurface,
  kD3D11Texture,
  kCVPixelBuffer,
};

AVBASE_MEDIA_EXPORT const char* GetNativeHandleKindName(NativeHandleKind kind);

struct AVBASE_MEDIA_EXPORT NativeHandle {
  NativeHandleKind kind{NativeHandleKind::kNone};
  const void* id{nullptr};
  int subresource{0};

  constexpr friend bool operator==(const NativeHandle&,
                                   const NativeHandle&) = default;
};

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
  VideoColorSpace color_space() const { return color_space_; }

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

  // Producer-side colour tagging, same window of validity as mutable_data():
  // the decoder sets it before the frame is first published.
  void set_color_space(VideoColorSpace cs) { color_space_ = cs; }

  bool IsMappable() const { return storage_type_ == StorageType::kStorageOwned; }

  // Zero-copy contract (avbase §6.2): a hardware-decoded frame is passed GPU
  // to GPU end to end; pixels are only read back when a consumer EXPLICITLY
  // calls ToI420(). Returns:
  //   - this same frame (new reference) when the pixels are already owned
  //     I420 CPU memory;
  //   - a freshly allocated owned I420 frame when the producer installed a
  //     readback path (the FFmpeg hw decoder does);
  //   - nullptr when no readback is possible (no producer path installed).
  // Never runs on the frame's own storage in place: the caller keeps using
  // the original frame for display while it reads.
  base::scoped_refptr<VideoFrame> ToI420() const;

  // Test/CLI helper: builds a zero-filled owned frame. Production decoders go
  // through VideoFramePool instead.
  static base::scoped_refptr<VideoFrame> CreateBlackFrame(
      VideoFormat format, Size coded_size, Size natural_size, Rational sar,
      base::TimeDelta timestamp, base::TimeDelta duration, int32_t serial);

  // The hardware-frame constructor. |handle| names the GPU object; it stays
  // owned by the producer's backing store and is only guaranteed valid while
  // this frame (and every reference cloned from it) is alive. |release_cb|
  // runs at last-unref and is what actually frees that backing store;
  // |to_i420_cb|, when set, is the explicit-readback path behind ToI420().
  // |format| is a LAYOUT hint (e.g. kNV12), not a CPU-plane promise --
  // planes() stays empty for this storage.
  static base::scoped_refptr<VideoFrame> WrapNativeBuffer(
      NativeHandle handle, VideoFormat format, Size coded_size,
      Size natural_size, Rational sar, base::TimeDelta timestamp,
      base::TimeDelta duration, int32_t serial,
      base::OnceClosure release_cb,
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()> to_i420_cb);

  // The typed GPU handle. Empty (kNone) unless the producer used
  // WrapNativeBuffer(). Supersedes opaque_handle(), which stays for the
  // dmabuf adapters that predate the kind tag.
  NativeHandle native_handle() const { return native_handle_; }

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
  NativeHandle native_handle_;
  VideoColorSpace color_space_;
  // Non-owned storage (hw frames, dmabufs): the producer-side teardown. Run
  // exactly once, at last unref, before the frame's memory goes away.
  base::OnceClosure release_cb_;
  // The explicit-readback path behind ToI420(); installed by the producer
  // that created a native-buffer frame. Null = no readback possible.
  base::RepeatingCallback<base::scoped_refptr<VideoFrame>()> to_i420_cb_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_VIDEO_FRAME_H_
