// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Standalone driver for the fuzz harness: runs every seed corpus file
// verbatim, then a fixed-seed series of deterministic mutations of them.
// It exists because the libFuzzer runtime is not universally available
// (Apple's Xcode clang ships without it), and a machine that cannot run
// coverage-guided fuzzing still needs to run the corpus in CI and under the
// sanitizer presets. Deterministic: fixed PRNG seed, fixed iteration count,
// so a crash is reproducible from the command line alone.
//
// usage: avbase_fuzz_demuxer_standalone [--iters n] [--file f]...

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

constexpr int kDefaultItersPerSeed = 64;
constexpr size_t kMaxMutatedSize = 1u << 20;

// One of each mutation class per round, so the corpus gets uniform exposure:
// bit flips (header corruption), truncation (moov at the end), byte stuffing
// (length-field lies), splicing (cross-container chimera).
std::vector<uint8_t> Mutate(const std::vector<uint8_t>& input,
                            std::mt19937& rng, int round) {
  std::vector<uint8_t> out = input;
  const size_t last = out.empty() ? 0 : out.size() - 1;
  std::uniform_int_distribution<size_t> pos_pick(0, last);
  std::uniform_int_distribution<int> byte_pick(0, 255);
  switch (round % 4) {
    case 0: {   // Bit flips: 8 random bits.
      for (int i = 0; i < 8 && !out.empty(); ++i) {
        const size_t pos = pos_pick(rng);
        out[pos] ^= static_cast<uint8_t>(1u << (round % 8));
      }
      break;
    }
    case 1: {   // Truncate: simulate a cut-off tail (mdat, moov).
      if (!out.empty()) {
        const size_t pos = pos_pick(rng);
        out.resize(pos);
      }
      break;
    }
    case 2: {   // Stuff: lie about a length field / splice in garbage.
      const size_t pos = out.empty() ? 0 : pos_pick(rng);
      const size_t count = 1 + (rng() % 32);
      for (size_t i = 0; i < count && pos + i < out.size(); ++i) {
        out[pos + i] = static_cast<uint8_t>(byte_pick(rng));
      }
      break;
    }
    case 3: {   // Splice: copy a slice of one file into another offset.
      if (out.size() > 16) {
        const size_t src = pos_pick(rng);
        const size_t dst = pos_pick(rng);
        const size_t len = 1 + (rng() % 64);
        for (size_t i = 0;
             i < len && src + i < out.size() && dst + i < out.size(); ++i) {
          out[dst + i] = out[src + i];
        }
      }
      break;
    }
  }
  if (out.size() > kMaxMutatedSize) {
    out.resize(kMaxMutatedSize);
  }
  return out;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  std::vector<uint8_t> bytes;
  if (f) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      bytes.insert(bytes.end(), buf, buf + n);
    }
    std::fclose(f);
  }
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {
  int iters = kDefaultItersPerSeed;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--iters" && i + 1 < argc) {
      iters = std::atoi(argv[++i]);
    } else {
      files.push_back(arg);
    }
  }
  if (files.empty()) {
    // Default corpus: the in-repo seeds.
    files = {
        AVBASE_SEEDS_DIR "/small_h264_aac_3s.mp4",
        AVBASE_SEEDS_DIR "/audio_only.m4a",
        AVBASE_SEEDS_DIR "/video_only.mp4",
        AVBASE_SEEDS_DIR "/truncated_header.mp4",
        AVBASE_SEEDS_DIR "/corrupt_prefix.mp4",
    };
  }

  std::mt19937 rng(0xA7B45E1u);   // Fixed: crashes must be reproducible.
  int inputs = 0;
  for (const std::string& file : files) {
    const std::vector<uint8_t> original = ReadFile(file);
    if (original.empty()) {
      std::fprintf(stderr, "fuzz-standalone: cannot read %s\n", file.c_str());
      continue;
    }
    // The seed itself, verbatim: the corpus regresses every run.
    LLVMFuzzerTestOneInput(original.data(), original.size());
    ++inputs;
    for (int round = 0; round < iters; ++round) {
      const std::vector<uint8_t> mutated = Mutate(original, rng, round);
      LLVMFuzzerTestOneInput(mutated.data(), mutated.size());
      ++inputs;
    }
    std::printf("fuzz-standalone: %s -> %d inputs, no crash\n", file.c_str(),
                iters + 1);
  }
  std::printf("fuzz-standalone: %d inputs total, exit clean\n", inputs);
  return 0;
}
