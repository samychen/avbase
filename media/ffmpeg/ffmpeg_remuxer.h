// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_REMUXER_H_
#define AVBASE_MEDIA_FFMPEG_REMUXER_H_

#include <string>

#include "media/base/media_error.h"
#include "media/media_export.h"

namespace avbase::media {

// Copies |src_path| into |dst_path| without re-encoding: stream parameters
// and packets move across with their timestamps rescaled, which is the
// Muxer capability MediaComponent's framework/ffmpeg/muxer.h provided,
// reduced to the packet-level form a player SDK needs (clip fixup, format
// conversion, demo export). The output container comes from the file
// extension (mp4/matroska/adts are enabled in tools/setup_ffmpeg.sh).
//
// Encoding-based recording builds on the same avformat plumbing but is a
// separate milestone. Synchronous by design: a 30-second clip remuxes in
// well under a second.
Status AVBASE_MEDIA_EXPORT RemuxContainer(const std::string& src_path,
                                          const std::string& dst_path);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_REMUXER_H_
