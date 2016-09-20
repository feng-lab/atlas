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

#ifdef _NEUTUBE_
#include <zsharedpointer.h>
#endif

#ifdef _NEUTUBE_
class ZStackDoc;
class ZStack;
#endif

namespace nim {

class ZSectionsRegistrationDialog : public QDialog
{
Q_OBJECT
public:
#ifdef _NEUTUBE_
  explicit ZSectionsRegistrationDialog(ZSharedPointer<ZStackDoc> doc, QWidget *parent = 0);
#endif

  explicit ZSectionsRegistrationDialog(QWidget* parent = nullptr);

signals:
#ifdef _NEUTUBE_
  void stackDocDelivered(ZStack* stack);
#endif

  void resultReady(ZImg* img, QString path);

protected:
  void registerSections();

  void processCanceled();

  void processFinished();

  void processError(const QString& e);

  void cancelButtonPressed();

  virtual void keyPressEvent(QKeyEvent* e) override;

private:
  void adjustInputImageWidget();

  void inputImagesChanged();

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

  std::atomic<bool> m_isCanceled;
  bool m_hasError;

  QProgressDialog* m_progressDialog;

  ZImg m_registeredImg;
};

} // namespace nim

