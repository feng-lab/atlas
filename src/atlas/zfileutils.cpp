#include "zfileutils.h"

#include "zlog.h"
#include <QProcess>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QRegExp>
#include <QtWidgets/QMessageBox>

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
  if (QFileInfo("/usr/bin/nautilus").exists() || QFileInfo("/usr/local/bin/nautilus").exists()) {
    QStringList args;
    args << filePath;
    QProcess::startDetached("nautilus", args);
  } else {
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? filePath : info.path()));
  }
#endif
}

QString ZFileUtils::getSaveFileName(QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
                                    QString* selectedFilter, QFileDialog::Options options)
{
#if defined(Q_OS_DARWIN) || defined(Q_OS_WIN)
  return QFileDialog::getSaveFileName(parent, caption, dir, filter, selectedFilter, options);
#else
  QFileDialog dialog(parent, caption, dir, filter);
  if (parent) {
    dialog.setWindowModality(Qt::WindowModal);
  }
  if (selectedFilter && !selectedFilter->isEmpty()) {
    dialog.selectNameFilter(*selectedFilter);
  }
  dialog.setOptions(options);
  QRegExp filter_regex(QLatin1String(R"((?:^\*\.(?!.*\()|\(\*\.)(\w+))"));
  QStringList filters = filter.split(QLatin1String(";;"));
//  if (!filters.isEmpty()) {
//    dialog.setNameFilter(filters.first());
//    if (filter_regex.indexIn(filters.first()) != -1) {
//      //LOG(INFO) << filter_regex.cap(1);
//      dialog.setDefaultSuffix(filter_regex.cap(1));
//    }
//  }
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  QString res;
  do {
    if (dialog.exec() == QDialog::Accepted) {
      if (selectedFilter) {
        *selectedFilter = dialog.selectedNameFilter();
      }
      res = dialog.selectedFiles().first();
      QFileInfo info(res);
      //LOG(INFO) << file_name << " " << dialog.selectedNameFilter();
      if (info.suffix().isEmpty() && !dialog.selectedNameFilter().isEmpty()) {
        if (filter_regex.indexIn(dialog.selectedNameFilter()) != -1) {
          QString extension = filter_regex.cap(1);
          //LOG(INFO) << extension;
          res += QLatin1String(".") + extension;

          info.setFile(res);
          if (info.exists()) {
            if (QMessageBox::Yes !=
                QMessageBox::question(&dialog, QString(),
                                      QString("A file named '%1' already exists. "
                                                "Do you want to replace it?").arg(res))) {
              res.clear();
            }
          }
        }
      }
    } else {
      return res;
    }
  } while (res.isEmpty());
  return res;
#endif
}

} // namespace nim

