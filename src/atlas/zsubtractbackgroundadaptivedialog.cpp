#include "zsubtractbackgroundadaptivedialog.h"

#include "zdoc.h"
#include "zexception.h"
#include "zimg.h"
#include "zselectfilewidget.h"
#include "zsubtractbackgroundadaptive.h"
#include "zsysteminfo.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>

namespace nim {

ZSubtractBackgroundAdaptiveDialog::ZSubtractBackgroundAdaptiveDialog(ZDoc& doc, QWidget* parent)
  : ZImgProcessDialog(doc, parent)
{
  setWindowTitle(tr("Subtract Background (Adaptive)"));

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
  connect(m_inputImageWidget, &ZSelectFileWidget::changed, this, &ZSubtractBackgroundAdaptiveDialog::inputImageChanged);

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
    row->addWidget(new QLabel(tr("Num Samples:"), paraGroup));
    m_numSamplesSpin = new QSpinBox(paraGroup);
    m_numSamplesSpin->setRange(1, 100);
    m_numSamplesSpin->setValue(5);
    row->addWidget(m_numSamplesSpin, /*stretch*/ 1);
    paraLayout->addLayout(row);
  }

  {
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Stride:"), paraGroup));
    m_strideSpin = new QSpinBox(paraGroup);
    m_strideSpin->setRange(1, 100);
    m_strideSpin->setValue(3);
    row->addWidget(m_strideSpin, /*stretch*/ 1);
    paraLayout->addLayout(row);
  }

  paraGroup->setLayout(paraLayout);
  layout->addWidget(paraGroup);

  layout->addWidget(createButtonBox(tr("Subtract"), tr("Cancel")));

  inputImageChanged();
}

QString ZSubtractBackgroundAdaptiveDialog::inputImagePath() const
{
  CHECK(m_inputImageWidget != nullptr);
  return m_inputImageWidget->getSelectedOpenFile();
}

QString ZSubtractBackgroundAdaptiveDialog::outputImagePath() const
{
  CHECK(m_outputImageWidget != nullptr);
  return m_outputImageWidget->getSelectedSaveFile();
}

QString ZSubtractBackgroundAdaptiveDialog::outputLogPath() const
{
  CHECK(m_outputLogWidget != nullptr);
  return m_outputLogWidget->getSelectedSaveFile();
}

int ZSubtractBackgroundAdaptiveDialog::channel0() const
{
  CHECK(m_channelCombo != nullptr);
  return std::max(0, m_channelCombo->currentIndex());
}

int ZSubtractBackgroundAdaptiveDialog::numSamples() const
{
  CHECK(m_numSamplesSpin != nullptr);
  return m_numSamplesSpin->value();
}

int ZSubtractBackgroundAdaptiveDialog::stride() const
{
  CHECK(m_strideSpin != nullptr);
  return m_strideSpin->value();
}

bool ZSubtractBackgroundAdaptiveDialog::loadResultEnabled() const
{
  CHECK(m_openResultCheck != nullptr);
  return m_openResultCheck->isChecked();
}

void ZSubtractBackgroundAdaptiveDialog::inputImageChanged()
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

    const QStringList outputPaths = makeUniqueOutputPaths(
      {fi.path() + "/" + fi.baseName() + QStringLiteral("_bg_sub_adaptive_ch1.nim"),
       fi.path() + "/" + fi.baseName() + QStringLiteral("_subtract_background_adaptive_ch1_log.txt")});
    CHECK(outputPaths.size() == 2);
    m_outputImageWidget->setFile(outputPaths[0]);
    m_outputLogWidget->setFile(outputPaths[1]);
  }
  catch (const ZException& e) {
    m_channelCombo->clear();
    QMessageBox::critical(this, QApplication::applicationName(), tr("Can not read input image:\n%1").arg(e.what()));
  }
}

ZImgProcessDialog::WorkerSpec ZSubtractBackgroundAdaptiveDialog::createWorkerSpec()
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
  const int nsample = numSamples();
  const int strideV = stride();
  const bool openResult = loadResultEnabled();

  WorkerSpec spec;
  spec.workerName = tr("Subtract Background (Adaptive)");
  spec.taskTitle =
    tr("Subtract Background (Adaptive): %1 -> %2").arg(QFileInfo(input).fileName(), QFileInfo(output).fileName());
  spec.successMessage = tr("Subtract Background (Adaptive) finished.");
  spec.makeWorker = [input, output, logPath, c, nsample, strideV]() -> std::unique_ptr<ZImgProcess> {
    auto worker = std::make_unique<ZSubtractBackgroundAdaptive>();
    worker->setInputImagePath(input);
    worker->setOutputImagePath(output);
    worker->setLogFile(logPath);
    worker->setChannel(c);
    worker->setNumSamples(nsample);
    worker->setStride(strideV);
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
