#pragma once

#include <QDialog>

class QTextBrowser;

namespace nim {

class ZInformationDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ZInformationDialog(QWidget* parent = nullptr);

  void setText(const QString& html);

private:
  QTextBrowser* m_view = nullptr;
};

} // namespace nim
