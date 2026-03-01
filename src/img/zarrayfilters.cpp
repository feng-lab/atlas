#include "zarrayfilters.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace nim {

void averageSmooth1DLegacyLike(const double* in, size_t length, int wndsize, double* out)
{
  if (in == nullptr || out == nullptr) {
    return;
  }
  if (length == 0) {
    return;
  }
  if (wndsize <= 0) {
    std::memcpy(out, in, sizeof(double) * length);
    return;
  }

  const int rightSpan = wndsize / 2;
  const int leftSpan = wndsize - rightSpan - 1;

  for (size_t i = 0; i < length; ++i) {
    size_t leftSide = 0;
    if (i > static_cast<size_t>(leftSpan)) {
      leftSide = i - static_cast<size_t>(leftSpan);
    }

    const size_t rightSide = std::min(i + static_cast<size_t>(rightSpan), length - 1);
    const size_t size = rightSide - leftSide + 1;

    double sum = 0.0;
    for (size_t j = leftSide; j <= rightSide; ++j) {
      sum += in[j];
    }
    out[i] = sum / static_cast<double>(size);
  }
}

void medianFilter1DLegacyLike(const double* in, size_t length, int wndsize, double* out)
{
  if (in == nullptr || out == nullptr) {
    return;
  }
  if (length == 0) {
    return;
  }
  if (wndsize <= 0) {
    std::memcpy(out, in, sizeof(double) * length);
    return;
  }

  std::vector<double> buffer(static_cast<size_t>(wndsize));

  const int rightSpan = wndsize / 2;
  const int leftSpan = wndsize - rightSpan - 1;

  for (size_t i = 0; i < length; ++i) {
    size_t leftSide = 0;
    if (i > static_cast<size_t>(leftSpan)) {
      leftSide = i - static_cast<size_t>(leftSpan);
    }

    const size_t rightSide = std::min(i + static_cast<size_t>(rightSpan), length - 1);
    const size_t size = rightSide - leftSide + 1;

    std::memcpy(buffer.data(), in + leftSide, sizeof(double) * size);
    std::sort(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));

    const size_t medpos = size / 2;
    if (size % 2 == 0) {
      // Legacy tie-break: choose the upper median unless the lower median equals the original center sample.
      if (buffer[medpos - 1] == in[i]) {
        out[i] = in[i];
      } else {
        out[i] = buffer[medpos];
      }
    } else {
      out[i] = buffer[medpos];
    }
  }
}

} // namespace nim
