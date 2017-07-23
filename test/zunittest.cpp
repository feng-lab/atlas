#include "zunittest.h"

#include <QDir>

#ifdef _WIN32
#define GET_TEST_DATA_DIR QDir("Z:\\Google Drive\\atlas_test_data")
#else
#define GET_TEST_DATA_DIR QDir(QDir::homePath() + "/Google Drive/atlas_test_data")
#endif

#include "zfilereadtest.h"
#include "zimggraphtest.h"
#include "zimgiteratortest.h"
#include "zsaturateoperationtest.h"
#include "zclustertest.h"
#include "zimgtest.h"
#include "zimgconnectedcomponentstest.h"
#include "zimgregionalextrematest.h"
#include "zimgautothresholdtest.h"
#include "zimgsigneddistancemaptest.h"
#include "zimagetoimagemetrictest.h"
#include "zimageutilstest.h"
#include "zimageaffinetransformtest.h"
#include "zimgncctest.h"
#include "ztreetest.h"

namespace nim {

int ZUnitTest::run()
{
  char arg0[] = "Atlas_test";
  char* argv[] = {&arg0[0], nullptr};
  int argc = std::extent<decltype(argv)>::value - 1;

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

} // namespace nim
