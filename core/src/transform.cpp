#include "rc/transform.h"

#include <algorithm>
#include <cmath>

namespace rc {
namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float deg2rad(float d) { return d * (kPi / 180.0f); }

// Both fit modes reduce to a ratio of the source and canvas extents projected onto
// the rotated axes, so the two magnitudes get computed the same way twice.
struct Trig {
  float c;  // |cos(theta)|
  float s;  // |sin(theta)|
};

Trig absTrig(float deg) {
  const float r = deg2rad(deg);
  return Trig{std::fabs(std::cos(r)), std::fabs(std::sin(r))};
}

bool degenerate(const TransformParams& p) {
  return p.srcWidth <= 0 || p.srcHeight <= 0 || p.dstWidth <= 0 || p.dstHeight <= 0;
}

}  // namespace

Mat3 Mat3::translate(float tx, float ty) {
  Mat3 r;
  r.m[2] = tx;
  r.m[5] = ty;
  return r;
}

Mat3 Mat3::scale(float sx, float sy) {
  Mat3 r;
  r.m[0] = sx;
  r.m[4] = sy;
  return r;
}

Mat3 Mat3::rotateDeg(float deg) {
  const float a = deg2rad(deg);
  const float c = std::cos(a);
  const float s = std::sin(a);
  // y-down coordinates, so this is clockwise as displayed.
  Mat3 r;
  r.m[0] = c;
  r.m[1] = -s;
  r.m[3] = s;
  r.m[4] = c;
  return r;
}

Mat3 Mat3::operator*(const Mat3& rhs) const {
  Mat3 out;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      float acc = 0.0f;
      for (int k = 0; k < 3; ++k) acc += m[row * 3 + k] * rhs.m[k * 3 + col];
      out.m[row * 3 + col] = acc;
    }
  }
  return out;
}

Vec2 Mat3::apply(Vec2 p) const {
  return Vec2{m[0] * p.x + m[1] * p.y + m[2], m[3] * p.x + m[4] * p.y + m[5]};
}

Vec2 rotatedBounds(const TransformParams& p) {
  if (degenerate(p)) return Vec2{0.0f, 0.0f};
  const Trig t = absTrig(p.rotationDeg);
  const float w = static_cast<float>(p.srcWidth);
  const float h = static_cast<float>(p.srcHeight);
  return Vec2{w * t.c + h * t.s, w * t.s + h * t.c};
}

Vec2 fitScale(const TransformParams& p) {
  if (degenerate(p)) return Vec2{0.0f, 0.0f};

  const Trig t = absTrig(p.rotationDeg);
  const float dw = static_cast<float>(p.dstWidth);
  const float dh = static_cast<float>(p.dstHeight);
  const float sw = static_cast<float>(p.srcWidth);
  const float sh = static_cast<float>(p.srcHeight);

  switch (p.fit) {
    case FitMode::Fit: {
      // Contain the source's rotated bounding box within the canvas.
      const Vec2 b = rotatedBounds(p);
      const float s = std::min(dw / b.x, dh / b.y);
      return Vec2{s, s};
    }
    case FitMode::Fill: {
      // Cover: map the CANVAS back into source space and require that rectangle to
      // fit inside the source. Note this is not the bounding-box formula with the
      // min swapped for a max -- that only covers the source's bbox, which at 45
      // degrees still leaves the canvas corners empty. Projecting the canvas the
      // other way is what actually guarantees full coverage at every angle.
      const float s = std::max((dw * t.c + dh * t.s) / sw, (dw * t.s + dh * t.c) / sh);
      return Vec2{s, s};
    }
    case FitMode::Stretch: {
      const Vec2 b = rotatedBounds(p);
      return Vec2{dw / b.x, dh / b.y};
    }
  }
  return Vec2{1.0f, 1.0f};
}

namespace {

// Forward map, source pixels -> destination pixels, read right to left:
// centre the source, flip, rotate, scale, pan, then place at the canvas centre.
Mat3 forward(const TransformParams& p, Vec2 s) {
  const Mat3 toOrigin = Mat3::translate(-0.5f * static_cast<float>(p.srcWidth),
                                        -0.5f * static_cast<float>(p.srcHeight));
  const Mat3 flip = Mat3::scale(p.flipH ? -1.0f : 1.0f, p.flipV ? -1.0f : 1.0f);
  const Mat3 rot = Mat3::rotateDeg(p.rotationDeg);
  const Mat3 scl = Mat3::scale(s.x, s.y);
  const Mat3 pan = Mat3::translate(p.panX, p.panY);
  const Mat3 toCentre = Mat3::translate(0.5f * static_cast<float>(p.dstWidth),
                                        0.5f * static_cast<float>(p.dstHeight));
  return toCentre * pan * scl * rot * flip * toOrigin;
}

Vec2 effectiveScale(const TransformParams& p) {
  const Vec2 f = fitScale(p);
  const float z = (p.zoom > 0.0f) ? p.zoom : 1.0f;
  return Vec2{f.x * z, f.y * z};
}

}  // namespace

Mat3 sourceToDest(const TransformParams& p) {
  if (degenerate(p)) return Mat3::identity();
  return forward(p, effectiveScale(p));
}

Mat3 destToSource(const TransformParams& p) {
  if (degenerate(p)) return Mat3::identity();
  const Vec2 s = effectiveScale(p);
  if (s.x == 0.0f || s.y == 0.0f) return Mat3::identity();

  // Compose the inverse from inverted primitives in reverse order rather than
  // inverting the product numerically -- exact, and cheaper.
  const Mat3 fromCentre = Mat3::translate(-0.5f * static_cast<float>(p.dstWidth),
                                          -0.5f * static_cast<float>(p.dstHeight));
  const Mat3 unpan = Mat3::translate(-p.panX, -p.panY);
  const Mat3 unscale = Mat3::scale(1.0f / s.x, 1.0f / s.y);
  const Mat3 unrot = Mat3::rotateDeg(-p.rotationDeg);
  // A flip is its own inverse.
  const Mat3 flip = Mat3::scale(p.flipH ? -1.0f : 1.0f, p.flipV ? -1.0f : 1.0f);
  const Mat3 fromOrigin = Mat3::translate(0.5f * static_cast<float>(p.srcWidth),
                                          0.5f * static_cast<float>(p.srcHeight));
  return fromOrigin * flip * unrot * unscale * unpan * fromCentre;
}

Vec2 panSlack(const TransformParams& p) {
  if (degenerate(p)) return Vec2{0.0f, 0.0f};

  const Vec2 s = effectiveScale(p);
  if (s.x <= 0.0f || s.y <= 0.0f) return Vec2{0.0f, 0.0f};

  const Trig t = absTrig(p.rotationDeg);
  const float dw = static_cast<float>(p.dstWidth);
  const float dh = static_cast<float>(p.dstHeight);

  // Half-extents, in source pixels, of the canvas mapped back through the inverse
  // transform. Whatever the source has beyond that is free to move into.
  const float needHalfW = 0.5f * ((dw / s.x) * t.c + (dh / s.y) * t.s);
  const float needHalfH = 0.5f * ((dw / s.x) * t.s + (dh / s.y) * t.c);

  return Vec2{std::max(0.0f, 0.5f * static_cast<float>(p.srcWidth) - needHalfW),
              std::max(0.0f, 0.5f * static_cast<float>(p.srcHeight) - needHalfH)};
}

void clampPanForCoverage(TransformParams& p) {
  if (degenerate(p)) return;

  const Vec2 s = effectiveScale(p);
  if (s.x <= 0.0f || s.y <= 0.0f) return;

  const Vec2 slack = panSlack(p);

  // destToSource shifts the sampled window by R(-theta) * S(1/s) * pan, so move the
  // pan vector into source axes before clamping. The flip and the sign are dropped:
  // the slack box is symmetric about the origin, so neither changes the result.
  const Mat3 toSourceAxes = Mat3::rotateDeg(-p.rotationDeg) * Mat3::scale(1.0f / s.x, 1.0f / s.y);
  Vec2 v = toSourceAxes.apply(Vec2{p.panX, p.panY});

  v.x = std::clamp(v.x, -slack.x, slack.x);
  v.y = std::clamp(v.y, -slack.y, slack.y);

  const Mat3 toCanvasAxes = Mat3::scale(s.x, s.y) * Mat3::rotateDeg(p.rotationDeg);
  const Vec2 clamped = toCanvasAxes.apply(v);
  p.panX = clamped.x;
  p.panY = clamped.y;
}

}  // namespace rc
