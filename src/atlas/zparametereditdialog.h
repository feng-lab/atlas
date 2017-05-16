#pragma once

#include "zparameter.h"
#include <QDialog>
#include <QLabel>
#include <QGridLayout>

namespace nim {

class ZParameterEditDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZParameterEditDialog(ZParameter& para, QWidget* parent = nullptr);

signals:

protected:
  void raiseAndActivate();

  void addWidget(QLabel* label, QWidget* wg, QGridLayout* lo);

protected:
  ZParameter& m_para;
};

} // namespace nim


