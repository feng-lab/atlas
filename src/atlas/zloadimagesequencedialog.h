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
  { return m_catScenes.get(); }

private:
  QPushButton* m_runButton;
  QPushButton* m_exitButton;
  QDialogButtonBox* m_buttonBox;
  ZSelectFileWidget* m_inputImagesFileWidget;
  ZStringIntOptionParameter m_catDimension;
  ZBoolParameter m_catScenes;
};

} // namespace nim

