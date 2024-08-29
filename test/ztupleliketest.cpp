#include "zimginterface.h"
#include "zvoxelcoordinate.h"
#include "zglmutils.h"
#include "ztest.h"

using namespace nim;

class Col4Test : public ::testing::Test
{
protected:
  col4 testColor{10, 20, 30, 40};
};

// Test structure binding
TEST_F(Col4Test, StructureBinding)
{
  auto [r, g, b, a] = testColor;
  EXPECT_EQ(r, 10);
  EXPECT_EQ(g, 20);
  EXPECT_EQ(b, 30);
  EXPECT_EQ(a, 40);
}

// Test constness
TEST_F(Col4Test, Constness)
{
  const col4& constRef = testColor;
  EXPECT_EQ(get<0>(constRef), 10);
  EXPECT_EQ(get<1>(constRef), 20);
  EXPECT_EQ(get<2>(constRef), 30);
  EXPECT_EQ(get<3>(constRef), 40);

  // This should not compile (uncomment to test):
  // std::get<0>(constRef) = 100;
  static_assert(std::is_const_v<std::remove_reference_t<decltype(get<0>(constRef))>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(get<1>(constRef))>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(get<2>(constRef))>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(get<3>(constRef))>>);
}

// Test reference binding
TEST_F(Col4Test, ReferenceBinding)
{
  auto& [r, g, b, a] = testColor;
  r = 100;
  EXPECT_EQ(testColor.r, 100);
}

// Test rvalue scenarios
TEST_F(Col4Test, RValueScenarios)
{
  // Using std::move to create an rvalue
  auto [r, g, b, a] = std::move(testColor);
  EXPECT_EQ(r, 10);
  EXPECT_EQ(g, 20);
  EXPECT_EQ(b, 30);
  EXPECT_EQ(a, 40);

  // Testing get<> with rvalue
  EXPECT_EQ(get<0>(col4{50, 60, 70, 80}), 50);
  EXPECT_EQ(get<1>(col4{50, 60, 70, 80}), 60);
  EXPECT_EQ(get<2>(col4{50, 60, 70, 80}), 70);
  EXPECT_EQ(get<3>(col4{50, 60, 70, 80}), 80);
}

// Test tuple_size
TEST_F(Col4Test, TupleSize)
{
  EXPECT_EQ(std::tuple_size<col4>::value, 4);
}

// Test tuple_element
TEST_F(Col4Test, TupleElement)
{
  bool isSameType = std::is_same<std::tuple_element<0, col4>::type, uint8_t>::value;
  EXPECT_TRUE(isSameType);
}

// Test get<> function
TEST_F(Col4Test, GetFunction)
{
  EXPECT_EQ(get<0>(testColor), 10);
  EXPECT_EQ(get<1>(testColor), 20);
  EXPECT_EQ(get<2>(testColor), 30);
  EXPECT_EQ(get<3>(testColor), 40);
}

// Test modifying through get<>
TEST_F(Col4Test, ModifyThroughGet)
{
  get<0>(testColor) = 100;
  EXPECT_EQ(testColor.r, 100);
}

TEST(Col4Test1, StructureBinding)
{
  col4 c{1, 2, 3, 4};
  auto [r, g, b, a] = c;
  EXPECT_EQ(r, 1);
  EXPECT_EQ(g, 2);
  EXPECT_EQ(b, 3);
  EXPECT_EQ(a, 4);
}

TEST(Col4Test1, Constness)
{
  const col4 c{1, 2, 3, 4};
  auto& [r, g, b, a] = c;
  EXPECT_EQ(r, 1);
  EXPECT_EQ(g, 2);
  EXPECT_EQ(b, 3);
  EXPECT_EQ(a, 4);

  // Ensure we can't modify the values
  static_assert(std::is_const_v<std::remove_reference_t<decltype(r)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(g)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(b)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(a)>>);
}

TEST(Col4Test1, ReferenceBinding)
{
  col4 c{1, 2, 3, 4};
  auto& [r, g, b, a] = c;

  // Modify through reference
  r = 5;
  g = 6;
  b = 7;
  a = 8;

  EXPECT_EQ(c.r, 5);
  EXPECT_EQ(c.g, 6);
  EXPECT_EQ(c.b, 7);
  EXPECT_EQ(c.a, 8);
}

TEST(Col4Test1, RValueBinding)
{
  auto [r, g, b, a] = col4{1, 2, 3, 4};
  EXPECT_EQ(r, 1);
  EXPECT_EQ(g, 2);
  EXPECT_EQ(b, 3);
  EXPECT_EQ(a, 4);
}

TEST(Col4Test1, RValueConstBinding)
{
  const auto [r, g, b, a] = col4{1, 2, 3, 4};
  EXPECT_EQ(r, 1);
  EXPECT_EQ(g, 2);
  EXPECT_EQ(b, 3);
  EXPECT_EQ(a, 4);

  static_assert(std::is_const_v<std::remove_reference_t<decltype(r)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(g)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(b)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(a)>>);
}

TEST(Col4Test1, TupleSize)
{
  EXPECT_EQ(std::tuple_size<col4>::value, 4);
}

TEST(Col4Test1, TupleElement)
{
  static_assert(std::is_same_v<std::tuple_element_t<0, col4>, uint8_t>);
  static_assert(std::is_same_v<std::tuple_element_t<1, col4>, uint8_t>);
  static_assert(std::is_same_v<std::tuple_element_t<2, col4>, uint8_t>);
  static_assert(std::is_same_v<std::tuple_element_t<3, col4>, uint8_t>);
}

TEST(Col4Test1, GetFunction)
{
  col4 c{1, 2, 3, 4};
  EXPECT_EQ(get<0>(c), 1);
  EXPECT_EQ(get<1>(c), 2);
  EXPECT_EQ(get<2>(c), 3);
  EXPECT_EQ(get<3>(c), 4);
}

TEST(Col4Test1, GetFunctionConst)
{
  const col4 c{1, 2, 3, 4};
  EXPECT_EQ(get<0>(c), 1);
  EXPECT_EQ(get<1>(c), 2);
  EXPECT_EQ(get<2>(c), 3);
  EXPECT_EQ(get<3>(c), 4);
}

TEST(Col4Test1, GetFunctionRValue)
{
  EXPECT_EQ(get<0>(col4{1, 2, 3, 4}), 1);
  EXPECT_EQ(get<1>(col4{1, 2, 3, 4}), 2);
  EXPECT_EQ(get<2>(col4{1, 2, 3, 4}), 3);
  EXPECT_EQ(get<3>(col4{1, 2, 3, 4}), 4);
}

class ZVoxelCoordinateTest : public ::testing::Test
{
protected:
  ZVoxelCoordinate testCoord{1, 2, 3, 4, 5};
};

// Test structure binding
TEST_F(ZVoxelCoordinateTest, StructureBinding)
{
  auto [x, y, z, c, t] = testCoord;
  EXPECT_EQ(x, 1);
  EXPECT_EQ(y, 2);
  EXPECT_EQ(z, 3);
  EXPECT_EQ(c, 4);
  EXPECT_EQ(t, 5);
}

// Test constness
TEST_F(ZVoxelCoordinateTest, Constness)
{
  const ZVoxelCoordinate& constRef = testCoord;
  auto& [x, y, z, c, t] = constRef;
  EXPECT_EQ(x, 1);
  EXPECT_EQ(y, 2);
  EXPECT_EQ(z, 3);
  EXPECT_EQ(c, 4);
  EXPECT_EQ(t, 5);

  static_assert(std::is_const_v<std::remove_reference_t<decltype(x)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(y)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(z)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(c)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(t)>>);

  // This should not compile (uncomment to test):
  // x = 10;
}

// Test reference binding
TEST_F(ZVoxelCoordinateTest, ReferenceBinding)
{
  auto& [x, y, z, c, t] = testCoord;
  x = 10;
  EXPECT_EQ(testCoord.x, 10);
}

// Test rvalue scenarios
TEST_F(ZVoxelCoordinateTest, RValueBinding)
{
  auto [x, y, z, c, t] = ZVoxelCoordinate{10, 20, 30, 40, 50};
  EXPECT_EQ(x, 10);
  EXPECT_EQ(y, 20);
  EXPECT_EQ(z, 30);
  EXPECT_EQ(c, 40);
  EXPECT_EQ(t, 50);
}

TEST_F(ZVoxelCoordinateTest, RValueConstBinding)
{
  const auto [x, y, z, c, t] = ZVoxelCoordinate{10, 20, 30, 40, 50};
  EXPECT_EQ(x, 10);
  EXPECT_EQ(y, 20);
  EXPECT_EQ(z, 30);
  EXPECT_EQ(c, 40);
  EXPECT_EQ(t, 50);

  static_assert(std::is_const_v<std::remove_reference_t<decltype(x)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(y)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(z)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(c)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(t)>>);
}

// Test tuple_size
TEST_F(ZVoxelCoordinateTest, TupleSize)
{
  EXPECT_EQ(std::tuple_size<ZVoxelCoordinate>::value, 5);
}

// Test tuple_element
TEST_F(ZVoxelCoordinateTest, TupleElement)
{
  static_assert(std::is_same_v<std::tuple_element_t<0, ZVoxelCoordinate>, ptrdiff_t>);
  static_assert(std::is_same_v<std::tuple_element_t<1, ZVoxelCoordinate>, ptrdiff_t>);
  static_assert(std::is_same_v<std::tuple_element_t<2, ZVoxelCoordinate>, ptrdiff_t>);
  static_assert(std::is_same_v<std::tuple_element_t<3, ZVoxelCoordinate>, ptrdiff_t>);
  static_assert(std::is_same_v<std::tuple_element_t<4, ZVoxelCoordinate>, ptrdiff_t>);
}

// Test get<> function
TEST_F(ZVoxelCoordinateTest, GetFunction)
{
  EXPECT_EQ(get<0>(testCoord), 1);
  EXPECT_EQ(get<1>(testCoord), 2);
  EXPECT_EQ(get<2>(testCoord), 3);
  EXPECT_EQ(get<3>(testCoord), 4);
  EXPECT_EQ(get<4>(testCoord), 5);
}

// Test get<> function with const
TEST_F(ZVoxelCoordinateTest, GetFunctionConst)
{
  const ZVoxelCoordinate& constRef = testCoord;
  EXPECT_EQ(get<0>(constRef), 1);
  EXPECT_EQ(get<1>(constRef), 2);
  EXPECT_EQ(get<2>(constRef), 3);
  EXPECT_EQ(get<3>(constRef), 4);
  EXPECT_EQ(get<4>(constRef), 5);
}

// Test get<> function with rvalue
TEST_F(ZVoxelCoordinateTest, GetFunctionRValue)
{
  EXPECT_EQ(get<0>(ZVoxelCoordinate{10, 20, 30, 40, 50}), 10);
  EXPECT_EQ(get<1>(ZVoxelCoordinate{10, 20, 30, 40, 50}), 20);
  EXPECT_EQ(get<2>(ZVoxelCoordinate{10, 20, 30, 40, 50}), 30);
  EXPECT_EQ(get<3>(ZVoxelCoordinate{10, 20, 30, 40, 50}), 40);
  EXPECT_EQ(get<4>(ZVoxelCoordinate{10, 20, 30, 40, 50}), 50);
}

// Test modifying through get<>
TEST_F(ZVoxelCoordinateTest, ModifyThroughGet)
{
  get<0>(testCoord) = 100;
  EXPECT_EQ(testCoord.x, 100);
}

class GlmVec4Test : public ::testing::Test
{
protected:
  glm::vec4 testVector{1.0f, 2.0f, 3.0f, 4.0f};
};

// Test structure binding
TEST_F(GlmVec4Test, StructureBinding)
{
  auto [x, y, z, w] = testVector;
  EXPECT_FLOAT_EQ(x, 1.0f);
  EXPECT_FLOAT_EQ(y, 2.0f);
  EXPECT_FLOAT_EQ(z, 3.0f);
  EXPECT_FLOAT_EQ(w, 4.0f);
}

// Test constness
TEST_F(GlmVec4Test, Constness)
{
  const glm::vec4 v{1.0f, 2.0f, 3.0f, 4.0f};
  auto& [x, y, z, w] = v;
  EXPECT_FLOAT_EQ(x, 1.0f);
  EXPECT_FLOAT_EQ(y, 2.0f);
  EXPECT_FLOAT_EQ(z, 3.0f);
  EXPECT_FLOAT_EQ(w, 4.0f);

  // Ensure we can't modify the values
  static_assert(std::is_const_v<std::remove_reference_t<decltype(x)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(y)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(z)>>);
  static_assert(std::is_const_v<std::remove_reference_t<decltype(w)>>);

  // This should not compile (uncomment to test):
  // x = 100.0f;
}

// Test reference binding
TEST_F(GlmVec4Test, ReferenceBinding)
{
  auto& [x, y, z, w] = testVector;
  x = 10.0f;
  EXPECT_FLOAT_EQ(testVector.x, 10.0f);
}

// Test rvalue scenarios
TEST_F(GlmVec4Test, RValueScenarios)
{
  // Using std::move to create an rvalue
  auto [x, y, z, w] = std::move(testVector);
  EXPECT_FLOAT_EQ(x, 1.0f);
  EXPECT_FLOAT_EQ(y, 2.0f);
  EXPECT_FLOAT_EQ(z, 3.0f);
  EXPECT_FLOAT_EQ(w, 4.0f);

  // Testing get<> with rvalue
  EXPECT_FLOAT_EQ(get<0>(glm::vec4{5.0f, 6.0f, 7.0f, 8.0f}), 5.0f);
  EXPECT_FLOAT_EQ(get<1>(glm::vec4{5.0f, 6.0f, 7.0f, 8.0f}), 6.0f);
  EXPECT_FLOAT_EQ(get<2>(glm::vec4{5.0f, 6.0f, 7.0f, 8.0f}), 7.0f);
  EXPECT_FLOAT_EQ(get<3>(glm::vec4{5.0f, 6.0f, 7.0f, 8.0f}), 8.0f);
}

// Test tuple_size
TEST_F(GlmVec4Test, TupleSize)
{
  EXPECT_EQ(std::tuple_size<glm::vec4>::value, 4);
}

// Test tuple_element
TEST_F(GlmVec4Test, TupleElement)
{
  bool isSameType = std::is_same<std::tuple_element<0, glm::vec4>::type, float>::value;
  EXPECT_TRUE(isSameType);
}

// Test get<> function
TEST_F(GlmVec4Test, GetFunction)
{
  EXPECT_FLOAT_EQ(get<0>(testVector), 1.0f);
  EXPECT_FLOAT_EQ(get<1>(testVector), 2.0f);
  EXPECT_FLOAT_EQ(get<2>(testVector), 3.0f);
  EXPECT_FLOAT_EQ(get<3>(testVector), 4.0f);
}

// Test modifying through get<>
TEST_F(GlmVec4Test, ModifyThroughGet)
{
  get<0>(testVector) = 10.0f;
  EXPECT_FLOAT_EQ(testVector.x, 10.0f);
}