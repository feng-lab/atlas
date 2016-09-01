#pragma once

#include "zparameteranimation.h"
#include "zparameterkey.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>

namespace nim {

class ZTimelineKeyEditDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZTimelineKeyEditDialog(ZParameterAnimation& paraAnimation, ZParameterKey& paraKey,
                                  QWidget* parent = nullptr);

  void setInitialValue();

signals:

protected:
  virtual void accept() override;

  virtual void reject() override;

  void raiseAndActivate();

  void addWidget(QLabel* label, QWidget* wg, QGridLayout* lo);

protected:
  ZParameterAnimation& m_paraAnimation;
  ZParameterKey& m_paraKey;

  ZDoubleParameter m_time;
  ZStringIntOptionParameter m_type;

  ZFloatParameter m_posTension;
  ZFloatParameter m_posContinuity;
  ZFloatParameter m_posBias;

  ZParameter* m_para;
};

} // namespace nim

