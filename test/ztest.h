#pragma once

#include "zglobal.h"
#include "zlog.h"
#include "zbenchtimer.h"
#include <QDir>
#include <gtest/gtest.h>

namespace nim {

inline QDir getTestDataDir()
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

} // namespace nim
