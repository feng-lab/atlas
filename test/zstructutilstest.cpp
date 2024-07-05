#include "zstructutils.h"
#include "ztest.h"

struct MySubStruct
{
  char ch;
  int16_t a;
  int32_t b;
  float c;
  double d;

  bool operator==(const MySubStruct& other) const
  {
    return ch == other.ch && a == other.a && b == other.b && c == other.c && d == other.d;
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
  using namespace nim;

  ASSERT_EQ(compactSize<MySubStruct>(), 19);
  ASSERT_EQ(compactSize<MyStruct>(), 37);

  uint8_t buffer[256];
  MyStruct original = {
    'a',
    {'b', -98, 32, 4.f, 5.},
    1,
    'c',
    2.0f,
    3.0
  };
  auto memSize = compactStructToMemory(buffer, sizeof(buffer), original);
  ASSERT_EQ(memSize, 37);

  MyStruct restored;
  readStructFromCompactMemory(restored, buffer, sizeof(buffer));

  ASSERT_EQ(original, restored);
}
