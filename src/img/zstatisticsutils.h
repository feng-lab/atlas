#pragma once

#include "zlog.h"
#include <QList>
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range.h>
#include <vector>
#include <utility>
#include <functional>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace nim {

#define MULTITHREAD_THRESHOLD 1e8

template<typename RandomAccessIterator>
class _MinMaxElementReduce
{
  RandomAccessIterator m_begin;
public:
  std::pair<RandomAccessIterator, RandomAccessIterator> m_minmax;

  void operator()(const tbb::blocked_range<RandomAccessIterator>& range)
  {
    for (RandomAccessIterator it = range.begin(); it != range.end(); ++it) {
      if (*it < *m_minmax.first)
        m_minmax.first = it;
      if (*it > *m_minmax.second)
        m_minmax.second = it;
    }
  }

  _MinMaxElementReduce(_MinMaxElementReduce& x, tbb::split /*unused*/)
    : m_begin(x.m_begin), m_minmax(m_begin, m_begin)
  {}

  void join(const _MinMaxElementReduce& y)
  {
    if (*y.m_minmax.first < *m_minmax.first) {
      m_minmax.first = y.m_minmax.first;
    }
    if (*y.m_minmax.second > *m_minmax.second) {
      m_minmax.second = y.m_minmax.second;
    }
  }

  explicit _MinMaxElementReduce(RandomAccessIterator begin)
    : m_begin(begin), m_minmax(m_begin, m_begin)
  {}
};

template<typename RandomAccessIterator>
std::pair<RandomAccessIterator, RandomAccessIterator>
minMaxElement(RandomAccessIterator begin, RandomAccessIterator end, bool useMultithreading = true)
{
  CHECK(end > begin);
  if (!useMultithreading || end - begin < MULTITHREAD_THRESHOLD) {
    return std::minmax_element(begin, end);
  } else {
    _MinMaxElementReduce<RandomAccessIterator> minmax(begin);
    tbb::parallel_reduce(tbb::blocked_range<RandomAccessIterator>(begin, end), minmax);
    return minmax.m_minmax;
  }
}

template<typename RandomAccessIterator, typename ResultType>
class _SumRangeReduce
{
public:
  ResultType m_sum;

  void operator()(const tbb::blocked_range<RandomAccessIterator>& range)
  {
    m_sum += std::accumulate(range.begin(), range.end(), ResultType(0));
  }

  _SumRangeReduce(_SumRangeReduce& /*unused*/, tbb::split /*unused*/) : m_sum(0)
  {}

  void join(const _SumRangeReduce& y)
  { m_sum += y.m_sum; }

  _SumRangeReduce() : m_sum(0)
  {}
};

template<typename RandomAccessIterator, typename ResultType>
ResultType
sumRange(RandomAccessIterator begin, RandomAccessIterator end, ResultType init, bool useMultithreading = true)
{
  if (!useMultithreading || end - begin < MULTITHREAD_THRESHOLD) {
    return std::accumulate(begin, end, init);
  } else {
    _SumRangeReduce <RandomAccessIterator, ResultType> sum;
    tbb::parallel_reduce(tbb::blocked_range<RandomAccessIterator>(begin, end), sum);
    return init + sum.m_sum;
  }
}

template<class RandomAccessIterator>
double mean(RandomAccessIterator begin, RandomAccessIterator end, bool useMultithreading = true)
{
  using ResultType = double;

  ResultType sum = sumRange(begin, end, static_cast<ResultType>(0), useMultithreading);
  return sum / (end - begin);
}

template<typename RandomAccessIterator, typename DiffIterator, typename ResultType>
class _StandardDeviationReduce
{
  RandomAccessIterator m_begin;
  DiffIterator m_diffbegin;
  ResultType m_meanV;
public:
  ResultType m_sqSum;

  void operator()(const tbb::blocked_range<size_t>& range)
  {
    ResultType meanVLocal = m_meanV;
    std::transform(m_begin + range.begin(), m_begin + range.end(), m_diffbegin + range.begin(),
                   [meanVLocal](ResultType v) { return v - meanVLocal; });
    m_sqSum += std::inner_product(m_diffbegin + range.begin(), m_diffbegin + range.end(), m_diffbegin + range.begin(),
                                  0.0);
  }

  _StandardDeviationReduce(_StandardDeviationReduce& x, tbb::split /*unused*/)
    : m_begin(x.m_begin), m_diffbegin(x.m_diffbegin), m_meanV(x.m_meanV), m_sqSum(0)
  {}

  void join(const _StandardDeviationReduce& y)
  {
    m_sqSum += y.m_sqSum;
  }

  _StandardDeviationReduce(RandomAccessIterator begin, DiffIterator diffbegin, ResultType mean)
    : m_begin(begin), m_diffbegin(diffbegin), m_meanV(mean), m_sqSum(0)
  {}
};

template<class RandomAccessIterator>
void meanAndStandardDeviation(RandomAccessIterator begin, RandomAccessIterator end,
                              double& meanV,
                              double& stdV,
                              bool bias = false,
                              bool useMultithreading = true)
{
  using ResultType = double;
  CHECK(end > begin);
  size_t size = end - begin;

  std::vector<ResultType> diff(size);
  meanV = mean(begin, end, useMultithreading);
  ResultType sq_sum;
  if (!useMultithreading || end - begin < MULTITHREAD_THRESHOLD) {
    std::transform(begin, end, diff.begin(),
                   [meanV](ResultType v) { return v - meanV; });
    sq_sum = std::inner_product(diff.begin(), diff.end(), diff.begin(), 0.0);
  } else {
    _StandardDeviationReduce <RandomAccessIterator, std::vector<ResultType>::iterator, ResultType> sqsum(begin,
                                                                                                         diff.begin(),
                                                                                                         meanV);
    tbb::parallel_reduce(tbb::blocked_range<size_t>(0, size), sqsum);
    sq_sum = sqsum.m_sqSum;
  }
  if (bias || size <= 1) {
    stdV = std::sqrt(sq_sum / size);
  } else {
    stdV = std::sqrt(sq_sum / (size - 1));
  }
}

template<class RandomAccessIterator>
double standardDeviation(RandomAccessIterator begin, RandomAccessIterator end, bool bias = false,
                         bool useMultithreading = true)
{
  double meanV;
  double stdV;
  meanAndStandardDeviation(begin, end, meanV, stdV, bias, useMultithreading);
  return stdV;
}

template<class RandomAccessIterator>
double median(RandomAccessIterator begin, RandomAccessIterator end)
{
  using ValueType = typename std::iterator_traits<RandomAccessIterator>::value_type;
  using ResultType = double;
  CHECK(end > begin);
  size_t size = end - begin;
  size_t middleIdx = size / 2;

  std::vector<ValueType> vec;
  vec.insert(vec.end(), begin, end);
  typename std::vector<ValueType>::iterator target = vec.begin() + middleIdx;
  std::nth_element(vec.begin(), target, vec.end());

  if (size % 2 != 0) { //Odd number of elements
    return static_cast<ResultType>(*target);
  } else {            //Even number of elements
    ResultType a = *target;
    return (a + *std::max_element(vec.begin(), target)) / 2.0;
  }
}

// will change input data
template<class RandomAccessIterator>
double medianInPlace(RandomAccessIterator begin, RandomAccessIterator end)
{
  using ResultType = double;
  CHECK(end > begin);
  size_t size = end - begin;
  size_t middleIdx = size / 2;

  RandomAccessIterator target = begin + middleIdx;
  std::nth_element(begin, target, end);

  if (size % 2 != 0) { //Odd number of elements
    return static_cast<ResultType>(*target);
  }            //Even number of elements
  ResultType a = *target;
  return (a + *std::max_element(begin, target)) / 2.0;
}

} // namespace nim
