#pragma once

namespace nim {

enum class ZMaskToROIOutputType
{
  Spline,
  SampledSpline,
  Polygon
};

enum class ZMaskToROISplineFallback
{
  UsePolygon,
  KeepBestSpline
};

struct ZMaskToROIOptions
{
  ZMaskToROIOutputType outputType = ZMaskToROIOutputType::Spline;
  ZMaskToROISplineFallback splineFallback = ZMaskToROISplineFallback::KeepBestSpline;

  // Pixel-space boundary tolerance used by polygon simplification and by the
  // adaptive natural-spline knot selector.
  double epsilonPx = 5.0;

  // Minimum arc-length distance between inserted spline knots. Zero disables
  // this smoothing constraint so epsilon may refine all the way to contour points.
  double minKnotSpacingPx = 0.0;

  // Sampled-spline mode preserves the historical fixed-stride contour sampling
  // behavior. The stride is min(maxPointSpacing, contourPointCount / targetPoints).
  int sampledSplineTargetPoints = 20;
  int sampledSplineMaxPointSpacing = 30;

  // Keep contour hierarchy and convert child contours into add/subtract ROI ops.
  bool preserveHoles = true;
};

} // namespace nim
