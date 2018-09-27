#pragma once

#include "zimginterface.h"
#include "zoptionparameter.h"
#include <QDialog>
#include <QDialogButtonBox>

namespace nim {

class ZSelectFileWidget;

class ZLoadImageSequenceDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZLoadImageSequenceDialog(const QString& title, QWidget* parent = nullptr);

  QStringList selectedFiles();

  Dimension alongDimension();

private:
  QPushButton* m_runButton;
  QPushButton* m_exitButton;
  QDialogButtonBox* m_buttonBox;
  ZSelectFileWidget* m_inputImagesFileWidget;
  ZStringIntOptionParameter m_catDimension;
};

} // namespace nim

