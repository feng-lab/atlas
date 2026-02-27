#pragma once

#include "zimg.h"
#include "znumericparameter.h"
#include "zselectfilewidget.h"
#include "zoptionparameter.h"
#include "zimgprocessdialog.h"
#include <QGroupBox>
#include <QLabel>
#include <memory>

namespace nim {

class ZChromaticShiftCorrectionDialog : public ZImgProcessDialog
{
  Q_OBJECT

public:
  explicit ZChromaticShiftCorrectionDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  WorkerSpec createWorkerSpec() override;

private:
  void adjustWidget();

  void inputImagesChanged();

  void init();

  void createIOGroupBox();

  void createParaGroupBox();

  void createOutputGroupBox();

  void logUsageInfo();

private:
  QGroupBox* m_ioGroupBox;
  QGroupBox* m_paraGroupBox;
  QGroupBox* m_outputGroupBox;

  ZSelectFileWidget* m_inputImagesFileWidget;
  ZSelectFileWidget* m_outputStackWidget;
  ZSelectFileWidget* m_outputLogFileWidget;
  ZBoolParameter m_openStackAfterRegistering;

  ZStringIntOptionParameter m_referenceChannel;
  ZStringIntOptionParameter m_targetChannel;
  ZStringStringOptionParameter m_method;
  ZBoolParameter m_removeBackground;
  ZBoolParameter m_removeHighForeground;
  ZIntParameter m_numScales;
  ZBoolParameter m_brightBackground;
  ZStringIntOptionParameter m_metric;
  ZStringIntOptionParameter m_transform;
  ZStringIntOptionParameter m_optimizer;
};

} // namespace nim
