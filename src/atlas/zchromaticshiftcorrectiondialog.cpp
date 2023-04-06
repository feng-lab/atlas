#include "zchromaticshiftcorrectiondialog.h"

#include "zselectfilewidget.h"
#include "zchromaticshiftcorrection.h"
#include "zstringutils.h"
#include "zlog.h"
#include "zsysteminfo.h"
#include "zlogwidget.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QMessageBox>
#include <QThread>

namespace nim {

ZChromaticShiftCorrectionDialog::ZChromaticShiftCorrectionDialog(QWidget* parent)
  : ZImgProcessDialog(parent)
  , m_openStackAfterRegistering("Open Result Image After Registering", true)
  , m_referenceChannel("Reference Channel:")
  , m_targetChannel("Far-red Channel:")
  , m_method("Method")
  , m_removeBackground("Remove Background", true)
  , m_removeHighForeground("Remove High Foreground", true)
  , m_numScales("Number of Scales", 3, 1, 5)
  , m_brightBackground("Background is Brighter", false)
  , m_metric("Metric")
  , m_transform("Transform")
  , m_optimizer("Optimizer")
{
  m_referenceChannel.clearOptions();
  m_referenceChannel.addOptionWithData(std::make_pair<QString, int>("Auto", 0));
  m_referenceChannel.select("Auto");
  m_targetChannel.clearOptions();
  m_targetChannel.addOptionWithData(std::make_pair<QString, int>("Auto", 0));
  m_targetChannel.select("Auto");
  init();

  logUsageInfo();
}

void ZChromaticShiftCorrectionDialog::createWorker(nim::ZImgProcess*& worker, QString& workerName)
{
  focusNextChild();

  if (m_inputImagesFileWidget->getSelectedOpenFile().isEmpty()) {
    throw ZException(QString("No input image. Abort."));
  }
  if (m_outputStackWidget->getSelectedSaveFile().isEmpty()) {
    throw ZException(QString("Result image file must be specified."));
  }
  if (m_outputLogFileWidget->getSelectedSaveFile().isEmpty()) {
    throw ZException(QString("Correction log file must be specified."));
  }

  auto refChannel = m_referenceChannel.associatedData() - 1;
  auto targetChannel = m_targetChannel.associatedData() - 1;

  auto* workertmp = new ZChromaticShiftCorrection(m_inputImagesFileWidget->getSelectedOpenFile(),
                                                  m_outputStackWidget->getSelectedSaveFile());
  if (refChannel >= 0) {
    workertmp->setReferenceChannel(refChannel);
  }
  if (targetChannel >= 0) {
    workertmp->setTargetChannel(targetChannel);
  }
  workertmp->setMethod(m_method.associatedData());
  workertmp->setRemoveBackground(m_removeBackground.get());
  workertmp->setRemoveHighForeground(m_removeHighForeground.get());
  workertmp->setBrightBackground(m_brightBackground.get());
  workertmp->setMetric(m_metric.get());
  workertmp->setTransform(m_transform.get());
  workertmp->setOptimizer(m_optimizer.get());
  workertmp->setLogFile(m_outputLogFileWidget->getSelectedSaveFile());
  workertmp->setNumScales(m_numScales.get());
  if (m_openStackAfterRegistering.get()) {
    connect(workertmp, &ZChromaticShiftCorrection::resultReady, this, &ZChromaticShiftCorrectionDialog::resultReady);
  }

  worker = workertmp;
  workerName = "Chromatic Shift Correction";
}

void ZChromaticShiftCorrectionDialog::adjustWidget()
{
  m_referenceChannel.setVisible(m_method.isSelected("Signal Matching"));
  m_removeBackground.setVisible(m_method.isSelected("Signal Matching"));
  m_removeHighForeground.setVisible(m_method.isSelected("Signal Matching"));
  m_numScales.setVisible(m_method.isSelected("Signal Matching"));
  m_brightBackground.setVisible(m_method.isSelected("Signal Matching"));
  m_metric.setVisible(m_method.isSelected("Signal Matching"));
  m_transform.setVisible(m_method.isSelected("Signal Matching"));
  m_optimizer.setVisible(m_method.isSelected("Signal Matching"));
}

void ZChromaticShiftCorrectionDialog::inputImagesChanged()
{
  QString fn = m_inputImagesFileWidget->getSelectedOpenFile();
  if (fn.isEmpty()) {
    return;
  }

  QFileInfo fi(fn);
  QString logFn = fi.path() + "/" + fi.baseName() + "_chromatic_shift_correction_log.txt";
  m_outputLogFileWidget->setFile(logFn);
  QString stackFn = fi.path() + "/" + fi.baseName() + "_chromatic_shift_corrected.nim";
  m_outputStackWidget->setFile(stackFn);

  size_t channelNumber;
  try {
    std::vector<ZImgInfo> info = ZImg::readImgInfos(fn);
    if (info.size() != 1 || info[0].isEmpty()) {
      throw ZIOException("Not supported image dimensions");
    }
    channelNumber = info[0].numChannels;
  }
  catch (const ZIOException& e) {
    QMessageBox::critical(this,
                          QApplication::applicationName(),
                          QString("Can not parse input image:\n%1").arg(e.what()));
    return;
  }

  m_referenceChannel.clearOptions();
  m_targetChannel.clearOptions();
  for (size_t i = 0; i < channelNumber; ++i) {
    m_referenceChannel.addOptionWithData(std::make_pair(QString("Ch%1").arg(i + 1), i + 1));
    m_targetChannel.addOptionWithData(std::make_pair(QString("Ch%1").arg(i + 1), i + 1));
  }
  m_referenceChannel.select("Ch1");
  m_targetChannel.select(QString("Ch%1").arg(channelNumber));
}

void ZChromaticShiftCorrectionDialog::init()
{
  m_method.addOptionsWithData(std::make_pair<QString, QString>("Signal Matching", "Registration"),
                              std::make_pair<QString, QString>("Use 40x_1z Preset", "40x_1z"),
                              std::make_pair<QString, QString>("Use 40x_2z Preset", "40x_2z"),
                              std::make_pair<QString, QString>("Use 40x_4z Preset", "40x_4z"),
                              std::make_pair<QString, QString>("Use 63x_1z Preset", "63x_1z"),
                              std::make_pair<QString, QString>("Use 63x_2z Preset", "63x_2z"),
                              std::make_pair<QString, QString>("Use 63x_4z Preset", "63x_4z"));
  m_method.select("Signal Matching");

  m_metric.addOptions("Normalized Cross-Correlation", "Normalized Mutual Information");
  m_metric.select("Normalized Cross-Correlation");

  m_transform.addOptions("Translation", "Rigid");
  m_transform.select("Translation");

  m_optimizer.addOptions("LBFGS", "BFGS", "Nonlinear Conjugate Gradient", "Steepest Descent");
  m_optimizer.select("LBFGS");

  createIOGroupBox();
  createParaGroupBox();
  createOutputGroupBox();

  auto mainLayout = new QVBoxLayout;
  mainLayout->addWidget(m_ioGroupBox);
  mainLayout->addWidget(m_paraGroupBox);
  mainLayout->addWidget(m_outputGroupBox);
  mainLayout->addWidget(createButtonBox("Correct"));
  setLayout(mainLayout);

  setWindowTitle(tr("Chromatic Shift Correction"));
}

void ZChromaticShiftCorrectionDialog::createIOGroupBox()
{
  m_ioGroupBox = new QGroupBox(tr("Inputs and Outputs"), this);
  // everything
  auto alllayout = new QVBoxLayout;

  m_inputImagesFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile,
                                                  "Input Image:",
                                                  tr("Images (*.nim *.tif *.tiff *.v3draw *.lsm *.jpg *.png)"),
                                                  ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_inputImagesFileWidget);
  connect(m_inputImagesFileWidget,
          &ZSelectFileWidget::changed,
          this,
          &ZChromaticShiftCorrectionDialog::inputImagesChanged);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_openLoadedStack.createNameLabel());
  //  hlayout->addWidget(m_openLoadedStack.createWidget());
  //  alllayout->addLayout(hlayout);

  m_outputStackWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                              "Output Aligned Image:",
                                              tr("Stack (*.nim)"),
                                              ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  alllayout->addWidget(m_outputStackWidget);

  m_outputLogFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                                "Output Log File:",
                                                tr("Log (*.txt)"),
                                                ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
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
  hlayout->addWidget(m_method.createNameLabel());
  hlayout->addWidget(m_method.createWidget());
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_targetChannel.createNameLabel());
  hlayout->addWidget(m_targetChannel.createWidget());
  // hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_referenceChannel.createNameLabel());
  hlayout->addWidget(m_referenceChannel.createWidget());
  // hlayout->addStretch(1);
  alllayout->addLayout(hlayout);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_removeBackground.createNameLabel());
  //  hlayout->addWidget(m_removeBackground.createWidget());
  //  alllayout->addLayout(hlayout);
  //
  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_removeHighForeground.createNameLabel());
  //  hlayout->addWidget(m_removeHighForeground.createWidget());
  //  alllayout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_numScales.setStyle("SPINBOX");
  hlayout->addWidget(m_numScales.createNameLabel());
  hlayout->addWidget(m_numScales.createWidget());
  alllayout->addLayout(hlayout);

  //  hlayout = new QHBoxLayout;
  //  hlayout->addWidget(m_brightBackground.createNameLabel());
  //  hlayout->addWidget(m_brightBackground.createWidget());
  //  alllayout->addLayout(hlayout);

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

  adjustWidget();

  connect(&m_method, &ZStringStringOptionParameter::valueChanged, this, &ZChromaticShiftCorrectionDialog::adjustWidget);
}

void ZChromaticShiftCorrectionDialog::createOutputGroupBox()
{
  m_outputGroupBox = new QGroupBox(tr("Output"), this);
  // everything
  auto alllayout = new QVBoxLayout;
  alllayout->addWidget(new ZLogWidget(false, this));

  m_outputGroupBox->setLayout(alllayout);
}

void ZChromaticShiftCorrectionDialog::logUsageInfo()
{
  LOG(INFO) << R"(
Usage:
  1. Select input image file.
  2. Select Method:
    Signal Matching: Correct the shift by find the best match between the far-red channel and the reference channel.
    Use ##x_#z Preset: Correct the shift by using the predetermined shift values for different combinations of len and zoom.
  3. Make sure the "Far-red Channel" and the "Reference Channel" are correct.
  4. Click the "Correct" button.

)";
}

} // namespace nim
