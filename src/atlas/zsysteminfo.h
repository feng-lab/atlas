#pragma once

#include <QDir>

namespace nim {

class ZSystemInfo
{
public:
  static ZSystemInfo& instance();

  ZSystemInfo();

  void logOSInfo() const;

  [[nodiscard]] QString shaderPath(const QString& filename = "") const;

  [[nodiscard]] QString fontPath(const QString& filename = "") const;

  static QDir resourcesDir();

  static QString resourcesDirPath();

  static QString jdkDirPath();

  static QString jarsDirPath();

  static QString applicationInstallDirPath();

  // return empty if can not find enough space
  static QString imgCachePath(size_t requiredSpaceInBytes);

  [[nodiscard]] static QDir logDir();

  [[nodiscard]] static QDir configDir();

  [[nodiscard]] QString lastOpenedObjPathQSettingLocation(const QString& typeName) const
  {
    return QString("%1/lastOpenedPath").arg(typeName);
  }

  [[nodiscard]] QString lastOpenedObjPath(const QString& typeName) const;

  void setLastOpenedObjPath(const QString& typeName, const QString& path) const;

  [[nodiscard]] int maxNumRecentFiles() const
  {
    return 20;
  }

  void addFileToRecentFileList(const QString& fileName) const;

  [[nodiscard]] QString lastOpenedImagePath() const
  {
    return lastOpenedObjPath("Image");
  }

  void setLastOpenedImagePath(const QString& path) const
  {
    setLastOpenedObjPath("Image", path);
  }

  static void removeOldLogs(int numberToKeep = 20);

protected:
  static void updateRecentFiles();

private:
  void detectOS();

  static QDir createLogDir();

protected:
  QString m_osString;

  QString m_fontPath;
  QString m_shaderPath;
};

} // namespace nim
