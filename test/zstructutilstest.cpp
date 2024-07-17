#include "zstructutils.h"
#include "ztest.h"
#include <fmt/ostream.h>

namespace nim {

enum class E : int16_t
{
  unknown = 0,
  A = 1,
  B = 2,
};

} // namespace nim

using namespace nim;

struct foobar
{
  int8_t a;
  E b;

  bool operator==(const foobar& other) const
  {
    return a == other.a && b == other.b;
  }
};

struct MySubStruct
{
  char ch;
  foobar fb;
  int16_t a;
  int32_t b;
  float c;
  char ch2;
  double d;

  bool operator==(const MySubStruct& other) const
  {
    return ch == other.ch && fb == other.fb && a == other.a && b == other.b && c == other.c && ch2 == other.ch2 &&
           d == other.d;
  }
};

struct MyStruct
{
  char ch;
  MySubStruct sub;
  int32_t a;
  char ch2;
  float b;
  double c;

  bool operator==(const MyStruct& other) const
  {
    return ch == other.ch && sub == other.sub && a == other.a && ch2 == other.ch2 && b == other.b && c == other.c;
  }
};

TEST(CompactStructTest, WriteAndReadStruct)
{
  fmt::print("size of foobar: {}\n", sizeof(foobar));
  fmt::print("size of MySubStruct: {}\n", sizeof(MySubStruct));
  fmt::print("size of MyStruct: {}\n", sizeof(MyStruct));

  ASSERT_EQ(compactSize<MySubStruct>(), 23);
  ASSERT_EQ(compactSize<MyStruct>(), 41);

  uint8_t buffer[256];

  MySubStruct subS = {
    'e',
    {127, E::B},
    -8,
    2,
    41.f,
    '2',
    52.
  };
  printStruct(subS);
  auto memSize = compactStructToMemory(buffer, sizeof(buffer), subS);
  ASSERT_EQ(memSize, 23);
  MySubStruct restoredSubS;
  readStructFromCompactMemory(restoredSubS, buffer, sizeof(buffer));
  ASSERT_EQ(subS, restoredSubS);

  MyStruct original = {
    'a',
    {'b', {-128, E::unknown}, -98, 32, 4.f, 't', 5.},
    1,
    'c',
    2.0f,
    3.0
  };
  fmt::print("\n");
  printStruct(original);

  memSize = compactStructToMemory(buffer, sizeof(buffer), original);
  ASSERT_EQ(memSize, 41);
  MyStruct restored;
  readStructFromCompactMemory(restored, buffer, sizeof(buffer));
  ASSERT_EQ(original, restored);
}
