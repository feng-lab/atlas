#include "zneutubedarrayqsort.h"

#include "zlog.h"

namespace nim::neutube {

void darrayQsortLegacy(std::vector<double>* values, std::vector<int>* outIndices)
{
  CHECK(values != nullptr);

  const int length = static_cast<int>(values->size());
  if (outIndices != nullptr) {
    outIndices->resize(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i) {
      (*outIndices)[static_cast<size_t>(i)] = i;
    }
  }

  if (length <= 1) {
    return;
  }

  constexpr int MaxLevels = 100;
  double piv = 0.0;
  int beg[MaxLevels];
  int end[MaxLevels];
  int i = 0;
  int L = 0;
  int R = 0;
  int swap = 0;
  int pivL = 0;

  beg[0] = 0;
  end[0] = length;
  while (i >= 0) {
    L = beg[i];
    R = end[i] - 1;
    if (L < R) {
      piv = (*values)[static_cast<size_t>(L)];
      if (outIndices != nullptr) {
        pivL = (*outIndices)[static_cast<size_t>(L)];
      }
      while (L < R) {
        while (((*values)[static_cast<size_t>(R)] >= piv) && (L < R)) {
          --R;
        }
        if (L < R) {
          if (outIndices != nullptr) {
            (*outIndices)[static_cast<size_t>(L)] = (*outIndices)[static_cast<size_t>(R)];
          }
          (*values)[static_cast<size_t>(L++)] = (*values)[static_cast<size_t>(R)];
        }
        while (((*values)[static_cast<size_t>(L)] <= piv) && (L < R)) {
          ++L;
        }
        if (L < R) {
          if (outIndices != nullptr) {
            (*outIndices)[static_cast<size_t>(R)] = (*outIndices)[static_cast<size_t>(L)];
          }
          (*values)[static_cast<size_t>(R--)] = (*values)[static_cast<size_t>(L)];
        }
      }
      (*values)[static_cast<size_t>(L)] = piv;
      if (outIndices != nullptr) {
        (*outIndices)[static_cast<size_t>(L)] = pivL;
      }

      beg[i + 1] = L + 1;
      end[i + 1] = end[i];
      end[i++] = L;

      if ((end[i] - beg[i]) > (end[i - 1] - beg[i - 1])) {
        swap = beg[i];
        beg[i] = beg[i - 1];
        beg[i - 1] = swap;

        swap = end[i];
        end[i] = end[i - 1];
        end[i - 1] = swap;
      }
    } else {
      --i;
    }
  }
}

} // namespace nim::neutube
