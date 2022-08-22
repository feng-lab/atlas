#pragma once

#include <QDir>

namespace nim {

class ZSystemInfo
{
public:
  static ZSystemInfo& instance();

  ZSystemInfo();

  void logOSInfo() const;

  [[nodiscard]] bool is3DSupported() const
  {
    return m_glInitialized;
  }

  [[nodiscard]] bool isStereoViewSupported() const
  {
    return m_stereoViewSupported;
  }

  void setStereoSupported(bool v)
  {
    m_stereoViewSupported = v;
  }

  // return false if failed
  virtual bool initializeGL();

  [[nodiscard]] QString errorMessage() const
  {
    return m_errorMsg;
  }

  [[nodiscard]] QString shaderPath(const QString& filename = "") const;

  [[nodiscard]] QString fontPath(const QString& filename = "") const;

  // return empty if can not find enough space
  static QString imgCachePath(size_t requiredSpaceInBytes);

  [[nodiscard]] static QDir logDir();

  [[nodiscard]] static QDir configDir();

  [[nodiscard]] static QDir resourceDir();

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

  QString m_errorMsg;

  QString m_fontPath;
  QString m_shaderPath;

  bool m_glInitialized = false;

  bool m_stereoViewSupported = false;
};

} // namespace nim
