#pragma once

#include "zimagetoimagemetric.h"
#include "gtest/gtest.h"

TEST(ZImageToImageMetric, test)
{
  using namespace nim;

  ZImageToImageMetric metric;

  std::vector<uint8_t> img1;
  std::vector<uint8_t> img2;
  img1.push_back(1); img1.push_back(2); img1.push_back(3); img1.push_back(4);
  img2.push_back(1); img2.push_back(2); img2.push_back(3); img2.push_back(5);
  metric.setType(ZImageToImageMetric::Type::MeanDifferences);
  EXPECT_EQ(-0.25, metric.value(img1.data(), img2.data(), 2, 2));
  metric.setType(ZImageToImageMetric::Type::MeanSquaredDifferences);
  EXPECT_EQ(0.25, metric.value(img1.data(), img2.data(), 2, 2));
  metric.setType(ZImageToImageMetric::Type::LogAbsoluteDifferences);
  EXPECT_NEAR(0.17328679513998632, metric.value(img1.data(), img2.data(), 2, 2), 1e-13);
  metric.setType(ZImageToImageMetric::Type::NormalizedCrossCorrelation);
  EXPECT_NEAR(-0.98270762982399074, metric.value(img1.data(), img2.data(), 2, 2), 1e-13);
  //metric.setType(ZImageToImageMetric::NormalizedMutualInformation);
  //EXPECT_NEAR(-2.8130318897682067, metric.value(img1.data(), img2.data(), 2, 2), 1e-13);
}

