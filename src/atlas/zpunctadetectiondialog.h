#pragma once

#include "zimgprocessdialog.h"
#include "znumericparameter.h"
#include "zselectfilewidget.h"
#include "zoptionparameter.h"
#include <QGroupBox>
#include <QLabel>

namespace nim {

class ZImg;

class ZPunctaDetectionDialog : public ZImgProcessDialog
{
Q_OBJECT
public:
  explicit ZPunctaDetectionDialog(QWidget* parent = nullptr);

signals:

protected:
  void createWorker(ZImgProcess*& worker, QString& workerName) override;

private:
  void adjustInputImageWidget();

  void inputImageChanged();

  void detectLSMResolution();

  void dendriteChannelChanged();

  void updateInterface(const QString& fn, size_t numChannel, double vsx, double vsy, double vsz);

  void init();

  void createIOGroupBox();

  void createParaGroupBox();

private:
  QGroupBox* m_ioGroupBox;
  QGroupBox* m_paraGroupBox;

  ZBoolParameter m_useCurrentActiveImage;
  ZSelectFileWidget* m_inputImageFileWidget;
  ZSelectFileWidget* m_inputSwcFilesWidget;
  ZSelectFileWidget* m_outputPunctaFileWidget;
  ZSelectFileWidget* m_outputSomaPunctaFileWidget;
  ZSelectFileWidget* m_outputLogFileWidget;

  ZDVec3Parameter m_voxelSize;
  QPushButton* m_detectResolutionButton;
  ZIntIntOptionParameter m_punctaChannel;
  ZIntParameter m_punctaThreshold;
  ZStringIntOptionParameter m_dendriteChannel;
  ZIntParameter m_tubeThreshold;
  ZDoubleParameter m_ambiguousFactor;
};

} // namespace nim

