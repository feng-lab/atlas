#include "zfileutils.h"

#include <QProcess>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>

namespace nim {

void ZFileUtils::showInGraphicalShell(const QString& filePath)
{
  QFileInfo info(filePath);
  if (!info.exists())
    return;

#ifdef Q_OS_MAC
  QStringList args;
  args << "-e";
  args << R"(tell application "Finder")";
  args << "-e";
  args << "activate";
  args << "-e";
  args << "select POSIX file \"" + filePath + "\"";
  args << "-e";
  args << "end tell";
  QProcess::startDetached("osascript", args);
#elif defined(Q_OS_WIN)
  QStringList args;
  args << "/select," << QDir::toNativeSeparators(filePath);
  QProcess::startDetached("explorer", args);
  //QString command = "explorer " + param;
  //QProcess::startDetached(command);
#else
  QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? filePath : info.path()));
#endif
}

} // namespace nim

