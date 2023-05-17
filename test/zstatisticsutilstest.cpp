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
  EXPECT_THROW(nim::parallel_minmax(data.begin(), data.end()), nim::ZException);
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
