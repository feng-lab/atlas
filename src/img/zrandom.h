#pragma once

#include <random>
#include <algorithm>
#include <vector>
#include <type_traits>
#include "zeigenutils.h"

#define ZRandomInstance nim::ZRandom::instance()
// always use ZRandomInstance or instance() to get the static instance of ZRandom, one engine is enough

namespace nim {

class ZRandom
{
public:
  ZRandom();

  static ZRandom& instance();

  inline int randInt(int maxValue = std::numeric_limits<int>::max(), int minValue = 0)
  {
    return randIntType(maxValue, minValue);
  }

  template<typename IntType>
  inline IntType randIntType(IntType maxValue = std::numeric_limits<IntType>::max(), IntType minValue = 0)
  {
    std::uniform_int_distribution<IntType> dist(minValue, maxValue);
    return dist(m_eng);
  }

  template<typename Real>
  inline Real randReal(Real maxValue = 1.0, Real minValue = 0.0)
  {
    std::uniform_real_distribution<Real> dist(minValue, maxValue);
    return dist(m_eng);
  }

  inline double randDouble(double maxValue = 1.0, double minValue = 0.0)
  {
    return randReal(maxValue, minValue);
  }

  template<typename T>
  inline typename Eigen::NumTraits<T>::NonInteger randNormal(T mean = 0, T sigma = 1)
  {
    std::normal_distribution<typename Eigen::NumTraits<T>::NonInteger> dist(mean,
                                                                            sigma);  //para type should be converted automatically
    return dist(m_eng);
  }

  // similar to matlab randn, if nCol == -1, nCol = nRow
  template<typename Real>
  Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic> randn(int nRow, int nCol = -1, Real mean = 0, Real sigma = 1)
  {
    CHECK(nRow > 0);
    if (nCol == -1)
      nCol = nRow;
    CHECK(nCol > 0);
    Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic> mat(nRow, nCol);
    for (int r = 0; r < nRow; r++)
      for (int c = 0; c < nCol; c++) {
        mat(r, c) = randNormal(mean, sigma);
      }
    return mat;
  }

  template<typename Real>
  Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic> randPositiveDefiniteMatrix(int dim)
  {
    Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic> tmp = randn<Real>(dim);
    Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic> Mat = tmp * tmp.transpose();
    while (!ZEigenUtils::matrixIsPositiveDefinite(Mat, 1e-5))
      Mat += Eigen::Matrix<Real, Eigen::Dynamic, 1>::Constant(dim, 0.001).asDiagonal();
    return Mat;
  }

  template<typename IntType>
  inline std::vector<IntType> randPermutation(IntType maxValue, IntType minValue = 0)
  {
    static_assert(std::is_integral<IntType>::value, "randPermutation requires integer type");
    std::vector<IntType> res;
    if (maxValue >= minValue) {
      res.resize(maxValue - minValue + 1);
      for (IntType i = minValue; i <= maxValue; ++i)
        res[i - minValue] = i;
      std::shuffle(res.begin(), res.end(), m_eng);
    }
    return res;
  }

  std::mt19937_64& engine()
  { return m_eng; }

private:
  std::mt19937_64 m_eng;
};

} // namespace nim
