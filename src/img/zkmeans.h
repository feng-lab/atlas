#pragma once

#include "zeigenutils.h"
#include "zbenchtimer.h"
#include "zrandom.h"
#include "zstatisticsutils.h"
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range.h>
#include <limits>
#include <vector>
#include <set>
#include <algorithm>

namespace nim {

template<typename NonInteger = double>
class ZTermCriteria
{
public:
  enum class Type
  {
    MaxIter,
    Eps,
    MaxIterAndEps
  };

  ZTermCriteria()
    : m_type(Type::MaxIterAndEps)
    , m_maxIter(200)
    , m_epsilon(1e-5)
  {}

  explicit ZTermCriteria(size_t maxIter)
    : m_type(Type::MaxIter)
    , m_maxIter(maxIter)
    , m_epsilon(1e-5)
  {}

  explicit ZTermCriteria(NonInteger epsilon)
    : m_type(Type::Eps)
    , m_maxIter(200)
    , m_epsilon(epsilon)
  {}

  ZTermCriteria(size_t maxIter, NonInteger epsilon)
    : m_type(Type::MaxIterAndEps)
    , m_maxIter(maxIter)
    , m_epsilon(epsilon)
  {}

  ZTermCriteria(Type type, size_t maxIter, NonInteger epsilon)
    : m_type(type)
    , m_maxIter(maxIter)
    , m_epsilon(epsilon)
  {}

  bool meet(size_t iter, NonInteger eps) const
  {
    switch (m_type) {
      case Type::MaxIter:
        return iter >= m_maxIter;
      case Type::Eps:
        return eps <= m_epsilon;
      case Type::MaxIterAndEps:
        return iter >= m_maxIter || eps < m_epsilon;
    }
    return true;
  }

  [[nodiscard]] bool willTestEPS() const
  {
    return m_type == Type::Eps || m_type == Type::MaxIterAndEps;
  }

  [[nodiscard]] bool willTestMaxIter() const
  {
    return m_type == Type::MaxIter || m_type == Type::MaxIterAndEps;
  }

  Type type() const
  {
    return m_type;
  }

  [[nodiscard]] size_t maxIter() const
  {
    return m_maxIter;
  }

  NonInteger epsilon() const
  {
    return m_epsilon;
  }

  void setEpsilon(NonInteger eps)
  {
    m_epsilon = eps;
  }

  void setMaxIter(size_t iter)
  {
    m_maxIter = iter;
  }

private:
  Type m_type;
  size_t m_maxIter;
  NonInteger m_epsilon;
};

enum class IterAlgorithmLogLevel
{
  Off,
  Iter,
  Final
};

template<typename NonInteger>
struct ZDistanceEuclidean
{
  Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>
  operator()(const Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& mat1,
             const Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& mat2) const
  {
    Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic> result(mat1.rows(), mat2.rows());

    for (Eigen::Index c = 0; c < result.cols(); ++c) {
      result.col(c) = (mat1.col(0).array() - mat2(c, 0)).abs2().matrix();
      for (Eigen::Index i = 1; i < mat2.cols(); ++i) {
        result.col(c) += (mat1.col(i).array() - mat2(c, i)).abs2().matrix();
      }
    }
    return result.cwiseSqrt();
  }
};

template<typename Real>
struct ZDistanceEuclideanSquared
{
  Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>
  operator()(const Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>& mat1,
             const Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>& mat2) const
  {
    Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic> result(mat1.rows(), mat2.rows());

    for (Eigen::Index c = 0; c < result.cols(); ++c) {
      result.col(c) = (mat1.col(0).array() - mat2(c, 0)).abs2().matrix();
      for (Eigen::Index i = 1; i < mat2.cols(); ++i) {
        result.col(c) += (mat1.col(i).array() - mat2(c, i)).abs2().matrix();
      }
    }
    return result;
  }

  void getCentroids(const Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>& mat,
                    const Eigen::VectorXi& labels,
                    Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>& centroids) const
  {
    centroids.setZero();
    Eigen::Matrix<Real, Eigen::Dynamic, 1> counts(centroids.rows());
    counts.setZero();
    for (Eigen::Index r = 0; r < mat.rows(); ++r) {
      ++counts(labels(r));
      centroids.row(labels(r)) += mat.row(r);
    }

    for (Eigen::Index c = 0; c < centroids.cols(); ++c) {
      centroids.col(c) = centroids.col(c).cwiseQuotient(counts);
    }
  }

  void getCentroids(const Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>& mat,
                    const Eigen::Matrix<Real, Eigen::Dynamic, 1>& weight,
                    const Eigen::VectorXi& labels,
                    Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>& centroids) const
  {
    centroids.setZero();
    Eigen::Matrix<Real, Eigen::Dynamic, 1> counts(centroids.rows());
    counts.setZero();
    for (Eigen::Index r = 0; r < mat.rows(); ++r) {
      counts(labels(r)) += weight(r);
      centroids.row(labels(r)) += mat.row(r) * weight(r);
    }

    for (Eigen::Index c = 0; c < centroids.cols(); ++c) {
      centroids.col(c) = centroids.col(c).cwiseQuotient(counts);
    }
  }

  static Eigen::Matrix<Real, Eigen::Dynamic, 1>
  pairDistance(const Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>& mat1,
               const Eigen::Matrix<Real, Eigen::Dynamic, Eigen::Dynamic>& mat2)
  {
    CHECK(mat1.rows() == mat2.rows() && mat1.cols() == mat2.cols());
    // return (mat1-mat2).rowwise().squaredNorm();
    return (mat1 - mat2).cwiseAbs2().rowwise().sum();
  }
};

template<class RandomAccessIterator, class RandomAccessIterator2>
typename Eigen::NumTraits<typename std::iterator_traits<RandomAccessIterator>::value_type>::NonInteger
weightedMedian(RandomAccessIterator dataBegin,
               RandomAccessIterator dataEnd,
               RandomAccessIterator2 weightBegin,
               RandomAccessIterator2 weightEnd,
               bool hasZeroWeight = false)
{
  using ValueType = typename std::iterator_traits<RandomAccessIterator>::value_type;
  using ValueType2 = typename std::iterator_traits<RandomAccessIterator2>::value_type;
  using ResultType = typename Eigen::NumTraits<ValueType>::NonInteger;
  CHECK(dataEnd > dataBegin && weightEnd - weightBegin >= dataEnd - dataBegin);
  if (hasZeroWeight) {
    std::vector<ResultType> data;
    std::vector<ResultType> weight;
    for (RandomAccessIterator2 iter = weightBegin; iter != weightEnd; ++iter) {
      if (std::abs(*iter) > std::numeric_limits<ValueType2>::epsilon()) {
        data.push_back(dataBegin[iter - weightBegin]);
        weight.push_back(*iter);
      }
    }
    CHECK(data.size() > 0);
    ResultType sum = 0;
    std::vector<size_t> sort_order = argSort(data.begin(), data.end());

    size_t i;
    for (i = 0; i < data.size(); ++i) {
      sum += weight[sort_order[i]];
    }
    while (sum > 0) {
      sum -= 2 * weight[sort_order[--i]];
    }

    ResultType right = data[sort_order[i]]; // the rightmost minimum point
    ResultType left =
      (std::abs(sum) <= std::numeric_limits<ResultType>::epsilon() &&
       std::abs(weight[sort_order[i]] - weight[sort_order[i - 1]]) <= std::numeric_limits<ResultType>::epsilon())
        ? data[sort_order[i - 1]]
        : right;
    return (left + right) / 2.0;
  } else {
    ResultType sum = 0;
    size_t n = dataEnd - dataBegin;
    std::vector<size_t> sort_order = argSort(dataBegin, dataEnd);

    size_t i;
    for (i = 0; i < n; ++i) {
      sum += weightBegin[sort_order[i]];
    }
    while (sum > 0) {
      sum -= 2 * weightBegin[sort_order[--i]];
    }

    ResultType right = dataBegin[sort_order[i]]; // the rightmost minimum point
    ResultType left = (std::abs(sum) <= std::numeric_limits<ResultType>::epsilon() &&
                       std::abs(weightBegin[sort_order[i]] - weightBegin[sort_order[i - 1]]) <=
                         std::numeric_limits<ResultType>::epsilon())
                        ? dataBegin[sort_order[i - 1]]
                        : right;
    return (left + right) / 2.0;
  }
}

template<typename NonInteger>
struct ZDistanceManhattan
{
  Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>
  operator()(const Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& mat1,
             const Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& mat2) const
  {
    Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic> result(mat1.rows(), mat2.rows());

    for (Eigen::Index c = 0; c < result.cols(); ++c) {
      result.col(c) = (mat1.col(0).array() - mat2(c, 0)).abs().matrix();
      for (Eigen::Index i = 1; i < mat2.cols(); ++i) {
        result.col(c) += (mat1.col(i).array() - mat2(c, i)).abs().matrix();
      }
    }
    return result;
  }

  void getCentroids(const Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& mat,
                    const Eigen::VectorXi& labels,
                    Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& centroids) const
  {
    // get median of each dimension
    Eigen::VectorXi counts(centroids.rows());
    std::vector<std::unique_ptr<Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>>> sepMats;
    std::vector<int> sepMatsRowIdxs;

    counts.setZero();
    for (Eigen::Index r = 0; r < labels.rows(); ++r) {
      ++counts(labels(r));
    }

    for (Eigen::Index i = 0; i < counts.rows(); ++i) {
      sepMats.emplace_back(
        std::make_unique<Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>>(counts(i), mat.cols()));
      sepMatsRowIdxs.push_back(0);
    }

    for (Eigen::Index r = 0; r < mat.rows(); ++r) {
      sepMats[labels(r)]->row(sepMatsRowIdxs[labels(r)]++) = mat.row(r);
    }

    for (size_t i = 0; i < sepMats.size(); ++i) {
      // get median of each col
      for (Eigen::Index c = 0; c < sepMats[i]->cols(); ++c) {
        centroids(i, c) = median(sepMats[i]->col(c).data(), sepMats[i]->col(c).data() + counts(i));
      }
    }
  }

  void getCentroids(const Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& mat,
                    const Eigen::Matrix<NonInteger, Eigen::Dynamic, 1>& weight,
                    const Eigen::VectorXi& labels,
                    Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& centroids) const
  {
    // get median of each dimension
    Eigen::VectorXi counts(centroids.rows());
    std::vector<std::unique_ptr<Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>>> sepMats;
    std::vector<std::vector<NonInteger>> sepWeights(centroids.rows());
    std::vector<int> sepMatsRowIdxs;

    counts.setZero();
    for (Eigen::Index r = 0; r < labels.rows(); ++r) {
      ++counts(labels(r));
    }

    for (Eigen::Index i = 0; i < counts.rows(); ++i) {
      sepMats.emplace_back(
        std::make_unique<Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>>(counts(i), mat.cols()));
      sepMatsRowIdxs.push_back(0);
    }

    for (Eigen::Index r = 0; r < mat.rows(); ++r) {
      sepMats[labels(r)]->row(sepMatsRowIdxs[labels(r)]++) = mat.row(r);
      sepWeights[labels(r)].push_back(weight(r));
    }

    for (size_t i = 0; i < sepMats.size(); ++i) {
      // get median of each col
      for (Eigen::Index c = 0; c < sepMats[i]->cols(); ++c) {
        centroids(i, c) = weightedMedian(sepMats[i]->col(c).data(),
                                         sepMats[i]->col(c).data() + counts(i),
                                         sepWeights[i].begin(),
                                         sepWeights[i].end());
      }
    }
  }
};

template<typename NonInteger>
struct ZDistanceChebychev
{
  Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>
  operator()(const Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& mat1,
             const Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic>& mat2) const
  {
    Eigen::Matrix<NonInteger, Eigen::Dynamic, Eigen::Dynamic> result(mat1.rows(), mat2.rows());

    for (Eigen::Index c = 0; c < result.cols(); ++c) {
      result.col(c) = (mat1.col(0).array() - mat2(c, 0)).abs().matrix();
      for (Eigen::Index i = 1; i < mat2.cols(); ++i) {
        result.col(c) = result.col(c).cwiseMax((mat1.col(i).array() - mat2(c, i)).abs().matrix());
      }
    }
    return result;
  }
};

template<class T, class WeightT, typename Distance>
class ZKMeans;

template<class T, class WeightT, typename Distance>
class _ZKmeansReduce
{
  const ZKMeans<T, WeightT, Distance>* m_kmeans;

public:
  typename ZKMeans<T, WeightT, Distance>::InterResult m_result;

  void operator()(const tbb::blocked_range<size_t>& range)
  {
    for (size_t i = range.begin(); i != range.end(); ++i) {
      typename ZKMeans<T, WeightT, Distance>::InterResult inter = m_kmeans->runOneAttempt();
      if (m_result.compactness > inter.compactness) {
        m_result = inter;
      }
    }
  }

  _ZKmeansReduce(_ZKmeansReduce& x, tbb::split)
    : m_kmeans(x.m_kmeans)
  {}

  void join(const _ZKmeansReduce& y)
  {
    if (m_result.compactness > y.m_result.compactness) {
      m_result = y.m_result;
    }
  }

  explicit _ZKmeansReduce(const ZKMeans<T, WeightT, Distance>* kmeans)
    : m_kmeans(kmeans)
  {}
};

template<class T,
         class WeightT = float,
         typename Distance = ZDistanceEuclideanSquared<typename MaxFloatType<T, WeightT>::type>>
class ZKMeans
{
public:
  using ResultDataType = typename MaxFloatType<T, WeightT>::type;
  using MatrixXrt = Eigen::Matrix<ResultDataType, Eigen::Dynamic, Eigen::Dynamic>; // matrix of result data type
  using VectorXrt = Eigen::Matrix<ResultDataType, Eigen::Dynamic, 1>; // vector of result data type
  using initCentersFunction = void (ZKMeans::*)(MatrixXrt& centroids) const;

  enum class InitCentersMethod
  {
    Random,
    KmeansPP,
    Gonzales
  };

  struct InterResult
  {
    InterResult(size_t nclasses, size_t nData, size_t nDims)
      : centroids(nclasses, nDims)
      , initCentroids(nclasses, nDims)
      , labels(nData)
      , compactness(std::numeric_limits<ResultDataType>::max())
    {}

    InterResult()
      : compactness(std::numeric_limits<ResultDataType>::max())
    {}

    void swap(InterResult& other) noexcept
    {
      std::swap(compactness, other.compactness);
      labels.swap(other.labels);
      centroids.swap(other.centroids);
      initCentroids.swap(other.initCentroids);
    }

    MatrixXrt centroids;
    MatrixXrt initCentroids;
    Eigen::VectorXi labels;
    ResultDataType compactness;
  };

  friend class _ZKmeansReduce<T, WeightT, Distance>;

  ZKMeans(const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>& data,
          size_t nclasses,
          size_t nattempts,
          ZTermCriteria<ResultDataType> termCriteria = ZTermCriteria<ResultDataType>(),
          InitCentersMethod initMethod = InitCentersMethod::KmeansPP,
          IterAlgorithmLogLevel logLevel = IterAlgorithmLogLevel::Off)
    : m_nclasses(nclasses)
    , m_nattemps(nattempts)
    , m_termCriteria(termCriteria)
    , m_distanceFun(Distance())
    , m_logLevel(logLevel)
    , m_hasWeight(false)
  {
    if constexpr (std::is_same_v<T, ResultDataType>) {
      // reinterpret_cast allowed (AliasedType is (possibly cv-qualified) DynamicType)
      m_pData = reinterpret_cast<const MatrixXrt*>(&data);
    } else {
      m_NonIntegerData = data.template cast<ResultDataType>();
      m_pData = &m_NonIntegerData;
    }
    checkData();
    switch (initMethod) {
      case InitCentersMethod::Random:
        m_initCentersFunPtr = &ZKMeans::initializeCentroidsRandom;
        break;
      case InitCentersMethod::KmeansPP:
        m_initCentersFunPtr = &ZKMeans::initializeCentroidsKMeansPP;
        break;
      case InitCentersMethod::Gonzales:
        m_initCentersFunPtr = &ZKMeans::initializeCentroidsGonzales;
        break;
    }
  }

  ZKMeans(const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>& data,
          const Eigen::Matrix<WeightT, Eigen::Dynamic, 1>& weight,
          size_t nclasses,
          size_t nattempts,
          ZTermCriteria<ResultDataType> termCriteria = ZTermCriteria<ResultDataType>(),
          InitCentersMethod initMethod = InitCentersMethod::KmeansPP,
          IterAlgorithmLogLevel logLevel = IterAlgorithmLogLevel::Off)
    : m_nclasses(nclasses)
    , m_nattemps(nattempts)
    , m_termCriteria(termCriteria)
    , m_distanceFun(Distance())
    , m_logLevel(logLevel)
    , m_hasWeight(true)
  {
    bool hasZeroWeight = false;
    for (Eigen::Index i = 0; i < data.rows(); ++i) {
      if (weight(i) < std::numeric_limits<ResultDataType>::epsilon() * 1e3) {
        hasZeroWeight = true;
        break;
      }
    }
    if (hasZeroWeight) {
      m_NonIntegerData = MatrixXrt(data.rows(), data.cols());
      m_NonIntegerWeight = VectorXrt(weight.rows());
      Eigen::Index numRows = 0;
      for (Eigen::Index i = 0; i < data.rows(); ++i) {
        if (weight(i) >= std::numeric_limits<ResultDataType>::epsilon() * 1e3) {
          m_NonIntegerData.row(numRows) = data.row(i).template cast<ResultDataType>();
          m_NonIntegerWeight(numRows++) = weight(i);
        }
      }
      m_NonIntegerData.conservativeResize(numRows, Eigen::NoChange);
      m_NonIntegerWeight.conservativeResize(numRows);
      m_pData = &m_NonIntegerData;
      m_pWeight = &m_NonIntegerWeight;
    } else {
      if constexpr (std::is_same_v<T, ResultDataType>) {
        m_pData = reinterpret_cast<const MatrixXrt*>(&data);
      } else {
        m_NonIntegerData = data.template cast<ResultDataType>();
        m_pData = &m_NonIntegerData;
      }
      if constexpr (std::is_same_v<WeightT, ResultDataType>) {
        m_pWeight = reinterpret_cast<const VectorXrt*>(&weight);
      } else {
        m_NonIntegerWeight = weight.template cast<ResultDataType>();
        m_pWeight = &m_NonIntegerWeight;
      }
    }

    checkData();
    switch (initMethod) {
      case InitCentersMethod::Random:
        m_initCentersFunPtr = &ZKMeans::initializeCentroidsRandom;
        break;
      case InitCentersMethod::KmeansPP:
        m_initCentersFunPtr = &ZKMeans::initializeCentroidsKMeansPP;
        break;
      case InitCentersMethod::Gonzales:
        m_initCentersFunPtr = &ZKMeans::initializeCentroidsGonzales;
        break;
    }
  }

  void setLogLevel(IterAlgorithmLogLevel logLevel)
  {
    m_logLevel = logLevel;
  }

  ResultDataType run(bool useMultithreading = true)
  {
    if (!m_hasEnoughData) {
      m_initCentroids = m_uniqueDatas;
      m_centroids = m_uniqueDatas;
      m_labels = m_uniqueLabels;
      if (m_logLevel == IterAlgorithmLogLevel::Iter || m_logLevel == IterAlgorithmLogLevel::Final) {
        LOG(INFO) << "KMeans Data is not enough";
        LOG(INFO) << "KMeans Final Centroids:\n" << m_centroids;
        LOG(INFO) << "KMeans Final Potential: 0";
      }
      return -1;
    }

    if (m_termCriteria.willTestEPS()) {
      m_termCriteria.setEpsilon(std::max(m_termCriteria.epsilon(), 0.));
      m_termCriteria.setEpsilon(m_termCriteria.epsilon() * m_termCriteria.epsilon()); // compare square dist
    }
    if (m_termCriteria.willTestMaxIter()) {
      m_termCriteria.setMaxIter(std::max<size_t>(m_termCriteria.maxIter(), 2));
    }

    if (m_nclasses == 1) {
      m_nattemps = 1;
      m_termCriteria.setMaxIter(2);
    }

    if (useMultithreading && m_nattemps > 1) {
      _ZKmeansReduce<T, WeightT, Distance> km(this);
      tbb::parallel_reduce(tbb::blocked_range<size_t>(0, m_nattemps), km);
      InterResult& result = km.m_result;

      m_centroids.swap(result.centroids);
      m_initCentroids.swap(result.initCentroids);
      m_labels.swap(result.labels);
      if (m_logLevel == IterAlgorithmLogLevel::Iter || m_logLevel == IterAlgorithmLogLevel::Final) {
        LOG(INFO) << "KMeans Initial Centroids:\n" << m_initCentroids;
        LOG(INFO) << "KMeans Final Centroids:\n" << m_centroids;
        LOG(INFO) << "KMeans Final Potential: " << result.compactness;
      }
      return result.compactness;
    }

    ResultDataType bestCompactness = std::numeric_limits<ResultDataType>::max();
    MatrixXrt oldCentroids(m_nclasses, m_pData->cols());
    MatrixXrt centroids(m_nclasses, m_pData->cols());
    MatrixXrt initCentroids(m_nclasses, m_pData->cols());
    Eigen::VectorXi labels(m_pData->rows());
    m_initCentroids.resize(m_nclasses, m_pData->cols());
    m_centroids.resize(m_nclasses, m_pData->cols());
    m_labels.resize(m_pData->rows());

    for (size_t a = 0; a < m_nattemps; ++a) {
      ResultDataType maxCenterShift = std::numeric_limits<ResultDataType>::max();
      ResultDataType compactness = 0;
      for (size_t iter = 0; !m_termCriteria.meet(iter, maxCenterShift); ++iter) {
        if (iter == 0) {
          (this->*m_initCentersFunPtr)(initCentroids);
          centroids = initCentroids;
        } else {
          centroids.swap(oldCentroids);
          centroids.setZero();
          // get centers
          if (m_hasWeight) {
            m_distanceFun.getCentroids(*m_pData, *m_pWeight, labels, centroids);
          } else {
            m_distanceFun.getCentroids(*m_pData, labels, centroids);
          }

          maxCenterShift = (centroids - oldCentroids).rowwise().squaredNorm().maxCoeff();
        }

        // get labels
        compactness = 0;
        MatrixXrt allDists = m_distanceFun(*m_pData, centroids);
        for (Eigen::Index r = 0; r < m_pData->rows(); ++r) {
          Eigen::Index index;
          if (m_hasWeight) {
            compactness += allDists.row(r).minCoeff(&index) * (*m_pWeight)(r);
          } else {
            compactness += allDists.row(r).minCoeff(&index);
          }
          labels(r) = static_cast<int>(index);
        }

        if (m_logLevel == IterAlgorithmLogLevel::Iter) {
          LOG(INFO) << "KMeans attempt " << a << " Iter: " << iter << " Potential: " << compactness;
          LOG(INFO) << "KMeans Centroids:\n" << centroids;
        }
      }

      if (compactness < bestCompactness) {
        bestCompactness = compactness;
        m_centroids.swap(centroids);
        m_initCentroids.swap(initCentroids);
        m_labels.swap(labels);
      }
    }

    if (m_logLevel == IterAlgorithmLogLevel::Iter || m_logLevel == IterAlgorithmLogLevel::Final) {
      LOG(INFO) << "KMeans Initial Centroids:\n" << m_initCentroids;
      LOG(INFO) << "KMeans Final Centroids:\n" << m_centroids;
      LOG(INFO) << "KMeans Final Potential: " << bestCompactness;
    }
    return bestCompactness;
  }

  [[nodiscard]] size_t numOfClusters() const
  {
    return m_nclasses;
  }

  MatrixXrt centroids() const
  {
    return m_centroids;
  }

  [[nodiscard]] Eigen::VectorXi labels() const
  {
    return m_labels;
  }

protected:
  bool checkData() // check if there are enough data points to make m_nclasses clusters
  {
    if (m_nclasses == 0 || m_pData->rows() == 0) {
      m_hasEnoughData = false;
      m_nclasses = 0;
      LOG(ERROR) << "number of class or number of data points is 0";
      return false;
    }
    if (m_hasWeight && m_pWeight->size() < m_pData->rows()) {
      m_hasEnoughData = false;
      m_nclasses = 0;
      LOG(ERROR) << "weight data is not enough: number of weight value is " << m_pWeight->size()
                 << " , number of data points is " << m_pData->rows();
      return false;
    }

    size_t nUniqueData = 0;
    m_uniqueDatas.resize(m_nclasses + 1, m_pData->cols());
    m_uniqueDatas.row(nUniqueData++) = m_pData->row(0);
    for (Eigen::Index r = 1; r < m_pData->rows(); ++r) {
      MatrixXrt dist = m_distanceFun(m_uniqueDatas.topRows(nUniqueData), m_pData->row(r));
      if ((dist.array() <= Eigen::NumTraits<ResultDataType>::dummy_precision()).any()) { // duplicate
        continue;
      }
      m_uniqueDatas.row(nUniqueData++) = m_pData->row(r);
      if (nUniqueData > m_nclasses) {
        m_hasEnoughData = true;
        return true;
      }
    }

    if (nUniqueData <= m_nclasses) {
      m_hasEnoughData = false;
      m_nclasses = nUniqueData;
      m_uniqueDatas.conservativeResize(nUniqueData, Eigen::NoChange);
      // get labels
      m_uniqueLabels.resize(m_pData->rows());
      MatrixXrt allDists = m_distanceFun(*m_pData, m_uniqueDatas);
      for (Eigen::Index r = 0; r < m_pData->rows(); ++r) {
        Eigen::Index index;
        allDists.row(r).minCoeff(&index);
        m_uniqueLabels(r) = static_cast<int>(index);
      }
    }
    return false;
  }

  InterResult runOneAttempt() const
  {
    InterResult result(m_nclasses, m_pData->rows(), m_pData->cols());
    MatrixXrt oldCentroids(m_nclasses, m_pData->cols());
    ResultDataType maxCenterShift = std::numeric_limits<ResultDataType>::max();
    for (size_t iter = 0; !m_termCriteria.meet(iter, maxCenterShift); iter++) {
      if (iter == 0) {
        (this->*m_initCentersFunPtr)(result.initCentroids);
        result.centroids = result.initCentroids;
      } else {
        result.centroids.swap(oldCentroids);
        result.centroids.setZero();
        // get centers
        if (m_hasWeight) {
          m_distanceFun.getCentroids(*m_pData, *m_pWeight, result.labels, result.centroids);
        } else {
          m_distanceFun.getCentroids(*m_pData, result.labels, result.centroids);
        }

        maxCenterShift = (result.centroids - oldCentroids).rowwise().squaredNorm().maxCoeff();
      }

      // get labels
      result.compactness = 0;
      MatrixXrt allDists = m_distanceFun(*m_pData, result.centroids);
      for (Eigen::Index r = 0; r < m_pData->rows(); ++r) {
        Eigen::Index index;
        if (m_hasWeight) {
          result.compactness += allDists.row(r).minCoeff(&index) * (*m_pWeight)(r);
        } else {
          result.compactness += allDists.row(r).minCoeff(&index);
        }
        result.labels(r) = static_cast<int>(index);
      }

      if (m_logLevel == IterAlgorithmLogLevel::Iter) {
        LOG(INFO) << "KMeans Iter: " << iter << " Potential: " << result.compactness;
        LOG(INFO) << "KMeans Centroids:\n" << result.centroids;
      }
    }
    return result;
  }

  void initializeCentroidsKMeansPP(
    MatrixXrt& centroids) const // Arthur & Vassilvitskii (2007) k-means++: The Advantages of Careful Seeding
  {
    std::vector<Eigen::Index> centerIdxs;
    Eigen::Index rnd = ZRandom::instance().randInt(m_pData->rows() - 1);
    centroids.row(0) = m_pData->row(rnd);
    centerIdxs.push_back(rnd);
    VectorXrt distSq(m_pData->rows());
    distSq = m_distanceFun(*m_pData, centroids.row(0));
    if (m_hasWeight) {
      distSq.cwiseProduct(*m_pWeight);
    }
    ResultDataType distSqSum = distSq.sum();

    const size_t numLocalTries = 5;
    for (size_t index = 1; index < m_nclasses; ++index) {
      ResultDataType bestNewDistSqSum = -1;
      Eigen::Index bestNewIndex = -1;
      for (size_t localTrial = 0; localTrial < numLocalTries; ++localTrial) {
        Eigen::Index newIdx = -1;
        while (newIdx == -1) {
          ResultDataType rndd = ZRandom::instance().randReal(distSqSum);
          // draw index with probability
          Eigen::Index j;
          for (j = 0; j < m_pData->rows() - 1; ++j) {
            if (rndd <= distSq(j)) {
              break;
            } else {
              rndd -= distSq(j);
            }
          }
          centroids.row(index) = m_pData->row(j);
          MatrixXrt cendist = m_distanceFun(centroids.topRows(index), centroids.row(index));
          if ((cendist.array() > Eigen::NumTraits<ResultDataType>::dummy_precision()).all()) {
            newIdx = j;
          }
        }
        ResultDataType newDistSqSum = 0;
        MatrixXrt dists = m_distanceFun(*m_pData, centroids.topRows(index + 1));
        if (m_hasWeight) {
          newDistSqSum = dists.rowwise().minCoeff().cwiseProduct(*m_pWeight).sum();
        } else {
          newDistSqSum = dists.rowwise().minCoeff().sum();
        }
        // store best result
        if (bestNewDistSqSum < 0 || newDistSqSum < bestNewDistSqSum) {
          bestNewDistSqSum = newDistSqSum;
          bestNewIndex = newIdx;
        }
      }
      centroids.row(index) = m_pData->row(bestNewIndex);
      centerIdxs.push_back(bestNewIndex);
      distSqSum = bestNewDistSqSum;
      MatrixXrt dists = m_distanceFun(*m_pData, centroids.topRows(index + 1));
      distSq = dists.rowwise().minCoeff();
      if (m_hasWeight) {
        distSq.cwiseProduct(*m_pWeight);
      }
    }
  }

  void initializeCentroidsRandom(MatrixXrt& centroids) const
  {
    std::vector<size_t> randNumbers = ZRandom::instance().randPermutation<size_t>(m_pData->rows() - 1, 0);

    centroids.row(0) = m_pData->row(randNumbers[0]);

    size_t index = 1;
    for (size_t i = 1; i < randNumbers.size(); ++i) {
      centroids.row(index) = m_pData->row(randNumbers[i]);
      MatrixXrt dist = m_distanceFun(centroids.topRows(index), centroids.row(index));
      if ((dist.array() <= Eigen::NumTraits<ResultDataType>::dummy_precision()).any()) { // duplicate centroid
        continue;
      } else {
        ++index;
        if (index >= m_nclasses) {
          break;
        }
      }
    }
    CHECK(index == m_nclasses);
  }

  void initializeCentroidsGonzales(MatrixXrt& centroids) const
  {
    Eigen::Index rnd = ZRandom::instance().randInt(m_pData->rows() - 1);
    centroids.row(0) = m_pData->row(rnd);
    size_t index = 1;
    for (; index < m_nclasses; ++index) {
      Eigen::Index bestIndex = -1;
      ResultDataType bestVal = 0;
      for (Eigen::Index j = 0; j < m_pData->rows(); ++j) {
        MatrixXrt dist = m_distanceFun(centroids.topRows(index), m_pData->row(j));
        ResultDataType mindist = dist.minCoeff();
        if (mindist > bestVal) {
          bestVal = mindist;
          bestIndex = j;
        }
      }
      CHECK(bestIndex >= 0);
      centroids.row(index) = m_pData->row(bestIndex);
    }
  }

private:
  MatrixXrt m_centroids;
  MatrixXrt m_initCentroids;
  Eigen::VectorXi m_labels;
  MatrixXrt m_NonIntegerData;
  VectorXrt m_NonIntegerWeight;
  const MatrixXrt* m_pData;
  const VectorXrt* m_pWeight;
  size_t m_nclasses;
  size_t m_nattemps;
  ZTermCriteria<ResultDataType> m_termCriteria;
  Distance m_distanceFun;

  initCentersFunction m_initCentersFunPtr;
  bool m_hasEnoughData = false; //
  MatrixXrt m_uniqueDatas; // This will be the centroids if we don't have enough data
  Eigen::VectorXi m_uniqueLabels; // see above
  IterAlgorithmLogLevel m_logLevel;
  bool m_hasWeight;
};

} // namespace nim
