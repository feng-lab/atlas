#include "zunittest.h"

#include <QDir>

namespace nim {

QDir getTestDataDir()
{
#ifdef _WIN32
  QDir res("Z:\\Google Drive\\code\\my\\atlas_test_data");
  if (!res.exists()) {
    res = QDir(QDir::homePath() + "\\atlas_test_data");
  }
  if (!res.exists()) {
    res = QDir(QDir::homePath() + "\\GoogleDrive\\code\\my\\atlas_test_data");
  }
  if (!res.exists()) {
    res = QDir(QDir::homePath() + "\\Google Drive\\code\\my\\atlas_test_data");
  }
  if (!res.exists()) {
    res = QDir(QDir::homePath() + "\\Google Drive\\My Drive\\code\\my\\atlas_test_data");
  }
  return res;
#elif defined(__APPLE__)
  QDir res(QDir::homePath() + "/Google Drive/code/my/atlas_test_data");
  if (!res.exists()) {
    res = QDir(QDir::homePath() + "/Google Drive/My Drive/code/my/atlas_test_data");
  }
  if (!res.exists()) {
    res = QDir(QDir::homePath() + "/atlas_test_data");
  }
  return res;
#else
  QDir res(QDir::homePath() + "/atlas_test_data");
  return res;
#endif
}

} // namespace

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
