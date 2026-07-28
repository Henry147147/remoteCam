#include "rcwin/test_pattern.h"

#include <cstring>

namespace rcwin {
namespace {

// 75% colour bars in BT.601 studio range, which is what NV12 carries. Written as YUV
// rather than converted from RGB at runtime: studio-vs-full range is the classic place
// to be quietly wrong, and a hard-coded table cannot drift.
constexpr Yuv kWhite{180, 128, 128};
constexpr Yuv kYellow{162, 44, 142};
constexpr Yuv kCyan{131, 156, 44};
constexpr Yuv kGreen{112, 72, 58};
constexpr Yuv kMagenta{84, 184, 198};
constexpr Yuv kRed{65, 100, 212};
constexpr Yuv kBlue{35, 212, 114};
constexpr Yuv kBlack{16, 128, 128};

constexpr Yuv kBars[8] = {kWhite, kYellow, kCyan, kGreen, kMagenta, kRed, kBlue, kBlack};

// Accent colours, chosen so the two styles are unmistakable in a small preview.
constexpr Yuv kPlaceholderInk{180, 128, 128};   // white on deep blue
constexpr Yuv kPlaceholderBg{35, 212, 114};
constexpr Yuv kWriterInk{16, 128, 128};         // black on bright green
constexpr Yuv kWriterBg{112, 72, 58};

// 5x7 glyphs, column-major, bit 0 = top row. Only the characters the two labels and
// the frame counter need -- a full font would be dead weight in a DLL loaded into
// svchost.
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;

struct Glyph {
  char ch;
  uint8_t col[kGlyphW];
};

constexpr Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}}, {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}}, {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}}, {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

const Glyph* findGlyph(char ch) {
  for (const Glyph& g : kFont) {
    if (g.ch == ch) return &g;
  }
  return nullptr;
}

void drawText(uint8_t* dst, const Nv12Layout& layout, int x, int y, int scale,
              const char* text, Yuv ink) {
  int cursor = x;
  for (const char* p = text; *p; ++p) {
    const Glyph* g = findGlyph(*p);
    if (g) {
      for (int col = 0; col < kGlyphW; ++col) {
        for (int row = 0; row < kGlyphH; ++row) {
          if (g->col[col] & (1u << row)) {
            nv12FillRect(dst, layout, cursor + col * scale, y + row * scale, scale, scale, ink);
          }
        }
      }
    }
    cursor += (kGlyphW + 1) * scale;
  }
}

int textWidth(const char* text, int scale) {
  int n = 0;
  for (const char* p = text; *p; ++p) ++n;
  return n > 0 ? (n * (kGlyphW + 1) - 1) * scale : 0;
}

void formatCounter(char* out, size_t cap, uint64_t frameIndex) {
  // Six digits, zero padded, wrapping at a million. Fixed width matters: a counter that
  // changes length shifts every following glyph, which makes a frozen-frame bug look
  // like a moving image.
  const unsigned n = static_cast<unsigned>(frameIndex % 1000000ull);
  const char digits[] = "0123456789";
  const char prefix[] = "FRAME ";
  size_t i = 0;
  for (const char* p = prefix; *p && i + 1 < cap; ++p) out[i++] = *p;
  unsigned divisor = 100000u;
  while (divisor > 0 && i + 1 < cap) {
    out[i++] = digits[(n / divisor) % 10];
    divisor /= 10;
  }
  out[i] = '\0';
}

}  // namespace

PatternRegions patternRegions(const Nv12Layout& layout) {
  PatternRegions r;
  if (layout.height <= 0) return r;
  // Two thirds static, snapped to an even row so the split lands on a chroma boundary
  // and neither region can be contaminated by the other's UV samples.
  r.staticTop = 0;
  r.staticHeight = ((layout.height * 2 / 3) + 1) & ~1;
  if (r.staticHeight >= layout.height) r.staticHeight = layout.height - 2;
  r.movingTop = r.staticHeight;
  r.movingHeight = layout.height - r.staticHeight;
  return r;
}

void renderPattern(uint8_t* dst, const Nv12Layout& layout, uint64_t frameIndex,
                   PatternStyle style) {
  if (!dst || layout.totalSize == 0) return;

  // Clear the whole allocation, padding included. Stride padding is never hashed or
  // displayed, but leaving it uninitialised makes the output non-deterministic under
  // a memory sanitiser and non-reproducible across runs for no benefit.
  std::memset(dst, 0, layout.totalSize);

  const PatternRegions r = patternRegions(layout);
  const bool placeholder = style == PatternStyle::Placeholder;
  const Yuv bg = placeholder ? kPlaceholderBg : kWriterBg;
  const Yuv ink = placeholder ? kPlaceholderInk : kWriterInk;
  const char* label = placeholder ? "REMOTECAM - WAITING FOR PHONE" : "REMOTECAM - SHM WRITER";

  // --- static region: colour bars plus the producer's label -------------------
  //
  // The label lives here rather than in the moving region on purpose. It is constant
  // per producer, so it stays inside the byte-identical guarantee, and it means the
  // static hash itself identifies *which* producer the probe is looking at.
  const int barW = layout.width / 8;
  for (int i = 0; i < 8; ++i) {
    const int x = i * barW;
    const int w = (i == 7) ? (layout.width - x) : barW;
    nv12FillRect(dst, layout, x, r.staticTop, w, r.staticHeight, kBars[i]);
  }

  // Shrink until the label fits. At 320 px wide the 4x scale overflows the frame and
  // nv12FillRect would silently clip both ends, leaving text that reads as corruption
  // rather than as a label -- which is the opposite of what a diagnostic frame is for.
  int labelScale = layout.height >= 720 ? 4 : 2;
  while (labelScale > 1 && textWidth(label, labelScale) > layout.width) --labelScale;
  const int labelBoxH = kGlyphH * labelScale + 12 * labelScale / 2;
  const int labelBoxY = r.staticTop + r.staticHeight / 2 - labelBoxH / 2;
  nv12FillRect(dst, layout, 0, labelBoxY, layout.width, labelBoxH, bg);
  drawText(dst, layout, (layout.width - textWidth(label, labelScale)) / 2,
           labelBoxY + (labelBoxH - kGlyphH * labelScale) / 2, labelScale, label, ink);

  // --- moving region: sweep bar plus frame counter ----------------------------
  nv12FillRect(dst, layout, 0, r.movingTop, layout.width, r.movingHeight, kBlack);

  // A sweep whose period is not a divisor of any plausible capture length, so a
  // capture can never accidentally sample the bar at the same position twice in a row.
  const int sweepW = layout.width / 24;
  const int travel = layout.width - sweepW;
  const int phase = travel > 0 ? static_cast<int>(frameIndex % 97ull) * travel / 96 : 0;
  nv12FillRect(dst, layout, phase, r.movingTop + r.movingHeight / 2, sweepW,
               r.movingHeight / 2, placeholder ? kCyan : kMagenta);

  char counter[32];
  formatCounter(counter, sizeof(counter), frameIndex);
  const int counterScale = layout.height >= 720 ? 5 : 2;
  drawText(dst, layout, (layout.width - textWidth(counter, counterScale)) / 2,
           r.movingTop + r.movingHeight / 4 - kGlyphH * counterScale / 2, counterScale, counter,
           kWhite);
}

}  // namespace rcwin
