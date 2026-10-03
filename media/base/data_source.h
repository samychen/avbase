// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/data_source.h` (BSD-3-Clause), reduced to what
// avbase needs. This is the extension point that replaces ijkplayer's ijkio
// cache framework: a host supplies its own DataSource (in-app downloader,
// encrypted stream, disk cache) instead of avbase hooking FFmpeg's protocol
// layer with a patched-in AVInputFormat.

#ifndef AVBASE_MEDIA_BASE_DATA_SOURCE_H_
#define AVBASE_MEDIA_BASE_DATA_SOURCE_H_

#include <stdint.h>

#include <cstddef>
#include <span>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/task_runner.h"
#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace avbase::media {

class AVBASE_MEDIA_EXPORT DataSource
    : public base::RefCountedThreadSafe<DataSource> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  // Lets the source ask for more data or defer reads while the consumer is
  // buffering.
  class Host {
   public:
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    virtual void RequestAdditionalBytes(size_t bytes) = 0;
    virtual void SetDeferring(bool deferred) = 0;

   protected:
    Host() = default;
    virtual ~Host() = default;
  };

  using ReadResult = base::expected<int, MediaError>;  // bytes read, 0 == EOF
  using ReadCB = base::OnceCallback<void(ReadResult)>;

  DataSource(const DataSource&) = delete;
  DataSource& operator=(const DataSource&) = delete;

  virtual void SetHost(Host* host) = 0;

  // Asynchronous read. |read_cb| runs on |task_runner|, never inline.
  virtual void Read(int64_t offset, size_t size, uint8_t* data,
                    base::scoped_refptr<base::TaskRunner> task_runner,
                    ReadCB read_cb) = 0;

  // Blocking variant used by the demuxer's own thread (docs/04 §2.1 D3).
  // Returns the number of bytes read, 0 at EOF, or an error.
  virtual ReadResult ReadBlocking(int64_t offset, size_t size,
                                  uint8_t* data) = 0;

  // Aborts any in-flight blocking read. Must return promptly; this is what
  // bounds Stop() (docs/04 §5.4, Δ15).
  virtual void Abort() = 0;

  virtual bool GetSize(int64_t* size_out) = 0;
  virtual bool IsStreaming() const = 0;
  virtual void SetBitrate(int bitrate) = 0;
  // True when the source can serve arbitrary offsets, i.e. seek is possible.
  virtual bool IsSeekable() const = 0;

 protected:
  // Virtual because DataSource has virtual members: RefCountedThreadSafe
  // deletes through static_cast<const T*>, so without a virtual destructor the
  // derived half would never be destroyed. This is exactly the rule documented
  // in base/memory/ref_counted.h — a class with virtual members declares its
  // own virtual destructor, a class without does not.
  friend class base::RefCountedThreadSafe<DataSource>;
  DataSource();
  virtual ~DataSource();
};

// Reads from an in-memory span. Used by tests and by hosts that download first.
class AVBASE_MEDIA_EXPORT MemoryDataSource final : public DataSource {
 public:
  MemoryDataSource(const uint8_t* data, size_t size);
  ~MemoryDataSource() override;

  void SetHost(Host* host) override;
  void Read(int64_t offset, size_t size, uint8_t* data,
            base::scoped_refptr<base::TaskRunner> task_runner,
            ReadCB read_cb) override;
  ReadResult ReadBlocking(int64_t offset, size_t size, uint8_t* data) override;
  void Abort() override;
  bool GetSize(int64_t* size_out) override;
  bool IsStreaming() const override { return false; }
  void SetBitrate(int) override {}
  bool IsSeekable() const override { return true; }

 private:
  std::span<const uint8_t> bytes_;
  bool aborted_{false};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_DATA_SOURCE_H_
