// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// avbase-inspect: a diagnostic CLI over the real media stack.
//
// Why this exists: ijkplayer's only window into its own behaviour is a wall of
// LOGD lines from ff_ffplay.c, and several classes of bug -- a wrong time base, a
// sign inversion in the sync correction, a decoder that silently produces no
// frames -- are invisible in that output. Each subcommand prints the intermediate
// values a developer would otherwise need a debugger for, using exactly the code
// paths the player uses and no test doubles.
//
// Bug #32 (a master clock offset by negative uptime) was found by `sync` on its
// first run against a real file, after 300+ unit tests had passed.

#include <stdio.h>

#include <string>

#include "platform/ffmpeg/log_bridge.h"
#include "tools/inspect/inspect_common.h"

int main(int argc, char** argv) {
  avbase::Options opts;
  if (!avbase::ParseArgs(argc, argv, &opts)) {
    return 1;
  }
  // Route FFmpeg's own logging through avbase's so a single --verbose controls
  // both. Without this, libavformat's warnings go to stderr unformatted and
  // interleave with the report.
  avbase::platform::ffmpeg::InstallLogBridge();

  if (opts.subcommand == "probe") {
    return avbase::RunProbe(opts);
  }
  if (opts.subcommand == "decode") {
    return avbase::RunDecode(opts);
  }
  if (opts.subcommand == "sync") {
    return avbase::RunSync(opts);
  }
  fprintf(stderr, "error: unknown subcommand '%s'\n", opts.subcommand.c_str());
  avbase::Usage();
  return 1;
}
