#include "zenhancelinedialog.h"

#include "zdoc.h"
#include "zenhanceline.h"
#include "zexception.h"
#include "zimg.h"
#include "zselectfilewidget.h"
#include "zsysteminfo.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

namespace nim {

ZEnhanceLineDialog::ZEnhanceLineDialog(ZDoc& doc, QWidget* parent)
  : ZImgProcessDialog(doc, parent)
{
  setWindowTitle(tr("Enhance Line"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(10);

  const QString imgStartDir = ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image");

  auto* ioGroup = new QGroupBox(tr("Input and Output"), this);
  auto* ioLayout = new QVBoxLayout(ioGroup);

  m_inputImageWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile,
                                             tr("Input Image:"),
                                             tr("Images (*.nim *.tif *.tiff *.v3draw *.lsm *.jpg *.png)"),
                                             imgStartDir,
                                             QString(),
                                             QBoxLayout::LeftToRight,
                                             ioGroup);
  ioLayout->addWidget(m_inputImageWidget);
  connect(m_inputImageWidget, &ZSelectFileWidget::changed, this, &ZEnhanceLineDialog::inputImageChanged);

  m_outputImageWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                              tr("Output Image:"),
                                              tr("Stack (*.nim)"),
                                              imgStartDir,
                                              QString(),
                                              QBoxLayout::LeftToRight,
                                              ioGroup);
  ioLayout->addWidget(m_outputImageWidget);

  m_outputLogWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                            tr("Output Log File:"),
                                            tr("Log (*.txt)"),
                                            imgStartDir,
                                            QString(),
                                            QBoxLayout::LeftToRight,
                                            ioGroup);
  ioLayout->addWidget(m_outputLogWidget);

  m_openResultCheck = new QCheckBox(tr("Open output image when finished"), ioGroup);
  m_openResultCheck->setChecked(true);
  ioLayout->addWidget(m_openResultCheck);

  ioGroup->setLayout(ioLayout);
  layout->addWidget(ioGroup);

  auto* paraGroup = new QGroupBox(tr("Parameters"), this);
  auto* paraLayout = new QVBoxLayout(paraGroup);

  {
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Channel:"), paraGroup));
    m_channelCombo = new QComboBox(paraGroup);
    row->addWidget(m_channelCombo, /*stretch*/ 1);
    paraLayout->addLayout(row);
  }

  {
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Sigma:"), paraGroup));
    m_sigmaSpin = new QDoubleSpinBox(paraGroup);
    m_sigmaSpin->setRange(0.01, 100.0);
    m_sigmaSpin->setDecimals(3);
    m_sigmaSpin->setSingleStep(0.1);
    m_sigmaSpin->setValue(1.0);
    row->addWidget(m_sigmaSpin, /*stretch*/ 1);
    paraLayout->addLayout(row);
  }

  paraGroup->setLayout(paraLayout);
  layout->addWidget(paraGroup);

  layout->addWidget(createButtonBox(tr("Enhance"), tr("Cancel")));

  inputImageChanged();
}

QString ZEnhanceLineDialog::inputImagePath() const
{
  CHECK(m_inputImageWidget != nullptr);
  return m_inputImageWidget->getSelectedOpenFile();
}

QString ZEnhanceLineDialog::outputImagePath() const
{
  CHECK(m_outputImageWidget != nullptr);
  return m_outputImageWidget->getSelectedSaveFile();
}

QString ZEnhanceLineDialog::outputLogPath() const
{
  CHECK(m_outputLogWidget != nullptr);
  return m_outputLogWidget->getSelectedSaveFile();
}

int ZEnhanceLineDialog::channel0() const
{
  CHECK(m_channelCombo != nullptr);
  return std::max(0, m_channelCombo->currentIndex());
}

double ZEnhanceLineDialog::sigma() const
{
  CHECK(m_sigmaSpin != nullptr);
  return m_sigmaSpin->value();
}

bool ZEnhanceLineDialog::loadResultEnabled() const
{
  CHECK(m_openResultCheck != nullptr);
  return m_openResultCheck->isChecked();
}

void ZEnhanceLineDialog::inputImageChanged()
{
  CHECK(m_channelCombo != nullptr);
  CHECK(m_outputImageWidget != nullptr);
  CHECK(m_outputLogWidget != nullptr);

  const QString fn = inputImagePath();
  if (fn.trimmed().isEmpty()) {
    m_channelCombo->clear();
    return;
  }

  QFileInfo fi(fn);
  try {
    const ZImgInfo info = ZImg::readImgInfos(fn).at(0);
    m_channelCombo->clear();
    for (size_t c = 0; c < info.numChannels; ++c) {
      m_channelCombo->addItem(QStringLiteral("Ch%1").arg(c + 1));
    }
    if (m_channelCombo->count() > 0) {
      m_channelCombo->setCurrentIndex(0);
    }

    const QString outImage = fi.path() + "/" + fi.baseName() + QStringLiteral("_enhance_line_ch1.nim");
    const QString outLog = fi.path() + "/" + fi.baseName() + QStringLiteral("_enhance_line_ch1_log.txt");
    m_outputImageWidget->setFile(outImage);
    m_outputLogWidget->setFile(outLog);
  }
  catch (const ZException& e) {
    m_channelCombo->clear();
    QMessageBox::critical(this, QApplication::applicationName(), tr("Can not read input image:\n%1").arg(e.what()));
  }
}

ZImgProcessDialog::WorkerSpec ZEnhanceLineDialog::createWorkerSpec()
{
  const QString input = inputImagePath();
  if (input.trimmed().isEmpty()) {
    throw ZException("Please select an input image file.");
  }

  const QString output = outputImagePath();
  if (output.trimmed().isEmpty()) {
    throw ZException("Please select an output image file.");
  }

  const QString logPath = outputLogPath();
  if (logPath.trimmed().isEmpty()) {
    throw ZException("Please select an output log file.");
  }

  const int c = channel0();
  const double sigmaV = sigma();
  const bool openResult = loadResultEnabled();

  WorkerSpec spec;
  spec.workerName = tr("Enhance Line");
  spec.taskTitle = tr("Enhance Line: %1 -> %2").arg(QFileInfo(input).fileName(), QFileInfo(output).fileName());
  spec.successMessage = tr("Enhance Line finished.");
  spec.makeWorker = [input, output, logPath, c, sigmaV]() -> std::unique_ptr<ZImgProcess> {
    auto worker = std::make_unique<ZEnhanceLine>();
    worker->setInputImagePath(input);
    worker->setOutputImagePath(output);
    worker->setLogFile(logPath);
    worker->setChannel(c);
    worker->setSigma(sigmaV);
    return worker;
  };

  if (openResult) {
    spec.onSuccessUi = [output](ZDoc& doc, ZBackgroundTask&) {
      doc.loadFile(output);
    };
  }

  return spec;
}

} // namespace nim
