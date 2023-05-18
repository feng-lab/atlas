#pragma once

#include "zlog.h"
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

template<typename Iterator, typename Compare>
Iterator parallel_min_element(Iterator first, Iterator last, Compare comp)
{
  if (first == last) {
    return first; // handle empty range
  }

  return tbb::parallel_reduce(
    tbb::blocked_range<Iterator>(first, last),
    first,
    [&](const tbb::blocked_range<Iterator>& r, Iterator init) -> Iterator {
      auto min = std::min_element(r.begin(), r.end(), comp);
      return comp(*min, *init) ? min : init;
    },
    [comp](Iterator x, Iterator y) -> Iterator {
      return comp(*x, *y) ? x : y;
    });
}

template<typename Iterator>
Iterator parallel_min_element(Iterator first, Iterator last)
{
  return parallel_min_element(first, last, std::less<>());
}

template<typename Iterator, typename Compare>
Iterator parallel_max_element(Iterator first, Iterator last, Compare comp)
{
  if (first == last) {
    return first; // handle empty range
  }

  return tbb::parallel_reduce(
    tbb::blocked_range<Iterator>(first, last),
    first,
    [&](const tbb::blocked_range<Iterator>& r, Iterator init) -> Iterator {
      auto max = std::max_element(r.begin(), r.end(), comp);
      return comp(*init, *max) ? max : init;
    },
    [comp](Iterator x, Iterator y) -> Iterator {
      return comp(*y, *x) ? x : y;
    });
}

template<typename Iterator>
Iterator parallel_max_element(Iterator first, Iterator last)
{
  return parallel_max_element(first, last, std::less<>());
}

template<typename Iterator, typename Compare>
std::pair<Iterator, Iterator> parallel_minmax_element(Iterator first, Iterator last, Compare comp)
{
  if (first == last) {
    return std::make_pair(first, first); // handle empty range
  }

  return tbb::parallel_reduce(
    tbb::blocked_range<Iterator>(first, last),
    std::make_pair(first, first),
    [&](const tbb::blocked_range<Iterator>& r, std::pair<Iterator, Iterator> init) -> std::pair<Iterator, Iterator> {
      auto minmax = std::minmax_element(r.begin(), r.end(), comp);
      return std::make_pair(comp(*minmax.first, *init.first) ? minmax.first : init.first,
                            comp(*init.second, *minmax.second) ? minmax.second : init.second);
    },
    [comp](std::pair<Iterator, Iterator> x, std::pair<Iterator, Iterator> y) -> std::pair<Iterator, Iterator> {
      return std::make_pair(comp(*x.first, *y.first) ? x.first : y.first,
                            comp(*y.second, *x.second) ? x.second : y.second);
    });
}

template<typename Iterator>
std::pair<Iterator, Iterator> parallel_minmax_element(Iterator first, Iterator last)
{
  return parallel_minmax_element(first, last, std::less<>());
}

template<typename Iterator, typename Compare>
std::pair<typename std::iterator_traits<Iterator>::value_type, typename std::iterator_traits<Iterator>::value_type>
parallel_minmax(Iterator first, Iterator last, Compare comp)
{
  if (first == last) {
    throw ZException("Empty range"); // handle empty range
  }

  using value_type = typename std::iterator_traits<Iterator>::value_type;

  return tbb::parallel_reduce(
    tbb::blocked_range<Iterator>(first, last),
    std::make_pair(*first, *first),
    [&](const tbb::blocked_range<Iterator>& r,
        std::pair<value_type, value_type> init) -> std::pair<value_type, value_type> {
      auto minmax = std::minmax_element(r.begin(), r.end(), comp);
      return std::make_pair(comp(*minmax.first, init.first) ? *minmax.first : init.first,
                            comp(init.second, *minmax.second) ? *minmax.second : init.second);
    },
    [comp](std::pair<value_type, value_type> x,
           std::pair<value_type, value_type> y) -> std::pair<value_type, value_type> {
      return std::make_pair(comp(x.first, y.first) ? x.first : y.first, comp(y.second, x.second) ? x.second : y.second);
    });
}

template<typename Iterator>
std::pair<typename std::iterator_traits<Iterator>::value_type, typename std::iterator_traits<Iterator>::value_type>
parallel_minmax(Iterator first, Iterator last)
{
  return parallel_minmax(first, last, std::less<>());
}

template<typename RandomAccessIterator, typename ResultType>
class SumRangeReduce_Impl
{
public:
  ResultType m_sum;

  void operator()(const tbb::blocked_range<RandomAccessIterator>& range)
  {
    m_sum += std::accumulate(range.begin(), range.end(), ResultType(0));
  }

  SumRangeReduce_Impl(SumRangeReduce_Impl& /*unused*/, tbb::split /*unused*/)
    : m_sum(0)
  {}

  void join(const SumRangeReduce_Impl& y)
  {
    m_sum += y.m_sum;
  }

  SumRangeReduce_Impl()
    : m_sum(0)
  {}
};

template<typename RandomAccessIterator, typename ResultType>
ResultType
sumRange(RandomAccessIterator begin, RandomAccessIterator end, ResultType init, bool useMultithreading = true)
{
  if (!useMultithreading || end - begin < MULTITHREAD_THRESHOLD) {
    return std::accumulate(begin, end, init);
  } else {
    SumRangeReduce_Impl<RandomAccessIterator, ResultType> sum;
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
class StandardDeviationReduce_Impl
{
  RandomAccessIterator m_begin;
  DiffIterator m_diffbegin;
  ResultType m_meanV;

public:
  ResultType m_sqSum;

  void operator()(const tbb::blocked_range<size_t>& range)
  {
    ResultType meanVLocal = m_meanV;
    std::transform(m_begin + range.begin(),
                   m_begin + range.end(),
                   m_diffbegin + range.begin(),
                   [meanVLocal](ResultType v) {
                     return v - meanVLocal;
                   });
    m_sqSum +=
      std::inner_product(m_diffbegin + range.begin(), m_diffbegin + range.end(), m_diffbegin + range.begin(), 0.0);
  }

  StandardDeviationReduce_Impl(StandardDeviationReduce_Impl& x, tbb::split /*unused*/)
    : m_begin(x.m_begin)
    , m_diffbegin(x.m_diffbegin)
    , m_meanV(x.m_meanV)
    , m_sqSum(0)
  {}

  void join(const StandardDeviationReduce_Impl& y)
  {
    m_sqSum += y.m_sqSum;
  }

  StandardDeviationReduce_Impl(RandomAccessIterator begin, DiffIterator diffbegin, ResultType mean)
    : m_begin(begin)
    , m_diffbegin(diffbegin)
    , m_meanV(mean)
    , m_sqSum(0)
  {}
};

template<class RandomAccessIterator>
void meanAndStandardDeviation(RandomAccessIterator begin,
                              RandomAccessIterator end,
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
    std::transform(begin, end, diff.begin(), [meanV](ResultType v) {
      return v - meanV;
    });
    sq_sum = std::inner_product(diff.begin(), diff.end(), diff.begin(), 0.0);
  } else {
    StandardDeviationReduce_Impl<RandomAccessIterator, std::vector<ResultType>::iterator, ResultType> sqsum(
      begin,
      diff.begin(),
      meanV);
    tbb::parallel_reduce(tbb::blocked_range<size_t>(0, size), sqsum);
    sq_sum = sqsum.m_sqSum;
  }
  if (bias || size <= 1) {
    stdV = std::sqrt(sq_sum / size);
  } else {
    stdV = std::sqrt(sq_sum / (size - 1.));
  }
}

template<class RandomAccessIterator>
double standardDeviation(RandomAccessIterator begin,
                         RandomAccessIterator end,
                         bool bias = false,
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

  if (size % 2 != 0) { // Odd number of elements
    return static_cast<ResultType>(*target);
  } else { // Even number of elements
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

  if (size % 2 != 0) { // Odd number of elements
    return static_cast<ResultType>(*target);
  } // Even number of elements
  ResultType a = *target;
  return (a + *std::max_element(begin, target)) / 2.0;
}

} // namespace nim
