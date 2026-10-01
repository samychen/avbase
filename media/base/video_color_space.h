// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/video_color_space.h` (BSD-3-Clause), reduced
// to the axes avbase actually transports. The enum VALUES match the ones in
// docs/11 §3 and the FFmpeg bridge (platform/ffmpeg) maps them 1:1 from
// AVFrame's colour metadata, so no libav type ever appears here (C4).

#ifndef AVBASE_MEDIA_BASE_VIDEO_COLOR_SPACE_H_
#define AVBASE_MEDIA_BASE_VIDEO_COLOR_SPACE_H_

#include <stdint.h>

#include <string>

#include "media/media_export.h"

namespace avbase::media {

enum class ColorMatrix : int8_t {
  kUnknown = 0,
  kIdentity,
  kSMPTE170M,   // BT.601 (NTSC); what most SD content means.
  kBT709,       // HD content, and every container that says nothing but is HD.
  kBT2020Ncl,   // Non-constant luminance BT.2020 (HDR, UHD).
  kBT2020Cl,
};

enum class ColorTransfer : int8_t {
  kUnknown = 0,
  kBT709,       // Includes BT.601's ~2.2 gamma in practice.
  kGamma22,
  kSRGB,
  kSMPTE2084,   // PQ / HDR10.
  kARIBStdB67,  // HLG.
};

enum class ColorPrimaries : int8_t {
  kUnknown = 0,
  kSMPTE170M,   // BT.601.
  kBT709,
  kBT2020,
};

enum class ColorRange : int8_t {
  kUnknown = 0,
  kLimited,     // MPEG-style; the overwhelmingly common case.
  kFull,        // JPEG-style.
};

struct AVBASE_MEDIA_EXPORT VideoColorSpace {
  ColorMatrix matrix{ColorMatrix::kUnknown};
  ColorPrimaries primaries{ColorPrimaries::kUnknown};
  ColorTransfer transfer{ColorTransfer::kUnknown};
  ColorRange range{ColorRange::kUnknown};

  constexpr friend bool operator==(const VideoColorSpace&,
                                   const VideoColorSpace&) = default;

  bool IsSpecified() const {
    return matrix != ColorMatrix::kUnknown ||
           primaries != ColorPrimaries::kUnknown ||
           transfer != ColorTransfer::kUnknown;
  }

  std::string AsDebugString() const;
};

AVBASE_MEDIA_EXPORT const char* GetColorMatrixName(ColorMatrix matrix);
AVBASE_MEDIA_EXPORT const char* GetColorTransferName(ColorTransfer transfer);
AVBASE_MEDIA_EXPORT const char* GetColorPrimariesName(ColorPrimaries p);
AVBASE_MEDIA_EXPORT const char* GetColorRangeName(ColorRange range);

// FFmpeg does not always tag hardware-decoded frames with colour metadata the
// way the container did (the hwaccel path frequently reports "unspecified" for
// streams whose container actually said BT.709) -- avbase risk §14.4. When the
// decoder bridge sees an unspecified frame it falls back to this table, keyed
// on what the CONTAINER said (codec, profile, coded size, HDR flag) rather
// than the frame tag. The rules are the ones in ITU-R BT.709 §4 / common
// muxer practice; they are data so that they can be exhaustively unit tested
// (video_color_space_unittest.cc) instead of argued about.
//
// Exact matches win over the size rows: a codec that always means one colour
// space can be pinned below without disturbing the generic ladder. The
// codec/profile parameters were dropped when the first version of the table
// turned out to be purely size- and HDR-based; if a pinned codec row ever
// becomes necessary, extend the signature then rather than keeping dead
// parameters.
AVBASE_MEDIA_EXPORT VideoColorSpace GuessColorSpaceFallback(
    int coded_width, int coded_height, bool has_hdr_metadata);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_VIDEO_COLOR_SPACE_H_
