#ifndef ZANIMATIONEXPORTWIDGET_H
#define ZANIMATIONEXPORTWIDGET_H

#include "z3dgl.h"
#include <QScrollArea>
#include <QDir>
#include "zoptionparameter.h"
#include "znumericparameter.h"
#include "zstringparameter.h"

class QPushButton;
class QRadioButton;
class QGroupBox;

namespace nim {

class ZSelectFileWidget;

class ZAnimationExportWidget : public QScrollArea
{
  Q_OBJECT
public:
  explicit ZAnimationExportWidget(bool is2DAni = false, QWidget *parent = nullptr);

  // In stereo rendering mode, we can only capture stereo image.
  // In normal rendering mode or if stereo is not supported by graphic card,
  // we can check this checkbox to capture stereo image, or not to capture normal image
  void setCaptureStereoImage(bool v) { m_captureStereoImage.set(v); if (v) m_captureStereoImage.setVisible(false);}

  virtual QSize sizeHint() const override;

signals:
  void exportFixedSize3DAnimation(const QDir& dir, const QString &filename, double framePerSecond, int width, int height, Z3DScreenShotType sst);
  void export3DAnimation(const QDir& dir, const QString &filename, double framePerSecond, Z3DScreenShotType sst);
  void exportFixedSize2DAnimation(const QDir& dir, const QString &filename, double framePerSecond, int width, int height);
  void export2DAnimation(const QDir& dir, const QString &filename, double framePerSecond);

public slots:
  void captureButtonPressed();
  void updateImageSizeWidget();

protected slots:
  void adjustWidget();

private:
  void createWidget();

  bool m_group;

  QGroupBox *m_groupBox;

  ZBoolParameter m_captureStereoImage;
  ZStringIntOptionParameter m_stereoImageType;
  ZBoolParameter m_useWindowSize;
  ZIVec2Parameter m_customSize;
  ZDoubleParameter m_framePerSecond;

  ZSelectFileWidget *m_folderWidget;
  ZStringParameter m_filename;
  QPushButton *m_captureButton;

  QString m_lastFName;

  bool m_is2DAnimation;
};

} // namespace nim

#endif // ZANIMATIONEXPORTWIDGET_H
