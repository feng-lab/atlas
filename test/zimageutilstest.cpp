#include "zimage2dutils.h"
#include "zimage3dutils.h"
#include "zimg.h"
#include "zimageresizehwy.h"
#include "zimgregioniterator.h"
#include "ztest.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <type_traits>

namespace {

template<typename T>
nim::ZImg makeSyntheticResizeImage(size_t width, size_t height, size_t depth)
{
  nim::ZImgInfo info(width, height, depth);
  info.setVoxelFormat<T>();
  nim::ZImg img(info);

  T* data = img.timeData<T>(0);
  for (size_t i = 0; i < img.timeVoxelNumber(); ++i) {
    const double value =
      0.5 + 0.5 * std::sin(static_cast<double>(i) * 0.071) * std::cos(static_cast<double>(i % width) * 0.113);
    if constexpr (std::is_integral_v<T>) {
      if constexpr (std::is_signed_v<T>) {
        data[i] = static_cast<T>(std::llround((value - 0.5) * 100.0));
      } else {
        const uint64_t maxValue =
          std::min<uint64_t>(static_cast<uint64_t>(std::numeric_limits<T>::max()), static_cast<uint64_t>(200));
        data[i] = static_cast<T>(std::llround(value * static_cast<double>(maxValue)));
      }
    } else {
      data[i] = static_cast<T>(value);
    }
  }

  return img;
}

template<typename T>
double resizeParityTolerance()
{
  if constexpr (std::is_integral_v<T>) {
    return 1.0;
  } else {
    return 1.0e-9;
  }
}

template<typename T>
void expectImagesNear(const nim::ZImg& expected, const nim::ZImg& actual)
{
  ASSERT_TRUE(expected.info().isSameSize(actual.info()));
  ASSERT_TRUE(expected.info().isSameType(actual.info()));

  const T* expectedData = expected.timeData<T>(0);
  const T* actualData = actual.timeData<T>(0);
  for (size_t i = 0; i < expected.timeVoxelNumber(); ++i) {
    EXPECT_NEAR(static_cast<double>(expectedData[i]), static_cast<double>(actualData[i]), resizeParityTolerance<T>())
      << "voxel " << i;
  }
}

template<typename T>
void expectResizeMatchesReference(const nim::ZImg& img,
                                  size_t outWidth,
                                  size_t outHeight,
                                  size_t outDepth,
                                  nim::Interpolant interpolant,
                                  bool antialiasing,
                                  bool antialiasingForNearest)
{
  nim::ZImgInfo outInfo = img.info();
  outInfo.voxelSizeX *= static_cast<double>(outInfo.width) / outWidth;
  outInfo.voxelSizeY *= static_cast<double>(outInfo.height) / outHeight;
  outInfo.voxelSizeZ *= static_cast<double>(outInfo.depth) / outDepth;
  outInfo.width = outWidth;
  outInfo.height = outHeight;
  outInfo.depth = outDepth;

  nim::ZImg expected(outInfo);
  nim::ZImg actual(outInfo);
  if (outDepth == img.depth()) {
    for (size_t t = 0; t < img.numTimes(); ++t) {
      for (size_t c = 0; c < img.numChannels(); ++c) {
        for (size_t z = 0; z < img.depth(); ++z) {
          nim::image2DResize(img.planeData<T>(z, c, t),
                             img.width(),
                             img.height(),
                             expected.planeData<T>(z, c, t),
                             outWidth,
                             outHeight,
                             interpolant,
                             antialiasing,
                             antialiasingForNearest,
                             true);
          nim::image2DResizeHighway(img.planeData<T>(z, c, t),
                                    img.width(),
                                    img.height(),
                                    actual.planeData<T>(z, c, t),
                                    outWidth,
                                    outHeight,
                                    interpolant,
                                    antialiasing,
                                    antialiasingForNearest,
                                    true);
        }
      }
    }
  } else {
    for (size_t t = 0; t < img.numTimes(); ++t) {
      for (size_t c = 0; c < img.numChannels(); ++c) {
        nim::image3DResize(img.channelData<T>(c, t),
                           img.width(),
                           img.height(),
                           img.depth(),
                           expected.channelData<T>(c, t),
                           outWidth,
                           outHeight,
                           outDepth,
                           interpolant,
                           antialiasing,
                           antialiasingForNearest,
                           true);
        nim::image3DResizeHighway(img.channelData<T>(c, t),
                                  img.width(),
                                  img.height(),
                                  img.depth(),
                                  actual.channelData<T>(c, t),
                                  outWidth,
                                  outHeight,
                                  outDepth,
                                  interpolant,
                                  antialiasing,
                                  antialiasingForNearest,
                                  true);
      }
    }
  }

  expectImagesNear<T>(expected, actual);

  const nim::ZImg integrated =
    img.resized(outWidth, outHeight, outDepth, interpolant, antialiasing, antialiasingForNearest, true);
  expectImagesNear<T>(expected, integrated);
}

template<typename T>
void expectHighwayResizeSupportsType()
{
  const nim::ZImg img2D = makeSyntheticResizeImage<T>(32, 28, 2);
  expectResizeMatchesReference<T>(img2D, 19, 17, img2D.depth(), nim::Interpolant::Linear, false, false);

  const nim::ZImg img3D = makeSyntheticResizeImage<T>(18, 16, 10);
  expectResizeMatchesReference<T>(img3D, 11, 12, 7, nim::Interpolant::Cubic, true, false);
}

} // namespace

TEST(ZImageUtils, GaussianKernel2D)
{
  using namespace nim;

  std::vector<double> gk = create2DGaussianKernel(2., 2., 6, 5);
  double res[] = {0.0159964394504393, 0.0263736699774059, 0.0338644625823236, 0.0338644625823236, 0.0263736699774059,
                  0.0159964394504393, 0.0232746820648490, 0.0383734633890993, 0.0492725023179401, 0.0492725023179401,
                  0.0383734633890993, 0.0232746820648490, 0.0263736699774059, 0.0434828306781744, 0.0558330597803054,
                  0.0558330597803054, 0.0434828306781744, 0.0263736699774059, 0.0232746820648490, 0.0383734633890993,
                  0.0492725023179401, 0.0492725023179401, 0.0383734633890993, 0.0232746820648490, 0.0159964394504393,
                  0.0263736699774059, 0.0338644625823236, 0.0338644625823236, 0.0263736699774059, 0.0159964394504393};
  for (size_t i = 0; i < gk.size(); ++i) {
    ASSERT_NEAR(res[i], gk[i], 1e-14);
  }
}

TEST(ZImageUtils, GaussianFilter2D)
{
  using namespace nim;

  try {
    ZImg img(getTestDataDir().filePath("img/im2d1.tif"));
    ZImg dres(getTestDataDir().filePath("img/im2d1doublefilterres.tif"));

    ZImg cpy = img;
    ZImg dcpy = img.castTo<double>();

    image2DGaussianFilter(dcpy.timeData<double>(0), dcpy.width(), dcpy.height(), 1, 1, dcpy.timeData<double>(0), 5, 5);

    image2DGaussianFilter(cpy.timeData<uint8_t>(0), cpy.width(), cpy.height(), 1, 1, cpy.timeData<uint8_t>(0), 5, 5);

    ZImgRegionConstIterator<double> itdres(dres);
    ZImgRegionConstIterator<double> itdcpy(dcpy);
    ZImgRegionConstIterator<uint8_t> itcpy(cpy);
    for (; !itdres.isAtEnd(); ++itdres, ++itdcpy, ++itcpy) {
      ASSERT_NEAR(*itdres, *itdcpy, 1e-10) << *itcpy << " " << itdres.index();
      ASSERT_EQ(roundTo<uint8_t>(*itdres), *itcpy) << *itdres << " " << *itdcpy << " " << itdres.index();
    }

    cpy = img;
    dcpy = img.castTo<double>();
    std::vector<double> gk = create2DGaussianKernel(1., 1., 5, 5);

    image2DFilter(dcpy.timeData<double>(0), dcpy.width(), dcpy.height(), gk.data(), 5, 5, dcpy.timeData<double>(0));

    image2DFilter(cpy.timeData<uint8_t>(0), cpy.width(), cpy.height(), gk.data(), 5, 5, cpy.timeData<uint8_t>(0));

    ZImgRegionConstIterator<double> it1dres(dres);
    ZImgRegionConstIterator<double> it1dcpy(dcpy);
    ZImgRegionConstIterator<uint8_t> it1cpy(cpy);
    for (; !it1dres.isAtEnd(); ++it1dres, ++it1dcpy, ++it1cpy) {
      ASSERT_NEAR(*it1dres, *it1dcpy, 1e-10) << *it1cpy << " " << it1dres.index();
      ASSERT_EQ(roundTo<uint8_t>(*it1dres), *it1cpy) << *it1dres << " " << *it1dcpy << " " << it1dres.index();
    }
  }
  catch (const ZException& e) {
    LOG(WARNING) << e.what();
  }
}

TEST(ZImageUtils, GaussianFilter3D)
{
  using namespace nim;

  try {
    ZImg img(getTestDataDir().filePath("img/im3d1.tif"));
    ZImg dres(getTestDataDir().filePath("img/im3d1doublefilterres.raw"));

    ZImg cpy = img;
    ZImg dcpy = img.castTo<double>();

    image3DGaussianFilter(dcpy.timeData<double>(0),
                          dcpy.width(),
                          dcpy.height(),
                          dcpy.depth(),
                          1,
                          1,
                          1,
                          dcpy.timeData<double>(0),
                          5,
                          5,
                          5);

    image3DGaussianFilter(cpy.timeData<uint8_t>(0),
                          cpy.width(),
                          cpy.height(),
                          cpy.depth(),
                          1,
                          1,
                          1,
                          cpy.timeData<uint8_t>(0),
                          5,
                          5,
                          5);

    ZImgRegionConstIterator<double> itdres(dres);
    ZImgRegionConstIterator<double> itdcpy(dcpy);
    ZImgRegionConstIterator<uint8_t> itcpy(cpy);
    for (; !itdres.isAtEnd(); ++itdres, ++itdcpy, ++itcpy) {
      ASSERT_NEAR(*itdres, *itdcpy, 1e-10) << *itcpy << " " << itdres.index();
      ASSERT_EQ(roundTo<uint8_t>(*itdres), *itcpy) << *itdres << " " << *itdcpy << " " << itdres.index();
    }

    cpy = img;
    dcpy = img.castTo<double>();
    std::vector<double> gk = create3DGaussianKernel(1., 1., 1., 5, 5, 5);

    image3DFilter(dcpy.timeData<double>(0),
                  dcpy.width(),
                  dcpy.height(),
                  dcpy.depth(),
                  gk.data(),
                  5,
                  5,
                  5,
                  dcpy.timeData<double>(0));

    image3DFilter(cpy.timeData<uint8_t>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  gk.data(),
                  5,
                  5,
                  5,
                  cpy.timeData<uint8_t>(0));

    ZImgRegionConstIterator<double> it1dres(dres);
    ZImgRegionConstIterator<double> it1dcpy(dcpy);
    ZImgRegionConstIterator<uint8_t> it1cpy(cpy);
    for (; !it1dres.isAtEnd(); ++it1dres, ++it1dcpy, ++it1cpy) {
      ASSERT_NEAR(*it1dres, *it1dcpy, 1e-10) << *it1cpy << " " << it1dres.index();
      ASSERT_EQ(roundTo<uint8_t>(*it1dres), *it1cpy) << *it1dres << " " << *it1dcpy << " " << it1dres.index();
    }
  }
  catch (const ZException& e) {
    LOG(WARNING) << e.what();
  }
}

TEST(ZImageUtils, ImageFlip)
{
  using namespace nim;

  std::vector<int> testPad(36);
  for (int i = 0; i < 36; ++i) {
    testPad[i] = i + 1;
  }

  image2DFlip(testPad.data(), 6, 6, Dimension::X);
  int res1[] = {6,  5,  4,  3,  2,  1,  12, 11, 10, 9,  8,  7,  18, 17, 16, 15, 14, 13,
                24, 23, 22, 21, 20, 19, 30, 29, 28, 27, 26, 25, 36, 35, 34, 33, 32, 31};
  for (size_t i = 0; i < testPad.size(); ++i) {
    ASSERT_EQ(res1[i], testPad[i]);
  }

  image2DFlip(testPad.data(), 6, 6, Dimension::Y);
  int res2[] = {36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
                18, 17, 16, 15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1};
  for (size_t i = 0; i < testPad.size(); ++i) {
    ASSERT_EQ(res2[i], testPad[i]);
  }

  image2DFlip(testPad.data(), 6, 6, Dimension::X);
  int res3[] = {31, 32, 33, 34, 35, 36, 25, 26, 27, 28, 29, 30, 19, 20, 21, 22, 23, 24,
                13, 14, 15, 16, 17, 18, 7,  8,  9,  10, 11, 12, 1,  2,  3,  4,  5,  6};
  for (size_t i = 0; i < testPad.size(); ++i) {
    ASSERT_EQ(res3[i], testPad[i]);
  }

  image2DFlip(testPad.data(), 6, 6, Dimension::Y);
  for (size_t i = 0; i < testPad.size(); ++i) {
    ASSERT_EQ(int(i + 1), testPad[i]);
  }
}

TEST(ZImageUtils, ImageTranspose)
{
  using namespace nim;

  std::vector<int> mat(36);
  for (int i = 0; i < 36; ++i) {
    mat[i] = i + 1;
  }

  image2DTranspose(mat.data(), 6, 6);
  int res1[] = {1, 7,  13, 19, 25, 31, 2, 8,  14, 20, 26, 32, 3, 9,  15, 21, 27, 33,
                4, 10, 16, 22, 28, 34, 5, 11, 17, 23, 29, 35, 6, 12, 18, 24, 30, 36};
  for (size_t i = 0; i < mat.size(); ++i) {
    ASSERT_EQ(res1[i], mat[i]);
  }

  image2DTranspose(mat.data(), 6, 6);
  for (size_t i = 0; i < mat.size(); ++i) {
    ASSERT_EQ(int(i + 1), mat[i]);
  }

  size_t w = 4023;
  size_t h = 3058;
  mat.resize(w * h);
  std::default_random_engine engine; // or other engine as std::mt19937
  std::uniform_int_distribution<int> distr;

  std::ranges::generate(mat, [&]() {
    return distr(engine);
  });
  auto oriMat = mat;
  image2DTranspose(mat.data(), w, h);
  for (size_t i = 0; i < w; ++i) {
    for (size_t j = 0; j < h; ++j) {
      ASSERT_EQ(oriMat[j * w + i], mat[i * h + j]);
    }
  }

  std::ranges::generate(mat, [&]() {
    return distr(engine);
  });
  oriMat = mat;
  image2DTranspose(mat.data(), w, h);
  for (size_t i = 0; i < w; ++i) {
    for (size_t j = 0; j < h; ++j) {
      ASSERT_EQ(oriMat[j * w + i], mat[i * h + j]);
    }
  }

  w = 2238;
  h = 2238;
  mat.resize(w * h);

  std::ranges::generate(mat, [&]() {
    return distr(engine);
  });
  oriMat = mat;
  image2DTranspose(mat.data(), w, h);
  for (size_t i = 0; i < w; ++i) {
    for (size_t j = 0; j < h; ++j) {
      ASSERT_EQ(oriMat[j * w + i], mat[i * h + j]);
    }
  }

  std::ranges::generate(mat, [&]() {
    return distr(engine);
  });
  oriMat = mat;
  image2DTranspose(mat.data(), w, h);
  for (size_t i = 0; i < w; ++i) {
    for (size_t j = 0; j < h; ++j) {
      ASSERT_EQ(oriMat[j * w + i], mat[i * h + j]);
    }
  }
}

TEST(ZImageUtils, ImagePad)
{
  using namespace nim;

  std::vector<int> testPad(36);
  for (int i = 0; i < 36; ++i) {
    testPad[i] = i + 1;
  }

  int width = 7 + 6 + 3;
  int height = 4 + 6 + 2;
  std::vector<int> padded((7 + 6 + 3) * (4 + 6 + 2));
  image2DPad(testPad.data(), 6, 6, 7, 3, 4, 2, padded.data(), PadOption::Constant, 0);
  for (int i = 0; i < width; ++i) {
    for (int j = 0; j < height; ++j) {
      if (i >= 7 && j >= 4 && i < 7 + 6 && j < 4 + 6) {
        ASSERT_EQ(testPad[i - 7 + (j - 4) * 6], padded[i + j * width]);
      } else {
        ASSERT_EQ(0, padded[i + j * width]);
      }
    }
  }

  image2DPad(testPad.data(), 6, 6, 7, 3, 4, 2, padded.data(), PadOption::Constant, 5);
  for (int i = 0; i < width; ++i) {
    for (int j = 0; j < height; ++j) {
      if (i >= 7 && j >= 4 && i < 7 + 6 && j < 4 + 6) {
        ASSERT_EQ(testPad[i - 7 + (j - 4) * 6], padded[i + j * width]);
      } else {
        ASSERT_EQ(5, padded[i + j * width]);
      }
    }
  }

  image2DPad(testPad.data(), 6, 6, 7, 3, 4, 2, padded.data(), PadOption::Symmetric);
  int res1[] = {24, 24, 23, 22, 21, 20, 19, 19, 20, 21, 22, 23, 24, 24, 23, 22, 18, 18, 17, 16, 15, 14, 13, 13,
                14, 15, 16, 17, 18, 18, 17, 16, 12, 12, 11, 10, 9,  8,  7,  7,  8,  9,  10, 11, 12, 12, 11, 10,
                6,  6,  5,  4,  3,  2,  1,  1,  2,  3,  4,  5,  6,  6,  5,  4,  6,  6,  5,  4,  3,  2,  1,  1,
                2,  3,  4,  5,  6,  6,  5,  4,  12, 12, 11, 10, 9,  8,  7,  7,  8,  9,  10, 11, 12, 12, 11, 10,
                18, 18, 17, 16, 15, 14, 13, 13, 14, 15, 16, 17, 18, 18, 17, 16, 24, 24, 23, 22, 21, 20, 19, 19,
                20, 21, 22, 23, 24, 24, 23, 22, 30, 30, 29, 28, 27, 26, 25, 25, 26, 27, 28, 29, 30, 30, 29, 28,
                36, 36, 35, 34, 33, 32, 31, 31, 32, 33, 34, 35, 36, 36, 35, 34, 36, 36, 35, 34, 33, 32, 31, 31,
                32, 33, 34, 35, 36, 36, 35, 34, 30, 30, 29, 28, 27, 26, 25, 25, 26, 27, 28, 29, 30, 30, 29, 28};
  for (size_t i = 0; i < padded.size(); ++i) {
    ASSERT_EQ(res1[i], padded[i]);
  }

  image2DPad(testPad.data(), 6, 6, 7, 3, 4, 2, padded.data(), PadOption::Replicate);
  int res2[] = {1,  1,  1,  1,  1,  1,  1,  1,  2,  3,  4,  5,  6,  6,  6,  6,  1,  1,  1,  1,  1,  1,  1,  1,
                2,  3,  4,  5,  6,  6,  6,  6,  1,  1,  1,  1,  1,  1,  1,  1,  2,  3,  4,  5,  6,  6,  6,  6,
                1,  1,  1,  1,  1,  1,  1,  1,  2,  3,  4,  5,  6,  6,  6,  6,  1,  1,  1,  1,  1,  1,  1,  1,
                2,  3,  4,  5,  6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  8,  9,  10, 11, 12, 12, 12, 12,
                13, 13, 13, 13, 13, 13, 13, 13, 14, 15, 16, 17, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19,
                20, 21, 22, 23, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 25, 26, 27, 28, 29, 30, 30, 30, 30,
                31, 31, 31, 31, 31, 31, 31, 31, 32, 33, 34, 35, 36, 36, 36, 36, 31, 31, 31, 31, 31, 31, 31, 31,
                32, 33, 34, 35, 36, 36, 36, 36, 31, 31, 31, 31, 31, 31, 31, 31, 32, 33, 34, 35, 36, 36, 36, 36};
  for (size_t i = 0; i < padded.size(); ++i) {
    ASSERT_EQ(res2[i], padded[i]);
  }

  image2DPad(testPad.data(), 6, 6, 7, 3, 4, 2, padded.data(), PadOption::Circular);
  int res3[] = {18, 13, 14, 15, 16, 17, 18, 13, 14, 15, 16, 17, 18, 13, 14, 15, 24, 19, 20, 21, 22, 23, 24, 19,
                20, 21, 22, 23, 24, 19, 20, 21, 30, 25, 26, 27, 28, 29, 30, 25, 26, 27, 28, 29, 30, 25, 26, 27,
                36, 31, 32, 33, 34, 35, 36, 31, 32, 33, 34, 35, 36, 31, 32, 33, 6,  1,  2,  3,  4,  5,  6,  1,
                2,  3,  4,  5,  6,  1,  2,  3,  12, 7,  8,  9,  10, 11, 12, 7,  8,  9,  10, 11, 12, 7,  8,  9,
                18, 13, 14, 15, 16, 17, 18, 13, 14, 15, 16, 17, 18, 13, 14, 15, 24, 19, 20, 21, 22, 23, 24, 19,
                20, 21, 22, 23, 24, 19, 20, 21, 30, 25, 26, 27, 28, 29, 30, 25, 26, 27, 28, 29, 30, 25, 26, 27,
                36, 31, 32, 33, 34, 35, 36, 31, 32, 33, 34, 35, 36, 31, 32, 33, 6,  1,  2,  3,  4,  5,  6,  1,
                2,  3,  4,  5,  6,  1,  2,  3,  12, 7,  8,  9,  10, 11, 12, 7,  8,  9,  10, 11, 12, 7,  8,  9};
  for (size_t i = 0; i < padded.size(); ++i) {
    ASSERT_EQ(res3[i], padded[i]);
  }

  testPad.resize(5);
  padded.resize(30);
  for (int x = -15; x < 15; ++x) {
    padded[x + 15] = getImage2DPixelValue(testPad.data(), 5, 1, x, 0, PadOption::Constant, 3);
  }
  int res4[] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 2, 3, 4, 5, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
  for (size_t i = 0; i < padded.size(); ++i) {
    ASSERT_EQ(res4[i], padded[i]);
  }

  for (int x = -15; x < 15; ++x) {
    padded[x + 15] = getImage2DPixelValue(testPad.data(), 5, 1, x, 0, PadOption::Symmetric, 3);
  }
  int res5[] = {5, 4, 3, 2, 1, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 1, 2, 3, 4, 5};
  for (size_t i = 0; i < padded.size(); ++i) {
    ASSERT_EQ(res5[i], padded[i]);
  }

  for (int x = -15; x < 15; ++x) {
    padded[x + 15] = getImage2DPixelValue(testPad.data(), 5, 1, x, 0, PadOption::Replicate, 3);
  }
  int res6[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
  for (size_t i = 0; i < padded.size(); ++i) {
    ASSERT_EQ(res6[i], padded[i]);
  }

  for (int x = -15; x < 15; ++x) {
    padded[x + 15] = getImage2DPixelValue(testPad.data(), 5, 1, x, 0, PadOption::Circular, 3);
  }
  int res7[] = {1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 1, 2, 3, 4, 5};
  for (size_t i = 0; i < padded.size(); ++i) {
    ASSERT_EQ(res7[i], padded[i]);
  }
}

TEST(ZImageUtils, Resize2D)
{
  using namespace nim;

  std::vector<double> img(16);
  for (size_t i = 0; i < img.size(); ++i) {
    img[i] = i + 1;
  }

  std::vector<double> imgOut(4);
  double eps = std::numeric_limits<double>::epsilon();

  image2DResize(img.data(), 4, 4, imgOut.data(), 2, 2, Interpolant::Linear, false);
  double res1[] = {3.5, 5.5, 11.5, 13.5};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res1[i], imgOut[i], eps);
  }

  image2DResize(img.data(), 4, 4, imgOut.data(), 2, 2, Interpolant::Linear, true);
  double res1aa[] = {4.125, 5.875, 11.125, 12.875};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res1aa[i], imgOut[i], eps);
  }

  image2DResize(img.data(), 4, 4, imgOut.data(), 2, 2, Interpolant::Nearest);
  double res2[] = {6, 8, 14, 16};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res2[i], imgOut[i], eps);
  }

  imgOut.resize(56);
  image2DResize(img.data(), 4, 4, imgOut.data(), 7, 8, Interpolant::Linear);
  double res3[] = {1,  1.35714285714286, 1.92857142857143, 2.50000000000000, 3.07142857142857, 3.64285714285714, 4,
                   2,  2.35714285714286, 2.92857142857143, 3.50000000000000, 4.07142857142857, 4.64285714285714, 5,
                   4,  4.35714285714286, 4.92857142857143, 5.50000000000000, 6.07142857142857, 6.64285714285714, 7,
                   6,  6.35714285714286, 6.92857142857143, 7.50000000000000, 8.07142857142857, 8.64285714285714, 9,
                   8,  8.35714285714286, 8.92857142857143, 9.50000000000000, 10.0714285714286, 10.6428571428571, 11,
                   10, 10.3571428571429, 10.9285714285714, 11.5000000000000, 12.0714285714286, 12.6428571428571, 13,
                   12, 12.3571428571429, 12.9285714285714, 13.5000000000000, 14.0714285714286, 14.6428571428571, 15,
                   13, 13.3571428571429, 13.9285714285714, 14.5000000000000, 15.0714285714286, 15.6428571428571, 16};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res3[i], imgOut[i], 1e-13);
  }

  imgOut.resize(16);
  image2DResize(img.data(), 4, 4, imgOut.data(), 8, 2, Interpolant::Linear, false);
  double res4[] = {3, 3.25, 3.75, 4.25, 4.75, 5.25, 5.75, 6, 11, 11.25, 11.75, 12.25, 12.75, 13.25, 13.75, 14};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res4[i], imgOut[i], 1e-13);
  }

  image2DResize(img.data(), 4, 4, imgOut.data(), 8, 2, Interpolant::Linear, true);
  double res4aa[] =
    {3.5, 3.75, 4.25, 4.75, 5.25, 5.75, 6.25, 6.5, 10.5, 10.75, 11.25, 11.75, 12.25, 12.75, 13.25, 13.5};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res4aa[i], imgOut[i], 1e-13);
  }

  imgOut.resize(4);
  image2DResize(img.data(), 4, 4, imgOut.data(), 2, 2, Interpolant::Cubic, false);
  double res5[] = {3.18750000000000, 5.31250000000000, 11.6875000000000, 13.8125000000000};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res5[i], imgOut[i], 1e-13);
  }

  image2DResize(img.data(), 4, 4, imgOut.data(), 2, 2, Interpolant::Cubic, true);
  double res5aa[] = {3.59765625, 5.55859375, 11.44140625, 13.40234375};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res5aa[i], imgOut[i], 1e-13);
  }

  imgOut.resize(42);
  image2DResize(img.data(), 4, 4, imgOut.data(), 7, 6, Interpolant::Cubic);
  double res6[] = {
    0.702374203649714, 1.05186399956808, 1.69472114242522, 2.26851851851852, 2.84231589461181, 3.48517303746896,
    3.83466283338732,  2.68385568513120, 3.03334548104956, 3.67620262390670, 4.25000000000000, 4.82379737609330,
    5.46665451895044,  5.81614431486880, 5.60052235179786, 5.95001214771622, 6.59286929057337, 7.16666666666666,
    7.74046404275996,  8.38332118561710, 8.73281098153547, 8.26718901846453, 8.61667881438289, 9.25953595724004,
    9.83333333333333,  10.4071307094266, 11.0499878522838, 11.3994776482021, 11.1838556851312, 11.5333454810496,
    12.1762026239067,  12.7500000000000, 13.3237973760933, 13.9666545189504, 14.3161443148688, 13.1653371666127,
    13.5148269625310,  14.1576841053882, 14.7314814814815, 15.3052788575748, 15.9481360004319, 16.2976257963503};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res6[i], imgOut[i], 1e-13);
  }

  imgOut.resize(10);
  image2DResize(img.data(), 4, 4, imgOut.data(), 2, 5, Interpolant::Cubic, false);
  double res7[] = {1.27550000000000,
                   3.40050000000000,
                   4.11150000000000,
                   6.23650000000000,
                   7.43750000000000,
                   9.56250000000000,
                   10.7635000000000,
                   12.8885000000000,
                   13.5995000000000,
                   15.7245000000000};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res7[i], imgOut[i], 1e-13);
  }

  image2DResize(img.data(), 4, 4, imgOut.data(), 2, 5, Interpolant::Cubic, true);
  double res7aa[] = {1.35753125,
                     3.31846875,
                     4.19353125,
                     6.15446875,
                     7.51953125,
                     9.48046875,
                     10.84553125,
                     12.80646875,
                     13.68153125,
                     15.64246875};
  for (size_t i = 0; i < imgOut.size(); ++i) {
    ASSERT_NEAR(res7aa[i], imgOut[i], 1e-13);
  }
}

TEST(ZImageUtils, Resize3D)
{
  using namespace nim;

  try {
    ZImg img(getTestDataDir().filePath("img/im3d1.tif"));
    img = img.castTo<double>();

    ZImg res(getTestDataDir().filePath("img/im3d1_resize_cubic_res.tif"));
    ZImg cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Cubic,
                  false);

    ZImgRegionConstIterator<double> itres(res);
    ZImgRegionConstIterator<double> itcpy(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }

    res = ZImg(getTestDataDir().filePath("img/im3d1_resize_cubic_aa_res.tif"));
    cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Cubic,
                  true);

    itres = ZImgRegionConstIterator<double>(res);
    itcpy = ZImgRegionConstIterator<double>(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }

    res = ZImg(getTestDataDir().filePath("img/im3d1_resize_lanczos2_res.tif"));
    cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Lanczos2,
                  false);

    itres = ZImgRegionConstIterator<double>(res);
    itcpy = ZImgRegionConstIterator<double>(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }

    res = ZImg(getTestDataDir().filePath("img/im3d1_resize_lanczos2_aa_res.tif"));
    cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Lanczos2,
                  true);

    itres = ZImgRegionConstIterator<double>(res);
    itcpy = ZImgRegionConstIterator<double>(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }

    res = ZImg(getTestDataDir().filePath("img/im3d1_resize_lanczos3_res.tif"));
    cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Lanczos3,
                  false);

    itres = ZImgRegionConstIterator<double>(res);
    itcpy = ZImgRegionConstIterator<double>(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }

    res = ZImg(getTestDataDir().filePath("img/im3d1_resize_lanczos3_aa_res.tif"));
    cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Lanczos3,
                  true);

    itres = ZImgRegionConstIterator<double>(res);
    itcpy = ZImgRegionConstIterator<double>(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }

    res = ZImg(getTestDataDir().filePath("img/im3d1_resize_nearest_res.tif"));
    cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Nearest,
                  false,
                  false);

    itres = ZImgRegionConstIterator<double>(res);
    itcpy = ZImgRegionConstIterator<double>(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }

    res = ZImg(getTestDataDir().filePath("img/im3d1_resize_linear_res.tif"));
    cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Linear,
                  false);

    itres = ZImgRegionConstIterator<double>(res);
    itcpy = ZImgRegionConstIterator<double>(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }

    res = ZImg(getTestDataDir().filePath("img/im3d1_resize_linear_aa_res.tif"));
    cpy = res;
    image3DResize(img.timeData<double>(0),
                  img.width(),
                  img.height(),
                  img.depth(),
                  cpy.timeData<double>(0),
                  cpy.width(),
                  cpy.height(),
                  cpy.depth(),
                  Interpolant::Linear,
                  true);

    itres = ZImgRegionConstIterator<double>(res);
    itcpy = ZImgRegionConstIterator<double>(cpy);
    for (; !itres.isAtEnd(); ++itres, ++itcpy) {
      ASSERT_NEAR(*itres, *itcpy, 1e-10) << *itcpy << " " << itres.index();
    }
  }
  catch (const ZException& e) {
    LOG(WARNING) << e.what();
  }
}

TEST(ZImageUtils, HighwayResize2DMatchesReferenceForMultipleInterpolants)
{
  using namespace nim;

  const ZImg img = makeSyntheticResizeImage<uint16_t>(64, 48, 3);

  expectResizeMatchesReference<uint16_t>(img, 32, 24, img.depth(), Interpolant::Cubic, true, false);
  expectResizeMatchesReference<uint16_t>(img, 37, 29, img.depth(), Interpolant::Linear, false, false);
  expectResizeMatchesReference<uint16_t>(img, 23, 19, img.depth(), Interpolant::Nearest, false, true);
}

TEST(ZImageUtils, HighwayResize3DMatchesReferenceForMultipleInterpolants)
{
  using namespace nim;

  const ZImg img = makeSyntheticResizeImage<double>(32, 40, 24);

  expectResizeMatchesReference<double>(img, 20, 20, 20, Interpolant::Cubic, true, false);
  expectResizeMatchesReference<double>(img, 27, 31, 17, Interpolant::Linear, false, false);
  expectResizeMatchesReference<double>(img, 24, 30, 18, Interpolant::Lanczos2, true, false);
}

TEST(ZImageUtils, HighwayResize3DIntegerOutputMatchesReference)
{
  using namespace nim;

  const ZImg img8 = makeSyntheticResizeImage<uint8_t>(32, 40, 24);
  expectResizeMatchesReference<uint8_t>(img8, 20, 20, 20, Interpolant::Cubic, true, false);

  const ZImg img16 = makeSyntheticResizeImage<uint16_t>(32, 40, 24);
  expectResizeMatchesReference<uint16_t>(img16, 27, 31, 17, Interpolant::Linear, false, false);
}

TEST(ZImageUtils, HighwayResizeSupportsAllZImgVoxelTypes)
{
  expectHighwayResizeSupportsType<uint8_t>();
  expectHighwayResizeSupportsType<uint16_t>();
  expectHighwayResizeSupportsType<uint32_t>();
  expectHighwayResizeSupportsType<uint64_t>();
  expectHighwayResizeSupportsType<int8_t>();
  expectHighwayResizeSupportsType<int16_t>();
  expectHighwayResizeSupportsType<int32_t>();
  expectHighwayResizeSupportsType<int64_t>();
  expectHighwayResizeSupportsType<float>();
  expectHighwayResizeSupportsType<double>();
}
