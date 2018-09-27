#pragma once

#include "zimg.h"
#include "znumericparameter.h"
#include "zselectfilewidget.h"
#include "zoptionparameter.h"
#include <QDialog>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QProgressDialog>
#include <QLabel>
#include <memory>

namespace nim {

class ZChromaticShiftCorrectionDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZChromaticShiftCorrectionDialog(QWidget* parent = nullptr);

signals:
  void resultReady(QString path);

protected:
  void correctShift();

  void processCanceled();

  void processFinished();

  void processError(const QString& e);

  void cancelButtonPressed();

  void keyPressEvent(QKeyEvent* e) override;

private:
  void adjustInputImageWidget();

  void inputImagesChanged();

  void init();

  void createIOGroupBox();

  void createParaGroupBox();

  void createOutputGroupBox();

private:
  QGroupBox* m_ioGroupBox;
  QGroupBox* m_paraGroupBox;
  QGroupBox* m_outputGroupBox;
  QPushButton* m_runButton;
  QPushButton* m_exitButton;
  QDialogButtonBox* m_buttonBox;

  ZSelectFileWidget* m_inputImagesFileWidget;
  ZSelectFileWidget* m_outputStackWidget;
  ZSelectFileWidget* m_outputLogFileWidget;
  ZBoolParameter m_openStackAfterRegistering;

  ZStringIntOptionParameter m_referenceChannel;
  ZStringIntOptionParameter m_targetChannel;
  ZBoolParameter m_removeBackground;
  ZBoolParameter m_removeHighForeground;
  ZIntParameter m_numScales;
  ZBoolParameter m_brightBackground;
  ZStringIntOptionParameter m_metric;
  ZStringIntOptionParameter m_transform;
  ZStringIntOptionParameter m_optimizer;

  std::atomic<bool> m_isCanceled;
  bool m_hasError;

  QProgressDialog* m_progressDialog;

  ZImg m_correctedImg;
};

} // namespace nim



