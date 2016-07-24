#include "zsectionsregistrationdialog.h"

#include <QVBoxLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMessageBox>
#include "zselectfilewidget.h"
#ifdef _NEUTUBE_
#include "zstack.hxx"
#include "zstackdoc.h"
#endif
#include "zsectionsregistration.h"
#include <QThread>
#include "zimgstackinterface.h"
#include "zstringutils.h"
#include "zlog.h"

namespace nim {

#ifdef _NEUTUBE_
ZSectionsRegistrationDialog::ZSectionsRegistrationDialog(std::tr1::shared_ptr<ZStackDoc> doc, QWidget *parent)
  : QDialog(parent)
  , m_doc(doc)
  , m_useCurrentActiveImage("Use Current Active Image", true)
  , m_openLoadedStack("Open Original Image Sequence", true)
  , m_openStackAfterRegistering("Open Result Image After Registering", true)
  , m_referenceChannel("Reference Channel:")
  , m_referenceImageIndex("Reference Image Index", 0, 0, 90000)
  , m_removeBackground("Remove Background", true)
  , m_removeHighForeground("Remove High Foreground", true)
  , m_numScales("Number of Scales", 3, 1, 5)
  , m_metric("Metric")
  , m_transform("Transform")
  , m_optimizer("Optimizer")
{
  int channelNumber = m_doc->stackChannelNumber();
  m_referenceChannel.clearOptions();
  m_referenceChannel.addOptionWithData(qMakePair<QString,int>("Auto", 0));
  for (int i=0; i<channelNumber; ++i) {
    m_referenceChannel.addOptionWithData(qMakePair(QString("Ch%1").arg(i+1), i+1));
  }

  m_referenceChannel.select("Auto");
  m_referenceImageIndex.setRange(0, m_doc->stack()->depth()-1);

  init();

  QString fn = QString::fromStdString(m_doc->stackSourcePath());
  if (true || QFile::exists(fn)) {
    QFileInfo fi(fn);
    QString logFn = fi.path() + "/" + replaceLastInteger(fi.baseName(), "_all") + "_sections_registration_log.txt";
    m_outputLogFileWidget->setFile(logFn);
    QString stackFn = fi.path() + "/" + replaceLastInteger(fi.baseName(), "_all") + "_aligned_stack.tif";
    m_outputStackWidget->setFile(stackFn);
  }
}
#endif

ZSectionsRegistrationDialog::ZSectionsRegistrationDialog(QWidget *parent)
  : QDialog(parent)
  , m_useCurrentActiveImage("Use Current Active Image", false)
  , m_openLoadedStack("Open Original Image Sequence", true)
  , m_openStackAfterRegistering("Open Result Image After Registering", true)
  , m_referenceChannel("Reference Channel:")
  , m_referenceImageIndex("Reference Image Index", 0, 0, 90000)
  , m_removeBackground("Remove Background", true)
  , m_removeHighForeground("Remove High Foreground", true)
  , m_numScales("Number of Scales", 3, 1, 5)
  , m_numNeighbors("Number of Neighbors", 1, 1, 50)
  , m_allowFlip("Allow Flip", false)
  , m_brightBackground("Background is Brighter", false)
  , m_metric("Metric")
  , m_transform("Transform")
  , m_optimizer("Optimizer")
{
  m_referenceChannel.clearOptions();
  m_referenceChannel.addOptionWithData(qMakePair<QString,int>("Auto", 0));
  m_referenceChannel.select("Auto");
  init();
}

void ZSectionsRegistrationDialog::registerSections()
{
  focusNextChild();
  ZImg img;
#ifdef _NEUTUBE_
  if (m_useCurrentActiveImage.get() && m_doc && m_doc->hasStackData()) {
    img = wrapZStackAsZImg(*m_doc->stack());
  } else if (!m_useCurrentActiveImage.get()) {
#endif
  if (!m_useCurrentActiveImage.get()) {
    try {
      img.load(m_inputImagesFileWidget->getSelectedMultipleOpenFiles(), Dimension::Z, 0, FileFormat::Unknown, true, m_brightBackground.get());
    }
    catch (const ZException & e) {
      QMessageBox::critical(this, "Read Image Error", e.what());
      return;
    }

    if (img.is2DImg()) {
      QMessageBox::critical(this, "only one slice", QString("Do not need align"));
      return;
    } else if (!img.is3DImg()) {
      LINFO() << img.info().toQString();
      QMessageBox::critical(this, "time sequence not supported", QString("Can not align time sequence image"));
      return;
    }
    if (m_openLoadedStack.get()) {
#ifdef _NEUTUBE_
      ZImg cpyImg = img;
      ZStack* stack = imgToZStack(cpyImg);
      stack->setSource(replaceLastInteger(m_inputImagesFileWidget->getSelectedMultipleOpenFiles()[0], "_all").toStdString());
      emit stackDocDelivered(stack);
#endif
    }
  } else {
    QMessageBox::critical(this, "No Image", "No Image to Align.");
    return;
  }

  if (m_outputStackWidget->getSelectedSaveFile().isEmpty()) {
    QMessageBox::critical(this, "No output Image file", "Result image file must be specified.");
    return;
  }
  if (m_outputLogFileWidget->getSelectedSaveFile().isEmpty()) {
    QMessageBox::critical(this, "No output log file", "Registration log file must be specified.");
    return;
  }
  int refChannel = m_referenceChannel.associatedData() - 1;

  m_isCanceled = false;
  m_hasError = false;
  ZSectionsRegistration *worker = new ZSectionsRegistration(img, m_referenceImageIndex.get(), m_registeredImg);
  if (refChannel >= 0)
    worker->setReferenceChannel(refChannel);
  worker->setRemoveBackground(m_removeBackground.get());
  worker->setRemoveHighForeground(m_removeHighForeground.get());
  worker->setAllowFlip(m_allowFlip.get());
  worker->setBrightBackground(m_brightBackground.get());
  worker->setMetric(m_metric.get());
  worker->setTransform(m_transform.get());
  worker->setOptimizer(m_optimizer.get());
  worker->setCancelFlag(&m_isCanceled);
  worker->setLogFile(m_outputLogFileWidget->getSelectedSaveFile());
  worker->setNumScales(m_numScales.get());
  worker->setNumNeighbors(m_numNeighbors.get());

  m_progressDialog = new QProgressDialog(this);
  m_progressDialog->setLabelText("Registering Sections...");
  m_progressDialog->setAutoReset(false);
  m_progressDialog->setAttribute(Qt::WA_DeleteOnClose);
  QObject::disconnect(m_progressDialog, &QProgressDialog::canceled, m_progressDialog, &QProgressDialog::cancel);
  connect(worker, qOverload<int>(&ZSectionsRegistration::progressChanged), m_progressDialog, &QProgressDialog::setValue);
  connect(worker, &ZSectionsRegistration::canceled, this, &ZSectionsRegistrationDialog::processCanceled);
  connect(worker, &ZSectionsRegistration::processError, this, &ZSectionsRegistrationDialog::processError);
  connect(m_progressDialog, &QProgressDialog::canceled, this, &ZSectionsRegistrationDialog::cancelButtonPressed);

  QThread *thread = new QThread(this);
  connect(thread, &QThread::started, worker, &ZSectionsRegistration::run);
  connect(worker, &ZSectionsRegistration::canceled, thread, &QThread::quit);
  connect(worker, &ZSectionsRegistration::finished, thread, &QThread::quit);
  connect(worker, &ZSectionsRegistration::processError, thread, &QThread::quit);
  connect(thread, &QThread::finished, worker, &ZSectionsRegistration::deleteLater);
  connect(thread, &QThread::finished, m_progressDialog, &QProgressDialog::reset);
  connect(thread, &QThread::finished, this, &ZSectionsRegistrationDialog::processFinished);
  connect(thread, &QThread::finished, thread, &QThread::deleteLater);
  worker->moveToThread(thread);

  thread->start();
  m_progressDialog->exec();
}

void ZSectionsRegistrationDialog::processCanceled()
{
  QMessageBox::critical(this, "Canceled",
                        "Sections Registration is canceled by user.");
  m_registeredImg.clear();
}

void ZSectionsRegistrationDialog::processFinished()
{
  if (!m_isCanceled && !m_hasError) {
    // first save img
    try {
      m_registeredImg.save(m_outputStackWidget->getSelectedSaveFile());
    }
    catch (const ZException & e) {
      QMessageBox::critical(this, "Can not save result image", e.what());
      return;
    }
    // if need open
    if (m_openStackAfterRegistering.get()) {
      emit resultReady(&m_registeredImg, m_outputStackWidget->getSelectedSaveFile());
    }
    m_registeredImg.clear();
    // done
    QMessageBox::information(this, "Finished",
                             "Sections Registration Finished.");
  }
}

void ZSectionsRegistrationDialog::processError(const QString &e)
{
  m_hasError = true;
  QMessageBox::critical(this, "Error",
                        QString("Error During Registration: %1").arg(e));
  m_registeredImg.clear();
}

void ZSectionsRegistrationDialog::cancelButtonPressed()
{
  m_progressDialog->setLabelText("Canceling...");
  m_isCanceled = true;
}

void ZSectionsRegistrationDialog::keyPressEvent(QKeyEvent *e)
{
  switch (e->key()) {
  case Qt::Key_Return:
  case Qt::Key_Enter:
    break;
  default:
    QDialog::keyPressEvent(e);
  }
}

void ZSectionsRegistrationDialog::adjustInputImageWidget()
{
  m_inputImagesFileWidget->setVisible(!m_useCurrentActiveImage.get());
  m_openLoadedStack.setVisible(!m_useCurrentActiveImage.get());
}

void ZSectionsRegistrationDialog::inputImagesChanged()
{
  QStringList fns = m_inputImagesFileWidget->getSelectedMultipleOpenFiles();
  if (fns.empty())
    return;

  QString fn = fns[0];
  QFileInfo fi(fn);
  QString logFn = fi.path() + "/" + replaceLastInteger(fi.baseName(), "_all") + "_sections_registration_log.txt";
  m_outputLogFileWidget->setFile(logFn);
  QString stackFn = fi.path() + "/" + replaceLastInteger(fi.baseName(), "_all") + "_aligned_stack.tif";
  m_outputStackWidget->setFile(stackFn);

  int channelNumber = 0;
  int numFrames = 0;
  try {
    std::vector<ZImgInfo> info = ZImg::readImgInfo(fns, Dimension::Z, nullptr, FileFormat::Unknown, true);
    if (info.size() != 1) {
      throw ZIOException("Not supported image dimensions");
    }
    channelNumber = info[0].numChannels;
    numFrames = info[0].depth;
  }
  catch (const ZIOException & e) {
    QMessageBox::critical(this, "Can not parse input image", e.what());
  }

  m_referenceChannel.clearOptions();
  m_referenceChannel.addOptionWithData(qMakePair<QString,int>("Auto", 0));
  for (int i=0; i<channelNumber; ++i) {
    m_referenceChannel.addOptionWithData(qMakePair(QString("Ch%1").arg(i+1), i+1));
  }

  m_referenceChannel.select("Auto");

  m_referenceImageIndex.setRange(0, numFrames-1);
}

void ZSectionsRegistrationDialog::init()
{
  m_metric.addOptions("Mean Differences", "Mean Squared Differences",
                      "Log Absolute Differences",
                      "Normalized Cross-Correlation",
                      "Normalized Mutual Information");
  m_metric.select("Log Absolute Differences");

  m_transform.addOptions("YTranslation", "Translation", "Rigid", "Similarity", "Affine");
  m_transform.select("Rigid");

  m_optimizer.addOptions("LBFGS", "BFGS", "Nonlinear Conjugate Gradient", "Steepest Descent");
  m_optimizer.select("LBFGS");

  createIOGroupBox();
  createParaGroupBox();

  m_runButton = new QPushButton(tr("Register"), this);
  m_exitButton = new QPushButton(tr("Exit"), this);
  m_buttonBox = new QDialogButtonBox(Qt::Horizontal, this);
  m_buttonBox->addButton(m_exitButton, QDialogButtonBox::RejectRole);
  m_buttonBox->addButton(m_runButton, QDialogButtonBox::ActionRole);
  connect(m_exitButton, &QPushButton::clicked, this, &ZSectionsRegistrationDialog::reject);
  connect(m_runButton, &QPushButton::clicked, this, &ZSectionsRegistrationDialog::registerSections);

  QVBoxLayout *mainLayout = new QVBoxLayout;
  mainLayout->addWidget(m_ioGroupBox);
  mainLayout->addWidget(m_paraGroupBox);
  mainLayout->addWidget(m_buttonBox);
  setLayout(mainLayout);

  setWindowTitle(tr("Sections Registration"));
}

void ZSectionsRegistrationDialog::createIOGroupBox()
{
  m_ioGroupBox = new QGroupBox(tr("Inputs and Outputs"), this);
  // everything
  QVBoxLayout *alllayout = new QVBoxLayout;

#ifdef _NEUTUBE_
  QHBoxLayout *hlayout;
  if (m_doc) {
    hlayout = new QHBoxLayout;
    hlayout->addWidget(m_useCurrentActiveImage.createNameLabel(), 0, Qt::AlignLeft);
    hlayout->addWidget(m_useCurrentActiveImage.createWidget(), 0, Qt::AlignLeft);
    hlayout->addStretch(1);
    alllayout->addLayout(hlayout);
    connect(&m_useCurrentActiveImage, &ZBoolParameter::valueChanged, this, &ZSectionsRegistrationDialog::adjustInputImageWidget);
  }
#endif
  m_inputImagesFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenMultipleFilesWithFilter, "Input Sections:",
                                                 tr("Images (*.tif *.tiff *.raw *.lsm *.jpg *.png)"));
  m_inputImagesFileWidget->setCompareFunc(naturalSortLessThan);
  alllayout->addWidget(m_inputImagesFileWidget);
  connect(m_inputImagesFileWidget, &ZSelectFileWidget::changed, this, &ZSectionsRegistrationDialog::inputImagesChanged);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_openLoadedStack.createNameLabel());
  //  hlayout->addWidget(m_openLoadedStack.createWidget());
  //  alllayout->addLayout(hlayout);

  adjustInputImageWidget();

  m_outputStackWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output Aligned Image:",
                                                tr("Stack (*.tif *.raw)"));
  alllayout->addWidget(m_outputStackWidget);

  m_outputLogFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output Log File:",
                                                tr("Log (*.txt)"));
  alllayout->addWidget(m_outputLogFileWidget);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_openStackAfterRegistering.createNameLabel());
  //  hlayout->addWidget(m_openStackAfterRegistering.createWidget());
  //  alllayout->addLayout(hlayout);

  m_ioGroupBox->setLayout(alllayout);
}

void ZSectionsRegistrationDialog::createParaGroupBox()
{
  m_paraGroupBox = new QGroupBox(tr("Parameters"), this);
  // everything
  QVBoxLayout *alllayout = new QVBoxLayout;

  QHBoxLayout *hlayout = new QHBoxLayout;
  hlayout->addWidget(m_referenceChannel.createNameLabel());
  hlayout->addWidget(m_referenceChannel.createWidget());
  //hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_referenceImageIndex.setStyle("SPINBOX");
  hlayout->addWidget(m_referenceImageIndex.createNameLabel());
  hlayout->addWidget(m_referenceImageIndex.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_removeBackground.createNameLabel());
  hlayout->addWidget(m_removeBackground.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_removeHighForeground.createNameLabel());
  hlayout->addWidget(m_removeHighForeground.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_numScales.setStyle("SPINBOX");
  hlayout->addWidget(m_numScales.createNameLabel());
  hlayout->addWidget(m_numScales.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_numNeighbors.setStyle("SPINBOX");
  hlayout->addWidget(m_numNeighbors.createNameLabel());
  hlayout->addWidget(m_numNeighbors.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_allowFlip.createNameLabel());
  hlayout->addWidget(m_allowFlip.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_brightBackground.createNameLabel());
  hlayout->addWidget(m_brightBackground.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_metric.createNameLabel());
  hlayout->addWidget(m_metric.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_transform.createNameLabel());
  hlayout->addWidget(m_transform.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_optimizer.createNameLabel());
  hlayout->addWidget(m_optimizer.createWidget());
  alllayout->addLayout(hlayout);

  m_paraGroupBox->setLayout(alllayout);
}

} // namespace nim

