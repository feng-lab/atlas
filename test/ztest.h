#pragma once

#include "zglobal.h"
#include "zlog.h"
#include "zbenchtimer.h"
#include "zrandom.h"
#include "zcpuinfo.h"
#include <QDir>
#include <QString>
#if __has_include(<gtest/gtest.h>)
#include <gtest/gtest.h>
#endif

#ifndef ATLAS_THIRDPARTY_BUILD_DIR
#error "ATLAS_THIRDPARTY_BUILD_DIR must be defined for Atlas img tests"
#endif

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

inline QDir atlasTestThirdPartyBuildDir()
{
  return QDir(QStringLiteral(ATLAS_THIRDPARTY_BUILD_DIR));
}

inline QString atlasTestJavaExecutableName()
{
#ifdef _WIN32
  return QStringLiteral("java.exe");
#else
  return QStringLiteral("java");
#endif
}

inline QDir atlasTestJreDir()
{
  const QString jreName = ZCpuInfo::instance().isX86_64 ? QStringLiteral("jre") : QStringLiteral("jre-arm");
  QDir jreDir(atlasTestThirdPartyBuildDir().filePath(jreName));
#ifdef __APPLE__
  jreDir = QDir(jreDir.filePath(QStringLiteral("Contents/Home")));
#endif
  return jreDir;
}

inline QString atlasTestJavaExecutablePath()
{
  return atlasTestJreDir().filePath(QStringLiteral("bin/") + atlasTestJavaExecutableName());
}

inline QDir atlasTestJarsDir()
{
  return QDir(atlasTestThirdPartyBuildDir().filePath(QStringLiteral("jars")));
}

inline QString atlasTestBioFormatsBridgeJarPath()
{
  return atlasTestJarsDir().filePath(QStringLiteral("atlas-bioformats-bridge.jar"));
}

inline QString atlasTestBioFormatsJarPath()
{
  return atlasTestJarsDir().filePath(QStringLiteral("bioformats_package.jar"));
}

} // namespace nim
