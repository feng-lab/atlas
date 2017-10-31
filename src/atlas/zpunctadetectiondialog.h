#pragma once

#include "znumericparameter.h"
#include "zselectfilewidget.h"
#include "zoptionparameter.h"
#include <QDialog>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QProgressDialog>
#include <QLabel>
#ifdef _NEUTUBE_
class ZStackDoc;
class ZStack;
#endif

namespace nim {

class ZImg;

class ZPunctaDetectionDialog : public QDialog
{
Q_OBJECT
public:
#ifdef _NEUTUBE_
  explicit ZPunctaDetectionDialog(ZSharedPointer<ZStackDoc> doc, QWidget *parent = 0);
#endif

  explicit ZPunctaDetectionDialog(QWidget* parent = nullptr);

signals:
#ifdef _NEUTUBE_
  void stackDocDelivered(ZStack* stack);
#endif

protected:
  void detect();

  void processCanceled();

  void processFinished();

  void processError(const QString& e);

  void cancelButtonPressed();

  //void reject() override;

  void keyPressEvent(QKeyEvent* e) override;
  //void closeEvent(QCloseEvent *e) override;

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
#ifdef _NEUTUBE_
  ZSharedPointer<ZStackDoc> m_doc;
#endif

  QGroupBox* m_ioGroupBox;
  QGroupBox* m_paraGroupBox;
  QPushButton* m_runButton;
  QPushButton* m_exitButton;
  QDialogButtonBox* m_buttonBox;

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

  std::atomic<bool> m_isCanceled;
  bool m_hasError;

  QProgressDialog* m_progressDialog;
};

} // namespace nim

