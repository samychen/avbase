// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/data_source_descriptor.h"

#include "media/base/data_source.h"   // Complete type: scoped_refptr<DataSource> needs it.

#include <utility>

namespace avbase::media {

// static
DataSourceDescriptor DataSourceDescriptor::FromUri(std::string_view uri) {
  DataSourceDescriptor d;
  d.kind = Kind::kUri;
  d.uri = std::string(uri);
  return d;
}

// static
DataSourceDescriptor DataSourceDescriptor::FromMemory(const uint8_t* data,
                                                      size_t size) {
  DataSourceDescriptor d;
  d.kind = Kind::kMemoryBuffer;
  d.buffer = data;
  d.buffer_size = size;
  return d;
}

// static
DataSourceDescriptor DataSourceDescriptor::FromFileDescriptor(int fd,
                                                              int64_t offset,
                                                              int64_t length) {
  DataSourceDescriptor d;
  d.kind = Kind::kFileDescriptor;
  d.fd = fd;
  d.fd_offset = offset;
  d.fd_length = length;
  return d;
}

// static
DataSourceDescriptor DataSourceDescriptor::FromSource(
    base::scoped_refptr<DataSource> source) {
  DataSourceDescriptor d;
  d.kind = Kind::kCustomSource;
  d.custom = std::move(source);
  return d;
}

}  // namespace avbase::media
