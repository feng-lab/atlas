#pragma once

#include <QDialog>
#include <QDialogButtonBox>

namespace nim {

class ZSelectFileWidget;

class ZLoadImageSequenceDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZLoadImageSequenceDialog(const QString& title, const QString& startDir, QWidget* parent = nullptr);

  QStringList getSelectedFiles();

private:
  QPushButton* m_runButton;
  QPushButton* m_exitButton;
  QDialogButtonBox* m_buttonBox;
  ZSelectFileWidget* m_inputImagesFileWidget;
};

} // namespace nim

