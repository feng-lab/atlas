#include "zfileutils.h"

#include <QProcess>
#include <QFileInfo>
#include <QDir>

namespace nim {

ZFileUtils::ZFileUtils() = default;

ZFileUtils::~ZFileUtils() = default;

void ZFileUtils::showInGraphicalShell(const QString& filePath)
{
  if (!QFileInfo(filePath).exists())
    return;

#ifdef Q_OS_MAC
  QStringList args;
  args << "-e";
  args << "tell application \"Finder\"";
  args << "-e";
  args << "activate";
  args << "-e";
  args << "select POSIX file \"" + filePath + "\"";
  args << "-e";
  args << "end tell";
  QProcess::startDetached("osascript", args);
#endif

#ifdef Q_OS_WIN
  QStringList args;
  args << "/select," << QDir::toNativeSeparators(filePath);
  QProcess::startDetached("explorer", args);
  //QString command = "explorer " + param;
  //QProcess::startDetached(command);
#endif
}

} // namespace nim

