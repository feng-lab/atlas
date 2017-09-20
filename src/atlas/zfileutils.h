#pragma once

#include <QString>
#include <QFileDialog>

namespace nim {

class ZFileUtils
{
public:
  static void showInGraphicalShell(const QString& filePath);

  static QString getSaveFileName(QWidget *parent = nullptr,
                                 const QString &caption = QString(),
                                 const QString &dir = QString(),
                                 const QString &filter = QString(),
                                 QString *selectedFilter = nullptr,
                                 QFileDialog::Options options = QFileDialog::Options());
};

} // namespace nim

