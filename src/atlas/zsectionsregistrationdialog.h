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

class ZSectionsRegistrationDialog : public ZImgProcessDialog
{
Q_OBJECT
public:
  explicit ZSectionsRegistrationDialog(QWidget* parent = nullptr);

signals:
  void resultReady(QString path);

protected:
  void createWorker(ZImgProcess*& worker, QString& workerName) override;

private:
  void adjustInputImageWidget();

  void inputImagesChanged();

  void init();

  void createIOGroupBox();

  void createParaGroupBox();

private:
  QGroupBox* m_ioGroupBox;
  QGroupBox* m_paraGroupBox;

  ZBoolParameter m_useCurrentActiveImage;
  ZSelectFileWidget* m_inputImagesFileWidget;
  ZBoolParameter m_openLoadedStack;
  ZSelectFileWidget* m_outputStackWidget;
  ZSelectFileWidget* m_outputLogFileWidget;
  ZBoolParameter m_openStackAfterRegistering;

  ZStringIntOptionParameter m_referenceChannel;
  ZIntParameter m_referenceImageIndex;
  ZBoolParameter m_removeBackground;
  ZBoolParameter m_removeHighForeground;
  ZIntParameter m_numScales;
  ZIntParameter m_numNeighbors;
  ZBoolParameter m_allowFlip;
  ZBoolParameter m_brightBackground;
  ZStringIntOptionParameter m_metric;
  ZStringIntOptionParameter m_transform;
  ZStringIntOptionParameter m_optimizer;
};

} // namespace nim

