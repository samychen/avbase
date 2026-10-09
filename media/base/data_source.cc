// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/data_source.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "base/functional/bind.h"

namespace avbase::media {

DataSource::DataSource() = default;
DataSource::~DataSource() = default;

MemoryDataSource::MemoryDataSource(const uint8_t* data, size_t size)
    : bytes_(data, size) {}

MemoryDataSource::~MemoryDataSource() = default;

void MemoryDataSource::SetHost(Host*) {}

DataSource::ReadResult
MemoryDataSource::ReadBlocking(int64_t offset, size_t size, uint8_t* data) {
  if (aborted_) {
    return base::unexpected(
        MediaError(ErrorCode::kAborted, "read aborted",
                   "offset = " + std::to_string(offset),
                   "this source was aborted; create a new one to read again"));
  }
  if (offset < 0 || static_cast<uint64_t>(offset) >= bytes_.size()) {
    return 0;  // EOF.
  }
  const size_t available = bytes_.size() - static_cast<size_t>(offset);
  const size_t n = std::min(size, available);
  std::memcpy(data, bytes_.data() + offset, n);
  return static_cast<int>(n);
}

void MemoryDataSource::Read(int64_t offset, size_t size, uint8_t* data,
                            base::scoped_refptr<base::TaskRunner> task_runner,
                            ReadCB read_cb) {
  ReadResult result = ReadBlocking(offset, size, data);
  if (task_runner) {
    // Never run the callback inline: callers rely on Read() returning before
    // their completion handler runs, otherwise re-entrancy becomes possible.
    //
    // The lambda wrapper is needed because base::OnceCallback is invoked with
    // Run(), not operator(), so std::invoke (which BindOnce uses internally)
    // cannot call it directly.
    task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](ReadCB cb, ReadResult r) { std::move(cb).Run(std::move(r)); },
            std::move(read_cb), std::move(result)));
    return;
  }
  std::move(read_cb).Run(std::move(result));
}

void MemoryDataSource::Abort() {
  aborted_ = true;
}

bool MemoryDataSource::GetSize(int64_t* size_out) {
  if (!size_out) {
    return false;
  }
  *size_out = static_cast<int64_t>(bytes_.size());
  return true;
}

}  // namespace avbase::media
