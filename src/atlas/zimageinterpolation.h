#pragma once

#include "zimginterface.h"

namespace nim {

class ZImageInterpolation
{
public:
  ZImageInterpolation(Interpolant interp, PadOption padOption = PadOption::Constant, double fillValue = 0.0);

#if 0
  //  'bound'
  //  Assigns values from the fill value array to points that map outside the array and repeats border
  // elements of the array for points that map inside the array (same as 'replicate'). When interpolant
  // is 'nearest', this pad method produces the same results as 'constant'. 'bound' is like 'constant', but avoids
  // mixing fill values and input image values.

  //  'circular'
  //  Pads array with circular repetition of elements within the dimension. Same as padarray.

  //  'constant'
  //  Generates an output array with smooth-looking edges (except when using nearest-neighbor interpolation).
  // For output points that map near the edge of the input array (either inside or outside),
  // it combines input image and fill values. When interpolant is 'nearest', this pad method produces the
  // same results as 'bound'.

  //  'replicate'
  //  Pads array by repeating border elements of array. Same as padarray.

  //  'symmetric'
  //  Pads array with mirror reflections of itself. Same as padarray.
  enum PadMethod {
    Bound, Replicate, Circular, Constant, Symmetric
  };
#else

  // see above, instead we use global PadOption, the only different is bound, we set it as an option for Replicate
  // call this if you want "bound" behavior, and m_padOption must be Replicate and fillValue must be provided
  void setBoundInBorder(bool v)
  { m_boundInBorder = v; }

#endif

  void setPadOption(PadOption po)
  { m_padOption = po; }

  void setInterpolant(Interpolant ip)
  { m_interpolant = ip; }

  PadOption padOption() const
  { return m_padOption; }

  void setFillValue(double v)
  { m_fillValue = v; }

  Interpolant interpolant() const
  { return m_interpolant; }

  // http://www.paulinternet.nl/?page=bicubic
  double cubicInterpolate(double p[4], double x) const;

  double bicubicInterpolate(double p[4][4], double x, double y) const;

  double tricubicInterpolate(double p[4][4][4], double x, double y, double z) const;

  double nCubicInterpolate(int n, double* p, double coordinates[]) const;

  template<typename TPixel>
  double sample(const TPixel* img, size_t width, size_t height, double x, double y) const;

  template<typename TPixel>
  double sample(const TPixel* img, size_t width, size_t height, size_t depth,
                double x, double y, double z) const;

protected:
  inline bool inBound(size_t width, size_t height, double x, double y) const
  { return x >= 0. && x < static_cast<double>(width) && y >= 0. && y < static_cast<double>(height); }

  inline bool inBound(size_t width, size_t height, size_t depth, double x, double y, double z) const
  {
    return x >= 0. && x < static_cast<double>(width) && y >= 0. && y < static_cast<double>(height) && z >= 0. &&
           z < static_cast<double>(depth);
  }

private:
  Interpolant m_interpolant;
  bool m_boundInBorder;
  PadOption m_padOption;
  double m_fillValue;
};

} // namespace nim

