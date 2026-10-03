// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_DATA_SOURCE_DESCRIPTOR_H_
#define AVBASE_MEDIA_BASE_DATA_SOURCE_DESCRIPTOR_H_

#include <stdint.h>

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "base/memory/scoped_refptr.h"
#include "media/base/data_source.h"  // Complete type: the implicit ~DataSourceDescriptor needs it.
#include "media/media_export.h"

namespace avbase::media {

// Describes where media comes from. Exactly one member is meaningful, selected
// by |kind|.
//
// This type lives in media/base rather than player/public because
// media::Demuxer takes it as a parameter and media/ must not depend on player/
// (docs/02 §2.1). player/public/player.h re-exports it into namespace avbase so
// SDK callers still spell it avbase::DataSourceDescriptor.
struct AVBASE_MEDIA_EXPORT DataSourceDescriptor {
  enum class Kind {
    kUri = 0,         // Open |uri| through FFmpeg's protocol layer.
    kMemoryBuffer,    // Play from an already-downloaded buffer.
    kFileDescriptor,  // Play from an fd at an offset (Android assets, pipes).
    kCustomSource,    // Play through a host-supplied media::DataSource.
  };

  static DataSourceDescriptor FromUri(std::string_view uri);
  static DataSourceDescriptor FromMemory(const uint8_t* data, size_t size);
  static DataSourceDescriptor FromFileDescriptor(int fd, int64_t offset,
                                                 int64_t length);
  static DataSourceDescriptor
  FromSource(base::scoped_refptr<DataSource> source);

  Kind kind{Kind::kUri};
  std::string uri;
  const uint8_t* buffer{nullptr};
  size_t buffer_size{0};
  int fd{-1};
  int64_t fd_offset{0};
  int64_t fd_length{-1};
  base::scoped_refptr<DataSource> custom;
  // Forwarded verbatim to FFmpeg's AVDictionary (extra_format_options plus any
  // per-source overrides).
  std::map<std::string, std::string> options;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_DATA_SOURCE_DESCRIPTOR_H_
