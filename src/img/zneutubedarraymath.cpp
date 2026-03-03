#include "zneutubedarraymath.h"

#include <cmath>

namespace nim {

double darrayMaxLegacyLike(std::span<const double> data, size_t* idx)
{
  size_t maxIdx = 0;
  for (size_t i = 1; i < data.size(); ++i) {
    if (data[i] > data[maxIdx]) {
      maxIdx = i;
    }
  }

  if (idx != nullptr) {
    *idx = maxIdx;
  }

  return data[maxIdx];
}

double darrayDotNLegacyLike(std::span<const double> a, std::span<const double> b)
{
  double d = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double p = a[i] * b[i];
    if (!std::isnan(p)) {
      d += p;
    }
  }

  return d;
}

double darrayDotNWLegacyLike(std::span<const double> a, std::span<const double> b)
{
  // Port of tz_darray.c::darray_dot_nw(). This is a NaN-aware dot product that
  // balances positive/negative weights based on which samples are valid.
  double w1 = 0.0;
  double w2 = 0.0;
  double nw1 = 0.0;
  double nw2 = 0.0;

  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] > 0.0) {
      w1 += a[i];
    } else {
      w2 -= a[i];
    }

    if (!std::isnan(b[i])) {
      if (a[i] > 0.0) {
        nw1 += a[i];
      } else {
        nw2 -= a[i];
      }
    }
  }

  if (nw1 > 0.0 && nw2 > 0.0) {
    w1 /= nw1;
    w2 /= nw2;
  }

  double d = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!(std::isnan(a[i]) || std::isnan(b[i]))) {
      if (a[i] > 0.0) {
        d += a[i] * b[i] * w1;
      } else {
        d += a[i] * b[i] * w2;
      }
    }
  }

  return d;
}

double darraySumNLegacyLike(std::span<const double> a)
{
  double sum = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!std::isnan(a[i])) {
      sum += a[i];
    }
  }
  return sum;
}

double darrayMeanNLegacyLike(std::span<const double> a)
{
  return darraySumNLegacyLike(a) / static_cast<double>(a.size());
}

double darrayCorrcoefNLegacyLike(std::span<const double> a, std::span<const double> b)
{
  const double mu1 = darrayMeanNLegacyLike(a);
  const double mu2 = darrayMeanNLegacyLike(b);

  double r = 0.0;
  double v1 = 0.0;
  double v2 = 0.0;

  for (size_t i = 0; i < a.size(); ++i) {
    if (!(std::isnan(a[i]) || std::isnan(b[i]))) {
      const double sd1 = a[i] - mu1;
      const double sd2 = b[i] - mu2;
      r += sd1 * sd2;
      v1 += sd1 * sd1;
      v2 += sd2 * sd2;
    }
  }

  if (v1 == 0.0 || v2 == 0.0) {
    return 0.0;
  }

  return r / std::sqrt(v1 * v2);
}

} // namespace nim
