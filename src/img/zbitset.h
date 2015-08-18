#ifndef ZBITSET_H
#define ZBITSET_H

#include <bitset>

namespace nim {

template<size_t N>
size_t bitsetRangeToValue(const std::bitset<N> &bitSet, size_t startBit, size_t endBit)
{
  size_t mask = 1;
  size_t res = 0;
  endBit = std::min(endBit, bitSet.size());
  for (size_t i=startBit; i<endBit; ++i) {
    if (bitSet[i])
      res |= mask;
    mask <<= 1;
  }
  return res;
}

}

#endif // ZBITSET_H
