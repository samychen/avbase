// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/text_raster.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/logging.h"

#if defined(AVBASE_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace avbase::media {
namespace {

int ToU8(float v) {
  return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

#if defined(AVBASE_HAVE_FREETYPE)

// Blits a glyph bitmap into |out| keeping whichever pixel is more opaque
// (the "max" composite MediaComponent uses for shadow/stroke passes, so an
// outline never eats the fill drawn over it).
void BlitGlyphMax(RgbaBitmap* out, int dst_x, int dst_y, const FT_Bitmap& bmp,
                  int r, int g, int b, float alpha) {
  const float a = std::clamp(alpha, 0.0f, 1.0f);
  for (unsigned int row = 0; row < bmp.rows; ++row) {
    const int y = dst_y + static_cast<int>(row);
    if (y < 0 || y >= out->height) {
      continue;
    }
    const unsigned char* src =
        bmp.buffer + static_cast<size_t>(row) * static_cast<size_t>(bmp.pitch);
    for (unsigned int col = 0; col < bmp.width; ++col) {
      const int x = dst_x + static_cast<int>(col);
      if (x < 0 || x >= out->width) {
        continue;
      }
      const unsigned char cover =
          bmp.pixel_mode == FT_PIXEL_MODE_MONO
              ? static_cast<unsigned char>(
                    (((src[col >> 3] >> (7 - (col & 7))) & 1) ? 255 : 0) * a +
                    0.5f)
              : static_cast<unsigned char>(src[col] * a + 0.5f);
      if (cover == 0) {
        continue;
      }
      uint8_t* px = out->rgba.data() +
                    (static_cast<size_t>(y) * static_cast<size_t>(out->width) +
                     static_cast<size_t>(x)) *
                        4u;
      if (cover >= px[3]) {
        px[0] = static_cast<uint8_t>(r);
        px[1] = static_cast<uint8_t>(g);
        px[2] = static_cast<uint8_t>(b);
        px[3] = cover;
      }
    }
  }
}

// Blits a glyph pixel-replacing (the fill pass, drawn last).
void BlitGlyphReplace(RgbaBitmap* out, int dst_x, int dst_y,
                      const FT_Bitmap& bmp, int r, int g, int b, float alpha) {
  const float a = std::clamp(alpha, 0.0f, 1.0f);
  for (unsigned int row = 0; row < bmp.rows; ++row) {
    const int y = dst_y + static_cast<int>(row);
    if (y < 0 || y >= out->height) {
      continue;
    }
    const unsigned char* src =
        bmp.buffer + static_cast<size_t>(row) * static_cast<size_t>(bmp.pitch);
    for (unsigned int col = 0; col < bmp.width; ++col) {
      const int x = dst_x + static_cast<int>(col);
      if (x < 0 || x >= out->width) {
        continue;
      }
      const unsigned char cover =
          bmp.pixel_mode == FT_PIXEL_MODE_MONO
              ? static_cast<unsigned char>(
                    (((src[col >> 3] >> (7 - (col & 7))) & 1) ? 255 : 0) * a +
                    0.5f)
              : static_cast<unsigned char>(src[col] * a + 0.5f);
      if (cover == 0) {
        continue;
      }
      uint8_t* px = out->rgba.data() +
                    (static_cast<size_t>(y) * static_cast<size_t>(out->width) +
                     static_cast<size_t>(x)) *
                        4u;
      px[0] = static_cast<uint8_t>(r);
      px[1] = static_cast<uint8_t>(g);
      px[2] = static_cast<uint8_t>(b);
      px[3] = cover;
    }
  }
}

// Decodes the next UTF-8 code point; returns 0 on malformed input (consumes
// one byte so the scan always advances).
uint32_t DecodeUtf8(const std::string& s, size_t* i) {
  const auto lead = static_cast<unsigned char>(s[*i]);
  size_t len = 0;
  uint32_t cp = 0;
  if (lead < 0x80) {
    ++*i;
    return lead;
  } else if ((lead & 0xE0) == 0xC0) {
    len = 2;
    cp = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    len = 3;
    cp = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    len = 4;
    cp = lead & 0x07;
  } else {
    ++*i;
    return 0;
  }
  if (*i + len > s.size()) {
    ++*i;
    return 0;
  }
  for (size_t k = 1; k < len; ++k) {
    const auto cont = static_cast<unsigned char>(s[*i + k]);
    if ((cont & 0xC0) != 0x80) {
      *i += k;
      return 0;
    }
    cp = (cp << 6) | (cont & 0x3F);
  }
  *i += len;
  return cp;
}

std::vector<std::string> SplitLines(const std::string& utf8) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= utf8.size()) {
    size_t end = utf8.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(utf8.substr(start));
      break;
    }
    size_t line_end = end;
    if (line_end > start && utf8[line_end - 1] == '\r') {
      --line_end;
    }
    lines.push_back(utf8.substr(start, line_end - start));
    start = end + 1;
  }
  if (lines.empty()) {
    lines.emplace_back();
  }
  return lines;
}

#endif  // defined(AVBASE_HAVE_FREETYPE)

}  // namespace

TextRaster::TextRaster() = default;

TextRaster::~TextRaster() = default;

bool TextRaster::Rasterize(const std::string& font_path, int pixel_size,
                           const TextRasterStyle& style,
                           const std::string& utf8, RgbaBitmap* out) {
#if defined(AVBASE_HAVE_FREETYPE)
  if (!out || pixel_size <= 0 || utf8.empty()) {
    return false;
  }
  FT_Library library = nullptr;
  if (FT_Init_FreeType(&library) != 0) {
    LOG(ERROR) << "text raster: FT_Init_FreeType failed";
    return false;
  }
  // Per-call face/library: cue rasterization is a few frames apart at most,
  // and a cache buys correctness questions (face invalidation when a font
  // file changes) that the overlay path does not need answered yet.
  FT_Face face = nullptr;
  if (FT_New_Face(library, font_path.c_str(), 0, &face) != 0) {
    LOG(ERROR) << "text raster: cannot open font " << font_path;
    FT_Done_FreeType(library);
    return false;
  }
  FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size));

  // ---- Pass 1: measure. Per codepoint advance, tracking the rightmost ink
  // edge so trailing whitespace does not inflate the bitmap.
  struct LineLayout {
    int width{1};
    int ascent{0};   // px above the baseline.
    int descent{0};  // px below the baseline (positive).
    std::vector<FT_UInt> glyph_indices;
    std::vector<int> offsets;  // pen offset per glyph, from the line origin.
  };
  std::vector<LineLayout> layouts;
  for (const std::string& line : SplitLines(utf8)) {
    LineLayout layout;
    int pen_x = 0;
    size_t i = 0;
    while (i < line.size()) {
      const uint32_t cp = DecodeUtf8(line, &i);
      if (cp == 0) {
        continue;
      }
      const FT_UInt index = FT_Get_Char_Index(face, cp);
      if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_RENDER) != 0) {
        // Missing glyph: advance by a rough space so the line keeps length.
        pen_x += pixel_size / 4;
        continue;
      }
      const int gx = pen_x + face->glyph->bitmap_left;
      layout.width = std::max(layout.width,
                              gx + static_cast<int>(face->glyph->bitmap.width));
      layout.ascent = std::max(layout.ascent, face->glyph->bitmap_top);
      layout.descent =
          std::max(layout.descent, static_cast<int>(face->glyph->bitmap.rows) -
                                       face->glyph->bitmap_top);
      layout.glyph_indices.push_back(index);
      layout.offsets.push_back(pen_x);
      pen_x += face->glyph->advance.x >> 6;
    }
    layouts.push_back(std::move(layout));
  }

  const int line_height = pixel_size + pixel_size / 4;
  int bitmap_w = 1;
  for (const LineLayout& l : layouts) {
    bitmap_w = std::max(bitmap_w, l.width);
  }
  const int stroke_r =
      style.stroke_width_px > 0.0f
          ? std::max(1, static_cast<int>(std::ceil(style.stroke_width_px)))
          : 0;
  const int shadow_dx = static_cast<int>(std::lround(style.shadow_offset_x));
  const int shadow_dy = static_cast<int>(std::lround(style.shadow_offset_y));
  const bool has_shadow =
      (shadow_dx != 0 || shadow_dy != 0) && style.shadow_rgba[3] > 0.001f;
  RgbaBitmap bitmap;
  bitmap.width = bitmap_w + 2 * stroke_r + std::max(0, shadow_dx);
  bitmap.height = static_cast<int>(layouts.size()) * line_height +
                  2 * stroke_r + std::max(0, shadow_dy);
  bitmap.rgba.assign(static_cast<size_t>(bitmap.width) *
                         static_cast<size_t>(bitmap.height) * 4u,
                     0);

  // ---- Pass 2: draw, baseline-anchored per line.
  const int fill_r = ToU8(style.fill_rgba[0]);
  const int fill_g = ToU8(style.fill_rgba[1]);
  const int fill_b = ToU8(style.fill_rgba[2]);
  const int stroke_r8 = ToU8(style.stroke_rgba[0]);
  const int stroke_g8 = ToU8(style.stroke_rgba[1]);
  const int stroke_b8 = ToU8(style.stroke_rgba[2]);
  const int shadow_r8 = ToU8(style.shadow_rgba[0]);
  const int shadow_g8 = ToU8(style.shadow_rgba[1]);
  const int shadow_b8 = ToU8(style.shadow_rgba[2]);

  for (size_t li = 0; li < layouts.size(); ++li) {
    const LineLayout& layout = layouts[li];
    const int baseline =
        static_cast<int>(li) * line_height + stroke_r + layout.ascent;
    for (size_t gi = 0; gi < layout.glyph_indices.size(); ++gi) {
      if (FT_Load_Glyph(face, layout.glyph_indices[gi], FT_LOAD_RENDER) != 0) {
        continue;
      }
      const int base_x =
          stroke_r + layout.offsets[gi] + face->glyph->bitmap_left;
      const int base_y = baseline - face->glyph->bitmap_top;
      if (has_shadow) {
        BlitGlyphMax(&bitmap, base_x + shadow_dx, base_y + shadow_dy,
                     face->glyph->bitmap, shadow_r8, shadow_g8, shadow_b8,
                     style.shadow_rgba[3]);
      }
      if (stroke_r > 0) {
        const int r2 = stroke_r * stroke_r;
        for (int oy = -stroke_r; oy <= stroke_r; ++oy) {
          for (int ox = -stroke_r; ox <= stroke_r; ++ox) {
            if (ox * ox + oy * oy > r2 || (ox == 0 && oy == 0)) {
              continue;
            }
            BlitGlyphMax(&bitmap, base_x + ox, base_y + oy, face->glyph->bitmap,
                         stroke_r8, stroke_g8, stroke_b8, style.stroke_rgba[3]);
          }
        }
      }
      BlitGlyphReplace(&bitmap, base_x, base_y, face->glyph->bitmap, fill_r,
                       fill_g, fill_b, style.fill_rgba[3]);
    }
  }

  FT_Done_Face(face);
  FT_Done_FreeType(library);
  *out = std::move(bitmap);
  return true;
#else
  (void)font_path;
  (void)pixel_size;
  (void)style;
  (void)utf8;
  (void)out;
  return false;
#endif
}

bool TextRaster::Rasterize(const std::string& font_path, int pixel_size,
                           const float fill_rgba[4], const std::string& utf8,
                           RgbaBitmap* out) {
  TextRasterStyle style;
  style.fill_rgba[0] = fill_rgba[0];
  style.fill_rgba[1] = fill_rgba[1];
  style.fill_rgba[2] = fill_rgba[2];
  style.fill_rgba[3] = fill_rgba[3];
  // A 1px dark outline keeps text legible on bright frames; the shadow pass
  // is what MediaComponent's subtitle style uses for the same reason.
  style.stroke_width_px = 1.0f;
  return Rasterize(font_path, pixel_size, style, utf8, out);
}

}  // namespace avbase::media
