// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_color_space.h"

#include <string>

namespace avbase::media {
namespace {

// The size-class ladder. Evaluated top to bottom; the first row whose height
// bracket contains the coded height wins. 576 is the last row of PAL SD, 600
// cleanly separates SD from 720p HD. HDR overrides everything (row 0).
struct FallbackRow {
  int min_height;  // Inclusive.
  int max_height;  // Inclusive; INT_MAX means unbounded.
  bool hdr_only;   // Row only applies when has_hdr_metadata.
  VideoColorSpace cs;
};

constexpr FallbackRow kFallbackTable[] = {
    // HDR10/HLG containers are BT.2020 by definition of their transfer.
    {0, INT_MAX, true,
     VideoColorSpace{ColorMatrix::kBT2020Ncl, ColorPrimaries::kBT2020,
                     ColorTransfer::kSMPTE2084, ColorRange::kLimited}},
    // HD and up: BT.709. This row is what repairs hw-decoded frames whose
    // hwaccel reported "unspecified" for ordinary HD streams (the common
    // real-world case; see the risk note in video_color_space.h).
    {600, INT_MAX, false,
     VideoColorSpace{ColorMatrix::kBT709, ColorPrimaries::kBT709,
                     ColorTransfer::kBT709, ColorRange::kLimited}},
    // SD: BT.601, expressed as SMPTE170M. 525-line NTSC and 625-line PAL
    // content both land here; kBT470BG vs kSMPTE170M differ only in the
    // chroma center, which every renderer treats identically.
    {0, 599, false,
     VideoColorSpace{ColorMatrix::kSMPTE170M, ColorPrimaries::kSMPTE170M,
                     ColorTransfer::kBT709, ColorRange::kLimited}},
};

}  // namespace

const char* GetColorMatrixName(ColorMatrix matrix) {
  switch (matrix) {
  case ColorMatrix::kUnknown:
    return "unknown";
  case ColorMatrix::kIdentity:
    return "identity";
  case ColorMatrix::kSMPTE170M:
    return "smpte170m";
  case ColorMatrix::kBT709:
    return "bt709";
  case ColorMatrix::kBT2020Ncl:
    return "bt2020ncl";
  case ColorMatrix::kBT2020Cl:
    return "bt2020cl";
  }
  return "invalid";
}

const char* GetColorTransferName(ColorTransfer transfer) {
  switch (transfer) {
  case ColorTransfer::kUnknown:
    return "unknown";
  case ColorTransfer::kBT709:
    return "bt709";
  case ColorTransfer::kGamma22:
    return "gamma22";
  case ColorTransfer::kSRGB:
    return "srgb";
  case ColorTransfer::kSMPTE2084:
    return "smpte2084";
  case ColorTransfer::kARIBStdB67:
    return "arib-std-b67";
  }
  return "invalid";
}

const char* GetColorPrimariesName(ColorPrimaries primaries) {
  switch (primaries) {
  case ColorPrimaries::kUnknown:
    return "unknown";
  case ColorPrimaries::kSMPTE170M:
    return "smpte170m";
  case ColorPrimaries::kBT709:
    return "bt709";
  case ColorPrimaries::kBT2020:
    return "bt2020";
  }
  return "invalid";
}

const char* GetColorRangeName(ColorRange range) {
  switch (range) {
  case ColorRange::kUnknown:
    return "unknown";
  case ColorRange::kLimited:
    return "limited";
  case ColorRange::kFull:
    return "full";
  }
  return "invalid";
}

std::string VideoColorSpace::AsDebugString() const {
  std::string out = GetColorMatrixName(matrix);
  out += "/";
  out += GetColorPrimariesName(primaries);
  out += "/";
  out += GetColorTransferName(transfer);
  out += "/";
  out += GetColorRangeName(range);
  return out;
}

VideoColorSpace GuessColorSpaceFallback(int coded_width, int coded_height,
                                        bool has_hdr_metadata) {
  // The current ladder is size- and HDR-based only, which covers every
  // container avbase's testdata and the common streaming profiles exercise.
  (void)coded_width;
  for (const FallbackRow& row : kFallbackTable) {
    if (row.hdr_only && !has_hdr_metadata) {
      continue;
    }
    if (coded_height >= row.min_height && coded_height <= row.max_height) {
      return row.cs;
    }
  }
  return VideoColorSpace{};
}

}  // namespace avbase::media
