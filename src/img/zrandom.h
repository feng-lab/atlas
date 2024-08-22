#pragma once

#include <algorithm>
#include <random>
#include <vector>

// use instance() to get the static instance of ZRandom, one engine is enough

namespace nim {

class ZRandom
{
public:
  // instance is thread_local so safe to use in multiple threads
  static ZRandom& instance();

  ZRandom()
    : m_eng(rd())
  {}

  template<typename IntType = int>
  IntType randInt(IntType maxValue = std::numeric_limits<IntType>::max(), IntType minValue = 0)
  {
    std::uniform_int_distribution<IntType> dist(minValue, maxValue);
    return dist(m_eng);
  }

  template<typename Real = double>
  Real randReal(Real maxValue = 1.0, Real minValue = 0.0)
  {
    std::uniform_real_distribution<Real> dist(minValue, maxValue);
    return dist(m_eng);
  }

  template<typename IntType>
  std::vector<IntType> randPermutation(IntType maxValue, IntType minValue = 0)
  {
    static_assert(std::is_integral_v<IntType>, "randPermutation requires integer type");
    std::vector<IntType> res;
    if (maxValue >= minValue) {
      res.resize(maxValue - minValue + 1);
      for (IntType i = minValue; i <= maxValue; ++i) {
        res[i - minValue] = i;
      }
      std::shuffle(res.begin(), res.end(), m_eng);
    }
    return res;
  }

  std::mt19937_64& engine()
  {
    return m_eng;
  }

private:
  std::random_device rd;
  std::mt19937_64 m_eng;
};

} // namespace nim
