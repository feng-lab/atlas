#include "zchromaticshiftcorrectiondialog.h"

#include "zselectfilewidget.h"
#include "zchromaticshiftcorrection.h"
#include "zimgstackinterface.h"
#include "zstringutils.h"
#include "zlog.h"
#include "zsysteminfo.h"
#include "zlogwidget.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>

namespace nim {

ZChromaticShiftCorrectionDialog::ZChromaticShiftCorrectionDialog(QWidget* parent)
  : QDialog(parent)
  , m_openStackAfterRegistering("Open Result Image After Registering", true)
  , m_referenceChannel("Reference Channel:")
  , m_targetChannel("Far-red Channel:")
  , m_removeBackground("Remove Background", true)
  , m_removeHighForeground("Remove High Foreground", true)
  , m_numScales("Number of Scales", 3, 1, 5)
  , m_brightBackground("Background is Brighter", false)
  , m_metric("Metric")
  , m_transform("Transform")
  , m_optimizer("Optimizer")
{
  m_referenceChannel.clearOptions();
  m_referenceChannel.addOptionWithData(qMakePair<QString, int>("Auto", 0));
  m_referenceChannel.select("Auto");
  m_targetChannel.clearOptions();
  m_targetChannel.addOptionWithData(qMakePair<QString, int>("Auto", 0));
  m_targetChannel.select("Auto");
  init();
}

void ZChromaticShiftCorrectionDialog::correctShift()
{
  focusNextChild();
  ZImg img;

  try {
    img.load(m_inputImagesFileWidget->getSelectedOpenFile());
  }
  catch (const ZException& e) {
    QMessageBox::critical(this, qApp->applicationName(), "Read Image Error.\n" + e.what());
    return;
  }

  if (img.numChannels() <= 1) {
    QMessageBox::critical(this, qApp->applicationName(), QString("Only one channel.\nDo not need align"));
    return;
  }
  if (img.numTimes() > 1) {
    LOG(INFO) << img.info().toQString();
    QMessageBox::critical(this, qApp->applicationName(), QString("Can not align time sequence image"));
    return;
  }

  if (m_outputStackWidget->getSelectedSaveFile().isEmpty()) {
    QMessageBox::critical(this, qApp->applicationName(), "Result image file must be specified.");
    return;
  }
  if (m_outputLogFileWidget->getSelectedSaveFile().isEmpty()) {
    QMessageBox::critical(this, qApp->applicationName(), "Registration log file must be specified.");
    return;
  }
  int refChannel = m_referenceChannel.associatedData() - 1;
  int targetChannel = m_targetChannel.associatedData() - 1;

  m_isCanceled = false;
  m_hasError = false;
  ZChromaticShiftCorrection* worker = new ZChromaticShiftCorrection(img, m_correctedImg);
  if (refChannel >= 0)
    worker->setReferenceChannel(refChannel);
  if (targetChannel >= 0)
    worker->setTargetChannel(targetChannel);
  worker->setRemoveBackground(m_removeBackground.get());
  worker->setRemoveHighForeground(m_removeHighForeground.get());
  worker->setBrightBackground(m_brightBackground.get());
  worker->setMetric(m_metric.get());
  worker->setTransform(m_transform.get());
  worker->setOptimizer(m_optimizer.get());
  worker->setCancelFlag(&m_isCanceled);
  worker->setLogFile(m_outputLogFileWidget->getSelectedSaveFile());
  worker->setNumScales(m_numScales.get());

  m_progressDialog = new QProgressDialog(this);
  m_progressDialog->setLabelText("Correcting Chromatic Shift...");
  m_progressDialog->setAutoReset(false);
  m_progressDialog->setAttribute(Qt::WA_DeleteOnClose);
  QObject::disconnect(m_progressDialog, &QProgressDialog::canceled, m_progressDialog, &QProgressDialog::cancel);
  connect(worker, qOverload<int>(&ZChromaticShiftCorrection::progressChanged), m_progressDialog,
          &QProgressDialog::setValue);
  connect(worker, &ZChromaticShiftCorrection::canceled, this, &ZChromaticShiftCorrectionDialog::processCanceled);
  connect(worker, &ZChromaticShiftCorrection::processError, this, &ZChromaticShiftCorrectionDialog::processError);
  connect(m_progressDialog, &QProgressDialog::canceled, this, &ZChromaticShiftCorrectionDialog::cancelButtonPressed);

  QThread* thread = new QThread(this);
  connect(thread, &QThread::started, worker, &ZChromaticShiftCorrection::run);
  connect(worker, &ZChromaticShiftCorrection::canceled, thread, &QThread::quit);
  connect(worker, &ZChromaticShiftCorrection::finished, thread, &QThread::quit);
  connect(worker, &ZChromaticShiftCorrection::processError, thread, &QThread::quit);
  connect(thread, &QThread::finished, worker, &ZChromaticShiftCorrection::deleteLater);
  connect(thread, &QThread::finished, m_progressDialog, &QProgressDialog::reset);
  connect(thread, &QThread::finished, this, &ZChromaticShiftCorrectionDialog::processFinished);
  connect(thread, &QThread::finished, thread, &QThread::deleteLater);
  worker->moveToThread(thread);

  thread->start();
  m_progressDialog->exec();
}

void ZChromaticShiftCorrectionDialog::processCanceled()
{
  QMessageBox::critical(this, qApp->applicationName(),
                        "Canceled by user.");
  m_correctedImg.clear();
}

void ZChromaticShiftCorrectionDialog::processFinished()
{
  if (!m_isCanceled && !m_hasError) {
    // first save img
    try {
      m_correctedImg.save(m_outputStackWidget->getSelectedSaveFile());
      m_correctedImg.clear();
    }
    catch (const ZException& e) {
      QMessageBox::critical(this, qApp->applicationName(), "Can not save result image.\n" + e.what());
      return;
    }
    // if need open
    if (m_openStackAfterRegistering.get()) {
      emit resultReady(m_outputStackWidget->getSelectedSaveFile());
    }
    // done
    QMessageBox::information(this, qApp->applicationName(),
                             "Chromatic Shift Correction Finished.");
  }
}

void ZChromaticShiftCorrectionDialog::processError(const QString& e)
{
  m_hasError = true;
  QMessageBox::critical(this, qApp->applicationName(),
                        QString("Error During Correction: %1").arg(e));
  m_correctedImg.clear();
}

void ZChromaticShiftCorrectionDialog::cancelButtonPressed()
{
  m_progressDialog->setLabelText("Canceling...");
  m_isCanceled = true;
}

void ZChromaticShiftCorrectionDialog::keyPressEvent(QKeyEvent* e)
{
  switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
      break;
    default:
      QDialog::keyPressEvent(e);
  }
}

void ZChromaticShiftCorrectionDialog::adjustInputImageWidget()
{
}

void ZChromaticShiftCorrectionDialog::inputImagesChanged()
{
  QString fn = m_inputImagesFileWidget->getSelectedOpenFile();
  if (fn.isEmpty())
    return;

  QFileInfo fi(fn);
  QString logFn = fi.path() + "/" + fi.baseName() + "_chromatic_shift_correction_log.txt";
  m_outputLogFileWidget->setFile(logFn);
  QString stackFn = fi.path() + "/" + fi.baseName() + "_chromatic_shift_corrected.nim";
  m_outputStackWidget->setFile(stackFn);

  int channelNumber = 0;
  int numFrames = 0;
  try {
    std::vector<ZImgInfo> info = ZImg::readImgInfo(fn, nullptr, FileFormat::Unknown);
    if (info.size() != 1) {
      throw ZIOException("Not supported image dimensions");
    }
    channelNumber = info[0].numChannels;
    numFrames = info[0].depth;
  }
  catch (const ZIOException& e) {
    QMessageBox::critical(this, qApp->applicationName(), "Can not parse input image.\n" + e.what());
  }

  m_referenceChannel.clearOptions();
  m_targetChannel.clearOptions();
  for (int i = 0; i < channelNumber; ++i) {
    m_referenceChannel.addOptionWithData(qMakePair(QString("Ch%1").arg(i + 1), i + 1));
    m_targetChannel.addOptionWithData(qMakePair(QString("Ch%1").arg(i + 1), i + 1));
  }
  m_referenceChannel.select("Ch1");
  m_targetChannel.select(QString("Ch%1").arg(channelNumber));
}

void ZChromaticShiftCorrectionDialog::init()
{
  m_metric.addOptions("Normalized Cross-Correlation",
                      "Normalized Mutual Information");
  m_metric.select("Normalized Cross-Correlation");

  m_transform.addOptions("Translation", "Rigid");
  m_transform.select("Translation");

  m_optimizer.addOptions("LBFGS", "BFGS", "Nonlinear Conjugate Gradient", "Steepest Descent");
  m_optimizer.select("LBFGS");

  createIOGroupBox();
  createParaGroupBox();
  createOutputGroupBox();

  m_runButton = new QPushButton(tr("Correct"), this);
  m_exitButton = new QPushButton(tr("Exit"), this);
  m_buttonBox = new QDialogButtonBox(Qt::Horizontal, this);
  m_buttonBox->addButton(m_exitButton, QDialogButtonBox::RejectRole);
  m_buttonBox->addButton(m_runButton, QDialogButtonBox::ActionRole);
  connect(m_exitButton, &QPushButton::clicked, this, &ZChromaticShiftCorrectionDialog::reject);
  connect(m_runButton, &QPushButton::clicked, this, &ZChromaticShiftCorrectionDialog::correctShift);

  auto mainLayout = new QVBoxLayout;
  mainLayout->addWidget(m_ioGroupBox);
  mainLayout->addWidget(m_paraGroupBox);
  mainLayout->addWidget(m_outputGroupBox);
  mainLayout->addWidget(m_buttonBox);
  setLayout(mainLayout);

  setWindowTitle(tr("Sections Registration"));
}

void ZChromaticShiftCorrectionDialog::createIOGroupBox()
{
  m_ioGroupBox = new QGroupBox(tr("Inputs and Outputs"), this);
  // everything
  auto alllayout = new QVBoxLayout;

  m_inputImagesFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile,
                                                  "Input Image:",
                                                  tr("Images (*.nim *.tif *.tiff *.v3draw *.lsm *.jpg *.png)"));
  m_inputImagesFileWidget->setStartDirQSettingLocation(ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_inputImagesFileWidget);
  connect(m_inputImagesFileWidget, &ZSelectFileWidget::changed, this, &ZChromaticShiftCorrectionDialog::inputImagesChanged);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_openLoadedStack.createNameLabel());
  //  hlayout->addWidget(m_openLoadedStack.createWidget());
  //  alllayout->addLayout(hlayout);

  adjustInputImageWidget();

  m_outputStackWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output Aligned Image:",
                                              tr("Stack (*.nim)"));
  m_outputStackWidget->setStartDirQSettingLocation(ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_outputStackWidget);

  m_outputLogFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile, "Output Log File:",
                                                tr("Log (*.txt)"));
  m_outputLogFileWidget->setStartDirQSettingLocation(ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_outputLogFileWidget);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_openStackAfterRegistering.createNameLabel());
  //  hlayout->addWidget(m_openStackAfterRegistering.createWidget());
  //  alllayout->addLayout(hlayout);

  m_ioGroupBox->setLayout(alllayout);
}

void ZChromaticShiftCorrectionDialog::createParaGroupBox()
{
  m_paraGroupBox = new QGroupBox(tr("Parameters"), this);
  // everything
  auto alllayout = new QVBoxLayout;

  auto hlayout = new QHBoxLayout;
  hlayout->addWidget(m_referenceChannel.createNameLabel());
  hlayout->addWidget(m_referenceChannel.createWidget());
  //hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_targetChannel.createNameLabel());
  hlayout->addWidget(m_targetChannel.createWidget());
  //hlayout->addStretch(1);
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

void ZChromaticShiftCorrectionDialog::createOutputGroupBox()
{
  m_outputGroupBox = new QGroupBox(tr("Output"), this);
  // everything
  auto alllayout = new QVBoxLayout;
  alllayout->addWidget(new ZLogWidget(false, this));

  m_outputGroupBox->setLayout(alllayout);
}

} // namespace nim


