// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/text_raster.h"

#include <gtest/gtest.h>

namespace avbase::media {
namespace {

// The rasterizer needs a real font file; this suite runs wherever one of the
// known demo fonts exists (macOS, Debian/Ubuntu with the dejavu package) and
// skips otherwise. FreeType itself is optional at build time: without it
// Rasterize() always returns false and the suite asserts exactly that.
bool FindFont(std::string* out) {
  for (const char* path : {"/System/Library/Fonts/Helvetica.ttc",
                           "/System/Library/Fonts/Supplemental/Arial.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
    if (std::FILE* f = std::fopen(path, "rb")) {
      std::fclose(f);
      *out = path;
      return true;
    }
  }
  return false;
}

int64_t CountNonTransparent(const RgbaBitmap& bitmap) {
  int64_t count = 0;
  for (size_t i = 3; i < bitmap.rgba.size(); i += 4) {
    if (bitmap.rgba[i] > 0) {
      ++count;
    }
  }
  return count;
}

TEST(TextRasterTest, RasterizesSingleLine) {
  std::string font;
  if (!FindFont(&font)) {
    GTEST_SKIP() << "no known system font on this host";
  }
  TextRaster raster;
  RgbaBitmap bitmap;
#ifdef AVBASE_HAVE_FREETYPE
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  ASSERT_TRUE(raster.Rasterize(font, 28, white, "avbase", &bitmap));
  EXPECT_GT(bitmap.width, 0);
  EXPECT_GT(bitmap.height, 0);
  EXPECT_EQ(bitmap.rgba.size(),
            static_cast<size_t>(bitmap.width) * bitmap.height * 4);
  // Real ink, not an empty bitmap.
  EXPECT_GT(CountNonTransparent(bitmap), 100);
#else
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  EXPECT_FALSE(raster.Rasterize(font, 28, white, "avbase", &bitmap));
#endif
}

TEST(TextRasterTest, MultilineGrowsHeight) {
  std::string font;
  if (!FindFont(&font)) {
    GTEST_SKIP() << "no known system font on this host";
  }
#ifdef AVBASE_HAVE_FREETYPE
  TextRaster raster;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  RgbaBitmap one_line;
  ASSERT_TRUE(raster.Rasterize(font, 28, white, "line one", &one_line));
  RgbaBitmap two_lines;
  ASSERT_TRUE(
      raster.Rasterize(font, 28, white, "line one\nline two", &two_lines));
  EXPECT_GT(two_lines.height, one_line.height);
#else
  GTEST_SKIP() << "built without FreeType";
#endif
}

TEST(TextRasterTest, InvalidFontFails) {
#ifdef AVBASE_HAVE_FREETYPE
  TextRaster raster;
  RgbaBitmap bitmap;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  EXPECT_FALSE(
      raster.Rasterize("/nonexistent/font.ttf", 28, white, "x", &bitmap));
  EXPECT_FALSE(
      raster.Rasterize("/nonexistent/font.ttf", 28, white, "", &bitmap));
#else
  GTEST_SKIP() << "built without FreeType";
#endif
}

}  // namespace
}  // namespace avbase::media
