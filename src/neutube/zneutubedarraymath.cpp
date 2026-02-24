#include "zneutubedarraymath.h"

#include "zlog.h"

#include <cmath>

namespace nim::neutube {

double darrayMaxLegacyLike(const double* data, size_t length, size_t* idx)
{
  CHECK(data != nullptr);
  CHECK(length > 0);

  size_t maxIdx = 0;
  for (size_t i = 1; i < length; ++i) {
    if (data[i] > data[maxIdx]) {
      maxIdx = i;
    }
  }

  if (idx != nullptr) {
    *idx = maxIdx;
  }

  return data[maxIdx];
}

double darrayDotNLegacyLike(const double* a, const double* b, size_t length)
{
  CHECK(a != nullptr);
  CHECK(b != nullptr);

  double d = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const double p = a[i] * b[i];
    if (!std::isnan(p)) {
      d += p;
    }
  }

  return d;
}

double darrayDotNWLegacyLike(const double* a, const double* b, size_t length)
{
  CHECK(a != nullptr);
  CHECK(b != nullptr);

  // Port of tz_darray.c::darray_dot_nw(). This is a NaN-aware dot product that
  // balances positive/negative weights based on which samples are valid.
  double w1 = 0.0;
  double w2 = 0.0;
  double nw1 = 0.0;
  double nw2 = 0.0;

  for (size_t i = 0; i < length; ++i) {
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
  for (size_t i = 0; i < length; ++i) {
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

double darraySumNLegacyLike(const double* a, size_t length)
{
  CHECK(a != nullptr);

  double sum = 0.0;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isnan(a[i])) {
      sum += a[i];
    }
  }
  return sum;
}

double darrayMeanNLegacyLike(const double* a, size_t length)
{
  CHECK(length > 0);
  return darraySumNLegacyLike(a, length) / static_cast<double>(length);
}

double darrayCorrcoefNLegacyLike(const double* a, const double* b, size_t length)
{
  CHECK(a != nullptr);
  CHECK(b != nullptr);

  const double mu1 = darrayMeanNLegacyLike(a, length);
  const double mu2 = darrayMeanNLegacyLike(b, length);

  double r = 0.0;
  double v1 = 0.0;
  double v2 = 0.0;

  for (size_t i = 0; i < length; ++i) {
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

} // namespace nim::neutube
