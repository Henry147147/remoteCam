// Tests for rc::transform.
//
// These are deliberately dependency-free so the headline feature's math can be
// verified on any machine -- including the Linux dev box, which cannot build or run
// a single line of the D3D11 pipeline this matrix eventually feeds.
//
// The properties that matter are geometric, not numeric: Fit must never crop, Fill
// must never expose an empty corner at ANY angle, and the forward and backward maps
// must be exact inverses. Those are swept across angles rather than spot-checked.

#include "rc/transform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

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

void checkNear(float got, float want, float tol, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failures;
    std::printf("  FAIL: %s (got %.6f, want %.6f, tol %g)\n", what.c_str(), got, want, tol);
  }
}

void checkPoint(rc::Vec2 got, float wx, float wy, float tol, const std::string& what) {
  checkNear(got.x, wx, tol, what + ".x");
  checkNear(got.y, wy, tol, what + ".y");
}

bool inRect(rc::Vec2 p, float w, float h, float tol) {
  return p.x >= -tol && p.y >= -tol && p.x <= w + tol && p.y <= h + tol;
}

std::vector<rc::Vec2> corners(float w, float h) {
  return {{0, 0}, {w, 0}, {w, h}, {0, h}};
}

// ---------------------------------------------------------------------------

void testMat3Primitives() {
  std::printf("Mat3 primitives\n");

  checkPoint(rc::Mat3::identity().apply({3, 7}), 3, 7, 1e-5f, "identity");
  checkPoint(rc::Mat3::translate(10, -4).apply({3, 7}), 13, 3, 1e-5f, "translate");
  checkPoint(rc::Mat3::scale(2, 0.5f).apply({3, 8}), 6, 4, 1e-5f, "scale");

  // Positive angles are clockwise in y-down coordinates: +x rotates onto +y.
  checkPoint(rc::Mat3::rotateDeg(90).apply({1, 0}), 0, 1, 1e-5f, "rotate90 of +x");
  checkPoint(rc::Mat3::rotateDeg(90).apply({0, 1}), -1, 0, 1e-5f, "rotate90 of +y");

  // Composition applies right-hand factor first.
  const rc::Mat3 m = rc::Mat3::translate(5, 0) * rc::Mat3::scale(2, 2);
  checkPoint(m.apply({1, 1}), 7, 2, 1e-5f, "translate*scale ordering");
}

void testIdentityCase() {
  std::printf("Identity when source and canvas match\n");

  rc::TransformParams p;
  p.srcWidth = p.dstWidth = 1920;
  p.srcHeight = p.dstHeight = 1080;

  const rc::Mat3 d2s = rc::destToSource(p);
  checkPoint(d2s.apply({0, 0}), 0, 0, 1e-3f, "top-left");
  checkPoint(d2s.apply({1920, 1080}), 1920, 1080, 1e-3f, "bottom-right");
  checkPoint(d2s.apply({960, 540}), 960, 540, 1e-3f, "centre");
}

void testRoundTrip() {
  std::printf("destToSource is the exact inverse of sourceToDest\n");

  const rc::FitMode modes[] = {rc::FitMode::Fit, rc::FitMode::Fill, rc::FitMode::Stretch};
  int cases = 0;

  for (int angle = -180; angle <= 180; angle += 17) {
    for (rc::FitMode mode : modes) {
      for (int flip = 0; flip < 4; ++flip) {
        rc::TransformParams p;
        p.srcWidth = 1280;
        p.srcHeight = 720;
        p.dstWidth = 1920;
        p.dstHeight = 1080;
        p.rotationDeg = static_cast<float>(angle);
        p.fit = mode;
        p.flipH = (flip & 1) != 0;
        p.flipV = (flip & 2) != 0;
        p.zoom = 1.3f;
        p.panX = 11.0f;
        p.panY = -7.0f;

        const rc::Mat3 s2d = rc::sourceToDest(p);
        const rc::Mat3 d2s = rc::destToSource(p);

        for (rc::Vec2 c : corners(1280, 720)) {
          const rc::Vec2 back = d2s.apply(s2d.apply(c));
          checkPoint(back, c.x, c.y, 1e-2f, "round trip");
        }
        ++cases;
      }
    }
  }
  std::printf("  (%d parameter combinations)\n", cases);
}

void testQuarterTurn() {
  std::printf("90 degrees maps a landscape source onto a portrait canvas exactly\n");

  rc::TransformParams p;
  p.srcWidth = 100;
  p.srcHeight = 50;
  p.dstWidth = 50;
  p.dstHeight = 100;
  p.rotationDeg = 90.0f;
  p.fit = rc::FitMode::Fit;

  checkNear(rc::fitScale(p).x, 1.0f, 1e-5f, "quarter turn needs no rescale");

  // Rotating clockwise sends the source's top-left corner to the canvas top-right.
  const rc::Mat3 s2d = rc::sourceToDest(p);
  checkPoint(s2d.apply({0, 0}), 50, 0, 1e-3f, "src top-left");
  checkPoint(s2d.apply({100, 0}), 50, 100, 1e-3f, "src top-right");
  checkPoint(s2d.apply({100, 50}), 0, 100, 1e-3f, "src bottom-right");
}

void testFitNeverCrops() {
  std::printf("Fit keeps every source pixel inside the canvas, at every angle\n");

  const int sizes[][2] = {{1920, 1080}, {1080, 1920}, {640, 640}, {1280, 720}};

  for (int angle = -180; angle <= 180; angle += 3) {
    for (const auto& s : sizes) {
      rc::TransformParams p;
      p.srcWidth = s[0];
      p.srcHeight = s[1];
      p.dstWidth = 1920;
      p.dstHeight = 1080;
      p.rotationDeg = static_cast<float>(angle);
      p.fit = rc::FitMode::Fit;

      const rc::Mat3 s2d = rc::sourceToDest(p);
      for (rc::Vec2 c : corners(static_cast<float>(s[0]), static_cast<float>(s[1]))) {
        const rc::Vec2 d = s2d.apply(c);
        check(inRect(d, 1920.0f, 1080.0f, 0.5f),
              "source corner escaped the canvas at " + std::to_string(angle) + " deg");
      }
    }
  }
}

void testFillNeverLeavesEmptyCorners() {
  std::printf("Fill covers the canvas at every angle -- the headline guarantee\n");

  const int sizes[][2] = {{1920, 1080}, {1080, 1920}, {640, 640}, {1280, 720}, {4032, 3024}};

  for (int angle = -180; angle <= 180; ++angle) {
    for (const auto& s : sizes) {
      rc::TransformParams p;
      p.srcWidth = s[0];
      p.srcHeight = s[1];
      p.dstWidth = 1920;
      p.dstHeight = 1080;
      p.rotationDeg = static_cast<float>(angle);
      p.fit = rc::FitMode::Fill;

      // Every canvas corner must sample from inside the source. If any lands
      // outside, the user sees a hard black wedge -- exactly the artefact that
      // makes odd angles unusable in other tools.
      const rc::Mat3 d2s = rc::destToSource(p);
      for (rc::Vec2 c : corners(1920.0f, 1080.0f)) {
        const rc::Vec2 sp = d2s.apply(c);
        check(inRect(sp, static_cast<float>(s[0]), static_cast<float>(s[1]), 0.5f),
              "empty corner at " + std::to_string(angle) + " deg for " +
                  std::to_string(s[0]) + "x" + std::to_string(s[1]));
      }
    }
  }
}

void testFillIsTight() {
  std::printf("Fill scales no more than necessary\n");

  // A square source on a square canvas at 45 degrees needs exactly sqrt(2).
  rc::TransformParams p;
  p.srcWidth = p.srcHeight = 100;
  p.dstWidth = p.dstHeight = 100;
  p.rotationDeg = 45.0f;
  p.fit = rc::FitMode::Fill;
  checkNear(rc::fitScale(p).x, std::sqrt(2.0f), 1e-4f, "45 degree cover scale");

  // Shrinking below that must expose a corner, which proves the bound is tight
  // rather than merely sufficient.
  p.zoom = 0.99f;
  const rc::Mat3 d2s = rc::destToSource(p);
  bool escaped = false;
  for (rc::Vec2 c : corners(100.0f, 100.0f)) {
    if (!inRect(d2s.apply(c), 100.0f, 100.0f, 0.0f)) escaped = true;
  }
  check(escaped, "scaling below the Fill bound should expose a corner");
}

void testFlips() {
  std::printf("Flips are independent of rotation\n");

  rc::TransformParams p;
  p.srcWidth = p.dstWidth = 100;
  p.srcHeight = p.dstHeight = 100;

  p.flipH = true;
  checkPoint(rc::sourceToDest(p).apply({0, 0}), 100, 0, 1e-3f, "flipH sends left to right");

  p.flipH = false;
  p.flipV = true;
  checkPoint(rc::sourceToDest(p).apply({0, 0}), 0, 100, 1e-3f, "flipV sends top to bottom");

  // Both flips together are a half turn.
  p.flipH = true;
  checkPoint(rc::sourceToDest(p).apply({0, 0}), 100, 100, 1e-3f, "both flips");
}

void testPanClamping() {
  std::printf("Clamped pan preserves coverage\n");

  rc::TransformParams p;
  p.srcWidth = 1920;
  p.srcHeight = 1080;
  p.dstWidth = 1280;
  p.dstHeight = 720;
  p.fit = rc::FitMode::Fill;
  p.zoom = 1.5f;

  const rc::Vec2 slack = rc::panSlack(p);
  check(slack.x > 0.0f && slack.y > 0.0f, "zoomed-in Fill should allow some pan");

  // Drag far past the limit from several directions; coverage must survive all of
  // them. Pushing on both axes at once is the case that a naive per-axis clamp gets
  // wrong, because at these angles a canvas-space drag eats slack on both source
  // axes simultaneously.
  const float pushes[][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}, {-1, -1}, {-1, 1}};

  for (int angle = -180; angle <= 180; angle += 11) {
    for (const auto& push : pushes) {
      p.rotationDeg = static_cast<float>(angle);
      p.panX = push[0] * 100000.0f;
      p.panY = push[1] * 100000.0f;
      rc::clampPanForCoverage(p);

      const rc::Mat3 d2s = rc::destToSource(p);
      for (rc::Vec2 c : corners(1280.0f, 720.0f)) {
        check(inRect(d2s.apply(c), 1920.0f, 1080.0f, 1.0f),
              "clamped pan still exposed an edge at " + std::to_string(angle) + " deg");
      }
    }
  }

  // Clamping is idempotent: an already-legal pan must survive untouched.
  p.rotationDeg = 21.0f;
  p.panX = 100000.0f;
  p.panY = 40000.0f;
  rc::clampPanForCoverage(p);
  const float onceX = p.panX;
  const float onceY = p.panY;
  rc::clampPanForCoverage(p);
  checkNear(p.panX, onceX, 1e-2f, "clamp is idempotent (x)");
  checkNear(p.panY, onceY, 1e-2f, "clamp is idempotent (y)");

  // At exactly the Fill bound the binding axis has no slack at all. The other axis
  // generally does -- Fill takes the max of two ratios, so only one can be tight.
  p.zoom = 1.0f;
  p.rotationDeg = 33.0f;
  const rc::Vec2 tight = rc::panSlack(p);
  checkNear(std::min(tight.x, tight.y), 0.0f, 1e-3f, "one axis is tight at the Fill bound");
}

void testDirectGeometryValues() {
  std::printf("Direct bounds, Stretch scale and pan slack values\n");

  rc::TransformParams p;
  p.srcWidth = 4;
  p.srcHeight = 3;
  p.dstWidth = 12;
  p.dstHeight = 8;
  p.rotationDeg = 90.0f;
  const rc::Vec2 bounds = rc::rotatedBounds(p);
  checkNear(bounds.x, 3.0f, 1e-5f, "a quarter turn swaps the direct bound width");
  checkNear(bounds.y, 4.0f, 1e-5f, "a quarter turn swaps the direct bound height");

  p.rotationDeg = 0.0f;
  p.fit = rc::FitMode::Stretch;
  const rc::Vec2 stretch = rc::fitScale(p);
  checkNear(stretch.x, 3.0f, 1e-5f, "Stretch uses the exact horizontal ratio");
  checkNear(stretch.y, 8.0f / 3.0f, 1e-5f, "Stretch uses the exact vertical ratio");

  p.srcWidth = 1920;
  p.srcHeight = 1080;
  p.dstWidth = 1280;
  p.dstHeight = 720;
  p.fit = rc::FitMode::Fill;
  p.zoom = 2.0f;
  const rc::Vec2 slack = rc::panSlack(p);
  checkNear(slack.x, 480.0f, 1e-3f, "two-times zoom leaves 480 source pixels of x slack");
  checkNear(slack.y, 270.0f, 1e-3f, "two-times zoom leaves 270 source pixels of y slack");
}

void testDegenerateInputs() {
  std::printf("Degenerate sizes fall back to identity rather than dividing by zero\n");

  rc::TransformParams p;  // all zeros
  const rc::Mat3 d2s = rc::destToSource(p);
  checkPoint(d2s.apply({5, 9}), 5, 9, 1e-5f, "zero-sized transform is identity");
  checkNear(rc::fitScale(p).x, 0.0f, 1e-5f, "zero-sized fit scale");

  p.srcWidth = 100;
  p.srcHeight = 100;
  p.dstWidth = 100;
  p.dstHeight = 100;
  p.zoom = 0.0f;  // a slider dragged to zero must not produce NaNs
  const rc::Mat3 z = rc::destToSource(p);
  check(std::isfinite(z.apply({1, 1}).x), "zero zoom stays finite");

  p.zoom = std::numeric_limits<float>::quiet_NaN();
  p.panX = std::numeric_limits<float>::infinity();
  p.panY = -std::numeric_limits<float>::infinity();
  const rc::Mat3 hostileControls = rc::destToSource(p);
  const rc::Vec2 mapped = hostileControls.apply({25, 75});
  check(std::isfinite(mapped.x) && std::isfinite(mapped.y),
        "non-finite zoom and pan are sanitized");

  p.rotationDeg = std::numeric_limits<float>::quiet_NaN();
  checkPoint(rc::destToSource(p).apply({5, 9}), 5, 9, 1e-5f,
             "non-finite rotation falls back to identity");
  rc::clampPanForCoverage(p);
  checkNear(p.panX, 0.0f, 1e-5f, "invalid transform clears pan x");
  checkNear(p.panY, 0.0f, 1e-5f, "invalid transform clears pan y");
}

}  // namespace

int main() {
  std::printf("rc::transform\n\n");

  testMat3Primitives();
  testIdentityCase();
  testRoundTrip();
  testQuarterTurn();
  testFitNeverCrops();
  testFillNeverLeavesEmptyCorners();
  testFillIsTight();
  testFlips();
  testPanClamping();
  testDirectGeometryValues();
  testDegenerateInputs();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
