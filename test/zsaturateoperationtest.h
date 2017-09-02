#pragma once

#include "gtest/gtest.h"

#include "zsaturateoperation.h"

#include "zioutils.h"
#include <iostream>
#include <vector>

namespace {

using namespace nim;

template<typename T>
std::vector<T> readSaturateTestData(const QString &filename)
{
  std::ifstream inputFileStream;
  openFileStream(inputFileStream, filename, std::ios::in | std::ios::binary);

  uint64_t numPoints;
  readStream(inputFileStream, &numPoints, 8);
  CHECK(numPoints % 3 == 0);

  std::vector<T> res(numPoints);
  readStream(inputFileStream, res.data(), res.size() * sizeof(T));

  return res;
}

}  // namespace

TEST(saturate, ReinterpretCast)
{
  // reinterpret_cast result is unspecified because alignment requirement of __m128i (16) is stricter than uint8_t

  //  An object pointer can be explicitly converted to an object pointer of a different type.71 When a prvalue v of
  //  object pointer type is converted to the object pointer type “pointer to cv T”, the result is static_cast<cv
  //    T*>(static_cast<cv void*>(v)). Converting a prvalue of type “pointer to T1” to the type “pointer to
  //  T2” (where T1 and T2 are object types and where the alignment requirements of T2 are no stricter than
  //  those of T1) and back to its original type yields the original pointer value.

  //  A prvalue of type “pointer to cv1 void” can be converted to a prvalue of type “pointer to cv2 T,” where T is
  //  an object type and cv2 is the same cv-qualification as, or greater cv-qualification than, cv1. The null pointer
  //  value is converted to the null pointer value of the destination type. If the original pointer value represents
  //  the address A of a byte in memory and A satisfies the alignment requirement of T, then the resulting pointer
  //  value represents the same address as the original pointer value, that is, A. The result of any other such
  //  pointer conversion is unspecified.
  LOG(INFO) << "Alignment of uint8_t: " << alignof(uint8_t);
  LOG(INFO) << "Alignment of int8_t: " << alignof(int8_t);
  LOG(INFO) << "Alignment of uint16_t: " << alignof(uint16_t);
  LOG(INFO) << "Alignment of int16_t: " << alignof(int16_t);
  LOG(INFO) << "Alignment of __m128i: " << alignof(__m128i);

  for (int k = 0; k < 10; ++k) {
    auto data = std::make_unique<uint8_t[]>(200);
    for (size_t j = 0; j < 200 - 16; ++j) {
      ASSERT_EQ(static_cast<const void*>(data.get() + j),
                static_cast<const void*>(reinterpret_cast<const __m128i*>(data.get() + j)));
      ASSERT_EQ(static_cast<void*>(data.get() + j),
                static_cast<void*>(reinterpret_cast<__m128i*>(data.get() + j)));
    }
  }

  for (int k = 0; k < 10; ++k) {
    auto data = std::make_unique<int8_t[]>(200);
    for (size_t j = 0; j < 200 - 16; ++j) {
      ASSERT_EQ(reinterpret_cast<intptr_t>(data.get() + j),
                reinterpret_cast<intptr_t>(reinterpret_cast<const __m128i*>(data.get() + j)));
      ASSERT_EQ(reinterpret_cast<intptr_t>(data.get() + j),
                reinterpret_cast<intptr_t>(reinterpret_cast<__m128i*>(data.get() + j)));
    }
  }

  for (int k = 0; k < 10; ++k) {
    auto data = std::make_unique<uint16_t[]>(200);
    for (size_t j = 0; j < 200 - 16; ++j) {
      ASSERT_EQ(reinterpret_cast<intptr_t>(data.get() + j),
                reinterpret_cast<intptr_t>(reinterpret_cast<const __m128i*>(data.get() + j)));
      ASSERT_EQ(reinterpret_cast<intptr_t>(data.get() + j),
                reinterpret_cast<intptr_t>(reinterpret_cast<__m128i*>(data.get() + j)));
    }
  }

  for (int k = 0; k < 10; ++k) {
    auto data = std::make_unique<int16_t[]>(200);
    for (size_t j = 0; j < 200 - 16; ++j) {
      ASSERT_EQ(reinterpret_cast<intptr_t>(data.get() + j),
                reinterpret_cast<intptr_t>(reinterpret_cast<const __m128i*>(data.get() + j)));
      ASSERT_EQ(reinterpret_cast<intptr_t>(data.get() + j),
                reinterpret_cast<intptr_t>(reinterpret_cast<__m128i*>(data.get() + j)));
    }
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"
#endif

TEST(saturate, add)
{
  using namespace nim;

  std::vector<uint8_t> datau8 = readSaturateTestData<uint8_t>(getTestDataDir().filePath("uint8_add_test.dat"));
  for (size_t i=0; i<datau8.size(); i+=3) {
    ASSERT_EQ(datau8[i+2], saturate_add(datau8[i], datau8[i+1])) << (int)datau8[i] << " " << (int)datau8[i+1];
  }
  std::vector<uint16_t> datau16 = readSaturateTestData<uint16_t>(getTestDataDir().filePath("uint16_add_test.dat"));
  for (size_t i=0; i<datau16.size(); i+=3) {
    ASSERT_EQ(datau16[i+2], saturate_add(datau16[i], datau16[i+1])) << (int)datau16[i] << " " << (int)datau16[i+1];
  }
  std::vector<uint32_t> datau32 = readSaturateTestData<uint32_t>(getTestDataDir().filePath("uint32_add_test.dat"));
  for (size_t i=0; i<datau32.size(); i+=3) {
    ASSERT_EQ(datau32[i+2], saturate_add(datau32[i], datau32[i+1])) << datau32[i] << " " << datau32[i+1];
  }
  std::vector<uint64_t> datau64 = readSaturateTestData<uint64_t>(getTestDataDir().filePath("uint64_add_test.dat"));
  for (size_t i=0; i<datau64.size(); i+=3) {
    ASSERT_EQ(datau64[i+2], saturate_add(datau64[i], datau64[i+1])) << datau64[i] << " " << datau64[i+1];
  }
  std::vector<int8_t> datai8 = readSaturateTestData<int8_t>(getTestDataDir().filePath("int8_add_test.dat"));
  for (size_t i=0; i<datai8.size(); i+=3) {
    ASSERT_EQ(datai8[i+2], saturate_add(datai8[i], datai8[i+1])) << (int)datai8[i] << " " << (int)datai8[i+1];
  }
  std::vector<int16_t> datai16 = readSaturateTestData<int16_t>(getTestDataDir().filePath("int16_add_test.dat"));
  for (size_t i=0; i<datai16.size(); i+=3) {
    ASSERT_EQ(datai16[i+2], saturate_add(datai16[i], datai16[i+1])) << (int)datai16[i] << " " << (int)datai16[i+1];
  }
  std::vector<int32_t> datai32 = readSaturateTestData<int32_t>(getTestDataDir().filePath("int32_add_test.dat"));
  for (size_t i=0; i<datai32.size(); i+=3) {
    ASSERT_EQ(datai32[i+2], saturate_add(datai32[i], datai32[i+1])) << datai32[i] << " " << datai32[i+1];
  }
  std::vector<int64_t> datai64 = readSaturateTestData<int64_t>(getTestDataDir().filePath("int64_add_test.dat"));
  for (size_t i=0; i<datai64.size(); i+=3) {
    ASSERT_EQ(datai64[i+2], saturate_add(datai64[i], datai64[i+1])) << datai64[i] << " " << datai64[i+1];
  }

  ASSERT_EQ(1, saturate_add(0, 1));
  ASSERT_EQ(2, saturate_add(1, 1));
  ASSERT_EQ(4, saturate_add(2, 2));
  ASSERT_EQ(-1, saturate_add(0, -1));
  ASSERT_EQ(0, saturate_add(1, -1));
  ASSERT_EQ(1, saturate_add(3, -2));

  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX), 0));
  ASSERT_EQ(INT8_MAX-1, saturate_add(int8_t(INT8_MAX-5), 4));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX-5), 5));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX-5), 6));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX-5), 7));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX), 0.1));
  ASSERT_EQ(INT8_MAX-1, saturate_add(int8_t(INT8_MAX-5), 4.1));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX-5), 5.1));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX-5), 6.1));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX-5), 7.1));
  ASSERT_EQ(INT8_MAX-1, saturate_add(int8_t(INT8_MAX/2), INT8_MAX/2));
  ASSERT_EQ(INT8_MAX, saturate_add(int8_t(INT8_MAX-10), INT8_MAX-10));
  ASSERT_EQ(INT8_MIN+1, saturate_add(int8_t(INT8_MIN), 1));
  ASSERT_EQ(INT8_MIN+2, saturate_add(int8_t(INT8_MIN), 2));
  ASSERT_EQ(INT8_MIN, saturate_add(int8_t(INT8_MIN), -1));
  ASSERT_EQ(INT8_MIN, saturate_add(int8_t(INT8_MIN), INT8_MIN));
  ASSERT_EQ(-1, saturate_add(int8_t(INT8_MIN), INT8_MAX));

  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX), 0));
  ASSERT_EQ(UINT8_MAX-1, saturate_add(uint8_t(UINT8_MAX-5), 4));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX-5), 5));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX-5), 6));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX-5), 7));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX), 0.1));
  ASSERT_EQ(UINT8_MAX-1, saturate_add(uint8_t(UINT8_MAX-5), 4.1));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX-5), 5.1));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX-5), 6.1));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX-5), 7.1));
  ASSERT_EQ(UINT8_MAX-1, saturate_add(uint8_t(UINT8_MAX/2), UINT8_MAX/2));
  ASSERT_EQ(UINT8_MAX, saturate_add(uint8_t(UINT8_MAX-10), UINT8_MAX-10));
  ASSERT_EQ(0, saturate_add(uint8_t(0), -10));
  ASSERT_EQ(0, saturate_add(uint8_t(0), -1));
  ASSERT_EQ(0, saturate_add(uint8_t(0), 0));
  ASSERT_EQ(1, saturate_add(uint8_t(0), 1));
  ASSERT_EQ(2, saturate_add(uint8_t(0), 2));


  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX), 0));
  ASSERT_EQ(INT16_MAX-1, saturate_add(int16_t(INT16_MAX-5), 4));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX-5), 5));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX-5), 6));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX-5), 7));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX), 0.1));
  ASSERT_EQ(INT16_MAX-1, saturate_add(int16_t(INT16_MAX-5), 4.1));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX-5), 5.1));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX-5), 6.1));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX-5), 7.1));
  ASSERT_EQ(INT16_MAX-1, saturate_add(int16_t(INT16_MAX/2), INT16_MAX/2));
  ASSERT_EQ(INT16_MAX, saturate_add(int16_t(INT16_MAX-10), INT16_MAX-10));
  ASSERT_EQ(INT16_MIN+1, saturate_add(int16_t(INT16_MIN), 1));
  ASSERT_EQ(INT16_MIN+2, saturate_add(int16_t(INT16_MIN), 2));
  ASSERT_EQ(INT16_MIN, saturate_add(int16_t(INT16_MIN), -1));
  ASSERT_EQ(INT16_MIN, saturate_add(int16_t(INT16_MIN), INT16_MIN));
  ASSERT_EQ(-1, saturate_add(int16_t(INT16_MIN), INT16_MAX));

  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX), 0));
  ASSERT_EQ(UINT16_MAX-1, saturate_add(uint16_t(UINT16_MAX-5), 4));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX-5), 5));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX-5), 6));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX-5), 7));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX), 0.1));
  ASSERT_EQ(UINT16_MAX-1, saturate_add(uint16_t(UINT16_MAX-5), 4.1));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX-5), 5.1));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX-5), 6.1));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX-5), 7.1));
  ASSERT_EQ(UINT16_MAX-1, saturate_add(uint16_t(UINT16_MAX/2), UINT16_MAX/2));
  ASSERT_EQ(UINT16_MAX, saturate_add(uint16_t(UINT16_MAX-10), UINT16_MAX-10));
  ASSERT_EQ(0, saturate_add(uint16_t(0), -10));
  ASSERT_EQ(0, saturate_add(uint16_t(0), -1));
  ASSERT_EQ(0, saturate_add(uint16_t(0), 0));
  ASSERT_EQ(1, saturate_add(uint16_t(0), 1));
  ASSERT_EQ(2, saturate_add(uint16_t(0), 2));

  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX, 0));
  ASSERT_EQ(INT32_MAX-1, saturate_add(INT32_MAX-5, 4));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX-5, 5));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX-5, 6));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX-5, 7));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX, 0.1));
  ASSERT_EQ(INT32_MAX-1, saturate_add(INT32_MAX-5, 4.1));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX-5, 5.1));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX-5, 6.1));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX-5, 7.1));
  ASSERT_EQ(INT32_MAX-1, saturate_add(INT32_MAX/2, INT32_MAX/2));
  ASSERT_EQ(INT32_MAX, saturate_add(INT32_MAX-10, INT32_MAX-10));
  ASSERT_EQ(INT32_MIN+1, saturate_add(INT32_MIN, 1));
  ASSERT_EQ(INT32_MIN+2, saturate_add(INT32_MIN, 2));
  ASSERT_EQ(INT32_MIN, saturate_add(INT32_MIN, -1));
  ASSERT_EQ(INT32_MIN, saturate_add(INT32_MIN, INT32_MIN));
  ASSERT_EQ(-1, saturate_add(INT32_MIN, INT32_MAX));

  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX), 0));
  ASSERT_EQ(UINT32_MAX-1, saturate_add(uint32_t(UINT32_MAX-5), 4));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX-5), 5));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX-5), 6));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX-5), 7));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX), 0.1));
  ASSERT_EQ(UINT32_MAX-1, saturate_add(uint32_t(UINT32_MAX-5), 4.1));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX-5), 5.1));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX-5), 6.1));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX-5), 7.1));
  ASSERT_EQ(UINT32_MAX-1, saturate_add(uint32_t(UINT32_MAX/2), UINT32_MAX/2));
  ASSERT_EQ(UINT32_MAX, saturate_add(uint32_t(UINT32_MAX-10), UINT32_MAX-10));
  ASSERT_EQ(uint32_t(0), saturate_add(uint32_t(0), -10));
  ASSERT_EQ(uint32_t(0), saturate_add(uint32_t(0), -1));
  ASSERT_EQ(uint32_t(0), saturate_add(uint32_t(0), 0));
  ASSERT_EQ(uint32_t(1), saturate_add(uint32_t(0), 1));
  ASSERT_EQ(uint32_t(2), saturate_add(uint32_t(0), 2));

  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX), 0));
  ASSERT_EQ(INT64_MAX-1, saturate_add(int64_t(INT64_MAX-5), 4));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX-5), 5));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX-5), 6));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX-5), 7));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX), 0.1));
  //ASSERT_EQ(INT64_MAX-1, saturate_add(int64_t(INT64_MAX-5), 4.1));  // failed because float type don't have enough precision
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX-5), 5.1));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX-5), 6.1));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX-5), 7.1));
  ASSERT_EQ(INT64_MAX-1, saturate_add(int64_t(INT64_MAX/2), INT64_MAX/2));
  ASSERT_EQ(INT64_MAX, saturate_add(int64_t(INT64_MAX-10), INT64_MAX-10));
  ASSERT_EQ(INT64_MIN+1, saturate_add(int64_t(INT64_MIN), 1));
  ASSERT_EQ(INT64_MIN+2, saturate_add(int64_t(INT64_MIN), 2));
  ASSERT_EQ(INT64_MIN, saturate_add(int64_t(INT64_MIN), -1));
  ASSERT_EQ(INT64_MIN, saturate_add(int64_t(INT64_MIN), INT64_MIN));
  ASSERT_EQ(-1, saturate_add(int64_t(INT64_MIN), INT64_MAX));

  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX), 0));
  ASSERT_EQ(UINT64_MAX-1, saturate_add(uint64_t(UINT64_MAX-5), 4));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX-5), 5));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX-5), 6));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX-5), 7));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX), 0.1));
  //ASSERT_EQ(UINT64_MAX-1, saturate_add(uint64_t(UINT64_MAX-5), 4.1)); // failed because float type don't have enough precision
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX-5), 5.1));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX-5), 6.1));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX-5), 7.1));
  ASSERT_EQ(UINT64_MAX-1, saturate_add(uint64_t(UINT64_MAX/2), UINT64_MAX/2));
  ASSERT_EQ(UINT64_MAX, saturate_add(uint64_t(UINT64_MAX-10), UINT64_MAX-10));
  ASSERT_EQ(uint64_t(0), saturate_add(uint64_t(0), -10));
  ASSERT_EQ(uint64_t(0), saturate_add(uint64_t(0), -1));
  ASSERT_EQ(uint64_t(0), saturate_add(uint64_t(0), 0));
  ASSERT_EQ(uint64_t(1), saturate_add(uint64_t(0), 1));
  ASSERT_EQ(uint64_t(2), saturate_add(uint64_t(0), 2));

  int64_t tmp = INT64_MIN;
  ASSERT_EQ(uint64_t(INT64_MAX) + 1, static_cast<uint64_t>(-tmp));
  ++tmp;
  ASSERT_EQ(uint64_t(INT64_MAX), static_cast<uint64_t>(-tmp));

  ASSERT_EQ(2 * uint64_t(INT64_MAX) + 1, static_cast<uint64_t>(INT64_MAX - INT64_MIN));
  ASSERT_EQ(2 * uint64_t(INT64_MAX), static_cast<uint64_t>(INT64_MAX - (INT64_MIN + 1)));
  ASSERT_EQ(uint64_t(INT64_MAX) + 1, static_cast<uint64_t>(INT64_MAX - int64_t(-1)));
  ASSERT_EQ(uint64_t(INT64_MAX) + 2, static_cast<uint64_t>(INT64_MAX - int64_t(-2)));

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

  std::vector<uint8_t> datau8 = readSaturateTestData<uint8_t>(getTestDataDir().filePath("uint8_sub_test.dat"));
  for (size_t i=0; i<datau8.size(); i+=3) {
    ASSERT_EQ(datau8[i+2], saturate_sub(datau8[i], datau8[i+1])) << (int)datau8[i] << " " << (int)datau8[i+1];
  }
  std::vector<uint16_t> datau16 = readSaturateTestData<uint16_t>(getTestDataDir().filePath("uint16_sub_test.dat"));
  for (size_t i=0; i<datau16.size(); i+=3) {
    ASSERT_EQ(datau16[i+2], saturate_sub(datau16[i], datau16[i+1])) << (int)datau16[i] << " " << (int)datau16[i+1];
  }
  std::vector<uint32_t> datau32 = readSaturateTestData<uint32_t>(getTestDataDir().filePath("uint32_sub_test.dat"));
  for (size_t i=0; i<datau32.size(); i+=3) {
    ASSERT_EQ(datau32[i+2], saturate_sub(datau32[i], datau32[i+1])) << datau32[i] << " " << datau32[i+1];
  }
  std::vector<uint64_t> datau64 = readSaturateTestData<uint64_t>(getTestDataDir().filePath("uint64_sub_test.dat"));
  for (size_t i=0; i<datau64.size(); i+=3) {
    ASSERT_EQ(datau64[i+2], saturate_sub(datau64[i], datau64[i+1])) << datau64[i] << " " << datau64[i+1];
  }
  std::vector<int8_t> datai8 = readSaturateTestData<int8_t>(getTestDataDir().filePath("int8_sub_test.dat"));
  for (size_t i=0; i<datai8.size(); i+=3) {
    ASSERT_EQ(datai8[i+2], saturate_sub(datai8[i], datai8[i+1])) << (int)datai8[i] << " " << (int)datai8[i+1];
  }
  std::vector<int16_t> datai16 = readSaturateTestData<int16_t>(getTestDataDir().filePath("int16_sub_test.dat"));
  for (size_t i=0; i<datai16.size(); i+=3) {
    ASSERT_EQ(datai16[i+2], saturate_sub(datai16[i], datai16[i+1])) << (int)datai16[i] << " " << (int)datai16[i+1];
  }
  std::vector<int32_t> datai32 = readSaturateTestData<int32_t>(getTestDataDir().filePath("int32_sub_test.dat"));
  for (size_t i=0; i<datai32.size(); i+=3) {
    ASSERT_EQ(datai32[i+2], saturate_sub(datai32[i], datai32[i+1])) << datai32[i] << " " << datai32[i+1];
  }
  std::vector<int64_t> datai64 = readSaturateTestData<int64_t>(getTestDataDir().filePath("int64_sub_test.dat"));
  for (size_t i=0; i<datai64.size(); i+=3) {
    ASSERT_EQ(datai64[i+2], saturate_sub(datai64[i], datai64[i+1])) << datai64[i] << " " << datai64[i+1];
  }

  ASSERT_EQ(9, saturate_sub(10, 1));
  ASSERT_EQ(8, saturate_sub(10, 2));
  ASSERT_EQ(5, saturate_sub(10, 5));
  ASSERT_EQ(3, saturate_sub(10, 7));
  ASSERT_EQ(0, saturate_sub(10, 10));
  ASSERT_EQ(-1, saturate_sub(10, 11));

  ASSERT_EQ(INT8_MIN+5, saturate_sub(int8_t(INT8_MIN+5), 0));
  ASSERT_EQ(INT8_MIN+3, saturate_sub(int8_t(INT8_MIN+5), 2));
  ASSERT_EQ(INT8_MIN, saturate_sub(int8_t(INT8_MIN+5), 5));
  ASSERT_EQ(INT8_MIN, saturate_sub(int8_t(INT8_MIN+5), 6));
  ASSERT_EQ(int8_t(0), saturate_sub(int8_t(INT8_MIN), INT8_MIN));
  ASSERT_EQ(int8_t(-5), saturate_sub(int8_t(INT8_MAX-5), INT8_MAX));
  ASSERT_EQ(INT8_MIN, saturate_sub(int8_t(INT8_MIN), INT8_MAX));

  ASSERT_EQ(uint8_t(5), saturate_sub(uint8_t(5), 0));
  ASSERT_EQ(uint8_t(3), saturate_sub(uint8_t(5), 2));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(5), 5));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(5), 6));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(0), 0));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(UINT8_MAX-5), UINT8_MAX));
  ASSERT_EQ(uint8_t(0), saturate_sub(uint8_t(0), UINT8_MAX));

  ASSERT_EQ(INT16_MIN+5, saturate_sub(int16_t(INT16_MIN+5), 0));
  ASSERT_EQ(INT16_MIN+3, saturate_sub(int16_t(INT16_MIN+5), 2));
  ASSERT_EQ(INT16_MIN, saturate_sub(int16_t(INT16_MIN+5), 5));
  ASSERT_EQ(INT16_MIN, saturate_sub(int16_t(INT16_MIN+5), 6));
  ASSERT_EQ(int16_t(0), saturate_sub(int16_t(INT16_MIN), INT16_MIN));
  ASSERT_EQ(int16_t(-5), saturate_sub(int16_t(INT16_MAX-5), INT16_MAX));
  ASSERT_EQ(INT16_MIN, saturate_sub(int16_t(INT16_MIN), INT16_MAX));

  ASSERT_EQ(uint16_t(5), saturate_sub(uint16_t(5), 0));
  ASSERT_EQ(uint16_t(3), saturate_sub(uint16_t(5), 2));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(5), 5));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(5), 6));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(0), 0));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(UINT16_MAX-5), UINT16_MAX));
  ASSERT_EQ(uint16_t(0), saturate_sub(uint16_t(0), UINT16_MAX));

  ASSERT_EQ(INT32_MIN+5, saturate_sub(int32_t(INT32_MIN+5), 0));
  ASSERT_EQ(INT32_MIN+3, saturate_sub(int32_t(INT32_MIN+5), 2));
  ASSERT_EQ(INT32_MIN, saturate_sub(int32_t(INT32_MIN+5), 5));
  ASSERT_EQ(INT32_MIN, saturate_sub(int32_t(INT32_MIN+5), 6));
  ASSERT_EQ(int32_t(0), saturate_sub(int32_t(INT32_MIN), INT32_MIN));
  ASSERT_EQ(int32_t(-5), saturate_sub(int32_t(INT32_MAX-5), INT32_MAX));
  ASSERT_EQ(INT32_MIN, saturate_sub(int32_t(INT32_MIN), INT32_MAX));

  ASSERT_EQ(uint32_t(5), saturate_sub(uint32_t(5), 0));
  ASSERT_EQ(uint32_t(3), saturate_sub(uint32_t(5), 2));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(5), 5));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(5), 6));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(0), 0));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(UINT32_MAX-5), UINT32_MAX));
  ASSERT_EQ(uint32_t(0), saturate_sub(uint32_t(0), UINT32_MAX));

  ASSERT_EQ(INT64_MIN+5, saturate_sub(int64_t(INT64_MIN+5), 0));
  ASSERT_EQ(INT64_MIN+3, saturate_sub(int64_t(INT64_MIN+5), 2));
  ASSERT_EQ(INT64_MIN, saturate_sub(int64_t(INT64_MIN+5), 5));
  ASSERT_EQ(INT64_MIN, saturate_sub(int64_t(INT64_MIN+5), 6));
  ASSERT_EQ(int64_t(0), saturate_sub(int64_t(INT64_MIN), INT64_MIN));
  ASSERT_EQ(int64_t(-5), saturate_sub(int64_t(INT64_MAX-5), INT64_MAX));
  ASSERT_EQ(INT64_MIN, saturate_sub(int64_t(INT64_MIN), INT64_MAX));

  ASSERT_EQ(uint64_t(5), saturate_sub(uint64_t(5), 0));
  ASSERT_EQ(uint64_t(3), saturate_sub(uint64_t(5), 2));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(5), 5));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(5), 6));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(0), 0));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(UINT64_MAX-5), UINT64_MAX));
  ASSERT_EQ(uint64_t(0), saturate_sub(uint64_t(0), UINT64_MAX));

  ASSERT_EQ(uint64_t(INT64_MAX) + 1, static_cast<uint64_t>(0 - INT64_MIN));
  ASSERT_EQ(uint64_t(INT64_MAX), static_cast<uint64_t>(int64_t(-1) - INT64_MIN));
  ASSERT_EQ(uint64_t(INT64_MAX) + 2, static_cast<uint64_t>(int64_t(1) - INT64_MIN));
  ASSERT_EQ(uint64_t(INT64_MAX) + 3, static_cast<uint64_t>(int64_t(2) - INT64_MIN));
}

TEST(saturate, mul)
{
  using namespace nim;

  ASSERT_EQ(int8_t(0), saturate_mul(int8_t(INT8_MAX), 0));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX-5), 4));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MAX-5), -5));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX-95), 6));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(-INT8_MAX+5), 7));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX-5), 5.1));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX-5), 6.1));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX-5), 7.1));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MAX/2), INT8_MAX/2));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MAX-10), -INT8_MAX));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MIN), 1));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MIN), 2));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MIN), -1));
  ASSERT_EQ(INT8_MAX, saturate_mul(int8_t(INT8_MIN), INT8_MIN));
  ASSERT_EQ(INT8_MIN, saturate_mul(int8_t(INT8_MIN), INT8_MAX));

  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(UINT8_MAX), 0));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX-5), 4));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(UINT8_MAX-5), -5));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX-5), 6));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(UINT8_MAX-5), -2.7));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX), 1.1));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX-5), 5.1));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX-5), 6.1));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX-5), 7.1));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX/2), UINT8_MAX/2));
  ASSERT_EQ(UINT8_MAX, saturate_mul(uint8_t(UINT8_MAX-10), UINT8_MAX-10));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(0), -10));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(0), -1));
  ASSERT_EQ(uint8_t(0), saturate_mul(uint8_t(0), 0));
  ASSERT_EQ(uint8_t(6), saturate_mul(uint8_t(6), 1));
  ASSERT_EQ(uint8_t(6), saturate_mul(uint8_t(3), 2));

  ASSERT_EQ(int16_t(0), saturate_mul(int16_t(INT16_MAX), 0));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX-5), 4));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MAX-5), -5));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX-95), 6));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(-INT16_MAX+5), 7));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX-5), 5.1));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX-5), 6.1));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX-5), 7.1));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MAX/2), INT16_MAX/2));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MAX-10), -INT16_MAX));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MIN), 1));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MIN), 2));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MIN), -1));
  ASSERT_EQ(INT16_MAX, saturate_mul(int16_t(INT16_MIN), INT16_MIN));
  ASSERT_EQ(INT16_MIN, saturate_mul(int16_t(INT16_MIN), INT16_MAX));

  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(UINT16_MAX), 0));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX-5), 4));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(UINT16_MAX-5), -5));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX-5), 6));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(UINT16_MAX-5), -2.7));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX), 1.1));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX-5), 5.1));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX-5), 6.1));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX-5), 7.1));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX/2), UINT16_MAX/2));
  ASSERT_EQ(UINT16_MAX, saturate_mul(uint16_t(UINT16_MAX-10), UINT16_MAX-10));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(0), -10));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(0), -1));
  ASSERT_EQ(uint16_t(0), saturate_mul(uint16_t(0), 0));
  ASSERT_EQ(uint16_t(6), saturate_mul(uint16_t(6), 1));
  ASSERT_EQ(uint16_t(6), saturate_mul(uint16_t(3), 2));

  ASSERT_EQ(int32_t(0), saturate_mul(int32_t(INT32_MAX), 0));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX-5), 4));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MAX-5), -5));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX-95), 6));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(-INT32_MAX+5), 7));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX-5), 5.1));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX-5), 6.1));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX-5), 7.1));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MAX/2), INT32_MAX/2));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MAX-10), -INT32_MAX));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MIN), 1));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MIN), 2));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MIN), -1));
  ASSERT_EQ(INT32_MAX, saturate_mul(int32_t(INT32_MIN), INT32_MIN));
  ASSERT_EQ(INT32_MIN, saturate_mul(int32_t(INT32_MIN), INT32_MAX));

  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(UINT32_MAX), 0));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX-5), 4));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(UINT32_MAX-5), -5));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX-5), 6));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(UINT32_MAX-5), -2.7));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX), 1.1));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX-5), 5.1));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX-5), 6.1));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX-5), 7.1));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX/2), UINT32_MAX/2));
  ASSERT_EQ(UINT32_MAX, saturate_mul(uint32_t(UINT32_MAX-10), UINT32_MAX-10));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(0), -10));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(0), -1));
  ASSERT_EQ(uint32_t(0), saturate_mul(uint32_t(0), 0));
  ASSERT_EQ(uint32_t(6), saturate_mul(uint32_t(6), 1));
  ASSERT_EQ(uint32_t(6), saturate_mul(uint32_t(3), 2));

  ASSERT_EQ(int64_t(0), saturate_mul(int64_t(INT64_MAX), 0));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX-5), 4));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MAX-5), -5));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX-95), 6));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(-INT64_MAX+5), 7));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX-5), 5.1));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX-5), 6.1));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX-5), 7.1));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MAX/2), INT64_MAX/2));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MAX-10), -INT64_MAX));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MIN), 1));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MIN), 2));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MIN), -1));
  ASSERT_EQ(INT64_MAX, saturate_mul(int64_t(INT64_MIN), INT64_MIN));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MIN), INT64_MAX));
  ASSERT_EQ(INT64_MIN, saturate_mul(int64_t(INT64_MIN), 1_u64));

  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(UINT64_MAX), 0));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX-5), 4));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(UINT64_MAX-5), -5));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX-5), 6));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(UINT64_MAX-5), -2.7));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX), 1.1));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX-5), 5.1));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX-5), 6.1));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX-5), 7.1));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX/2), UINT64_MAX/2));
  ASSERT_EQ(UINT64_MAX, saturate_mul(uint64_t(UINT64_MAX-10), UINT64_MAX-10));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(0), -10));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(0), -1));
  ASSERT_EQ(uint64_t(0), saturate_mul(uint64_t(0), 0));
  ASSERT_EQ(uint64_t(6), saturate_mul(uint64_t(6), 1));
  ASSERT_EQ(uint64_t(6), saturate_mul(uint64_t(3), 2));

  std::vector<uint8_t> datau8 = readSaturateTestData<uint8_t>(getTestDataDir().filePath("uint8_mul_test.dat"));
  for (size_t i=0; i<datau8.size(); i+=3) {
    ASSERT_EQ(datau8[i+2], saturate_mul(datau8[i], datau8[i+1])) << (int)datau8[i] << " " << (int)datau8[i+1];
  }
  std::vector<uint16_t> datau16 = readSaturateTestData<uint16_t>(getTestDataDir().filePath("uint16_mul_test.dat"));
  for (size_t i=0; i<datau16.size(); i+=3) {
    ASSERT_EQ(datau16[i+2], saturate_mul(datau16[i], datau16[i+1])) << (int)datau16[i] << " " << (int)datau16[i+1];
  }
  std::vector<uint32_t> datau32 = readSaturateTestData<uint32_t>(getTestDataDir().filePath("uint32_mul_test.dat"));
  for (size_t i=0; i<datau32.size(); i+=3) {
    ASSERT_EQ(datau32[i+2], saturate_mul(datau32[i], datau32[i+1])) << datau32[i] << " " << datau32[i+1];
  }
  std::vector<uint64_t> datau64 = readSaturateTestData<uint64_t>(getTestDataDir().filePath("uint64_mul_test.dat"));
  for (size_t i=0; i<datau64.size(); i+=3) {
    ASSERT_EQ(datau64[i+2], saturate_mul(datau64[i], datau64[i+1])) << datau64[i] << " " << datau64[i+1];
  }
  std::vector<int8_t> datai8 = readSaturateTestData<int8_t>(getTestDataDir().filePath("int8_mul_test.dat"));
  for (size_t i=0; i<datai8.size(); i+=3) {
    ASSERT_EQ(datai8[i+2], saturate_mul(datai8[i], datai8[i+1])) << (int)datai8[i] << " " << (int)datai8[i+1];
  }
  std::vector<int16_t> datai16 = readSaturateTestData<int16_t>(getTestDataDir().filePath("int16_mul_test.dat"));
  for (size_t i=0; i<datai16.size(); i+=3) {
    ASSERT_EQ(datai16[i+2], saturate_mul(datai16[i], datai16[i+1])) << (int)datai16[i] << " " << (int)datai16[i+1];
  }
  std::vector<int32_t> datai32 = readSaturateTestData<int32_t>(getTestDataDir().filePath("int32_mul_test.dat"));
  for (size_t i=0; i<datai32.size(); i+=3) {
    ASSERT_EQ(datai32[i+2], saturate_mul(datai32[i], datai32[i+1])) << datai32[i] << " " << datai32[i+1];
  }
  std::vector<int64_t> datai64 = readSaturateTestData<int64_t>(getTestDataDir().filePath("int64_mul_test.dat"));
  for (size_t i=0; i<datai64.size(); i+=3) {
    ASSERT_EQ(datai64[i+2], saturate_mul(datai64[i], datai64[i+1])) << datai64[i] << " " << datai64[i+1];
  }
}

TEST(saturate, div)
{
  using namespace nim;

  // matlab div do rounding
  std::vector<uint8_t> datau8 = readSaturateTestData<uint8_t>(getTestDataDir().filePath("uint8_div_test.dat"));
  for (size_t i=0; i<datau8.size(); i+=3) {
    ASSERT_NEAR(datau8[i+2], saturate_div(datau8[i], datau8[i+1]), 1) << (int)datau8[i] << " " << (int)datau8[i+1];
  }
  std::vector<uint16_t> datau16 = readSaturateTestData<uint16_t>(getTestDataDir().filePath("uint16_div_test.dat"));
  for (size_t i=0; i<datau16.size(); i+=3) {
    ASSERT_NEAR(datau16[i+2], saturate_div(datau16[i], datau16[i+1]), 1) << (int)datau16[i] << " " << (int)datau16[i+1];
  }
  std::vector<uint32_t> datau32 = readSaturateTestData<uint32_t>(getTestDataDir().filePath("uint32_div_test.dat"));
  for (size_t i=0; i<datau32.size(); i+=3) {
    ASSERT_NEAR(datau32[i+2], saturate_div(datau32[i], datau32[i+1]), 1) << datau32[i] << " " << datau32[i+1];
  }
  std::vector<uint64_t> datau64 = readSaturateTestData<uint64_t>(getTestDataDir().filePath("uint64_div_test.dat"));
  for (size_t i=0; i<datau64.size(); i+=3) {
    ASSERT_NEAR(datau64[i+2], saturate_div(datau64[i], datau64[i+1]), 1) << datau64[i] << " " << datau64[i+1];
  }
  std::vector<int8_t> datai8 = readSaturateTestData<int8_t>(getTestDataDir().filePath("int8_div_test.dat"));
  for (size_t i=0; i<datai8.size(); i+=3) {
    ASSERT_NEAR(datai8[i+2], saturate_div(datai8[i], datai8[i+1]), 1) << (int)datai8[i] << " " << (int)datai8[i+1];
  }
  std::vector<int16_t> datai16 = readSaturateTestData<int16_t>(getTestDataDir().filePath("int16_div_test.dat"));
  for (size_t i=0; i<datai16.size(); i+=3) {
    ASSERT_NEAR(datai16[i+2], saturate_div(datai16[i], datai16[i+1]), 1) << (int)datai16[i] << " " << (int)datai16[i+1];
  }
  std::vector<int32_t> datai32 = readSaturateTestData<int32_t>(getTestDataDir().filePath("int32_div_test.dat"));
  for (size_t i=0; i<datai32.size(); i+=3) {
    ASSERT_NEAR(datai32[i+2], saturate_div(datai32[i], datai32[i+1]), 1) << datai32[i] << " " << datai32[i+1];
  }
  std::vector<int64_t> datai64 = readSaturateTestData<int64_t>(getTestDataDir().filePath("int64_div_test.dat"));
  for (size_t i=0; i<datai64.size(); i+=3) {
    ASSERT_NEAR(datai64[i+2], saturate_div(datai64[i], datai64[i+1]), 1) << datai64[i] << " " << datai64[i+1];
  }

  ASSERT_EQ(INT64_MIN, saturate_div(int64_t(INT64_MIN), 1_u64));
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
