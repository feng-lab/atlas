#include "zbinarizeimagedialog.h"

#include "zbinarizeimage.h"
#include "zdoc.h"
#include "zexception.h"
#include "zimg.h"
#include "zselectfilewidget.h"
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

namespace {

constexpr int kThresholdModeAuto = 0;
constexpr int kThresholdModeManual = 1;

} // namespace

ZBinarizeImageDialog::ZBinarizeImageDialog(ZDoc& doc, QWidget* parent)
  : ZImgProcessDialog(doc, parent)
{
  setWindowTitle(tr("Binarize"));

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
  connect(m_inputImageWidget, &ZSelectFileWidget::changed, this, &ZBinarizeImageDialog::inputImageChanged);

  m_outputImageWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                              tr("Output Binary Image:"),
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
    row->addWidget(new QLabel(tr("Threshold Mode:"), paraGroup));
    m_thresholdModeCombo = new QComboBox(paraGroup);
    m_thresholdModeCombo->addItem(tr("Auto (LOCMAX)"), kThresholdModeAuto);
    m_thresholdModeCombo->addItem(tr("Manual"), kThresholdModeManual);
    m_thresholdModeCombo->setCurrentIndex(0);
    row->addWidget(m_thresholdModeCombo, /*stretch*/ 1);
    paraLayout->addLayout(row);

    connect(m_thresholdModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
      CHECK(m_thresholdModeCombo != nullptr);
      CHECK(m_thresholdSpin != nullptr);
      const bool autoEnabled = (m_thresholdModeCombo->currentData().toInt() == kThresholdModeAuto);
      m_thresholdSpin->setEnabled(!autoEnabled);
    });
  }

  {
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Threshold:"), paraGroup));
    m_thresholdSpin = new QSpinBox(paraGroup);
    m_thresholdSpin->setRange(0, 65535);
    m_thresholdSpin->setValue(0);
    row->addWidget(m_thresholdSpin, /*stretch*/ 1);
    paraLayout->addLayout(row);
  }

  paraGroup->setLayout(paraLayout);
  layout->addWidget(paraGroup);

  layout->addWidget(createButtonBox(tr("Binarize"), tr("Cancel")));

  inputImageChanged();

  CHECK(m_thresholdModeCombo != nullptr);
  CHECK(m_thresholdSpin != nullptr);
  m_thresholdSpin->setEnabled(false);
}

QString ZBinarizeImageDialog::inputImagePath() const
{
  CHECK(m_inputImageWidget != nullptr);
  return m_inputImageWidget->getSelectedOpenFile();
}

QString ZBinarizeImageDialog::outputImagePath() const
{
  CHECK(m_outputImageWidget != nullptr);
  return m_outputImageWidget->getSelectedSaveFile();
}

QString ZBinarizeImageDialog::outputLogPath() const
{
  CHECK(m_outputLogWidget != nullptr);
  return m_outputLogWidget->getSelectedSaveFile();
}

int ZBinarizeImageDialog::channel0() const
{
  CHECK(m_channelCombo != nullptr);
  return std::max(0, m_channelCombo->currentIndex());
}

bool ZBinarizeImageDialog::autoThresholdEnabled() const
{
  CHECK(m_thresholdModeCombo != nullptr);
  return (m_thresholdModeCombo->currentData().toInt() == kThresholdModeAuto);
}

int ZBinarizeImageDialog::threshold() const
{
  CHECK(m_thresholdSpin != nullptr);
  return m_thresholdSpin->value();
}

bool ZBinarizeImageDialog::loadResultEnabled() const
{
  CHECK(m_openResultCheck != nullptr);
  return m_openResultCheck->isChecked();
}

void ZBinarizeImageDialog::inputImageChanged()
{
  CHECK(m_channelCombo != nullptr);
  CHECK(m_thresholdSpin != nullptr);
  CHECK(m_outputImageWidget != nullptr);
  CHECK(m_outputLogWidget != nullptr);

  const QString fn = inputImagePath();
  if (fn.trimmed().isEmpty()) {
    m_channelCombo->clear();
    m_thresholdSpin->setRange(0, 65535);
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

    const int maxThre = (info.bytesPerVoxel <= 1) ? 255 : 65535;
    m_thresholdSpin->setRange(0, maxThre);

    const QStringList outputPaths =
      makeUniqueOutputPaths({fi.path() + "/" + fi.baseName() + QStringLiteral("_binarized_ch1.nim"),
                             fi.path() + "/" + fi.baseName() + QStringLiteral("_binarize_ch1_log.txt")});
    CHECK(outputPaths.size() == 2);
    m_outputImageWidget->setFile(outputPaths[0]);
    m_outputLogWidget->setFile(outputPaths[1]);
  }
  catch (const ZException& e) {
    m_channelCombo->clear();
    m_thresholdSpin->setRange(0, 65535);
    QMessageBox::critical(this, QApplication::applicationName(), tr("Can not read input image:\n%1").arg(e.what()));
  }
}

ZImgProcessDialog::WorkerSpec ZBinarizeImageDialog::createWorkerSpec()
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
  const bool autoThre = autoThresholdEnabled();
  const int thre = threshold();
  const bool openResult = loadResultEnabled();

  WorkerSpec spec;
  spec.workerName = tr("Binarize");
  spec.taskTitle = tr("Binarize: %1 -> %2").arg(QFileInfo(input).fileName(), QFileInfo(output).fileName());
  spec.successMessage = tr("Binarize finished.");
  spec.makeWorker = [input, output, logPath, c, autoThre, thre]() -> std::unique_ptr<ZImgProcess> {
    auto worker = std::make_unique<ZBinarizeImage>();
    worker->setInputImagePath(input);
    worker->setOutputImagePath(output);
    worker->setLogFile(logPath);
    worker->setChannel(c);
    worker->setThreshold(thre);
    worker->setThresholdMode(autoThre ? ZBinarizeImage::ThresholdMode::AutoLocmax
                                      : ZBinarizeImage::ThresholdMode::Manual);
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
