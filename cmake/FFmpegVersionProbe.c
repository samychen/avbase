/* Copyright 2026 The avbase Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Linked against the actual libavcodec FindFFmpeg selected and executed at
 * configure time (try_run): prints the LIBRARY's avcodec_version() so CMake
 * can compare it with the version parsed from the headers. A mismatch here
 * is the silent-SIGBUS class of failure — headers from one build, dylib/so
 * from another — which otherwise has no compile-time symptom.
 */
#include <libavcodec/avcodec.h>
#include <stdio.h>

int main(void)
{
    printf("%u\n", avcodec_version());
    return 0;
}
