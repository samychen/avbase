// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// C ABI smoke test -- docs/08 M15.
//
// WHY A SMOKE TEST AND NOT A UNIT TEST: the thing that can break here is not a
// branch, it is the BOUNDARY. A C++ unit test includes the C++ headers, so it
// proves nothing about whether avbase_c.h is valid C, whether a C caller can
// link it, or whether the values crossing the boundary are right. Those three
// questions are only answerable by compiling a real .c file with a C compiler
// and running it -- which is what this does.
//
// It also pins the two behaviours a C ABI most often gets wrong, both of which
// compile cleanly and fail only at runtime:
//   * a buffer contract that silently truncates instead of reporting the size
//     it needed, and
//   * a "no info yet" case that answers AVBASE_OK with zeroed fields.
//
// THIS FILE IS COMPILED BY THE C COMPILER, not by a hand-rolled shell line
// (see player/public/c/CMakeLists.txt): the compile step is the whole point of
// the test, and a flag the build system does not know about is a flag that
// silently stops applying. CMAKE_C_STANDARD 99 is set there and the warnings
// are -Werror, so a C++-ism reaching avbase_c.h turns this target red instead
// of turning the header into something only C++ callers can use.
//
// USAGE
//   capi_smoke_test [source.mp4]

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "player/public/c/avbase_c.h"

static int g_events = 0;
static int g_prepared = 0;
static int g_completed = 0;

static void on_event(void* user_data, int what, int64_t arg1, int64_t arg2,
                     const char* obj) {
  (void)user_data;
  (void)arg1;
  (void)arg2;
  (void)obj;
  ++g_events;
  if (what == 200) {  // kPrepared in the legacy quadruple.
    ++g_prepared;
  } else if (what == 300) {  // kCompleted.
    ++g_completed;
  }
}

static int failures = 0;

static void Check(int condition, const char* what) {
  if (condition) {
    printf("  ok    %s\n", what);
  } else {
    printf("  FAIL  %s\n", what);
    ++failures;
  }
}

int main(int argc, char** argv) {
  const char* uri = argc > 1 ? argv[1] : "tests/testdata/small_h264_aac_3s.mp4";

  printf("c_abi=%s engine=%s\n", avbase_c_version(), avbase_engine_version());

  AvbasePlayer* player = avbase_player_create();
  Check(player != NULL, "player_create returns a handle");
  if (player == NULL) {
    return 1;
  }
  avbase_player_set_event_callback(player, on_event, NULL);

  Check(avbase_player_set_data_source(player, uri) == AVBASE_OK,
        "set_data_source on a real file");
  Check(avbase_player_set_data_source(player, NULL) ==
            AVBASE_ERR_INVALID_ARGUMENT,
        "set_data_source rejects a NULL uri");
  Check(avbase_player_set_data_source(NULL, uri) == AVBASE_ERR_INVALID_ARGUMENT,
        "set_data_source rejects a NULL player");

  // Before prepare there is no media info, and saying so is the point: an
  // AVBASE_OK over zeroed fields would be indistinguishable from a real
  // zero-duration stream.
  AvbaseMediaInfo info;
  memset(&info, 0xAB, sizeof(info));
  Check(avbase_player_get_media_info(player, &info) == AVBASE_ERR_INVALID_STATE,
        "media_info before prepare is kInvalidState, not zeros-with-OK");

  Check(avbase_player_prepare_sync(player, 15000) == AVBASE_OK,
        "prepare_sync reaches prepared");

  Check(avbase_player_get_media_info(player, &info) == AVBASE_OK,
        "media_info after prepare succeeds");
  Check(info.duration_ms > 0, "duration is non-zero after prepare");
  Check(info.video_track_count >= 1, "at least one video track reported");
  Check(info.audio_track_count >= 1, "at least one audio track reported");

  avbase_player_start(player);
  for (int i = 0; i < 200 && !g_completed; ++i) {
    struct timespec sleep = {0, 50L * 1000L * 1000L};
    nanosleep(&sleep, NULL);
  }
  Check(g_prepared == 1, "exactly one kPrepared (what=200) arrived");
  Check(g_completed == 1, "kCompleted (what=300) arrived");
  Check(avbase_player_state(player) == AVBASE_STATE_COMPLETED,
        "state reaches AVBASE_STATE_COMPLETED");
  Check(g_events > 0, "the event channel is live");

  // The buffer contract: report the FULL length even when truncating, and NUL
  // terminate what was written. A caller that sizes from this must not need a
  // second guess.
  char small[8];
  memset(small, 'Z', sizeof(small));
  const int32_t needed =
      avbase_player_dump_diagnostics(player, small, (int32_t)sizeof(small));
  Check(needed > (int32_t)sizeof(small) - 1,
        "dump_diagnostics reports the size it needs");
  Check(small[sizeof(small) - 1] == '\0',
        "dump_diagnostics NUL-terminates inside the capacity");
  Check(avbase_player_dump_diagnostics(player, NULL, 0) == needed,
        "probing with no buffer returns the same size");

  Check(avbase_player_select_track(player, 999, 0) ==
            AVBASE_ERR_INVALID_ARGUMENT,
        "an unknown stream type is rejected, not forwarded");

  // Destroying null must be a no-op so cleanup paths can be unconditional.
  avbase_player_destroy(player);
  avbase_player_destroy(NULL);
  avbase_display_release(NULL);
  avbase_source_release(NULL);
  avbase_global_init();

  printf("%s\n", failures == 0 ? "check_capi: all checks passed"
                               : "check_capi: FAILED");
  return failures == 0 ? 0 : 1;
}
