#pragma once

#include "zimginterface.h"
#include "zoptionparameter.h"
#include "zparameter.h"
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

  bool catScences() const
  {
    return m_catScenes.get();
  }

private:
  QPushButton* m_runButton = nullptr;
  QPushButton* m_exitButton = nullptr;
  QDialogButtonBox* m_buttonBox = nullptr;
  ZSelectFileWidget* m_inputImagesFileWidget = nullptr;
  ZStringIntOptionParameter m_catDimension;
  ZBoolParameter m_catScenes;
};

} // namespace nim
