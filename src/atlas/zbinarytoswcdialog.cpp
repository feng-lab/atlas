#include "zbinarytoswcdialog.h"

#include "zdoc.h"
#include "zexception.h"
#include "zmessageboxhelpers.h"
#include "zselectfilewidget.h"
#include "zswcdoc.h"
#include "zsysteminfo.h"

#include "zjson.h"

#include "zneutubeskeletonizeprocess.h"

#include "zswc.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QMessageBox>

namespace nim {

ZBinaryToSwcDialog::ZBinaryToSwcDialog(ZDoc& doc, QWidget* parent)
  : ZImgProcessDialog(doc, parent)
  , m_doc(doc)
{
  setWindowTitle(tr("Binary -> SWC"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(10);

  auto* ioGroup = new QGroupBox(tr("Inputs and Outputs"), this);
  auto* ioLayout = new QVBoxLayout(ioGroup);

  const QString imageStartDir = ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image");
  const QString swcStartDir = ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Swc");

  m_inputImageWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile,
                                             tr("Input Image:"),
                                             tr("Images (*.nim *.tif *.tiff *.v3draw *.lsm *.jpg *.png)"),
                                             imageStartDir,
                                             QString(),
                                             QBoxLayout::LeftToRight,
                                             ioGroup);
  ioLayout->addWidget(m_inputImageWidget);

  m_skeletonizeConfigWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenSingleFile,
                                                    tr("Skeletonize Preset (optional):"),
                                                    tr("JSON (*.json)"),
                                                    imageStartDir,
                                                    QString(),
                                                    QBoxLayout::LeftToRight,
                                                    ioGroup);
  ioLayout->addWidget(m_skeletonizeConfigWidget);

  m_outputSwcWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                            tr("Output SWC:"),
                                            ZSwc::getQtWriteNameFilter(),
                                            swcStartDir,
                                            QString(),
                                            QBoxLayout::LeftToRight,
                                            ioGroup);
  ioLayout->addWidget(m_outputSwcWidget);

  m_outputLogWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                            tr("Output Log File:"),
                                            tr("Log (*.txt)"),
                                            swcStartDir,
                                            QString(),
                                            QBoxLayout::LeftToRight,
                                            ioGroup);
  ioLayout->addWidget(m_outputLogWidget);

  m_loadResultCheck = new QCheckBox(tr("Load output SWC into the scene when finished"), ioGroup);
  m_loadResultCheck->setChecked(true);
  ioLayout->addWidget(m_loadResultCheck);

  ioGroup->setLayout(ioLayout);
  layout->addWidget(ioGroup);

  auto* paramsGroup = new QGroupBox(tr("Skeletonization Parameters"), this);
  auto* paramsLayout = new QVBoxLayout(paramsGroup);

  {
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Length threshold:"), paramsGroup));
    m_lengthThresholdSpin = new QDoubleSpinBox(paramsGroup);
    m_lengthThresholdSpin->setDecimals(0);
    m_lengthThresholdSpin->setRange(0.0, 1000.0);
    m_lengthThresholdSpin->setValue(25.0);
    row->addWidget(m_lengthThresholdSpin);
    row->addStretch();
    paramsLayout->addLayout(row);
  }

  {
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Final length threshold:"), paramsGroup));
    m_finalLengthThresholdSpin = new QDoubleSpinBox(paramsGroup);
    m_finalLengthThresholdSpin->setDecimals(0);
    m_finalLengthThresholdSpin->setRange(0.0, 1000.0);
    m_finalLengthThresholdSpin->setValue(0.0);
    row->addWidget(m_finalLengthThresholdSpin);
    row->addStretch();
    paramsLayout->addLayout(row);
  }

  m_keepShortObjectsCheck = new QCheckBox(tr("Keep short objects"), paramsGroup);
  m_keepShortObjectsCheck->setToolTip(
    tr("Keep at least one branch even if an object is shorter than the length threshold"));
  paramsLayout->addWidget(m_keepShortObjectsCheck);

  m_connectAllCheck = new QCheckBox(tr("Connect all"), paramsGroup);
  m_connectAllCheck->setToolTip(tr("Connect all objects no matter where they are"));
  paramsLayout->addWidget(m_connectAllCheck);

  {
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Distance threshold:"), paramsGroup));
    m_distanceThresholdSpin = new QDoubleSpinBox(paramsGroup);
    m_distanceThresholdSpin->setDecimals(0);
    m_distanceThresholdSpin->setRange(0.0, 1000.0);
    m_distanceThresholdSpin->setValue(50.0);
    row->addWidget(m_distanceThresholdSpin);
    row->addStretch();
    paramsLayout->addLayout(row);
  }

  {
    auto* row = new QHBoxLayout();
    m_excludeSmallObjectsCheck = new QCheckBox(tr("Exclude small objects: Min size"), paramsGroup);
    m_excludeSmallObjectsCheck->setToolTip(tr("Exclude small objects from the reconstruction"));
    m_excludeSmallObjectsCheck->setChecked(true);
    row->addWidget(m_excludeSmallObjectsCheck);

    m_minObjSizeSpin = new QSpinBox(paramsGroup);
    m_minObjSizeSpin->setRange(0, 10000);
    m_minObjSizeSpin->setValue(5);
    row->addWidget(m_minObjSizeSpin);
    row->addStretch();
    paramsLayout->addLayout(row);
  }

  {
    auto* row = new QHBoxLayout();
    m_greyToBinaryCheck = new QCheckBox(tr("Grey to binary"), paramsGroup);
    row->addWidget(m_greyToBinaryCheck);

    m_grayOpCombo = new QComboBox(paramsGroup);
    m_grayOpCombo->addItem(QStringLiteral(">="));
    m_grayOpCombo->addItem(QStringLiteral("<="));
    m_grayOpCombo->addItem(QStringLiteral("="));
    row->addWidget(m_grayOpCombo);

    m_levelSpin = new QSpinBox(paramsGroup);
    m_levelSpin->setRange(1, 65535);
    m_levelSpin->setValue(1);
    row->addWidget(m_levelSpin);

    row->addStretch();
    paramsLayout->addLayout(row);
  }

  {
    auto* row = new QHBoxLayout();
    m_downsampleCheck = new QCheckBox(tr("Downsample"), paramsGroup);
    row->addWidget(m_downsampleCheck);

    row->addWidget(new QLabel(tr("x"), paramsGroup));
    m_dsXSpin = new QSpinBox(paramsGroup);
    m_dsXSpin->setRange(0, 10);
    m_dsXSpin->setValue(1);
    row->addWidget(m_dsXSpin);

    row->addWidget(new QLabel(tr("y"), paramsGroup));
    m_dsYSpin = new QSpinBox(paramsGroup);
    m_dsYSpin->setRange(0, 10);
    m_dsYSpin->setValue(1);
    row->addWidget(m_dsYSpin);

    row->addWidget(new QLabel(tr("z"), paramsGroup));
    m_dsZSpin = new QSpinBox(paramsGroup);
    m_dsZSpin->setRange(0, 10);
    m_dsZSpin->setValue(1);
    row->addWidget(m_dsZSpin);

    row->addStretch();
    paramsLayout->addLayout(row);
  }

  m_rebaseCheck = new QCheckBox(tr("Rebase"), paramsGroup);
  m_rebaseCheck->setToolTip(tr("Reset the starting point to a terminal point"));
  m_rebaseCheck->setChecked(true);
  paramsLayout->addWidget(m_rebaseCheck);

  m_fillHolesCheck = new QCheckBox(tr("Fill holes"), paramsGroup);
  m_fillHolesCheck->setChecked(false);
  paramsLayout->addWidget(m_fillHolesCheck);

  paramsGroup->setLayout(paramsLayout);
  layout->addWidget(paramsGroup);

  layout->addWidget(createButtonBox(tr("Skeletonize"), tr("Cancel")));

  refreshSkeletonizeUiEnabledState();
  rebuildSuggestedOutputs();

  connect(m_inputImageWidget, &ZSelectFileWidget::changed, this, [this]() {
    rebuildSuggestedOutputs();
  });

  connect(m_skeletonizeConfigWidget, &ZSelectFileWidget::changed, this, [this]() {
    const QString cfgPath = skeletonizeConfigPath();
    if (cfgPath.trimmed().isEmpty()) {
      return;
    }

    try {
      const json::object cfg = loadJsonObject(cfgPath);

      if (m_lengthThresholdSpin != nullptr) {
        if (auto it = cfg.find("minimalLength"); it != cfg.end() && it->value().is_number()) {
          const QSignalBlocker blocker(*m_lengthThresholdSpin);
          m_lengthThresholdSpin->setValue(it->value().to_number<double>());
        }
      }
      if (m_finalLengthThresholdSpin != nullptr) {
        if (auto it = cfg.find("finalMinimalLength"); it != cfg.end() && it->value().is_number()) {
          const QSignalBlocker blocker(*m_finalLengthThresholdSpin);
          m_finalLengthThresholdSpin->setValue(it->value().to_number<double>());
        }
      }
      if (m_keepShortObjectsCheck != nullptr) {
        if (auto it = cfg.find("keepingSingleObject"); it != cfg.end() && it->value().is_bool()) {
          const QSignalBlocker blocker(*m_keepShortObjectsCheck);
          m_keepShortObjectsCheck->setChecked(it->value().as_bool());
        }
      }
      if (m_rebaseCheck != nullptr) {
        if (auto it = cfg.find("rebase"); it != cfg.end() && it->value().is_bool()) {
          const QSignalBlocker blocker(*m_rebaseCheck);
          m_rebaseCheck->setChecked(it->value().as_bool());
        }
      }
      if (m_fillHolesCheck != nullptr) {
        if (auto it = cfg.find("fillingHole"); it != cfg.end() && it->value().is_bool()) {
          const QSignalBlocker blocker(*m_fillHolesCheck);
          m_fillHolesCheck->setChecked(it->value().as_bool());
        }
      }
      if (m_distanceThresholdSpin != nullptr && m_connectAllCheck != nullptr) {
        if (auto it = cfg.find("maximalDistance"); it != cfg.end() && it->value().is_number()) {
          const double dist = it->value().to_number<double>();
          const QSignalBlocker blocker1(*m_connectAllCheck);
          const QSignalBlocker blocker2(*m_distanceThresholdSpin);
          if (dist < 0.0) {
            m_connectAllCheck->setChecked(true);
          } else {
            m_connectAllCheck->setChecked(false);
            m_distanceThresholdSpin->setValue(dist);
          }
        }
      }
      if (m_excludeSmallObjectsCheck != nullptr && m_minObjSizeSpin != nullptr) {
        if (auto it = cfg.find("minimalObjectSize"); it != cfg.end() && it->value().is_int64()) {
          const int minSize = static_cast<int>(it->value().as_int64());
          const QSignalBlocker blocker1(*m_excludeSmallObjectsCheck);
          const QSignalBlocker blocker2(*m_minObjSizeSpin);
          m_excludeSmallObjectsCheck->setChecked(minSize > 0);
          m_minObjSizeSpin->setValue(std::max(0, minSize));
        }
      }
      if (m_downsampleCheck != nullptr && m_dsXSpin != nullptr && m_dsYSpin != nullptr && m_dsZSpin != nullptr) {
        if (auto it = cfg.find("downsampleInterval"); it != cfg.end() && it->value().is_array()) {
          const auto& arr = it->value().as_array();
          if (arr.size() == 3 && arr[0].is_int64() && arr[1].is_int64() && arr[2].is_int64()) {
            const int x = static_cast<int>(arr[0].as_int64());
            const int y = static_cast<int>(arr[1].as_int64());
            const int z = static_cast<int>(arr[2].as_int64());
            const QSignalBlocker blocker1(*m_downsampleCheck);
            const QSignalBlocker blocker2(*m_dsXSpin);
            const QSignalBlocker blocker3(*m_dsYSpin);
            const QSignalBlocker blocker4(*m_dsZSpin);
            m_downsampleCheck->setChecked((x > 0) || (y > 0) || (z > 0));
            m_dsXSpin->setValue(std::max(0, x));
            m_dsYSpin->setValue(std::max(0, y));
            m_dsZSpin->setValue(std::max(0, z));
          }
        }
      }

      refreshSkeletonizeUiEnabledState();
    }
    catch (const std::exception& e) {
      showCriticalWithDetails(this,
                              tr("Can not load skeletonize preset"),
                              tr("Preset: %1\n%2").arg(cfgPath, QString::fromUtf8(e.what())));
    }
  });

  connect(m_connectAllCheck, &QCheckBox::toggled, this, [this](bool) {
    refreshSkeletonizeUiEnabledState();
  });
  connect(m_excludeSmallObjectsCheck, &QCheckBox::toggled, this, [this](bool) {
    refreshSkeletonizeUiEnabledState();
  });
  connect(m_greyToBinaryCheck, &QCheckBox::toggled, this, [this](bool) {
    refreshSkeletonizeUiEnabledState();
  });
  connect(m_downsampleCheck, &QCheckBox::toggled, this, [this](bool) {
    refreshSkeletonizeUiEnabledState();
  });

  connect(m_outputSwcWidget, &ZSelectFileWidget::changed, this, [this]() {
    if (m_applyingSuggestedOutputs) {
      return;
    }
    m_outputSwcCustomized = true;

    if (m_outputLogCustomized) {
      return;
    }
    const QString swcPath = outputSwcPath();
    if (swcPath.isEmpty()) {
      return;
    }
    const QFileInfo fi(swcPath);
    const QString logPath = fi.dir().absoluteFilePath(fi.completeBaseName() + QStringLiteral("_log.txt"));
    m_applyingSuggestedOutputs = true;
    const QSignalBlocker blocker(*m_outputLogWidget);
    m_outputLogWidget->setFile(logPath);
    m_applyingSuggestedOutputs = false;
  });

  connect(m_outputLogWidget, &ZSelectFileWidget::changed, this, [this]() {
    if (m_applyingSuggestedOutputs) {
      return;
    }
    m_outputLogCustomized = true;
  });
}

void ZBinaryToSwcDialog::rebuildSuggestedOutputs()
{
  if (m_inputImageWidget == nullptr || m_outputSwcWidget == nullptr || m_outputLogWidget == nullptr) {
    return;
  }

  if (m_outputSwcCustomized && m_outputLogCustomized) {
    return;
  }

  const QString inputPath = inputImagePath();
  if (inputPath.trimmed().isEmpty()) {
    return;
  }

  const QFileInfo fi(inputPath);
  const QString suggestedSwc = fi.dir().absoluteFilePath(fi.completeBaseName() + QStringLiteral("_skel.swc"));
  const QString suggestedLog = fi.dir().absoluteFilePath(fi.completeBaseName() + QStringLiteral("_skel_log.txt"));

  m_applyingSuggestedOutputs = true;
  if (!m_outputSwcCustomized) {
    const QSignalBlocker blocker(*m_outputSwcWidget);
    m_outputSwcWidget->setFile(suggestedSwc);
  }
  if (!m_outputLogCustomized) {
    const QSignalBlocker blocker(*m_outputLogWidget);
    m_outputLogWidget->setFile(suggestedLog);
  }
  m_applyingSuggestedOutputs = false;
}

void ZBinaryToSwcDialog::refreshSkeletonizeUiEnabledState()
{
  if (m_connectAllCheck != nullptr && m_distanceThresholdSpin != nullptr) {
    m_distanceThresholdSpin->setEnabled(!m_connectAllCheck->isChecked());
  }
  if (m_excludeSmallObjectsCheck != nullptr && m_minObjSizeSpin != nullptr) {
    m_minObjSizeSpin->setEnabled(m_excludeSmallObjectsCheck->isChecked());
  }
  if (m_greyToBinaryCheck != nullptr && m_grayOpCombo != nullptr && m_levelSpin != nullptr) {
    const bool enabled = m_greyToBinaryCheck->isChecked();
    m_grayOpCombo->setEnabled(enabled);
    m_levelSpin->setEnabled(enabled);
  }
  if (m_downsampleCheck != nullptr && m_dsXSpin != nullptr && m_dsYSpin != nullptr && m_dsZSpin != nullptr) {
    const bool enabled = m_downsampleCheck->isChecked();
    m_dsXSpin->setEnabled(enabled);
    m_dsYSpin->setEnabled(enabled);
    m_dsZSpin->setEnabled(enabled);
  }
}

QString ZBinaryToSwcDialog::inputImagePath() const
{
  CHECK(m_inputImageWidget != nullptr);
  return m_inputImageWidget->getSelectedOpenFile();
}

QString ZBinaryToSwcDialog::skeletonizeConfigPath() const
{
  CHECK(m_skeletonizeConfigWidget != nullptr);
  return m_skeletonizeConfigWidget->getSelectedOpenFile();
}

QString ZBinaryToSwcDialog::outputSwcPath() const
{
  CHECK(m_outputSwcWidget != nullptr);
  return m_outputSwcWidget->getSelectedSaveFile();
}

QString ZBinaryToSwcDialog::outputLogPath() const
{
  CHECK(m_outputLogWidget != nullptr);
  return m_outputLogWidget->getSelectedSaveFile();
}

bool ZBinaryToSwcDialog::loadResultEnabled() const
{
  CHECK(m_loadResultCheck != nullptr);
  return m_loadResultCheck->isChecked();
}

ZImgProcessDialog::WorkerSpec ZBinaryToSwcDialog::createWorkerSpec()
{
  const QString input = inputImagePath();
  if (input.trimmed().isEmpty()) {
    throw ZException("Please select an input image file.");
  }

  const QString outputSwc = outputSwcPath();
  if (outputSwc.trimmed().isEmpty()) {
    throw ZException("Please select an output SWC file.");
  }

  const QString outputLog = outputLogPath();
  if (outputLog.trimmed().isEmpty()) {
    throw ZException("Please select an output log file.");
  }

  const QString cfg = skeletonizeConfigPath();
  const bool loadResult = loadResultEnabled();

  CHECK(m_lengthThresholdSpin != nullptr);
  CHECK(m_finalLengthThresholdSpin != nullptr);
  CHECK(m_keepShortObjectsCheck != nullptr);
  CHECK(m_connectAllCheck != nullptr);
  CHECK(m_distanceThresholdSpin != nullptr);
  CHECK(m_excludeSmallObjectsCheck != nullptr);
  CHECK(m_minObjSizeSpin != nullptr);
  CHECK(m_greyToBinaryCheck != nullptr);
  CHECK(m_grayOpCombo != nullptr);
  CHECK(m_levelSpin != nullptr);
  CHECK(m_downsampleCheck != nullptr);
  CHECK(m_dsXSpin != nullptr);
  CHECK(m_dsYSpin != nullptr);
  CHECK(m_dsZSpin != nullptr);
  CHECK(m_rebaseCheck != nullptr);
  CHECK(m_fillHolesCheck != nullptr);

  json::object cfgObj;
  if (!cfg.trimmed().isEmpty()) {
    cfgObj = loadJsonObject(cfg);
  }
  cfgObj["minimalLength"] = json::value_from(static_cast<int64_t>(m_lengthThresholdSpin->value()));
  cfgObj["finalMinimalLength"] = json::value_from(static_cast<int64_t>(m_finalLengthThresholdSpin->value()));
  cfgObj["keepingSingleObject"] = json::value_from(m_keepShortObjectsCheck->isChecked());
  cfgObj["rebase"] = json::value_from(m_rebaseCheck->isChecked());
  cfgObj["fillingHole"] = json::value_from(m_fillHolesCheck->isChecked());
  cfgObj["minimalObjectSize"] =
    json::value_from(static_cast<int64_t>(m_excludeSmallObjectsCheck->isChecked() ? m_minObjSizeSpin->value() : 0));

  const double dist = m_connectAllCheck->isChecked() ? -1.0 : m_distanceThresholdSpin->value();
  cfgObj["maximalDistance"] = json::value_from(dist);

  json::array downsampleInterval;
  if (m_downsampleCheck->isChecked()) {
    downsampleInterval.emplace_back(static_cast<int64_t>(m_dsXSpin->value()));
    downsampleInterval.emplace_back(static_cast<int64_t>(m_dsYSpin->value()));
    downsampleInterval.emplace_back(static_cast<int64_t>(m_dsZSpin->value()));
  } else {
    downsampleInterval.emplace_back(int64_t{0});
    downsampleInterval.emplace_back(int64_t{0});
    downsampleInterval.emplace_back(int64_t{0});
  }
  cfgObj["downsampleInterval"] = std::move(downsampleInterval);

  if (m_greyToBinaryCheck->isChecked()) {
    cfgObj["level"] = json::value_from(static_cast<int64_t>(m_levelSpin->value()));
    cfgObj["grayOp"] = json::value_from(static_cast<int64_t>(m_grayOpCombo->currentIndex()));
  } else {
    cfgObj.erase("level");
    cfgObj.erase("grayOp");
    cfgObj.erase("levelOp");
  }

  WorkerSpec spec;
  spec.workerName = tr("Binary -> SWC");
  spec.taskTitle = tr("Binary -> SWC: %1 -> %2").arg(QFileInfo(input).fileName(), QFileInfo(outputSwc).fileName());
  spec.successMessage = tr("Binary -> SWC finished.");
  spec.makeWorker =
    [input, cfg, cfgObj = std::move(cfgObj), outputSwc, outputLog]() mutable -> std::unique_ptr<ZImgProcess> {
    auto worker = std::make_unique<ZNeutubeSkeletonizeProcess>();
    worker->setInputImagePath(input);
    worker->setSkeletonizeConfigPath(cfg);
    worker->setSkeletonizeConfig(std::move(cfgObj));
    worker->setOutputSwcPath(outputSwc);
    worker->setLogFile(outputLog);
    return worker;
  };

  if (loadResult) {
    spec.onSuccessUi = [outputSwc](ZDoc& doc, ZBackgroundTask&) {
      if (!QFile::exists(outputSwc)) {
        QMessageBox::information(QApplication::activeWindow(),
                                 QApplication::applicationName(),
                                 QStringLiteral("No SWC was generated."));
        return;
      }

      QString err;
      const size_t id = doc.swcDoc().loadFile(outputSwc, err);
      if (id == 0) {
        showCriticalWithDetails(QApplication::activeWindow(),
                                QObject::tr("Can not load output SWC"),
                                QObject::tr("SWC: %1\n%2").arg(outputSwc, err));
      }
    };
  }

  return spec;
}

} // namespace nim
