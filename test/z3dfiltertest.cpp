#include "z3dfilter.h"

#include <gtest/gtest.h>

namespace nim {
namespace {

class TestFilter final : public Z3DFilter
{
public:
  void markAllOutputsValid()
  {
    setValid(MonoEye);
    setValid(LeftEye);
    setValid(RightEye);
  }

private:
  double process(Z3DEye) override
  {
    return 1.0;
  }
};

TEST(Z3DFilterTest, LogicalChangesRemainObservableWhilePhysicalOutputIsInvalid)
{
  TestFilter filter;
  int logicalChangeCount = 0;
  int physicalInvalidationCount = 0;
  QObject::connect(&filter, &Z3DFilter::renderInputChanged, [&logicalChangeCount]() {
    ++logicalChangeCount;
  });
  QObject::connect(&filter, &Z3DFilter::invalidated, [&physicalInvalidationCount]() {
    ++physicalInvalidationCount;
  });

  // Filters start physically invalid. Repeated logical changes must remain
  // visible without creating a false physical-validity edge.
  filter.invalidateResult();
  filter.invalidateResult();
  EXPECT_EQ(logicalChangeCount, 2);
  EXPECT_EQ(physicalInvalidationCount, 0);

  filter.markAllOutputsValid();
  filter.invalidateResult();
  EXPECT_EQ(logicalChangeCount, 3);
  EXPECT_EQ(physicalInvalidationCount, 1);

  // Once invalid again, only the unconditional logical signal repeats.
  filter.invalidateResult();
  EXPECT_EQ(logicalChangeCount, 4);
  EXPECT_EQ(physicalInvalidationCount, 1);
}

} // namespace
} // namespace nim
