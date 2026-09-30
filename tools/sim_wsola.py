#!/usr/bin/env python3
"""A numerical model of the WSOLA path, for claims that need measuring.

WHY THIS EXISTS
---------------
`media/filters/audio_renderer_algorithm.cc` was written without a compiler
available, and its unit test carries assertions about *signal processing*
-- "2x halves the duration", "2x preserves pitch to within 2%" -- whose correct
tolerances cannot be derived by reading code. Guessing them produced two test
failures that looked like implementation bugs and were not. This file is a
faithful Python model of the same algorithm, so those numbers can be measured
here, in seconds, instead of being asserted blind or reverse-engineered from a
ctest failure message.

It is a verification harness. It is not part of any build target, it is not run
by CI, and nothing in `media/` depends on it. `check_invariants.py` scans
`.h`/`.cc` only, so it is outside the ratchet too.

WHAT IT MODELS
--------------
`AudioFrameQueue` (Append/SeekFrames/PeekFrames, including the zero-fill),
`wsola_internals` (TimeToFrames, the periodic Hanning window, Similarity,
OptimalIndex) and the whole of `AudioRendererAlgorithm`'s WSOLA branch:
CanPerformWsola, TargetIsWithinSearchRegion, GetOptimalBlock,
UpdateOutputTime, RemoveOldInputFrames, WriteCompletedFramesTo,
RunOneWsolaIteration, RunWsola with its end-of-stream tail drain, and
FillBuffer's mode selection.

Two simplifications, both exact for the tests it reproduces:

* One channel instead of two. The fixture gives every channel the same signal,
  and `Similarity()` is a ratio, so the ranking of candidates is unchanged.
* Passthrough and resampler modes are not modelled. Everything here goes
  through WSOLA, which is the only mode with interesting behaviour.

`--chromium` swaps `OptimalIndex()` for a port of Chromium's real one
(`media/filters/wsola_internals.cc`: moving block energies, a decimated search
at stride 5 with quadratic interpolation of each local maximum, then an
11-candidate full search, and a per-channel similarity measure summed across
channels). That variant exists to answer one question -- whether ijkpp's
exhaustive search is responsible for the pure-sine pitch error -- and the answer
it gives is no: both report 452 Hz on a sine at 2x and 440.0 Hz on the
four-partial tone. See `--check`.

USAGE
-----
    python3 tools/sim_wsola.py --check          # assert the recorded numbers
    python3 tools/sim_wsola.py --check --chromium
    python3 tools/sim_wsola.py --rate 2.0 --stimulus tone
"""

import argparse
import math
import sys

# ---------------------------------------------------------------------------
# Constants, mirrored from AudioRendererAlgorithm::Initialize() at 48 kHz.
# ---------------------------------------------------------------------------

SAMPLE_RATE = 48000
OLA_WINDOW = 960          # TimeToFrames(20ms), forced even
OLA_HOP = OLA_WINDOW // 2
NUM_CANDIDATE_BLOCKS = 1440          # TimeToFrames(30ms)
SEARCH_BLOCK_FRAMES = NUM_CANDIDATE_BLOCKS + (OLA_WINDOW - 1)
SEARCH_BLOCK_CENTER_OFFSET = (NUM_CANDIDATE_BLOCKS // 2 +
                              (OLA_WINDOW // 2 - 1))
EXCLUDE_INTERVAL_FRAMES = 160
WSOLA_OUTPUT_FRAMES = OLA_WINDOW + OLA_HOP

K_SEARCH_DECIMATION = 5   # Chromium: kSearchDecimation
K_EPSILON = 1e-12         # Chromium: kEpsilon in MultiChannelSimilarityMeasure


def periodic_hanning(n):
    """wsola_internals' FillPeriodicHanningWindow / GetPeriodicHanningWindow."""
    step = 2.0 * math.pi / n
    return [0.5 * (1.0 - math.cos(step * i)) for i in range(n)]


# ---------------------------------------------------------------------------
# AudioFrameQueue
# ---------------------------------------------------------------------------

class FrameQueue:
    """A list of buffers plus a front offset, as in audio_frame_queue.cc."""

    def __init__(self):
        self.buffers = []
        self.front_offset = 0

    def append(self, samples):
        self.buffers.append(samples)

    def frames(self):
        return sum(len(b) for b in self.buffers) - self.front_offset

    def clear(self):
        self.buffers = []
        self.front_offset = 0

    def seek_frames(self, n):
        remaining = n
        while remaining > 0 and self.buffers:
            available = len(self.buffers[0]) - self.front_offset
            if remaining >= available:
                remaining -= available
                self.buffers.pop(0)
                self.front_offset = 0
            else:
                self.front_offset += remaining
                remaining = 0

    def peek_frames(self, num_frames, read_offset):
        """Zero-fills whatever the queue cannot supply, as PeekFrames does."""
        out = [0.0] * num_frames
        remaining = num_frames
        dst = 0
        skip = read_offset
        for i, buf in enumerate(self.buffers):
            if remaining <= 0:
                break
            offset = self.front_offset if i == 0 else 0
            available = len(buf) - offset
            if skip >= available:
                skip -= available
                continue
            offset += skip
            available -= skip
            skip = 0
            chunk = min(available, remaining)
            out[dst:dst + chunk] = buf[offset:offset + chunk]
            dst += chunk
            remaining -= chunk
        return out

    def read_frames(self, num_frames):
        available = self.frames()
        produced = min(num_frames, available)
        got = self.peek_frames(produced, 0)
        self.seek_frames(produced)
        return got


# ---------------------------------------------------------------------------
# wsola_internals: ijkpp's exhaustive variant
# ---------------------------------------------------------------------------

def similarity(search, offset, target):
    """Normalised cross-correlation over one channel (see the module docstring).

    ijkpp normalises once over the summed energies; Chromium normalises per
    channel and sums. Identical ranking when all channels carry one signal.
    """
    cross = 0.0
    target_energy = 0.0
    search_energy = 0.0
    for n in range(len(target)):
        tv = target[n]
        sv = search[offset + n]
        cross += tv * sv
        target_energy += tv * tv
        search_energy += sv * sv
    denominator = math.sqrt(target_energy * search_energy)
    if not (denominator > 0.0):
        return 0.0
    return cross / denominator


def optimal_index(search, target, exclude_begin, exclude_end, search_frames=0):
    """ijkpp's OptimalIndex: exhaustive, exact, ~5x Chromium's dot products."""
    available = (min(search_frames, len(search))
                 if search_frames > 0 else len(search))
    last = available - len(target)
    best_index = 0
    best_similarity = -2.0
    for n in range(0, last + 1):
        if exclude_begin <= n < exclude_end:
            continue
        score = similarity(search, n, target)
        if score > best_similarity:
            best_similarity = score
            best_index = n
    return best_index


# ---------------------------------------------------------------------------
# wsola_internals: Chromium's decimated variant (--chromium)
# ---------------------------------------------------------------------------

def _in_interval(n, interval):
    """Chromium's InInterval is closed at both ends; ijkpp's is half-open."""
    return interval[0] <= n <= interval[1]


def _dot_product(a, off_a, b, off_b, num_frames):
    return [sum(a[c][off_a + i] * b[c][off_b + i] for i in range(num_frames))
            for c in range(len(a))]


def _similarity_measure(dot_prod, energy_a, energy_b):
    total = 0.0
    for n in range(len(dot_prod)):
        total += dot_prod[n] / math.sqrt(energy_a[n] * energy_b[n] + K_EPSILON)
    return total


def _moving_block_energies(bus, frames_per_block):
    num_blocks = len(bus[0]) - (frames_per_block - 1)
    channels = len(bus)
    energy = [0.0] * (num_blocks * channels)
    for k in range(channels):
        ch = bus[k]
        energy[k] = sum(ch[i] * ch[i] for i in range(frames_per_block))
        for n in range(1, num_blocks):
            out_v = ch[n - 1]
            in_v = ch[n - 1 + frames_per_block]
            energy[k + n * channels] = (energy[k + (n - 1) * channels] -
                                        out_v * out_v + in_v * in_v)
    return energy


def _quadratic_interpolation(y):
    """Chromium: fit a parabola through three samples, return its peak."""
    a = 0.5 * (y[2] + y[0]) - y[1]
    b = 0.5 * (y[2] - y[0])
    c = y[1]
    if a == 0.0:
        return 0.0, y[1]
    extremum = -b / (2.0 * a)
    return extremum, a * extremum * extremum + b * extremum + c


def _decimated_search(exclude_interval, target, search, energy_target,
                      energy_candidate):
    channels = len(search)
    block_size = len(target[0])
    num_candidates = len(search[0]) - (block_size - 1)
    sim = [0.0, 0.0, 0.0]

    def score(n):
        return _similarity_measure(
            _dot_product(target, 0, search, n, block_size), energy_target,
            energy_candidate[n * channels:n * channels + channels])

    sim[0] = score(0)
    best_similarity = sim[0]
    best_index = 0

    n = K_SEARCH_DECIMATION
    if n >= num_candidates:
        return 0
    sim[1] = score(n)
    n += K_SEARCH_DECIMATION
    if n >= num_candidates:
        return K_SEARCH_DECIMATION if sim[1] > sim[0] else 0

    while n < num_candidates:
        sim[2] = score(n)
        if ((sim[1] > sim[0] and sim[1] >= sim[2]) or
                (sim[1] >= sim[0] and sim[1] > sim[2])):
            normalized, candidate_similarity = _quadratic_interpolation(sim)
            candidate = (n - K_SEARCH_DECIMATION +
                         int(normalized * K_SEARCH_DECIMATION + 0.5))
            if (candidate_similarity > best_similarity and
                    not _in_interval(candidate, exclude_interval)):
                best_index = candidate
                best_similarity = candidate_similarity
        elif (n + K_SEARCH_DECIMATION >= num_candidates and
              sim[2] > best_similarity and not _in_interval(n, exclude_interval)):
            best_index = n
            best_similarity = sim[2]
        sim[0] = sim[1]
        sim[1] = sim[2]
        n += K_SEARCH_DECIMATION
    return best_index


def chromium_optimal_index(search_block, target_block, exclude_interval):
    """Chromium's OptimalIndex. Buses here are lists of per-channel lists."""
    target_size = len(target_block[0])
    num_candidates = len(search_block[0]) - (target_size - 1)
    energy_candidate = _moving_block_energies(search_block, target_size)
    energy_target = _dot_product(target_block, 0, target_block, 0, target_size)
    coarse = _decimated_search(exclude_interval, target_block, search_block,
                               energy_target, energy_candidate)
    low = 0 if coarse < K_SEARCH_DECIMATION else coarse - K_SEARCH_DECIMATION
    high = min(num_candidates - 1, coarse + K_SEARCH_DECIMATION)
    best_similarity = float(1e-38)   # std::numeric_limits<float>::min()
    best_index = 0
    channels = len(search_block)
    for n in range(low, high + 1):
        if _in_interval(n, exclude_interval):
            continue
        score = _similarity_measure(
            _dot_product(target_block, 0, search_block, n, target_size),
            energy_target,
            energy_candidate[n * channels:n * channels + channels])
        if score > best_similarity:
            best_similarity = score
            best_index = n
    return best_index


# ---------------------------------------------------------------------------
# AudioRendererAlgorithm, WSOLA branch
# ---------------------------------------------------------------------------

class Algorithm:
    def __init__(self, use_chromium_search=False):
        self.queue = FrameQueue()
        self.use_chromium_search = use_chromium_search
        self.output_time = 0.0
        self.search_block_index = 0
        self.target_block_index = 0
        self.num_complete_frames = 0
        self.reached_end_of_stream = False
        self.ola_window = periodic_hanning(OLA_WINDOW)
        self.transition_window = periodic_hanning(OLA_WINDOW * 2)
        self.wsola_output = [0.0] * WSOLA_OUTPUT_FRAMES
        # instrumentation, for --report
        self.iterations = 0
        self.search_calls = 0
        self.within_region_calls = 0
        self.frames_seeked = 0
        self.tail_frames = 0

    # -- queue-facing ------------------------------------------------------
    def enqueue_buffer(self, samples):
        self.queue.append(samples)

    def mark_end_of_stream(self):
        self.reached_end_of_stream = True

    def buffered_frames(self):
        return self.queue.frames()

    # -- index bookkeeping -------------------------------------------------
    def search_block_frames(self):
        return SEARCH_BLOCK_FRAMES

    def effective_search_block_frames(self):
        full = self.search_block_frames()
        if not self.reached_end_of_stream:
            return full
        # A negative search_block_index_ means PeekAudioWithZeroPrepend()
        # prepends zeros, so subtracting it counts that prepend as content,
        # which is what bounds the candidate range.
        logical = self.buffered_frames() - self.search_block_index
        if logical < OLA_WINDOW:
            return 0
        return min(full, logical)

    def can_perform_wsola(self):
        size = self.effective_search_block_frames()
        if size == 0:
            return False
        frames = self.buffered_frames()
        return (self.target_block_index + OLA_WINDOW <= frames and
                self.search_block_index + size <= frames)

    def target_is_within_search_region(self):
        size = self.effective_search_block_frames()
        if size == 0:
            return False
        return (self.target_block_index >= self.search_block_index and
                self.target_block_index + OLA_WINDOW <=
                self.search_block_index + size)

    def peek_audio_with_zero_prepend(self, read_offset, num_frames):
        dest = [0.0] * num_frames
        write_offset = 0
        to_read = num_frames
        if read_offset < 0:
            zeros = min(-read_offset, num_frames)
            read_offset = 0
            to_read -= zeros
            write_offset = zeros
        if to_read > 0:
            got = self.queue.peek_frames(to_read, read_offset)
            dest[write_offset:write_offset + to_read] = got
        return dest

    def update_output_time(self, playback_rate, time_change):
        self.output_time += time_change
        center = int(self.output_time * playback_rate + 0.5)
        self.search_block_index = center - SEARCH_BLOCK_CENTER_OFFSET

    def remove_old_input_frames(self, playback_rate):
        earliest = min(self.target_block_index, self.search_block_index)
        if earliest <= 0:
            return
        self.queue.seek_frames(earliest)
        self.frames_seeked += earliest
        self.target_block_index -= earliest
        self.update_output_time(playback_rate,
                                -float(earliest) / playback_rate)

    # -- one iteration -----------------------------------------------------
    def get_optimal_block(self):
        if self.target_is_within_search_region():
            self.within_region_calls += 1
            optimal = self.target_block_index
            block = self.peek_audio_with_zero_prepend(optimal, OLA_WINDOW)
        else:
            self.search_calls += 1
            target_block = self.peek_audio_with_zero_prepend(
                self.target_block_index, OLA_WINDOW)
            search_block = self.peek_audio_with_zero_prepend(
                self.search_block_index, SEARCH_BLOCK_FRAMES)
            last_optimal = (self.target_block_index - OLA_HOP -
                            self.search_block_index)
            half = EXCLUDE_INTERVAL_FRAMES // 2
            if self.use_chromium_search:
                relative = chromium_optimal_index(
                    [search_block], [target_block],
                    (last_optimal - half, last_optimal + half))
            else:
                relative = optimal_index(
                    search_block, target_block, last_optimal - half,
                    last_optimal + half,
                    self.effective_search_block_frames())
            optimal = relative + self.search_block_index
            block = self.peek_audio_with_zero_prepend(optimal, OLA_WINDOW)
            window = self.transition_window
            for n in range(OLA_WINDOW):
                block[n] = (block[n] * window[n] +
                            target_block[n] * window[OLA_WINDOW + n])
        self.target_block_index = optimal + OLA_HOP
        return block

    def run_one_wsola_iteration(self, playback_rate):
        if not self.can_perform_wsola():
            return False
        # The room check ijkpp has and Chromium does not: FillBuffer() accepts
        # an arbitrary requested_frames, so num_complete_frames_ can otherwise
        # grow past wsola_output_.
        if self.num_complete_frames + OLA_WINDOW > WSOLA_OUTPUT_FRAMES:
            return False
        block = self.get_optimal_block()
        self.iterations += 1
        base = self.num_complete_frames
        for n in range(OLA_HOP):
            self.wsola_output[base + n] = (
                self.wsola_output[base + n] * self.ola_window[OLA_HOP + n] +
                block[n] * self.ola_window[n])
        for n in range(OLA_HOP, OLA_WINDOW):
            self.wsola_output[base + n] = block[n]
        self.num_complete_frames += OLA_HOP
        self.update_output_time(playback_rate, OLA_HOP)
        self.remove_old_input_frames(playback_rate)
        return True

    def write_completed_frames_to(self, requested_frames):
        rendered = min(self.num_complete_frames, requested_frames)
        if rendered <= 0:
            return []
        out = self.wsola_output[:rendered]
        self.wsola_output = (self.wsola_output[rendered:] +
                             [0.0] * rendered)
        self.num_complete_frames -= rendered
        return out

    def run_wsola(self, requested_frames, playback_rate):
        collected = []
        while True:
            chunk = self.write_completed_frames_to(
                requested_frames - len(collected))
            collected.extend(chunk)
            if len(collected) >= requested_frames:
                break
            if not self.run_one_wsola_iteration(playback_rate):
                break
        # The end-of-stream tail drain: ijkpp's, not Chromium's. Bounded to
        # under one window by effective_search_block_frames().
        if (self.reached_end_of_stream and
                len(collected) < requested_frames and
                not self.can_perform_wsola()):
            got = self.queue.read_frames(requested_frames - len(collected))
            collected.extend(got)
            self.frames_seeked += len(got)
            self.tail_frames += len(got)
        return collected

    def fill_buffer(self, requested_frames, playback_rate):
        if requested_frames <= 0 or playback_rate <= 0.0:
            return []
        return self.run_wsola(requested_frames, playback_rate)


# ---------------------------------------------------------------------------
# Stimuli and measurement, mirroring the unit test
# ---------------------------------------------------------------------------

TONE_FUNDAMENTAL = 440.0
TONE_RATIOS = (1.0, 2.00227, 2.99318, 4.87045)
TONE_GAINS = (0.50, 0.35, 0.25, 0.15)
TONE_PHASES = (0.0, 0.7, 2.1, 4.0)


def sine(frames, start_sample, freq=440.0, amplitude=0.5):
    """MakeSineBuffer: phase relative to the buffer start, so |start_sample|
    is ignored. The discontinuity that causes is documented on the C++ side."""
    return [amplitude * math.sin(2.0 * math.pi * freq * n / SAMPLE_RATE)
            for n in range(frames)]


def tone(frames, start_sample):
    """MakeToneBuffer: four detuned partials, phase-continuous across buffers."""
    out = []
    for i in range(frames):
        t = (start_sample + i) / SAMPLE_RATE
        sample = 0.0
        for k, ratio in enumerate(TONE_RATIOS):
            sample += TONE_GAINS[k] * math.sin(
                2.0 * math.pi * TONE_FUNDAMENTAL * ratio * t + TONE_PHASES[k])
        out.append(sample)
    return out


def feed(algorithm, seconds, generator, frames_per_buffer=4096):
    total = int(seconds * SAMPLE_RATE)
    done = 0
    while done < total:
        n = min(frames_per_buffer, total - done)
        algorithm.enqueue_buffer(generator(n, done))
        done += n
    algorithm.mark_end_of_stream()
    return total


def drain(algorithm, rate, frames_per_call, idle_limit=4):
    """The fixture's Drain(): stop after |idle_limit| calls that yield nothing."""
    out = []
    idle = 0
    while idle < idle_limit:
        got = algorithm.fill_buffer(frames_per_call, rate)
        if not got:
            idle += 1
            continue
        idle = 0
        out.extend(got)
    return out


def dominant_frequency(x, lo_hz, hi_hz):
    """The test's DominantFrequency: naive DFT on a 1 Hz grid."""
    best_freq = lo_hz
    best_magnitude = -1.0
    freq = lo_hz
    while freq <= hi_hz:
        omega = 2.0 * math.pi * freq / SAMPLE_RATE
        re = 0.0
        im = 0.0
        for n, sample in enumerate(x):
            re += sample * math.cos(omega * n)
            im -= sample * math.sin(omega * n)
        magnitude = re * re + im * im
        if magnitude > best_magnitude:
            best_magnitude = magnitude
            best_freq = freq
        freq += 1.0
    return best_freq


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------

def measure(generator, rate, seconds, use_chromium_search):
    algorithm = Algorithm(use_chromium_search)
    total_input = feed(algorithm, seconds, generator)
    rendered = drain(algorithm, rate, 1024)
    wsola_only = len(rendered) - algorithm.tail_frames
    ideal = int(total_input / rate)
    window = rendered[2 * OLA_WINDOW:2 * OLA_WINDOW + 4096]
    peak = (dominant_frequency(window, TONE_FUNDAMENTAL * 0.9,
                               TONE_FUNDAMENTAL * 1.1)
            if len(window) == 4096 else float("nan"))
    return {
        "rendered": len(rendered),
        "ideal": ideal,
        "duration_delta": len(rendered) - ideal,
        "duration_ok": abs(len(rendered) - ideal) <= 2 * OLA_WINDOW,
        "peak_hz": peak,
        "pitch_ok": abs(peak - TONE_FUNDAMENTAL) <= TONE_FUNDAMENTAL * 0.02,
        "tail_frames": algorithm.tail_frames,
        "wsola_frames": wsola_only,
        "wsola_rate": (algorithm.frames_seeked - algorithm.tail_frames) /
        wsola_only if wsola_only else float("nan"),
        "iterations": algorithm.iterations,
        "search_calls": algorithm.search_calls,
        "within_region_calls": algorithm.within_region_calls,
    }


# The four numbers this harness exists to reproduce, measured 2026-09-29. Each
# is a claim made in a comment in the C++ or in docs/PROGRESS.md; --check
# re-derives them so a future edit to the algorithm cannot leave the claim
# quietly stale. Peaks use exactly the unit test's method -- a naive DFT on a
# 1 Hz grid over a rectangular 4096-sample window taken from 2 * OLA_WINDOW --
# because a windowed or finer-grained variant gives different numbers on the
# degenerate stimulus (443.5 Hz rather than 440.0 Hz for sine at 0.5x), and the
# point is to reproduce what the test asserts, not what the signal is.
RECORDED = [
    # (stimulus, rate, seconds, rendered, peak_hz, note)
    ("sine", 2.0, 1.0, 25199, 452.0,
     "duration PASSes; pitch FAILs -- the sine is degenerate, not the port"),
    ("sine", 0.5, 1.0, 96239, 440.0,
     "both pass even on the degenerate stimulus: slowing down spreads the "
     "search region instead of compressing it, so the near-ties resolve "
     "closer to in-phase. That is why 0.5x never showed the problem"),
    ("tone", 2.0, 1.0, 25192, 440.0, "the acceptance case: pitch exact"),
    ("tone", 0.5, 0.5, 48239, 440.0, "pitch exact slowing down too"),
]

# The pre-fix number, for the record: with a full-width search block at end of
# stream the raw tail drain dumps all 2399 frames at 1x whatever the rate.
PRE_FIX_RENDERED_SINE_2X = 26066


def run_check(use_chromium_search):
    generators = {"sine": sine, "tone": tone}
    failures = 0
    print(f"OptimalIndex: "
          f"{'Chromium decimated' if use_chromium_search else 'ijkpp exhaustive'}")
    print()
    for name, rate, seconds, want_rendered, want_peak, note in RECORDED:
        got = measure(generators[name], rate, seconds, use_chromium_search)
        # Chromium's search picks slightly different blocks, so its frame
        # counts differ by a few hundred; the pitch is what must agree exactly,
        # and that is the claim under test.
        rendered_ok = (abs(got["rendered"] - want_rendered) <= 400
                       if use_chromium_search
                       else got["rendered"] == want_rendered)
        peak_ok = abs(got["peak_hz"] - want_peak) < 0.51
        ok = rendered_ok and peak_ok
        failures += 0 if ok else 1
        print(f"  {name:4s} rate={rate:<4} rendered={got['rendered']:6d} "
              f"(recorded {want_rendered:6d}) {'ok ' if rendered_ok else 'DIFF'}"
              f"  peak={got['peak_hz']:6.1f} Hz (recorded {want_peak:6.1f}) "
              f"{'ok ' if peak_ok else 'DIFF'}")
        print(f"       duration {'PASS' if got['duration_ok'] else 'FAIL'} "
              f"(delta {got['duration_delta']:+d}, tolerance "
              f"{2 * OLA_WINDOW})   pitch "
              f"{'PASS' if got['pitch_ok'] else 'FAIL'}   {note}")
    print()
    if not use_chromium_search:
        print(f"  pre-fix reference: sine at 2x rendered "
              f"{PRE_FIX_RENDERED_SINE_2X} frames before the search region was "
              f"shrunk at end of stream, i.e. {PRE_FIX_RENDERED_SINE_2X - 24000:+d}")
        print("  against an ideal 24000 with a tolerance of 1920 -- the "
              "RateTwoHalvesTheDuration failure.")
    print()
    print(f"sim_wsola: {'all recorded numbers reproduced' if not failures else str(failures) + ' MISMATCH'}")
    return 1 if failures else 0


def run_one(generator_name, rate, seconds, use_chromium_search):
    generators = {"sine": sine, "tone": tone}
    algorithm = Algorithm(use_chromium_search)
    total = feed(algorithm, seconds, generators[generator_name])
    rendered = drain(algorithm, rate, 1024)
    wsola_only = len(rendered) - algorithm.tail_frames
    print(f"stimulus={generator_name} rate={rate} input={total} frames")
    print(f"  rendered            : {len(rendered)}  (ideal {int(total / rate)}, "
          f"delta {len(rendered) - int(total / rate):+d}, "
          f"tolerance {2 * OLA_WINDOW})")
    print(f"  wsola iterations    : {algorithm.iterations} "
          f"(within-region {algorithm.within_region_calls}, "
          f"search {algorithm.search_calls})")
    print(f"  wsola-only frames   : {wsola_only}, achieved rate "
          f"{(algorithm.frames_seeked - algorithm.tail_frames) / wsola_only:.4f}"
          if wsola_only else "  wsola-only frames   : 0")
    print(f"  end-of-stream tail  : {algorithm.tail_frames} raw frames")
    window = rendered[2 * OLA_WINDOW:2 * OLA_WINDOW + 4096]
    if len(window) == 4096:
        peak = dominant_frequency(window, TONE_FUNDAMENTAL * 0.9,
                                  TONE_FUNDAMENTAL * 1.1)
        print(f"  dominant frequency  : {peak:.1f} Hz "
              f"(fundamental {TONE_FUNDAMENTAL:.1f}, "
              f"error {100 * (peak - TONE_FUNDAMENTAL) / TONE_FUNDAMENTAL:+.2f}%)")
    else:
        print("  dominant frequency  : not enough output to measure")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="re-derive the four recorded numbers and compare")
    parser.add_argument("--chromium", action="store_true",
                        help="use Chromium's decimated OptimalIndex instead of "
                             "ijkpp's exhaustive one")
    parser.add_argument("--rate", type=float, default=2.0)
    parser.add_argument("--seconds", type=float, default=1.0)
    parser.add_argument("--stimulus", choices=("sine", "tone"), default="tone")
    args = parser.parse_args(argv)
    if args.check:
        return run_check(args.chromium)
    run_one(args.stimulus, args.rate, args.seconds, args.chromium)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
