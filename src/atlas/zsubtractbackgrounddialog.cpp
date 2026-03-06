#include "zsubtractbackgrounddialog.h"

#include "zdoc.h"
#include "zexception.h"
#include "zimg.h"
#include "zselectfilewidget.h"
#include "zsubtractbackground.h"
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
#include <QSpinBox>
#include <QVBoxLayout>

namespace nim {

ZSubtractBackgroundDialog::ZSubtractBackgroundDialog(ZDoc& doc, QWidget* parent)
  : ZImgProcessDialog(doc, parent)
{
  setWindowTitle(tr("Subtract Background"));

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
  connect(m_inputImageWidget, &ZSelectFileWidget::changed, this, &ZSubtractBackgroundDialog::inputImageChanged);

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
    row->addWidget(new QLabel(tr("Min Foreground Ratio:"), paraGroup));
    m_minFrSpin = new QDoubleSpinBox(paraGroup);
    m_minFrSpin->setRange(0.0, 1.0);
    m_minFrSpin->setDecimals(3);
    m_minFrSpin->setSingleStep(0.01);
    m_minFrSpin->setValue(0.5);
    row->addWidget(m_minFrSpin, /*stretch*/ 1);
    paraLayout->addLayout(row);
  }

  {
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Max Iterations:"), paraGroup));
    m_maxIterSpin = new QSpinBox(paraGroup);
    m_maxIterSpin->setRange(0, 100);
    m_maxIterSpin->setValue(3);
    row->addWidget(m_maxIterSpin, /*stretch*/ 1);
    paraLayout->addLayout(row);
  }

  paraGroup->setLayout(paraLayout);
  layout->addWidget(paraGroup);

  layout->addWidget(createButtonBox(tr("Subtract"), tr("Cancel")));

  inputImageChanged();
}

QString ZSubtractBackgroundDialog::inputImagePath() const
{
  CHECK(m_inputImageWidget != nullptr);
  return m_inputImageWidget->getSelectedOpenFile();
}

QString ZSubtractBackgroundDialog::outputImagePath() const
{
  CHECK(m_outputImageWidget != nullptr);
  return m_outputImageWidget->getSelectedSaveFile();
}

QString ZSubtractBackgroundDialog::outputLogPath() const
{
  CHECK(m_outputLogWidget != nullptr);
  return m_outputLogWidget->getSelectedSaveFile();
}

int ZSubtractBackgroundDialog::channel0() const
{
  CHECK(m_channelCombo != nullptr);
  return std::max(0, m_channelCombo->currentIndex());
}

double ZSubtractBackgroundDialog::minForegroundRatio() const
{
  CHECK(m_minFrSpin != nullptr);
  return m_minFrSpin->value();
}

int ZSubtractBackgroundDialog::maxIterations() const
{
  CHECK(m_maxIterSpin != nullptr);
  return m_maxIterSpin->value();
}

bool ZSubtractBackgroundDialog::loadResultEnabled() const
{
  CHECK(m_openResultCheck != nullptr);
  return m_openResultCheck->isChecked();
}

void ZSubtractBackgroundDialog::inputImageChanged()
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

    const QStringList outputPaths =
      makeUniqueOutputPaths({fi.path() + "/" + fi.baseName() + QStringLiteral("_bg_sub_ch1.nim"),
                             fi.path() + "/" + fi.baseName() + QStringLiteral("_subtract_background_ch1_log.txt")});
    CHECK(outputPaths.size() == 2);
    m_outputImageWidget->setFile(outputPaths[0]);
    m_outputLogWidget->setFile(outputPaths[1]);
  }
  catch (const ZException& e) {
    m_channelCombo->clear();
    QMessageBox::critical(this, QApplication::applicationName(), tr("Can not read input image:\n%1").arg(e.what()));
  }
}

ZImgProcessDialog::WorkerSpec ZSubtractBackgroundDialog::createWorkerSpec()
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
  const double minFr = minForegroundRatio();
  const int maxIter = maxIterations();
  const bool openResult = loadResultEnabled();

  WorkerSpec spec;
  spec.workerName = tr("Subtract Background");
  spec.taskTitle = tr("Subtract Background: %1 -> %2").arg(QFileInfo(input).fileName(), QFileInfo(output).fileName());
  spec.successMessage = tr("Subtract Background finished.");
  spec.makeWorker = [input, output, logPath, c, minFr, maxIter]() -> std::unique_ptr<ZImgProcess> {
    auto worker = std::make_unique<ZSubtractBackground>();
    worker->setInputImagePath(input);
    worker->setOutputImagePath(output);
    worker->setLogFile(logPath);
    worker->setChannel(c);
    worker->setMinForegroundRatio(minFr);
    worker->setMaxIterations(maxIter);
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
