#pragma once

#include <QString>
#include <QDir>

namespace nim {

class ZSystemInfo
{
public:
  static ZSystemInfo& instance();

  ZSystemInfo();

  void logOSInfo() const;

  bool is3DSupported() const
  { return m_glInitialized; }

  bool isStereoViewSupported() const
  { return m_stereoViewSupported; }

  void setStereoSupported(bool v)
  { m_stereoViewSupported = v; }

  // return false if failed
  virtual bool initializeGL();

  QString errorMessage() const
  { return m_errorMsg; }

  QString shaderPath(const QString& filename = "") const;

  QString fontPath(const QString& filename = "") const;

  // return empty if can not find enough space
  QString imgCachePath(size_t requiredSpaceInBytes) const;

  QDir logDir() const;

  QString lastOpenedObjPath(const QString& typeName) const;

  void setLastOpenedObjPath(const QString& typeName, const QString& path) const;

  int maxNumRecentFiles() const
  { return 20; }

  void addFileToRecentFileList(const QString& fileName) const;

  QString lastOpenedImagePath() const
  { return lastOpenedObjPath("Image"); }

  void setLastOpenedImagePath(const QString& path) const
  { setLastOpenedObjPath("Image", path); }

protected:
  void updateRecentFiles() const;

private:
  void detectOS();

  QDir createLogDir() const;

protected:
  QString m_osString;

  QString m_errorMsg;

  QString m_fontPath;
  QString m_shaderPath;

  bool m_glInitialized;

  bool m_stereoViewSupported;
};

} // namespace nim

