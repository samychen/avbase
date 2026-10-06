// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_TEXT_RASTER_H_
#define AVBASE_PLATFORM_SDL2_TEXT_RASTER_H_

#include <stdint.h>

#include <string>
#include <vector>

namespace avbase::media {

// CPU-side RGBA8 bitmap, tightly packed, row-major, 4 bytes per pixel
// (premultiplied-alpha not required: alpha is straight, matching the GL
// blend function the overlay pass uses).
struct RgbaBitmap {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
};

// Fill + optional stroke/shadow style, mirroring MediaComponent's
// TextRasterStyle. Colors are 0..1 floats.
struct TextRasterStyle {
  float fill_rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float stroke_width_px = 0.0f;  // 0 = no stroke.
  float stroke_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float shadow_offset_x = 0.0f;
  float shadow_offset_y = 0.0f;
  float shadow_rgba[4] = {0.0f, 0.0f, 0.0f, 0.75f};
};

// FreeType-only text rasterizer: multi-line UTF-8 in, RgbaBitmap out.
//
// Ported from MediaComponent's framework/render/gl/freetype_text_raster.cc
// with one deliberate reduction: no HarfBuzz. Codepoints map through
// FT_Get_Char_Index with per-glyph advances, which is correct for Latin and
// CJK subtitle text (the common case) and wrong for complex scripts (Arabic,
// Devanagari) that need shaping -- the same limitation MediaComponent's own
// header documents ("不做完整 bidi 重排 / 自动 wrap"), minus the
// shaping it does add. Bring HarfBuzz in when a subtitle track needs it.
//
// AVBASE_HAVE_FREETYPE (set by the build when FreeType was found) gates the
// implementation; without it Rasterize() always returns false and the overlay
// slot still works with host-supplied bitmaps.
class TextRaster {
 public:
  TextRaster();
  ~TextRaster();
  TextRaster(const TextRaster&) = delete;
  TextRaster& operator=(const TextRaster&) = delete;

  // Renders |utf8| ('\n' = line break, no automatic wrapping) into |out|.
  // The bitmap is exactly large enough for the text; the caller positions it.
  bool Rasterize(const std::string& font_path, int pixel_size,
                 const TextRasterStyle& style, const std::string& utf8,
                 RgbaBitmap* out);

  // Fill-only convenience overload (mirrors MediaComponent's).
  bool Rasterize(const std::string& font_path, int pixel_size,
                 const float fill_rgba[4], const std::string& utf8,
                 RgbaBitmap* out);
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_SDL2_TEXT_RASTER_H_
