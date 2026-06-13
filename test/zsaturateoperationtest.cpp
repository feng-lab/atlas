#include "zsaturateoperation.h"
#include "zioutils.h"
#include "ztest.h"
#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

using namespace nim;

template<typename T>
struct SaturateTestData
{
  std::vector<T> lhs;
  std::vector<T> rhs;
  std::vector<T> expected;
};

template<typename T>
SaturateTestData<T> readSaturateTestData(const QString& filename)
{
  std::ifstream inputFileStream = openIFStream(filename, std::ios::in | std::ios::binary);

  uint64_t numPoints;
  readStream(inputFileStream, &numPoints, 8);
  CHECK(numPoints % 3 == 0);

  std::vector<T> raw(numPoints);
  readStream(inputFileStream, raw.data(), raw.size() * sizeof(T));

  const size_t count = raw.size() / 3;
  SaturateTestData<T> res;
  res.lhs.reserve(count);
  res.rhs.reserve(count);
  res.expected.reserve(count);
  for (size_t i = 0; i < raw.size(); i += 3) {
    res.lhs.push_back(raw[i]);
    res.rhs.push_back(raw[i + 1]);
    res.expected.push_back(raw[i + 2]);
  }

  return res;
}

template<typename T, typename Op>
void verifySaturateScalarData(const SaturateTestData<T>& data, Op op, const char* opName)
{
  CHECK(data.lhs.size() == data.rhs.size());
  CHECK(data.lhs.size() == data.expected.size());

  for (size_t i = 0; i < data.lhs.size(); ++i) {
    ASSERT_EQ(data.expected[i], op(data.lhs[i], data.rhs[i]))
      << opName << " lhs=" << +data.lhs[i] << " rhs=" << +data.rhs[i] << " i=" << i;
  }
}

template<typename T, typename Op>
void verifySaturateScalarDataNear(const SaturateTestData<T>& data, Op op, const char* opName)
{
  CHECK(data.lhs.size() == data.rhs.size());
  CHECK(data.lhs.size() == data.expected.size());

  for (size_t i = 0; i < data.lhs.size(); ++i) {
    ASSERT_NEAR(data.expected[i], op(data.lhs[i], data.rhs[i]), 1)
      << opName << " lhs=" << +data.lhs[i] << " rhs=" << +data.rhs[i] << " i=" << i;
  }
}

template<typename T, typename Op>
void verifySaturateArrayData(const SaturateTestData<T>& data, Op op, const char* opName)
{
  CHECK(data.lhs.size() == data.rhs.size());
  CHECK(data.lhs.size() == data.expected.size());

  std::vector<T> res(data.lhs.size());
  op(data.lhs.data(), data.rhs.data(), data.lhs.size(), res.data());
  for (size_t i = 0; i < data.lhs.size(); ++i) {
    ASSERT_EQ(data.expected[i], res[i]) << opName << " lhs=" << +data.lhs[i] << " rhs=" << +data.rhs[i] << " i=" << i;
  }
}

template<typename T>
void verifySaturateAddFileData(const QString& filename)
{
  const auto data = readSaturateTestData<T>(filename);
  verifySaturateScalarData<T>(
    data,
    [](T lhs, T rhs) {
      return saturate_add(lhs, rhs);
    },
    "scalar add");
  verifySaturateArrayData<T>(
    data,
    [](const T* lhs, const T* rhs, size_t count, T* res) {
      saturate_add<T, const T>(lhs, rhs, count, res);
    },
    "array add");
}

template<typename T>
void verifySaturateSubFileData(const QString& filename)
{
  const auto data = readSaturateTestData<T>(filename);
  verifySaturateScalarData<T>(
    data,
    [](T lhs, T rhs) {
      return saturate_sub(lhs, rhs);
    },
    "scalar sub");
  verifySaturateArrayData<T>(
    data,
    [](const T* lhs, const T* rhs, size_t count, T* res) {
      saturate_sub<T, const T>(lhs, rhs, count, res);
    },
    "array sub");
}

template<typename T>
void verifySaturateMulFileData(const QString& filename)
{
  const auto data = readSaturateTestData<T>(filename);
  verifySaturateScalarData<T>(
    data,
    [](T lhs, T rhs) {
      return saturate_mul(lhs, rhs);
    },
    "scalar mul");
}

template<typename T>
void verifySaturateDivFileData(const QString& filename)
{
  const auto data = readSaturateTestData<T>(filename);
  verifySaturateScalarDataNear<T>(
    data,
    [](T lhs, T rhs) {
      return saturate_div(lhs, rhs);
    },
    "scalar div");
}

template<typename T>
T saturateArrayTestValue(size_t i)
{
  if constexpr (std::is_signed_v<T>) {
    constexpr std::array<T, 11> values = {std::numeric_limits<T>::lowest(),
                                          static_cast<T>(std::numeric_limits<T>::lowest() + 1),
                                          static_cast<T>(-97),
                                          static_cast<T>(-3),
                                          static_cast<T>(-1),
                                          static_cast<T>(0),
                                          static_cast<T>(1),
                                          static_cast<T>(3),
                                          static_cast<T>(97),
                                          static_cast<T>(std::numeric_limits<T>::max() - 1),
                                          std::numeric_limits<T>::max()};
    return values[i % values.size()];
  } else {
    constexpr std::array<T, 11> values = {static_cast<T>(0),
                                          static_cast<T>(1),
                                          static_cast<T>(2),
                                          static_cast<T>(7),
                                          static_cast<T>(31),
                                          static_cast<T>(std::numeric_limits<T>::max() / 2),
                                          static_cast<T>(std::numeric_limits<T>::max() - 31),
                                          static_cast<T>(std::numeric_limits<T>::max() - 7),
                                          static_cast<T>(std::numeric_limits<T>::max() - 2),
                                          static_cast<T>(std::numeric_limits<T>::max() - 1),
                                          std::numeric_limits<T>::max()};
    return values[i % values.size()];
  }
}

template<typename T>
void verifySaturateArraySpecializations()
{
  constexpr size_t kPadding = 96;
  constexpr size_t kMaxCount = 137;
  constexpr std::array<size_t, 19> counts =
    {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 95, 128, kMaxCount};

  std::vector<T> x(kPadding + kMaxCount + kPadding);
  std::vector<T> y(kPadding + kMaxCount + kPadding);
  std::vector<T> res(kPadding + kMaxCount + kPadding);

  for (size_t i = 0; i < x.size(); ++i) {
    x[i] = saturateArrayTestValue<T>(i);
    y[i] = saturateArrayTestValue<T>(i * 5 + 3);
  }

  for (size_t offset = 0; offset < 32; ++offset) {
    const size_t xOffset = offset;
    const size_t yOffset = (offset * 3 + 5) % 32;
    const size_t resOffset = (offset * 7 + 11) % 32;
    const T addScalar = saturateArrayTestValue<T>(offset * 11 + 7);
    const T subScalar = saturateArrayTestValue<T>(offset * 13 + 9);

    for (const size_t count : counts) {
      std::fill(res.begin(), res.end(), saturateArrayTestValue<T>(17));
      saturate_add<T, const T>(x.data() + xOffset, y.data() + yOffset, count, res.data() + resOffset);
      for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(saturate_add(x[xOffset + i], y[yOffset + i]), res[resOffset + i])
          << "array add offset=" << offset << " count=" << count << " i=" << i;
      }

      std::fill(res.begin(), res.end(), saturateArrayTestValue<T>(19));
      saturate_add<T, T>(x.data() + xOffset, addScalar, count, res.data() + resOffset);
      for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(saturate_add(x[xOffset + i], addScalar), res[resOffset + i])
          << "scalar add offset=" << offset << " count=" << count << " i=" << i;
      }

      std::fill(res.begin(), res.end(), saturateArrayTestValue<T>(23));
      saturate_sub<T, const T>(x.data() + xOffset, y.data() + yOffset, count, res.data() + resOffset);
      for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(saturate_sub(x[xOffset + i], y[yOffset + i]), res[resOffset + i])
          << "array sub offset=" << offset << " count=" << count << " i=" << i;
      }

      std::fill(res.begin(), res.end(), saturateArrayTestValue<T>(29));
      saturate_sub<T, T>(x.data() + xOffset, subScalar, count, res.data() + resOffset);
      for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(saturate_sub(x[xOffset + i], subScalar), res[resOffset + i])
          << "scalar sub offset=" << offset << " count=" << count << " i=" << i;
      }
    }
  }
}

} // namespace

TEST(saturate, arrayAddSub)
{
  verifySaturateArraySpecializations<uint8_t>();
  verifySaturateArraySpecializations<int8_t>();
  verifySaturateArraySpecializations<uint16_t>();
  verifySaturateArraySpecializations<int16_t>();
  verifySaturateArraySpecializations<uint32_t>();
  verifySaturateArraySpecializations<int32_t>();
  verifySaturateArraySpecializations<uint64_t>();
  verifySaturateArraySpecializations<int64_t>();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"
#endif

TEST(saturate, add)
{
  using namespace nim;

  verifySaturateAddFileData<uint8_t>(getTestDataDir().filePath("uint8_add_test.dat"));
  verifySaturateAddFileData<uint16_t>(getTestDataDir().filePath("uint16_add_test.dat"));
  verifySaturateAddFileData<uint32_t>(getTestDataDir().filePath("uint32_add_test.dat"));
  verifySaturateAddFileData<uint64_t>(getTestDataDir().filePath("uint64_add_test.dat"));
  verifySaturateAddFileData<int8_t>(getTestDataDir().filePath("int8_add_test.dat"));
  verifySaturateAddFileData<int16_t>(getTestDataDir().filePath("int16_add_test.dat"));
  verifySaturateAddFileData<int32_t>(getTestDataDir().filePath("int32_add_test.dat"));
  verifySaturateAddFileData<int64_t>(getTestDataDir().filePath("int64_add_test.dat"));

  ASSERT_EQ(1, saturate_add(0, 1));
  ASSERT_EQ(2, saturate_add(1, 1));
  ASSERT_EQ(4, saturate_add(2, 2));
  ASSERT_EQ(-1, saturate_add(0, -1));
  ASSERT_EQ(0, saturate_add(1, -1));
  ASSERT_EQ(1, saturate_add(3, -2));

  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX), 0));
  ASSERT_EQ(INT8_MAX - 1, saturate_add(int8_t(INT8_MAX - 5), 4));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX - 5), 5));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX - 5), 6));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX - 5), 7));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX), 0.1));
  ASSERT_EQ(INT8_MAX - 1, saturate_add(int8_t(INT8_MAX - 5), 4.1));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX - 5), 5.1));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX - 5), 6.1));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX - 5), 7.1));
  ASSERT_EQ(INT8_MAX - 1, saturate_add(int8_t(INT8_MAX / 2), INT8_MAX / 2));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX - 10), INT8_MAX - 10));
  ASSERT_EQ(INT8_MIN + 1, saturate_add(int8_t(INT8_MIN), 1));
  ASSERT_EQ(INT8_MIN + 2, saturate_add(int8_t(INT8_MIN), 2));
  ASSERT_EQ(INT8_MIN, saturate_add(int8_t(INT8_MIN), -1));
  ASSERT_EQ(INT8_MIN, saturate_add(int8_t(INT8_MIN), INT8_MIN));
  ASSERT_EQ(-1, saturate_add(int8_t(INT8_MIN), INT8_MAX));

  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX), 0));
  ASSERT_EQ(UINT8_MAX - 1, saturate_add(uint8_t(UINT8_MAX - 5), 4));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX - 5), 5));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX - 5), 6));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX - 5), 7));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX), 0.1));
  ASSERT_EQ(UINT8_MAX - 1, saturate_add(uint8_t(UINT8_MAX - 5), 4.1));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX - 5), 5.1));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX - 5), 6.1));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX - 5), 7.1));
  ASSERT_EQ(UINT8_MAX - 1, saturate_add(uint8_t(UINT8_MAX / 2), UINT8_MAX / 2));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX - 10), UINT8_MAX - 10));
  ASSERT_EQ(0, saturate_add(uint8_t(0), -10));
  ASSERT_EQ(0, saturate_add(uint8_t(0), -1));
  ASSERT_EQ(0, saturate_add(uint8_t(0), 0));
  ASSERT_EQ(1, saturate_add(uint8_t(0), 1));
  ASSERT_EQ(2, saturate_add(uint8_t(0), 2));

  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX), 0));
  ASSERT_EQ(INT16_MAX - 1, saturate_add(int16_t(INT16_MAX - 5), 4));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX - 5), 5));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX - 5), 6));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX - 5), 7));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX), 0.1));
  ASSERT_EQ(INT16_MAX - 1, saturate_add(int16_t(INT16_MAX - 5), 4.1));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX - 5), 5.1));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX - 5), 6.1));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX - 5), 7.1));
  ASSERT_EQ(INT16_MAX - 1, saturate_add(int16_t(INT16_MAX / 2), INT16_MAX / 2));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX - 10), INT16_MAX - 10));
  ASSERT_EQ(INT16_MIN + 1, saturate_add(int16_t(INT16_MIN), 1));
  ASSERT_EQ(INT16_MIN + 2, saturate_add(int16_t(INT16_MIN), 2));
  ASSERT_EQ(INT16_MIN, saturate_add(int16_t(INT16_MIN), -1));
  ASSERT_EQ(INT16_MIN, saturate_add(int16_t(INT16_MIN), INT16_MIN));
  ASSERT_EQ(-1, saturate_add(int16_t(INT16_MIN), INT16_MAX));

  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX), 0));
  ASSERT_EQ(UINT16_MAX - 1, saturate_add(uint16_t(UINT16_MAX - 5), 4));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX - 5), 5));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX - 5), 6));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX - 5), 7));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX), 0.1));
  ASSERT_EQ(UINT16_MAX - 1, saturate_add(uint16_t(UINT16_MAX - 5), 4.1));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX - 5), 5.1));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX - 5), 6.1));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX - 5), 7.1));
  ASSERT_EQ(UINT16_MAX - 1, saturate_add(uint16_t(UINT16_MAX / 2), UINT16_MAX / 2));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX - 10), UINT16_MAX - 10));
  ASSERT_EQ(0, saturate_add(uint16_t(0), -10));
  ASSERT_EQ(0, saturate_add(uint16_t(0), -1));
  ASSERT_EQ(0, saturate_add(uint16_t(0), 0));
  ASSERT_EQ(1, saturate_add(uint16_t(0), 1));
  ASSERT_EQ(2, saturate_add(uint16_t(0), 2));

  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX, 0));
  ASSERT_EQ(INT32_MAX - 1, saturate_add(INT32_MAX - 5, 4));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX - 5, 5));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX - 5, 6));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX - 5, 7));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX, 0.1));
  ASSERT_EQ(INT32_MAX - 1, saturate_add(INT32_MAX - 5, 4.1));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX - 5, 5.1));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX - 5, 6.1));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX - 5, 7.1));
  ASSERT_EQ(INT32_MAX - 1, saturate_add(INT32_MAX / 2, INT32_MAX / 2));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX - 10, INT32_MAX - 10));
  ASSERT_EQ(INT32_MIN + 1, saturate_add(INT32_MIN, 1));
  ASSERT_EQ(INT32_MIN + 2, saturate_add(INT32_MIN, 2));
  ASSERT_EQ(INT32_MIN, saturate_add(INT32_MIN, -1));
  ASSERT_EQ(INT32_MIN, saturate_add(INT32_MIN, INT32_MIN));
  ASSERT_EQ(-1, saturate_add(INT32_MIN, INT32_MAX));

  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX), 0));
  ASSERT_EQ(UINT32_MAX - 1, saturate_add(uint32_t(UINT32_MAX - 5), 4));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX - 5), 5));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX - 5), 6));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX - 5), 7));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX), 0.1));
  ASSERT_EQ(UINT32_MAX - 1, saturate_add(uint32_t(UINT32_MAX - 5), 4.1));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX - 5), 5.1));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX - 5), 6.1));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX - 5), 7.1));
  ASSERT_EQ(UINT32_MAX - 1, saturate_add(uint32_t(UINT32_MAX / 2), UINT32_MAX / 2));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX - 10), UINT32_MAX - 10));
  ASSERT_EQ(uint32_t(0), saturate_add(uint32_t(0), -10));
  ASSERT_EQ(uint32_t(0), saturate_add(uint32_t(0), -1));
  ASSERT_EQ(uint32_t(0), saturate_add(uint32_t(0), 0));
  ASSERT_EQ(uint32_t(1), saturate_add(uint32_t(0), 1));
  ASSERT_EQ(uint32_t(2), saturate_add(uint32_t(0), 2));

  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX), 0));
  ASSERT_EQ(INT64_MAX - 1, saturate_add(int64_t(INT64_MAX - 5), 4));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX - 5), 5));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX - 5), 6));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX - 5), 7));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX), 0.1));
  // ASSERT_EQ(INT64_MAX-1, saturate_add(int64_t(INT64_MAX-5), 4.1));  // failed because float type don't have enough
  // precision
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX - 5), 5.1));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX - 5), 6.1));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX - 5), 7.1));
  ASSERT_EQ(INT64_MAX - 1, saturate_add(int64_t(INT64_MAX / 2), INT64_MAX / 2));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX - 10), INT64_MAX - 10));
  ASSERT_EQ(INT64_MIN + 1, saturate_add(int64_t(INT64_MIN), 1));
  ASSERT_EQ(INT64_MIN + 2, saturate_add(int64_t(INT64_MIN), 2));
  ASSERT_EQ(INT64_MIN, saturate_add(int64_t(INT64_MIN), -1));
  ASSERT_EQ(INT64_MIN, saturate_add(int64_t(INT64_MIN), INT64_MIN));
  ASSERT_EQ(-1, saturate_add(int64_t(INT64_MIN), INT64_MAX));

  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX), 0));
  ASSERT_EQ(UINT64_MAX - 1, saturate_add(uint64_t(UINT64_MAX - 5), 4));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX - 5), 5));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX - 5), 6));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX - 5), 7));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX), 0.1));
  // ASSERT_EQ(UINT64_MAX-1, saturate_add(uint64_t(UINT64_MAX-5), 4.1)); // failed because float type don't have enough
  // precision
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX - 5), 5.1));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX - 5), 6.1));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX - 5), 7.1));
  ASSERT_EQ(UINT64_MAX - 1, saturate_add(uint64_t(UINT64_MAX / 2), UINT64_MAX / 2));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX - 10), UINT64_MAX - 10));
  ASSERT_EQ(uint64_t(0), saturate_add(uint64_t(0), -10));
  ASSERT_EQ(uint64_t(0), saturate_add(uint64_t(0), -1));
  ASSERT_EQ(uint64_t(0), saturate_add(uint64_t(0), 0));
  ASSERT_EQ(uint64_t(1), saturate_add(uint64_t(0), 1));
  ASSERT_EQ(uint64_t(2), saturate_add(uint64_t(0), 2));

  int64_t tmp = INT64_MIN;
  ASSERT_EQ(uint64_t(INT64_MAX) + 1, static_cast<uint64_t>(-tmp));
  ++tmp;
  ASSERT_EQ(uint64_t(INT64_MAX), static_cast<uint64_t>(-tmp));

  ASSERT_EQ(2 * uint64_t(INT64_MAX) + 1, uint64_t(INT64_MAX) - uint64_t(INT64_MIN));
  ASSERT_EQ(2 * uint64_t(INT64_MAX), uint64_t(INT64_MAX) - uint64_t(INT64_MIN + 1));
  ASSERT_EQ(uint64_t(INT64_MAX) + 1, uint64_t(INT64_MAX) - uint64_t(int64_t(-1)));
  ASSERT_EQ(uint64_t(INT64_MAX) + 2, uint64_t(INT64_MAX) - uint64_t(int64_t(-2)));

  ASSERT_EQ(uint64_t(0), saturate_add(uint64_t(INT64_MAX), INT64_MIN));
  ASSERT_EQ(uint64_t(0), saturate_add(uint64_t(INT64_MAX) + 1, INT64_MIN));
  ASSERT_EQ(uint64_t(1), saturate_add(uint64_t(INT64_MAX) + 2, INT64_MIN));
  ASSERT_EQ(int64_t(-1), saturate_add(INT64_MIN, uint64_t(INT64_MAX)));
  ASSERT_EQ(int64_t(0), saturate_add(INT64_MIN, uint64_t(INT64_MAX) + 1));
  ASSERT_EQ(int64_t(1), saturate_add(INT64_MIN, uint64_t(INT64_MAX) + 2));
}

TEST(saturate, sub)
{
  using namespace nim;

  verifySaturateSubFileData<uint8_t>(getTestDataDir().filePath("uint8_sub_test.dat"));
  verifySaturateSubFileData<uint16_t>(getTestDataDir().filePath("uint16_sub_test.dat"));
  verifySaturateSubFileData<uint32_t>(getTestDataDir().filePath("uint32_sub_test.dat"));
  verifySaturateSubFileData<uint64_t>(getTestDataDir().filePath("uint64_sub_test.dat"));
  verifySaturateSubFileData<int8_t>(getTestDataDir().filePath("int8_sub_test.dat"));
  verifySaturateSubFileData<int16_t>(getTestDataDir().filePath("int16_sub_test.dat"));
  verifySaturateSubFileData<int32_t>(getTestDataDir().filePath("int32_sub_test.dat"));
  verifySaturateSubFileData<int64_t>(getTestDataDir().filePath("int64_sub_test.dat"));

  ASSERT_EQ(9, saturate_sub(10, 1));
  ASSERT_EQ(8, saturate_sub(10, 2));
  ASSERT_EQ(5, saturate_sub(10, 5));
  ASSERT_EQ(3, saturate_sub(10, 7));
  ASSERT_EQ(0, saturate_sub(10, 10));
  ASSERT_EQ(-1, saturate_sub(10, 11));

  ASSERT_EQ(INT8_MIN + 5, saturate_sub(int8_t(INT8_MIN + 5), 0));
  ASSERT_EQ(INT8_MIN + 3, saturate_sub(int8_t(INT8_MIN + 5), 2));
  ASSERT_EQ(INT8_MIN, saturate_sub(int8_t(INT8_MIN + 5), 5));
  ASSERT_EQ(INT8_MIN, saturate_sub(int8_t(INT8_MIN + 5), 6));
  ASSERT_EQ(int8_t(0), saturate_sub(int8_t(INT8_MIN), INT8_MIN));
  ASSERT_EQ(int8_t(-5), saturate_sub(int8_t(INT8_MAX - 5), INT8_MAX));
  ASSERT_EQ(INT8_MIN, saturate_sub(int8_t(INT8_MIN), INT8_MAX));

  ASSERT_EQ(uint8_t(5), saturate_sub(uint8_t(5), 0));
  ASSERT_EQ(uint8_t(3), saturate_sub(uint8_t(5), 2));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(5), 5));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(5), 6));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(0), 0));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(UINT8_MAX - 5), UINT8_MAX));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(0), UINT8_MAX));

  ASSERT_EQ(INT16_MIN + 5, saturate_sub(int16_t(INT16_MIN + 5), 0));
  ASSERT_EQ(INT16_MIN + 3, saturate_sub(int16_t(INT16_MIN + 5), 2));
  ASSERT_EQ(INT16_MIN, saturate_sub(int16_t(INT16_MIN + 5), 5));
  ASSERT_EQ(INT16_MIN, saturate_sub(int16_t(INT16_MIN + 5), 6));
  ASSERT_EQ(int16_t(0), saturate_sub(int16_t(INT16_MIN), INT16_MIN));
  ASSERT_EQ(int16_t(-5), saturate_sub(int16_t(INT16_MAX - 5), INT16_MAX));
  ASSERT_EQ(INT16_MIN, saturate_sub(int16_t(INT16_MIN), INT16_MAX));

  ASSERT_EQ(uint16_t(5), saturate_sub(uint16_t(5), 0));
  ASSERT_EQ(uint16_t(3), saturate_sub(uint16_t(5), 2));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(5), 5));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(5), 6));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(0), 0));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(UINT16_MAX - 5), UINT16_MAX));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(0), UINT16_MAX));

  ASSERT_EQ(INT32_MIN + 5, saturate_sub(int32_t(INT32_MIN + 5), 0));
  ASSERT_EQ(INT32_MIN + 3, saturate_sub(int32_t(INT32_MIN + 5), 2));
  ASSERT_EQ(INT32_MIN, saturate_sub(int32_t(INT32_MIN + 5), 5));
  ASSERT_EQ(INT32_MIN, saturate_sub(int32_t(INT32_MIN + 5), 6));
  ASSERT_EQ(int32_t(0), saturate_sub(int32_t(INT32_MIN), INT32_MIN));
  ASSERT_EQ(int32_t(-5), saturate_sub(int32_t(INT32_MAX - 5), INT32_MAX));
  ASSERT_EQ(INT32_MIN, saturate_sub(int32_t(INT32_MIN), INT32_MAX));

  ASSERT_EQ(uint32_t(5), saturate_sub(uint32_t(5), 0));
  ASSERT_EQ(uint32_t(3), saturate_sub(uint32_t(5), 2));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(5), 5));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(5), 6));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(0), 0));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(UINT32_MAX - 5), UINT32_MAX));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(0), UINT32_MAX));

  ASSERT_EQ(INT64_MIN + 5, saturate_sub(int64_t(INT64_MIN + 5), 0));
  ASSERT_EQ(INT64_MIN + 3, saturate_sub(int64_t(INT64_MIN + 5), 2));
  ASSERT_EQ(INT64_MIN, saturate_sub(int64_t(INT64_MIN + 5), 5));
  ASSERT_EQ(INT64_MIN, saturate_sub(int64_t(INT64_MIN + 5), 6));
  ASSERT_EQ(int64_t(0), saturate_sub(int64_t(INT64_MIN), INT64_MIN));
  ASSERT_EQ(int64_t(-5), saturate_sub(int64_t(INT64_MAX - 5), INT64_MAX));
  ASSERT_EQ(INT64_MIN, saturate_sub(int64_t(INT64_MIN), INT64_MAX));

  ASSERT_EQ(uint64_t(5), saturate_sub(uint64_t(5), 0));
  ASSERT_EQ(uint64_t(3), saturate_sub(uint64_t(5), 2));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(5), 5));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(5), 6));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(0), 0));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(UINT64_MAX - 5), UINT64_MAX));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(0), UINT64_MAX));

  ASSERT_EQ(uint64_t(INT64_MAX) + 1, uint64_t(0) - uint64_t(INT64_MIN));
  ASSERT_EQ(uint64_t(INT64_MAX), uint64_t(int64_t(-1)) - uint64_t(INT64_MIN));
  ASSERT_EQ(uint64_t(INT64_MAX) + 2, uint64_t(int64_t(1)) - uint64_t(INT64_MIN));
  ASSERT_EQ(uint64_t(INT64_MAX) + 3, uint64_t(int64_t(2)) - uint64_t(INT64_MIN));
}

TEST(saturate, mul)
{
  using namespace nim;

  ASSERT_EQ(int8_t(0), saturate_mul(int8_t(INT8_MAX), 0));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX - 5), 4));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MAX - 5), -5));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX - 95), 6));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(-INT8_MAX + 5), 7));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX - 5), 5.1));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX - 5), 6.1));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX - 5), 7.1));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX / 2), INT8_MAX / 2));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MAX - 10), -INT8_MAX));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MIN), 1));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MIN), 2));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MIN), -1));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MIN), INT8_MIN));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MIN), INT8_MAX));

  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(UINT8_MAX), 0));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX - 5), 4));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(UINT8_MAX - 5), -5));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX - 5), 6));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(UINT8_MAX - 5), -2.7));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX), 1.1));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX - 5), 5.1));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX - 5), 6.1));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX - 5), 7.1));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX / 2), UINT8_MAX / 2));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX - 10), UINT8_MAX - 10));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(0), -10));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(0), -1));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(0), 0));
  ASSERT_EQ(uint8_t(6), saturate_mul(uint8_t(6), 1));
  ASSERT_EQ(uint8_t(6), saturate_mul(uint8_t(3), 2));

  ASSERT_EQ(int16_t(0), saturate_mul(int16_t(INT16_MAX), 0));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX - 5), 4));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MAX - 5), -5));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX - 95), 6));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(-INT16_MAX + 5), 7));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX - 5), 5.1));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX - 5), 6.1));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX - 5), 7.1));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX / 2), INT16_MAX / 2));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MAX - 10), -INT16_MAX));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MIN), 1));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MIN), 2));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MIN), -1));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MIN), INT16_MIN));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MIN), INT16_MAX));

  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(UINT16_MAX), 0));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX - 5), 4));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(UINT16_MAX - 5), -5));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX - 5), 6));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(UINT16_MAX - 5), -2.7));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX), 1.1));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX - 5), 5.1));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX - 5), 6.1));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX - 5), 7.1));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX / 2), UINT16_MAX / 2));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX - 10), UINT16_MAX - 10));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(0), -10));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(0), -1));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(0), 0));
  ASSERT_EQ(uint16_t(6), saturate_mul(uint16_t(6), 1));
  ASSERT_EQ(uint16_t(6), saturate_mul(uint16_t(3), 2));

  ASSERT_EQ(int32_t(0), saturate_mul(int32_t(INT32_MAX), 0));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX - 5), 4));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MAX - 5), -5));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX - 95), 6));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(-INT32_MAX + 5), 7));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX - 5), 5.1));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX - 5), 6.1));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX - 5), 7.1));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX / 2), INT32_MAX / 2));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MAX - 10), -INT32_MAX));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MIN), 1));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MIN), 2));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MIN), -1));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MIN), INT32_MIN));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MIN), INT32_MAX));

  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(UINT32_MAX), 0));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX - 5), 4));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(UINT32_MAX - 5), -5));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX - 5), 6));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(UINT32_MAX - 5), -2.7));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX), 1.1));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX - 5), 5.1));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX - 5), 6.1));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX - 5), 7.1));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX / 2), UINT32_MAX / 2));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX - 10), UINT32_MAX - 10));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(0), -10));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(0), -1));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(0), 0));
  ASSERT_EQ(uint32_t(6), saturate_mul(uint32_t(6), 1));
  ASSERT_EQ(uint32_t(6), saturate_mul(uint32_t(3), 2));

  ASSERT_EQ(int64_t(0), saturate_mul(int64_t(INT64_MAX), 0));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX - 5), 4));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MAX - 5), -5));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX - 95), 6));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(-INT64_MAX + 5), 7));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX - 5), 5.1));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX - 5), 6.1));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX - 5), 7.1));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX / 2), INT64_MAX / 2));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MAX - 10), -INT64_MAX));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MIN), 1));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MIN), 2));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MIN), -1));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MIN), INT64_MIN));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MIN), INT64_MAX));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MIN), 1_u64));

  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(UINT64_MAX), 0));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX - 5), 4));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(UINT64_MAX - 5), -5));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX - 5), 6));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(UINT64_MAX - 5), -2.7));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX), 1.1));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX - 5), 5.1));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX - 5), 6.1));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX - 5), 7.1));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX / 2), UINT64_MAX / 2));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX - 10), UINT64_MAX - 10));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(0), -10));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(0), -1));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(0), 0));
  ASSERT_EQ(uint64_t(6), saturate_mul(uint64_t(6), 1));
  ASSERT_EQ(uint64_t(6), saturate_mul(uint64_t(3), 2));

  verifySaturateMulFileData<uint8_t>(getTestDataDir().filePath("uint8_mul_test.dat"));
  verifySaturateMulFileData<uint16_t>(getTestDataDir().filePath("uint16_mul_test.dat"));
  verifySaturateMulFileData<uint32_t>(getTestDataDir().filePath("uint32_mul_test.dat"));
  verifySaturateMulFileData<uint64_t>(getTestDataDir().filePath("uint64_mul_test.dat"));
  verifySaturateMulFileData<int8_t>(getTestDataDir().filePath("int8_mul_test.dat"));
  verifySaturateMulFileData<int16_t>(getTestDataDir().filePath("int16_mul_test.dat"));
  verifySaturateMulFileData<int32_t>(getTestDataDir().filePath("int32_mul_test.dat"));
  verifySaturateMulFileData<int64_t>(getTestDataDir().filePath("int64_mul_test.dat"));
}

TEST(saturate, div)
{
  using namespace nim;

  // matlab div do rounding
  verifySaturateDivFileData<uint8_t>(getTestDataDir().filePath("uint8_div_test.dat"));
  verifySaturateDivFileData<uint16_t>(getTestDataDir().filePath("uint16_div_test.dat"));
  verifySaturateDivFileData<uint32_t>(getTestDataDir().filePath("uint32_div_test.dat"));
  verifySaturateDivFileData<uint64_t>(getTestDataDir().filePath("uint64_div_test.dat"));
  verifySaturateDivFileData<int8_t>(getTestDataDir().filePath("int8_div_test.dat"));
  verifySaturateDivFileData<int16_t>(getTestDataDir().filePath("int16_div_test.dat"));
  verifySaturateDivFileData<int32_t>(getTestDataDir().filePath("int32_div_test.dat"));
  verifySaturateDivFileData<int64_t>(getTestDataDir().filePath("int64_div_test.dat"));

  ASSERT_EQ(INT64_MIN, saturate_div(int64_t(INT64_MIN), 1_u64));
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
