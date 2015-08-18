#ifndef ZTAKESCREENSHOTWIDGET_H
#define ZTAKESCREENSHOTWIDGET_H

#include "z3dgl.h"
#include <QWidget>
#include <QDir>
#include "zoptionparameter.h"
#include "znumericparameter.h"
#include "zstringparameter.h"

class QPushButton;
class QRadioButton;
class QGroupBox;

namespace nim {

class ZSelectFileWidget;

class ZTakeScreenShotWidget : public QWidget
{
  Q_OBJECT
public:
  explicit ZTakeScreenShotWidget(bool is2D = false, bool group = false, QWidget *parent = 0);
  // In stereo rendering mode, we can only capture stereo image.
  // In normal rendering mode or if stereo is not supported by graphic card,
  // we can check this checkbox to capture stereo image, or not to capture normal image
  void setCaptureStereoImage(bool v) { m_captureStereoImage.set(v); if (v) m_captureStereoImage.setVisible(false);}
  
signals:
  void takeScreenShot(const QString &filename, int width, int height);
  void takeScreenShot(const QString &filename);
  void takeScreenShot(const QString &filename, int width, int height, Z3DScreenShotType sst);
  void takeScreenShot(const QString &filename, Z3DScreenShotType sst);
  void takeSeriesScreenShot(const QDir& dir, const QString &namePrefix, glm::vec3 axis,
                            bool clockWise, int numFrame, int width, int height, Z3DScreenShotType sst);
  void takeSeriesScreenShot(const QDir& dir, const QString &namePrefix, glm::vec3 axis,
                            bool clockWise, int numFrame, Z3DScreenShotType sst);
  
public slots:
  void captureButtonPressed();
  void captureSequenceButtonPressed();
  void setFileNameSource();
  void updateImageSizeWidget();
  void prefixChanged();

protected slots:
  void adjustWidget();
  
private:
  void createWidget();

  bool m_group;

  QGroupBox *m_groupBox;

  ZStringIntOptionParameter m_mode;
  ZBoolParameter m_captureStereoImage;
  ZStringIntOptionParameter m_stereoImageType;
  ZBoolParameter m_useWindowSize;
  ZIVec2Parameter m_customSize;

  QRadioButton *m_useManualName;
  QRadioButton *m_useAutoName;

  ZSelectFileWidget *m_folderWidget;
  ZStringParameter m_namePrefix;
  QPushButton *m_captureButton;

  QString m_lastFName;
  int m_nextNumber;

  ZStringIntOptionParameter m_axis;
  ZBoolParameter m_clockwise;
  ZIntParameter m_timeInSecond;
  ZIntParameter m_framePerSecond;
  QPushButton *m_captureSequenceButton;

  bool m_is2D;
};

} // namespace nim

#endif // ZTAKESCREENSHOTWIDGET_H
