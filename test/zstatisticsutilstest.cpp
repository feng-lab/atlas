#include "zstatisticsutils.h"
#include "ztest.h"
#include <vector>
#include <random>
#include <algorithm>

TEST(ParallelTest, TestParallelMaxElement)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_max_element(data.begin(), data.end());
  EXPECT_EQ(*result, 10000);
}

TEST(ParallelTest, TestParallelMinElement)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_min_element(data.begin(), data.end());
  EXPECT_EQ(*result, 1);
}

TEST(ParallelTest, TestParallelMinMaxElement)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_minmax_element(data.begin(), data.end());
  EXPECT_EQ(*result.first, 1);
  EXPECT_EQ(*result.second, 10000);
}

TEST(ParallelTest, TestParallelMinMax)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_minmax(data.begin(), data.end());
  EXPECT_EQ(result.first, 1);
  EXPECT_EQ(result.second, 10000);
}

TEST(ParallelTest, TestEmptyRange)
{
  std::vector<int> data;
  EXPECT_EQ(nim::parallel_max_element(data.begin(), data.end()), data.end());
  EXPECT_EQ(nim::parallel_min_element(data.begin(), data.end()), data.end());
  EXPECT_EQ(nim::parallel_minmax_element(data.begin(), data.end()).first, data.end());
  EXPECT_EQ(nim::parallel_minmax_element(data.begin(), data.end()).second, data.end());
}

TEST(ParallelTest, TestParallelMaxElementWithComp)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_max_element(data.begin(), data.end(), std::greater<>());
  EXPECT_EQ(*result, 1); // with std::greater<>, max is the smallest number
}

TEST(ParallelTest, TestParallelMinElementWithComp)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_min_element(data.begin(), data.end(), std::greater<>());
  EXPECT_EQ(*result, 10000); // with std::greater<>, min is the largest number
}

TEST(ParallelTest, TestParallelMinMaxElementWithComp)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_minmax_element(data.begin(), data.end(), std::greater<>());
  EXPECT_EQ(*result.first, 10000); // with std::greater<>, min is the largest number
  EXPECT_EQ(*result.second, 1); // with std::greater<>, max is the smallest number
}

TEST(ParallelTest, TestParallelMinMaxWithComp)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_minmax(data.begin(), data.end(), std::greater<>());
  EXPECT_EQ(result.first, 10000); // with std::greater<>, min is the largest number
  EXPECT_EQ(result.second, 1); // with std::greater<>, max is the smallest number
}

TEST(ParallelTest, TestParallelMeanWithInts)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  double result = nim::parallel_mean(data.begin(), data.end());
  EXPECT_NEAR(result, 5000.5, 1e-5); // the exact mean is 5000.5
  result = nim::mean(data.begin(), data.end());
  EXPECT_NEAR(result, 5000.5, 1e-5); // the exact mean is 5000.5
}

TEST(ParallelTest, TestParallelMeanWithFloats)
{
  std::vector<float> data(10000);
  std::iota(data.begin(), data.end(), 1);
  double result = nim::parallel_mean(data.begin(), data.end());
  EXPECT_NEAR(result, 5000.5, 1e-5); // the exact mean is 5000.5
  result = nim::mean(data.begin(), data.end());
  EXPECT_NEAR(result, 5000.5, 1e-5); // the exact mean is 5000.5
}

TEST(ParallelTest, TestParallelMeanWithDoubles)
{
  std::vector<double> data(10000);
  std::iota(data.begin(), data.end(), 1);
  double result = nim::parallel_mean(data.begin(), data.end());
  EXPECT_NEAR(result, 5000.5, 1e-5); // the exact mean is 5000.5
  result = nim::mean(data.begin(), data.end());
  EXPECT_NEAR(result, 5000.5, 1e-5); // the exact mean is 5000.5
}

TEST(ParallelTest, TestParallelMeanAndVarianceWithInts)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_mean_and_variance(data.begin(), data.end());
  EXPECT_NEAR(result.first, 5000.5, 1e-5); // the exact mean is 5000.5
  EXPECT_NEAR(result.second, 8333333.25, 1e-5);
  result = nim::mean_and_variance(data.begin(), data.end());
  EXPECT_NEAR(result.first, 5000.5, 1e-5); // the exact mean is 5000.5
  EXPECT_NEAR(result.second, 8333333.25, 1e-5);
}

TEST(ParallelTest, TestParallelMeanAndSampleVarianceWithInts)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_mean_and_sample_variance(data.begin(), data.end());
  EXPECT_NEAR(result.first, 5000.5, 1e-5); // the exact mean is 5000.5
  EXPECT_NEAR(result.second, 8334166.666666667, 1e-5);
  result = nim::mean_and_sample_variance(data.begin(), data.end());
  EXPECT_NEAR(result.first, 5000.5, 1e-5); // the exact mean is 5000.5
  EXPECT_NEAR(result.second, 8334166.666666667, 1e-5);
}

TEST(ParallelTest, TestParallelMeanAndStandardDeviationWithInts)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_mean_and_standard_deviation(data.begin(), data.end());
  EXPECT_NEAR(result.first, 5000.5, 1e-5); // the exact mean is 5000.5
  EXPECT_NEAR(result.second, 2886.751331514372, 1e-5);
  result = nim::mean_and_standard_deviation(data.begin(), data.end());
  EXPECT_NEAR(result.first, 5000.5, 1e-5); // the exact mean is 5000.5
  EXPECT_NEAR(result.second, 2886.751331514372, 1e-5);
}

TEST(ParallelTest, TestParallelMeanAndSampleStandardDeviationWithInts)
{
  std::vector<int> data(10000);
  std::iota(data.begin(), data.end(), 1);
  auto result = nim::parallel_mean_and_sample_standard_deviation(data.begin(), data.end());
  EXPECT_NEAR(result.first, 5000.5, 1e-5); // the exact mean is 5000.5
  EXPECT_NEAR(result.second, 2886.8956799071675, 1e-5);
  result = nim::mean_and_sample_standard_deviation(data.begin(), data.end());
  EXPECT_NEAR(result.first, 5000.5, 1e-5); // the exact mean is 5000.5
  EXPECT_NEAR(result.second, 2886.8956799071675, 1e-5);
}

TEST(MedianTest, TestMedianOddNumberOfElements)
{
  std::vector<int> data{5, 2, 9, 1, 6};
  double result = nim::median(data.begin(), data.end());
  EXPECT_EQ(result, 5);
}

TEST(MedianTest, TestMedianEvenNumberOfElements)
{
  std::vector<int> data{5, 2, 9, 1};
  double result = nim::median(data.begin(), data.end());
  EXPECT_EQ(result, (2 + 5) / 2.0);
}

TEST(MedianTest, TestMedianSingleElement)
{
  std::vector<int> data{7};
  double result = nim::median(data.begin(), data.end());
  EXPECT_EQ(result, 7);
}

TEST(MedianTest, TestMedianLargeNumberOfElements)
{
  std::vector<int> data(1e6); // one million elements
  std::iota(data.begin(), data.end(), 1);
  double result = nim::median(data.begin(), data.end());
  EXPECT_EQ(result, (data[data.size() / 2 - 1] + data[data.size() / 2]) / 2.0); // median of evenly sized dataset
}

TEST(MedianTest, TestMedianNonIntegerValues)
{
  std::vector<double> data{5.5, 2.2, 9.9, 1.1, 6.6};
  double result = nim::median(data.begin(), data.end());
  EXPECT_DOUBLE_EQ(result, 5.5); // median of the sorted data
}

TEST(MedianTest, TestMedianRandomValues)
{
  std::default_random_engine generator;
  std::uniform_real_distribution<double> distribution(0.0, 1.0);

  std::vector<double> data(10000); // ten thousand elements
  std::generate(data.begin(), data.end(), [&]() {
    return distribution(generator);
  });
  std::sort(data.begin(), data.end()); // median requires sorted data
  double result = nim::median(data.begin(), data.end());
  EXPECT_NEAR(result, 0.5, 0.01); // median should be near 0.5 for uniformly distributed data
}
