// RemoteCam — geometric transform for the video pipeline.
//
// This is the math behind RemoteCam's headline feature: arbitrary-angle rotation
// of the phone image, with flip, zoom, pan and framing, computed once per frame on
// the CPU and handed to a single D3D11 pixel-shader pass as a 3x3 matrix.
//
// Coordinate system: pixels, origin at top-left, y grows downward (image
// convention, matches D3D11 texture space once divided by size).
//
// Positive `rotationDeg` rotates the image CLOCKWISE as the viewer sees it, so the
// UI's "rotate right" button maps to +90.
//
// The pipeline is a backward map: for each destination pixel the shader computes
// the source pixel to sample, so `destToSource` is the matrix that ships to the GPU.

#ifndef RC_TRANSFORM_H
#define RC_TRANSFORM_H

namespace rc {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

// Row-major 3x3 affine matrix. m[row * 3 + col].
struct Mat3 {
  float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

  static Mat3 identity() { return Mat3{}; }
  static Mat3 translate(float tx, float ty);
  static Mat3 scale(float sx, float sy);
  // Clockwise-as-displayed rotation by `deg`, about the origin.
  static Mat3 rotateDeg(float deg);

  Mat3 operator*(const Mat3& rhs) const;
  Vec2 apply(Vec2 p) const;
};

// How the source is framed into the output canvas when the two differ in size or
// aspect, or when rotation makes the source no longer axis-aligned.
enum class FitMode {
  // Whole source visible. Letterboxes; at non-multiples of 90 degrees the corners
  // of the canvas are empty.
  Fit,
  // Canvas fully covered, source cropped. Guarantees no empty corners at ANY
  // angle -- this is what makes odd angles usable.
  Fill,
  // Non-uniform: the source's rotated bounding box is mapped exactly onto the
  // canvas. Fills the frame but does not preserve aspect ratio.
  Stretch,
};

struct TransformParams {
  int srcWidth = 0;
  int srcHeight = 0;
  int dstWidth = 0;
  int dstHeight = 0;

  float rotationDeg = 0.0f;  // any real angle; positive = clockwise as displayed
  bool flipH = false;
  bool flipV = false;

  FitMode fit = FitMode::Fit;
  float zoom = 1.0f;  // multiplies the fit-derived scale; 1.0 == exactly the fit
  float panX = 0.0f;  // canvas pixels
  float panY = 0.0f;
};

// Scale in destination pixels per source pixel implied by the fit mode at this
// rotation, before `zoom` is applied. Uniform for Fit and Fill; may differ per
// axis for Stretch. Returns {0,0} if any dimension is non-positive.
Vec2 fitScale(const TransformParams& p);

// Destination pixel -> source pixel. This is the matrix the pixel shader uses.
Mat3 destToSource(const TransformParams& p);

// Source pixel -> destination pixel. Inverse of the above; used for hit-testing,
// drag-to-pan, and drawing UI overlays onto the preview.
Mat3 sourceToDest(const TransformParams& p);

// Axis-aligned bounding box, in source pixels, of the source rotated by
// `rotationDeg`. Rotation is about the source centre, so the box is centred too.
Vec2 rotatedBounds(const TransformParams& p);

// Room left over, as half-extents in SOURCE pixels along the SOURCE axes, between
// the sampled window and the edges of the source image. Zero on an axis means that
// axis is already tight and no movement along it is possible.
//
// Deliberately reported in source space: pan is applied in canvas space after
// rotation, so at angles that are not multiples of 90 degrees a canvas-space drag
// consumes slack on both source axes at once. There is no single "max panX" to
// return -- the permitted region is a rotated rectangle, not an axis-aligned box.
// Use this for UI readouts and clampPanForCoverage() to actually constrain a drag.
Vec2 panSlack(const TransformParams& p);

// Clamps `panX`/`panY` in place so the canvas stays fully covered. Projects the pan
// into source axes, clamps against panSlack() there, and maps back -- which is what
// makes the constraint correct at arbitrary angles. Call this after the user drags
// the preview so Fill mode can never reveal a hard edge.
void clampPanForCoverage(TransformParams& p);

}  // namespace rc

#endif  // RC_TRANSFORM_H
