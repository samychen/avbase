#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Shared JSONL helpers for the tools/ scripts.

Both check_bench_regression.py and golden_diff.py parse the same one-record-per-
line JSONL shape (blank lines skipped). Keeping the three-line loop in one place
stops the two from drifting if the format ever gains a wrinkle.
"""
import json
from pathlib import Path


def load_jsonl(path) -> list:
    """Parse a JSONL file into a list of records, skipping blank lines.

    |path| may be a str, Path, or any os.PathLike.
    """
    rows = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            rows.append(json.loads(line))
    return rows
