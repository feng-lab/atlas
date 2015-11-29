#include "zunittest.h"

#include <QDir>
#ifdef _WIN32
#define GET_TEST_DATA_DIR QDir("Z:\\Google Drive\\atlas_test_data")
#else
#define GET_TEST_DATA_DIR QDir("/Users/feng/Google Drive/atlas_test_data")
#endif

#include "test/zimggraphtest.h"
#include "test/zfilereadtest.h"
#include "test/zimgiteratortest.h"
#include "test/zsaturateoperationtest.h"
#include "test/zclustertest.h"
#include "test/zimgtest.h"
#include "test/zimgconnectedcomponentstest.h"
#include "test/zimgregionalextrematest.h"
#include "test/zimgautothresholdtest.h"
#include "test/zimgsigneddistancemaptest.h"
#include "test/zimagetoimagemetrictest.h"
#include "test/zimageutilstest.h"
#include "test/zimageaffinetransformtest.h"
#include "test/zimgncctest.h"

namespace nim {

ZUnitTest::ZUnitTest()
{
}

int ZUnitTest::run()
{
  char arg0[] = "atlas_test";
  char* argv[] = { &arg0[0], nullptr };
  int argc = (int)std::extent<decltype(argv)>::value - 1;

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

} // namespace nim
