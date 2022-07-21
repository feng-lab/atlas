#pragma once

#include "z3dgl.h"
#include "zoptionparameter.h"
#include "znumericparameter.h"
#include "zstringparameter.h"
#include <QScrollArea>
#include <QDir>

class QPushButton;

class QRadioButton;

class QGroupBox;

namespace nim {

class ZSelectFileWidget;

class ZAnimationExportWidget : public QScrollArea
{
Q_OBJECT
public:
  explicit ZAnimationExportWidget(bool is2DAni = false, QWidget* parent = nullptr);

  // In stereo rendering mode, we can only capture stereo image.
  // In normal rendering mode or if stereo is not supported by graphic card,
  // we can check this checkbox to capture stereo image, or not to capture normal image
  void setCaptureStereoImage(bool v)
  {
    m_captureStereoImage.set(v);
    if (v) m_captureStereoImage.setVisible(false);
  }

  QSize sizeHint() const override;

Q_SIGNALS:

  void exportFixedSize3DAnimation(const QString& filename,
                                  double framePerSecond,
                                  double startTime,
                                  double endTime,
                                  int width,
                                  int height,
                                  Z3DScreenShotType sst);

  void export3DAnimation(const QString& filename, double framePerSecond,
                         double startTime, double endTime,
                         Z3DScreenShotType sst);

  void exportFixedSize2DAnimation(const QString& filename, double framePerSecond, double startTime, double endTime,
                                  int width, int height);

  void export2DAnimation(const QString& filename, double framePerSecond, double startTime, double endTime);

private:
  void captureButtonPressed();

  void updateImageSizeWidget();

  void adjustWidget();

  void createWidget();

private:
  bool m_group;

  QGroupBox* m_groupBox = nullptr;

  ZBoolParameter m_captureStereoImage;
  ZStringIntOptionParameter m_stereoImageType;
  ZBoolParameter m_useWindowSize;
  ZIVec2Parameter m_customSize;
  ZDoubleParameter m_framePerSecond;
  ZDoubleParameter m_startTime;
  ZDoubleParameter m_endTime;

  ZSelectFileWidget* m_filenameWidget = nullptr;
  QPushButton* m_captureButton = nullptr;

  QString m_lastFName;

  bool m_is2DAnimation;
};

} // namespace nim

