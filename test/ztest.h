#pragma once

#include "zglobal.h"
#include "zlog.h"
#include "zbenchtimer.h"
#include "zrandom.h"
#include <QDir>
#include <gtest/gtest.h>

namespace nim {

inline QDir getTestDataDir()
{
#ifdef _WIN32
  auto res = QDir(ATLAS_TEST_DATA_DIR);
  if (!res.exists()) {
    res = QDir(QDir::homePath() + "\\Dropbox\\code\\my\\proxy\\static\\atlas_test_data");
  }
  return res;
#elif defined(__APPLE__)
  auto res = QDir(ATLAS_TEST_DATA_DIR);
  if (!res.exists()) {
    res = QDir(QDir::homePath() + "/Dropbox/code/my/proxy/static/atlas_test_data");
  }
  return res;
#else
  QDir res(ATLAS_TEST_DATA_DIR);
  return res;
#endif
}

} // namespace nim
