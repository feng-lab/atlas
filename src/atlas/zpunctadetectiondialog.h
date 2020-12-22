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
  QGroupBox* m_ioGroupBox = nullptr;
  QGroupBox* m_paraGroupBox = nullptr;

  ZBoolParameter m_useCurrentActiveImage;
  ZSelectFileWidget* m_inputImageFileWidget = nullptr;
  ZSelectFileWidget* m_inputSwcFilesWidget = nullptr;
  ZSelectFileWidget* m_outputPunctaFileWidget = nullptr;
  ZSelectFileWidget* m_outputSomaPunctaFileWidget = nullptr;
  ZSelectFileWidget* m_outputLogFileWidget = nullptr;

  ZDVec3Parameter m_voxelSize;
  QPushButton* m_detectResolutionButton = nullptr;
  ZIntIntOptionParameter m_punctaChannel;
  ZIntParameter m_punctaThreshold;
  ZIntParameter m_somaPunctaThreshold;
  ZStringIntOptionParameter m_dendriteChannel;
  ZIntParameter m_tubeThreshold;
  ZDoubleParameter m_ambiguousFactor;
};

} // namespace nim

