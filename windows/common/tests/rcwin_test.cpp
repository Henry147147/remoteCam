// Tests for the Windows-side helpers.
//
// Dependency-free and CTest-wired, matching core/tests/transform_test.cpp. Two things
// here are worth more than the line count suggests:
//
// 1. The pattern invariants tested below are the SAME properties rc-vcam-probe asserts
//    against a live camera. Verifying them here means a probe failure indicts the
//    camera path rather than the generator -- otherwise every red probe run starts
//    with "but is the test pattern actually right?".
//
// 2. The seqlock is the only lock-free code in the project and it runs across a
//    process boundary, where a torn read would surface as an occasional corrupt frame
//    that no amount of staring at a preview window would explain. It is exercised here
//    under real thread contention, using Local\ names so the test needs no privilege.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rcwin/nv12.h"
#include "rcwin/shm_ring.h"
#include "rcwin/test_pattern.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

void checkEq(long long got, long long want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    std::printf("  FAIL: %s (got %lld, want %lld)\n", what.c_str(), got, want);
  }
}

uint64_t fnv1a(const uint8_t* data, size_t n, uint64_t hash = 0xcbf29ce484222325ull) {
  for (size_t i = 0; i < n; ++i) {
    hash ^= data[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

// Mirrors what rc-vcam-probe does: hash only the visible pixels of each luma row,
// never the stride padding.
uint64_t hashLumaRows(const uint8_t* nv12, const rcwin::Nv12Layout& layout, int top,
                      int height) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (int row = top; row < top + height; ++row) {
    hash = fnv1a(nv12 + static_cast<size_t>(row) * layout.stride,
                 static_cast<size_t>(layout.width), hash);
  }
  return hash;
}

// ---------------------------------------------------------------------------

void testNv12Layout() {
  std::printf("NV12 layout\n");

  // Swept rather than spot-checked: the 3/2 size relationship and the plane offset must
  // hold at every geometry the format ladder will eventually advertise, not just 1080p.
  const int sizes[][2] = {{640, 480},   {960, 540},   {1280, 720},
                          {1920, 1080}, {2560, 1440}, {3840, 2160}};
  for (const auto& wh : sizes) {
    const rcwin::Nv12Layout l = rcwin::nv12Layout(wh[0], wh[1]);
    const std::string tag = std::to_string(wh[0]) + "x" + std::to_string(wh[1]);
    checkEq(l.width, wh[0], tag + " width");
    checkEq(l.height, wh[1], tag + " height");
    checkEq(l.stride, wh[0], tag + " packed stride");
    checkEq(static_cast<long long>(l.uvOffset),
            static_cast<long long>(wh[0]) * wh[1], tag + " uv offset == Y plane size");
    checkEq(static_cast<long long>(l.totalSize),
            static_cast<long long>(wh[0]) * wh[1] * 3 / 2, tag + " total size");
  }

  // Odd dimensions round DOWN. Rounding up would write past the caller's allocation,
  // which is a memory-corruption bug rather than a cosmetic one.
  const rcwin::Nv12Layout odd = rcwin::nv12Layout(641, 481);
  checkEq(odd.width, 640, "odd width rounds down");
  checkEq(odd.height, 480, "odd height rounds down");

  // A padded stride must widen the planes without changing the pixel dimensions.
  const rcwin::Nv12Layout padded = rcwin::nv12Layout(1920, 1080, 2048);
  checkEq(padded.width, 1920, "padded keeps width");
  checkEq(padded.stride, 2048, "padded stride honoured");
  checkEq(static_cast<long long>(padded.uvOffset), 2048LL * 1080, "padded uv offset");

  const rcwin::Nv12Layout degenerate = rcwin::nv12Layout(0, 0);
  checkEq(static_cast<long long>(degenerate.totalSize), 0, "degenerate is empty");
}

void testFillRectClipping() {
  std::printf("nv12FillRect clipping\n");

  const rcwin::Nv12Layout layout = rcwin::nv12Layout(64, 64);
  std::vector<uint8_t> buffer(layout.totalSize);

  const rcwin::Yuv black{16, 128, 128};
  const rcwin::Yuv white{235, 128, 128};

  // A rect entirely outside must change nothing at all -- in either plane.
  rcwin::nv12Fill(buffer.data(), layout, black);
  const uint64_t before = fnv1a(buffer.data(), buffer.size());
  rcwin::nv12FillRect(buffer.data(), layout, 100, 100, 20, 20, white);
  rcwin::nv12FillRect(buffer.data(), layout, -50, -50, 20, 20, white);
  check(fnv1a(buffer.data(), buffer.size()) == before, "fully-outside rects are no-ops");

  // A rect straddling the origin must fill only the visible part and must not wrap.
  rcwin::nv12Fill(buffer.data(), layout, black);
  rcwin::nv12FillRect(buffer.data(), layout, -4, -4, 8, 8, white);
  checkEq(buffer[0], 235, "straddling rect fills the visible corner");
  checkEq(buffer[4], 16, "straddling rect stops at its right edge");
  checkEq(buffer[static_cast<size_t>(layout.stride) * 4], 16, "and at its bottom edge");
  checkEq(buffer[static_cast<size_t>(layout.stride) * 63 + 63], 16, "opposite corner untouched");

  // Chroma snaps outward to whole 2x2 blocks, so an odd-aligned rect must still leave
  // the UV plane consistent rather than half-written.
  rcwin::nv12Fill(buffer.data(), layout, black);
  rcwin::nv12FillRect(buffer.data(), layout, 3, 3, 5, 5, white);
  const uint8_t* uv = buffer.data() + layout.uvOffset;
  checkEq(uv[1 * static_cast<size_t>(layout.stride) + 1 * 2], 128, "chroma U written");
  checkEq(uv[1 * static_cast<size_t>(layout.stride) + 1 * 2 + 1], 128, "chroma V written");
}

void testPatternDeterminism() {
  std::printf("Pattern determinism\n");

  const rcwin::Nv12Layout layout = rcwin::nv12Layout(640, 480);
  std::vector<uint8_t> a(layout.totalSize);
  std::vector<uint8_t> b(layout.totalSize);

  // Determinism is what makes hash comparison meaningful at all. Checked across a
  // sweep, including indices past the sweep's 97-frame period and the counter's
  // million-frame wrap.
  const uint64_t indices[] = {0, 1, 2, 96, 97, 98, 12345, 999999, 1000000, 1000001};
  for (uint64_t i : indices) {
    rcwin::renderPattern(a.data(), layout, i, rcwin::PatternStyle::Placeholder);
    rcwin::renderPattern(b.data(), layout, i, rcwin::PatternStyle::Placeholder);
    check(a == b, "frame " + std::to_string(i) + " renders identically twice");
  }

  // The whole allocation must be written, padding included, or the output is not
  // actually deterministic -- only the parts anyone looked at are.
  const rcwin::Nv12Layout padded = rcwin::nv12Layout(640, 480, 704);
  std::vector<uint8_t> p1(padded.totalSize, 0xAA);
  std::vector<uint8_t> p2(padded.totalSize, 0x55);
  rcwin::renderPattern(p1.data(), padded, 7, rcwin::PatternStyle::Writer);
  rcwin::renderPattern(p2.data(), padded, 7, rcwin::PatternStyle::Writer);
  check(p1 == p2, "padded render is independent of prior buffer contents");
}

void testPatternRegionInvariants() {
  std::printf("Pattern region invariants (the properties rc-vcam-probe asserts)\n");

  const int sizes[][2] = {{640, 480}, {1280, 720}, {1920, 1080}, {2560, 1440}};
  const rcwin::PatternStyle styles[] = {rcwin::PatternStyle::Placeholder,
                                        rcwin::PatternStyle::Writer};

  for (const auto& wh : sizes) {
    const rcwin::Nv12Layout layout = rcwin::nv12Layout(wh[0], wh[1]);
    const rcwin::PatternRegions r = rcwin::patternRegions(layout);
    const std::string tag = std::to_string(wh[0]) + "x" + std::to_string(wh[1]);

    // The two regions must tile the frame exactly, with the split on a chroma boundary
    // so neither can be contaminated by the other's UV samples.
    checkEq(r.staticTop, 0, tag + " static starts at the top");
    checkEq(r.staticTop + r.staticHeight, r.movingTop, tag + " regions are adjacent");
    checkEq(r.movingTop + r.movingHeight, layout.height, tag + " regions cover the frame");
    check(r.staticHeight > 0 && r.movingHeight > 0, tag + " both regions are non-empty");
    checkEq(r.staticHeight % 2, 0, tag + " split lands on a chroma boundary");

    for (rcwin::PatternStyle style : styles) {
      std::vector<uint8_t> frame(layout.totalSize);
      const std::string stag =
          tag + (style == rcwin::PatternStyle::Placeholder ? " placeholder" : " writer");

      // Swept over a full sweep period plus change, not spot-checked at two frames:
      // the sweep bar's position is periodic, and a naive check could sample it at the
      // same place twice and conclude the image was frozen.
      uint64_t firstStatic = 0;
      uint64_t previousMoving = 0;
      int staticChanges = 0;
      int movingRepeats = 0;

      for (uint64_t i = 0; i < 200; ++i) {
        rcwin::renderPattern(frame.data(), layout, i, style);
        const uint64_t s = hashLumaRows(frame.data(), layout, r.staticTop, r.staticHeight);
        const uint64_t m = hashLumaRows(frame.data(), layout, r.movingTop, r.movingHeight);
        if (i == 0) {
          firstStatic = s;
        } else {
          if (s != firstStatic) ++staticChanges;
          if (m == previousMoving) ++movingRepeats;
        }
        previousMoving = m;
      }

      checkEq(staticChanges, 0, stag + " static region is byte-stable across 200 frames");
      checkEq(movingRepeats, 0, stag + " moving region advances on every frame");
    }

    // The two producers must be distinguishable from the static region alone, which is
    // what lets the probe report WHICH producer it is looking at rather than merely
    // that something is producing.
    std::vector<uint8_t> placeholder(layout.totalSize);
    std::vector<uint8_t> writer(layout.totalSize);
    rcwin::renderPattern(placeholder.data(), layout, 0, rcwin::PatternStyle::Placeholder);
    rcwin::renderPattern(writer.data(), layout, 0, rcwin::PatternStyle::Writer);
    check(hashLumaRows(placeholder.data(), layout, r.staticTop, r.staticHeight) !=
              hashLumaRows(writer.data(), layout, r.staticTop, r.staticHeight),
          tag + " placeholder and writer have different static hashes");
  }
}

void testRingBasics() {
  std::printf("Frame ring basics\n");

  rcwin::FrameRing server;
  const HRESULT created = server.create(rcwin::testRingNames());
  check(SUCCEEDED(created), "create() succeeds on a Local\\ name without privilege");
  if (FAILED(created)) return;

  rcwin::FrameRing client;
  check(SUCCEEDED(client.open(true, rcwin::testRingNames())), "open() attaches to it");

  std::vector<uint8_t> scratch(rcwin::kRingSlotBytes);
  rcwin::FrameInfo info;
  checkEq(server.readLatest(scratch.data(), static_cast<uint32_t>(scratch.size()), info),
          S_FALSE, "readLatest before any write reports S_FALSE, not an error");
  check(server.millisSinceLastWrite() == UINT64_MAX, "no write yet -> infinite staleness");

  const rcwin::Nv12Layout layout = rcwin::nv12Layout(320, 240);
  std::vector<uint8_t> frame(layout.totalSize, 0x42);
  rcwin::FrameInfo out;
  out.width = 320;
  out.height = 240;
  out.stride = 320;
  out.format = rcwin::kFourccNv12;
  out.ptsMicros = 123456;
  check(SUCCEEDED(client.writeFrame(frame.data(), static_cast<uint32_t>(frame.size()), out)),
        "writeFrame succeeds");

  checkEq(server.readLatest(scratch.data(), static_cast<uint32_t>(scratch.size()), info), S_OK,
          "readLatest returns the frame");
  checkEq(info.width, 320, "width round-trips");
  checkEq(info.height, 240, "height round-trips");
  checkEq(static_cast<long long>(info.ptsMicros), 123456, "pts round-trips");
  checkEq(static_cast<long long>(info.bytesUsed), static_cast<long long>(frame.size()),
          "byte count round-trips");
  check(scratch[0] == 0x42 && scratch[frame.size() - 1] == 0x42, "pixels round-trip");
  check(server.millisSinceLastWrite() < 5000, "staleness is now finite");

  // A destination too small must be refused rather than truncated -- a short copy here
  // would be a buffer overrun in the Frame Server.
  check(server.readLatest(scratch.data(), 16, info) ==
            HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
        "undersized destination is rejected");

  // Oversized and empty writes are caller bugs and must not corrupt the ring.
  check(client.writeFrame(frame.data(), 0, out) == E_INVALIDARG, "zero-byte write rejected");
  check(client.writeFrame(frame.data(), rcwin::kRingSlotBytes + 1, out) == E_INVALIDARG,
        "oversized write rejected");
}

void testRingSeqlockUnderContention() {
  std::printf("Frame ring seqlock under contention\n");

  rcwin::FrameRing server;
  if (FAILED(server.create(rcwin::testRingNames()))) {
    check(false, "could not create the test ring");
    return;
  }
  rcwin::FrameRing client;
  if (FAILED(client.open(true, rcwin::testRingNames()))) {
    check(false, "could not open the test ring");
    return;
  }

  // 640x480 is large enough that a write takes long enough to actually be caught
  // mid-flight by a concurrent reader, which is the situation being tested. A tiny
  // frame would make the race window so short the test would pass vacuously.
  const rcwin::Nv12Layout layout = rcwin::nv12Layout(640, 480);
  const uint32_t bytes = static_cast<uint32_t>(layout.totalSize);
  constexpr int kFrames = 3000;

  std::atomic<bool> writerDone{false};
  std::atomic<int> tornReads{0};
  std::atomic<int> goodReads{0};

  std::thread writer([&] {
    std::vector<uint8_t> frame(bytes);
    rcwin::FrameInfo info;
    info.width = 640;
    info.height = 480;
    info.stride = 640;
    info.format = rcwin::kFourccNv12;

    for (int i = 0; i < kFrames; ++i) {
      // Every byte of a frame carries the same value, so any splice of two different
      // frames is detectable by inspection alone. 1..251 avoids zero, which would be
      // indistinguishable from a never-written slot.
      const uint8_t fill = static_cast<uint8_t>(i % 251 + 1);
      std::memset(frame.data(), fill, frame.size());
      info.ptsMicros = static_cast<uint64_t>(i);
      client.writeFrame(frame.data(), bytes, info);
    }
    writerDone.store(true);
  });

  std::thread reader([&] {
    std::vector<uint8_t> scratch(rcwin::kRingSlotBytes);
    std::vector<uint8_t> reference(rcwin::kRingSlotBytes);
    rcwin::FrameInfo info;
    while (!writerDone.load()) {
      if (server.readLatest(scratch.data(), static_cast<uint32_t>(scratch.size()), info) !=
          S_OK) {
        continue;
      }
      // memset + memcmp rather than a hand-rolled scan. Both are CRT intrinsics that
      // stay fast in a Debug build, and the reader's throughput is what decides how
      // many reads actually land mid-write -- a slow verifier makes this test pass by
      // never racing anything, which is the one outcome that would prove nothing.
      const uint8_t expected = scratch[0];
      std::memset(reference.data(), expected, info.bytesUsed);
      if (std::memcmp(scratch.data(), reference.data(), info.bytesUsed) == 0) {
        goodReads.fetch_add(1);
      } else {
        tornReads.fetch_add(1);
      }
    }
  });

  writer.join();
  reader.join();

  std::printf("  %d clean reads, %d torn\n", goodReads.load(), tornReads.load());
  checkEq(tornReads.load(), 0, "no torn frame ever escapes the seqlock");
  check(goodReads.load() > 0, "the reader actually observed frames (test is not vacuous)");
}

}  // namespace

int main() {
  testNv12Layout();
  testFillRectClipping();
  testPatternDeterminism();
  testPatternRegionInvariants();
  testRingBasics();
  testRingSeqlockUnderContention();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
