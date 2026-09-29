# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""The ported-constant table: what ijkpp inherited from ffplay, and where each
value lives.

Data for tools/extract_constants.py, kept in its own module for two reasons:
the table is the part that grows every milestone (M9 adds the buffering
constants, M12 the display ones), and a reviewer auditing 'are the A/V sync
thresholds still the ffplay ones?' should be able to read one file top to
bottom without wading through a parser.

Units are declared here, never inferred. ffplay is not self-consistent:
AV_SYNC_THRESHOLD_MIN is 0.04 (seconds) while MAX_SLEEP is 1000000
(microseconds), and AV_NOSYNC_THRESHOLD is 10.0 -- an *integral* number of
seconds that no 'is it a float?' heuristic can tell apart from 10 of anything
else. So every Spec carries `source_unit`.
"""

from __future__ import annotations


from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

# Canonical units. Every value is converted to its spec's unit before
# comparison, so 0.04 s in ffplay, base::Milliseconds(40) in ijkpp and 0.04 in
# docs/05 all meet as 40:
#   s -> seconds   ms -> milliseconds   us -> microseconds
#   b -> bytes     n -> count           r -> raw float (ratio / coefficient)
UNITS_PER_SECOND = {"s": 1.0, "ms": 1000.0, "us": 1_000_000.0}
TIME_UNITS = frozenset(UNITS_PER_SECOND)
BASE_TIME_KINDS = {"Seconds": "s", "Milliseconds": "ms", "Microseconds": "us"}

FATAL = ("MISMATCH", "MISMATCH_VS_DOCS", "LOCATIONS_DISAGREE", "NOT_IN_IJKPP",
         "NOT_IN_FFP", "DOCS_MISMATCH")



class Spec:
    """One ported constant and where each of its values lives."""

    def __init__(self, macro, unit, ijkpp=(), docs=None, source_unit=None,
                 deviation=None, note=""):
        self.macro = macro        # ffplay #define name, or None if ijkpp-only
        self.unit = unit          # canonical unit
        self.ijkpp = ijkpp        # tuple of (path, symbol); all must agree
        self.docs = docs          # expected value, written in source_unit
        self.source_unit = source_unit or unit
        self.deviation = deviation
        self.note = note


MC = "media/base/media_constants.h"
PC = "player/public/player_config.h"
ASC = "media/filters/legacy/av_sync_controller.h"
VFC = "media/filters/legacy/video_frame_compositor.h"

SPEC: list[Spec] = [
    # ---- buffer / queue sizing ----
    Spec("MAX_QUEUE_SIZE", "b",
         ((MC, "kDefaultMaxBufferBytes"), (PC, "BufferConfig::max_bytes")),
         15 * 1024 * 1024),
    Spec("MIN_MIN_FRAMES", "n", ((MC, "kMinMinFrames"),), 2),
    Spec("DEFAULT_MIN_FRAMES", "n",
         ((MC, "kDefaultMinFrames"), (PC, "BufferConfig::min_frames")), 5,
         note="docs/05 warns this one differs between forks"),
    Spec("MAX_MIN_FRAMES", "n", ((MC, "kMaxMinFrames"),), 50),
    Spec("VIDEO_PICTURE_QUEUE_SIZE_DEFAULT", "n",
         ((MC, "kVideoFrameQueueSizeDefault"),
          (PC, "VideoConfig::frame_queue_size")), 3),
    Spec("VIDEO_PICTURE_QUEUE_SIZE_MIN", "n",
         ((MC, "kVideoFrameQueueSizeMin"),), 2),
    Spec("VIDEO_PICTURE_QUEUE_SIZE_MAX", "n",
         ((MC, "kVideoFrameQueueSizeMax"),), 16),
    Spec("AUDIO_PICTURE_QUEUE_SIZE_DEFAULT", "n",
         ((MC, "kAudioFrameQueueSizeDefault"),
          (PC, "AudioConfig::frame_queue_size")), 9),
    Spec("SUBPICTURE_QUEUE_SIZE", "n", ((MC, "kTextFrameQueueSizeDefault"),), 16,
         note="docs/05 calls it kTextFrameQueueSize; the code says ...Default"),
    # ---- buffering high water marks ----
    Spec("DEFAULT_FIRST_HIGH_WATER_MARK_IN_MS", "ms",
         ((PC, "BufferConfig::first_high_water_mark"),), 100),
    Spec("DEFAULT_NEXT_HIGH_WATER_MARK_IN_MS", "ms",
         ((PC, "BufferConfig::next_high_water_mark"),), 1000),
    Spec("DEFAULT_LAST_HIGH_WATER_MARK_IN_MS", "ms",
         ((PC, "BufferConfig::last_high_water_mark"),), 5000),
    Spec("BUFFERING_CHECK_PER_MILLISECONDS", "ms", (), 500,
         note="consumer is player/buffer_controller.cc, not written yet (M9)"),
    Spec("BUFFERING_UPDATE_PER_MILLISECONDS", "ms", (), None, deviation="Δ4",
         note="ijkpp emits progress every 200ms; ffplay's 1000ms is the "
              "baseline the deviation is measured against, so there is "
              "deliberately no ijkpp symbol to compare"),
    Spec("MAX_ACCURATE_SEEK_TIMEOUT", "ms",
         ((PC, "SeekConfig::accurate_timeout"),), 5000),
    # ---- A/V sync: the R1 thresholds. source_unit="s" because ffplay spells
    # every one of these as a double number of seconds, including 10.0.
    Spec("AV_SYNC_THRESHOLD_MIN", "ms",
         ((VFC, "Thresholds::av_sync_threshold_min"),), 0.04, source_unit="s"),
    Spec("AV_SYNC_THRESHOLD_MAX", "ms",
         ((VFC, "Thresholds::av_sync_threshold_max"),), 0.1, source_unit="s"),
    Spec("AV_NOSYNC_THRESHOLD", "ms",
         ((VFC, "Thresholds::no_sync_threshold"),
          (ASC, "Thresholds::no_sync_threshold")), 10.0, source_unit="s"),
    Spec("AV_SYNC_FRAMEDUP_THRESHOLD", "ms",
         ((VFC, "Thresholds::sync_framedup_threshold"),), 0.01, source_unit="s",
         note="the code comment says '0.1 in ffplay' but the value is 10ms; "
              "ffplay's actual #define settles it -- run with --ijkplayer"),
    Spec("AV_DIFF_AVG_NB", "n", ((ASC, "Thresholds::audio_diff_avg_count"),), 10),
    Spec("AV_DIFF_AVG_COEF", "r", ((ASC, "Thresholds::audio_diff_avg_coef"),), 0.9),
    Spec("AV_DIFF_THRESHOLD", "r",
         ((ASC, "Thresholds::audio_diff_threshold"),), 0.1),
    Spec("SAMPLE_CORRECTION_PERCENT_MAX", "n",
         ((ASC, "Thresholds::sample_correction_percent_max"),), 10),
    Spec("MAX_SLEEP", "us", ((VFC, "Thresholds::max_sleep"),), 1_000_000),
    # ---- ijkpp-only: no ffplay counterpart, so nothing to cross-check.
    # Listed so that "every threshold is accounted for" stays true.
    Spec(None, "us", ((VFC, "Thresholds::min_sleep"),), None,
         note="ijkpp addition (docs/05 table 7, marked 🆕)"),
    Spec(None, "ms", ((VFC, "Thresholds::max_sane_frame_duration"),), None,
         note="ijkpp addition; ffplay hardcodes 10.0 inline instead"),
]

FFPLAY_FILES = ("ff_ffplay_def.h", "ff_ffplay_options.h", "ff_ffplay.c",
                "ff_ffplay.h", "ff_ffplay_inc.h", "ffplay.c")
DOCS_TABLE = "docs/05-迁移对照表.md"


# ---------------------------------------------------------------------------
# Self-test. Proves the parser works without an ijkplayer checkout so CI can
# gate on it. The synthetic values are the upstream ffplay / ijkplayer ones; if
# a real checkout disagrees, --ijkplayer says so and the checkout wins -- that
# is the entire point of the tool.
# ---------------------------------------------------------------------------

SELFTEST_DEFINES = """
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_MIN_FRAMES 2
#define DEFAULT_MIN_FRAMES 5
#define MAX_MIN_FRAMES 50
#define VIDEO_PICTURE_QUEUE_SIZE_DEFAULT 3
#define VIDEO_PICTURE_QUEUE_SIZE_MIN 2
#define VIDEO_PICTURE_QUEUE_SIZE_MAX 16
#define AUDIO_PICTURE_QUEUE_SIZE_DEFAULT 9
#define SUBPICTURE_QUEUE_SIZE 16
#define DEFAULT_FIRST_HIGH_WATER_MARK_IN_MS 100
#define DEFAULT_NEXT_HIGH_WATER_MARK_IN_MS 1000
#define DEFAULT_LAST_HIGH_WATER_MARK_IN_MS 5000
#define BUFFERING_CHECK_PER_MILLISECONDS 500
#define BUFFERING_UPDATE_PER_MILLISECONDS 1000
#define MAX_ACCURATE_SEEK_TIMEOUT 5000
#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_NOSYNC_THRESHOLD 10.0
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.01
#define AV_DIFF_AVG_NB 10
#define AV_DIFF_AVG_COEF 0.9
#define AV_DIFF_THRESHOLD 0.1
#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define MAX_SLEEP 1000000
#define DERIVED_SLEEP (MAX_SLEEP / 2)
/* non-constant macros: the parser must refuse these, not guess */
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))
"""

SELFTEST_EXPECT = [("MAX_QUEUE_SIZE", 15 * 1024 * 1024),
                   ("AV_SYNC_THRESHOLD_MIN", 0.04),
                   ("AV_NOSYNC_THRESHOLD", 10.0),
                   ("MAX_SLEEP", 1000000),
                   ("DERIVED_SLEEP", 500000),
                   ("DEFAULT_LAST_HIGH_WATER_MARK_IN_MS", 5000)]

# (value, kind, canonical unit, source unit, expected canonical)
SELFTEST_CANON = [
    (0.04, "", "ms", "s", 40.0),
    (10.0, "", "ms", "s", 10_000.0),          # integral seconds: the case a
    (0.01, "", "ms", "s", 10.0),              # "is it a float?" heuristic gets
    (1_000_000, "", "us", "us", 1_000_000.0),  # wrong
    (0.9, "", "r", "r", 0.9),
    (15 * 1024 * 1024, "", "b", "b", 15_728_640.0),
    (100, "Milliseconds", "ms", None, 100.0),
    (5, "Seconds", "ms", None, 5000.0),
    (1, "Seconds", "us", None, 1_000_000.0),
    (500, "Microseconds", "us", None, 500.0),
    (10, "Seconds", "ms", None, 10_000.0),
]


def convert(value, from_unit: str, to_unit: str):
    """Rescales between two time units; non-time units pass through.

    UNITS_PER_SECOND[x] is how many x fit in one second, so converting
    s -> ms multiplies by 1000/1 and ms -> s divides by it.
    """
    if value is None:
        return None
    value = float(value)
    if from_unit in TIME_UNITS and to_unit in TIME_UNITS:
        return value * UNITS_PER_SECOND[to_unit] / UNITS_PER_SECOND[from_unit]
    return value


def canonicalise(value, kind: str, unit: str, source_unit: str | None = None):
    """Normalises a literal to the spec's canonical unit.

    |kind| is the base:: helper an ijkpp literal used ("Seconds",
    "Milliseconds", "Microseconds"), which is authoritative. Otherwise
    |source_unit| says what unit the literal was written in -- required, because
    ffplay mixes seconds (0.04, 10.0) and microseconds (MAX_SLEEP 1000000) and
    no heuristic can tell 10.0 seconds from 10 of anything else.
    """
    if value is None:
        return None
    value = float(value)
    origin = BASE_TIME_KINDS.get(kind, source_unit or unit)
    return convert(value, origin, unit)
