#include "zcommandlineflags.h"
#include "zimginit.h"
#include "ztest.h"
#include <gtest/gtest.h>

#include <QString>

namespace {

class AtlasImgRuntimeEnvironment final : public ::testing::Environment
{
public:
  void SetUp() override
  {
    (void)nim::ZImgInit::instance(QString(),
                                  nim::atlasTestJreDir().absolutePath(),
                                  nim::atlasTestJarsDir().absolutePath(),
                                  false);
  }

  void TearDown() override
  {
    nim::ZImgInit::shutdown();
  }
};

} // namespace

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  nim::parseCommandLine(argc, argv);
  ::testing::AddGlobalTestEnvironment(new AtlasImgRuntimeEnvironment);
  return RUN_ALL_TESTS();
}
