#include "zautotracedialog.h"

#include "zdoc.h"
#include "zbackgroundtaskmanager.h"
#include "zexception.h"
#include "zimgdoc.h"
#include "zlog.h"
#include "zselectfilewidget.h"
#include "zsysteminfo.h"
#include "ztracesettings.h"
#include "zswcdoc.h"

#include "zneutubeautotraceprocess.h"
#include "zcancellation.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QBrush>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <limits>

namespace nim {

namespace {

void disableFirstComboRow(QComboBox* combo)
{
  if (combo == nullptr) {
    return;
  }

  auto* model = qobject_cast<QStandardItemModel*>(combo->model());
  if (model == nullptr) {
    return;
  }

  QStandardItem* item = model->item(0);
  if (item == nullptr) {
    return;
  }

  item->setEnabled(false);
}

[[nodiscard]] QString imageToolTip(ZDoc& doc, size_t imgObjId)
{
  const ZImgDoc& imgDoc = doc.imgDoc();
  if (!imgDoc.hasObjWithID(imgObjId)) {
    return doc.objNameWithModifiedMarkerAndID(imgObjId);
  }

  const ZImgInfo info = imgDoc.imgPackShared(imgObjId)->imgInfo();
  return QStringLiteral("%1\n%2×%3×%4, c=%5, t=%6, %7")
    .arg(doc.objNameWithModifiedMarkerAndID(imgObjId))
    .arg(info.width)
    .arg(info.height)
    .arg(info.depth)
    .arg(info.numChannels)
    .arg(info.numTimes)
    .arg(info.typeAsQString());
}

[[nodiscard]] QColor adjustedChannelTextColor(QColor channelColor, const QPalette& palette)
{
  const QColor base = palette.color(QPalette::Base);
  const bool baseIsDark = base.lightness() < 128;

  if (baseIsDark) {
    if (channelColor.lightness() < 80) {
      channelColor = channelColor.lighter(160);
    }
  } else {
    if (channelColor.lightness() > 200) {
      channelColor = channelColor.darker(160);
    }
  }

  channelColor.setAlpha(255);
  return channelColor;
}

[[nodiscard]] QString makeSafeStem(QString s)
{
  s = s.trimmed();
  if (s.isEmpty()) {
    return QStringLiteral("Atlas");
  }

  s.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
  s.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
  return s;
}

} // namespace

ZAutoTraceDialog::ZAutoTraceDialog(ZDoc& doc, QWidget* parent)
  : ZImgProcessDialog(doc, parent)
  , m_doc(doc)
{
  setWindowTitle(tr("Automatic Tracing"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(10);

  auto* sourceGroup = new QGroupBox(tr("Source"), this);
  auto* sourceForm = new QFormLayout(sourceGroup);
  sourceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  sourceForm->setRowWrapPolicy(QFormLayout::WrapLongRows);

  m_imageCombo = new QComboBox(sourceGroup);
  m_imageCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
  m_imageCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  sourceForm->addRow(tr("Image:"), m_imageCombo);

  m_channelCombo = new QComboBox(sourceGroup);
  m_channelCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
  m_channelCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  sourceForm->addRow(tr("Channel:"), m_channelCombo);

  m_timeCombo = new QComboBox(sourceGroup);
  m_timeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
  m_timeCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  sourceForm->addRow(tr("Time:"), m_timeCombo);

  layout->addWidget(sourceGroup);

  auto* optionsGroup = new QGroupBox(tr("Auto Trace"), this);
  auto* optionsLayout = new QVBoxLayout(optionsGroup);
  optionsLayout->setContentsMargins(8, 8, 8, 8);
  optionsLayout->setSpacing(8);

  auto* budgetRow = new QWidget(optionsGroup);
  auto* budgetLayout = new QHBoxLayout(budgetRow);
  budgetLayout->setContentsMargins(0, 0, 0, 0);
  budgetLayout->setSpacing(8);

  auto* budgetLabel = new QLabel(tr("Computational budget"), budgetRow);
  budgetLayout->addWidget(budgetLabel);

  m_levelLabel = new QLabel(budgetRow);
  budgetLayout->addWidget(m_levelLabel);

  m_levelSlider = new QSlider(Qt::Horizontal, budgetRow);
  m_levelSlider->setMinimum(1);
  m_levelSlider->setMaximum(6);
  m_levelSlider->setPageStep(2);
  m_levelSlider->setTickPosition(QSlider::TicksBelow);
  m_levelSlider->setTickInterval(1);
  budgetLayout->addWidget(m_levelSlider, 1);

  m_defaultLevelCheck = new QCheckBox(tr("Default"), budgetRow);
  m_defaultLevelCheck->setChecked(true);
  budgetLayout->addWidget(m_defaultLevelCheck);

  optionsLayout->addWidget(budgetRow);

  auto* hint = new QLabel(tr("Hint: More budget means longer computing time and sometimes improves results.\n"
                             "Using Default is recommended."),
                          optionsGroup);
  hint->setWordWrap(true);
  optionsLayout->addWidget(hint);

  m_resampleCheck = new QCheckBox(tr("Optimal Node Resampling"), optionsGroup);
  m_resampleCheck->setToolTip(
    tr("The final structure will be less redundant when this option is enabled (recommended)."));
  m_resampleCheck->setChecked(true);
  optionsLayout->addWidget(m_resampleCheck);

  m_downsampleCheck = new QCheckBox(tr("Trace on downsampled image"), optionsGroup);
  m_downsampleCheck->setToolTip(tr("Downsamples the selected channel/time before tracing.\n"
                                   "The output SWC is rescaled back to the original image coordinates."));
  m_downsampleCheck->setChecked(false);
  optionsLayout->addWidget(m_downsampleCheck);

  auto* dsRow = new QWidget(optionsGroup);
  auto* dsLayout = new QHBoxLayout(dsRow);
  dsLayout->setContentsMargins(20, 0, 0, 0);
  dsLayout->setSpacing(8);

  dsLayout->addWidget(new QLabel(tr("XY ratio:"), dsRow));
  m_downsampleRatioXYSpin = new QSpinBox(dsRow);
  m_downsampleRatioXYSpin->setMinimum(1);
  m_downsampleRatioXYSpin->setMaximum(std::numeric_limits<int>::max());
  m_downsampleRatioXYSpin->setValue(2);
  dsLayout->addWidget(m_downsampleRatioXYSpin);

  dsLayout->addWidget(new QLabel(tr("Z ratio:"), dsRow));
  m_downsampleRatioZSpin = new QSpinBox(dsRow);
  m_downsampleRatioZSpin->setMinimum(1);
  m_downsampleRatioZSpin->setMaximum(std::numeric_limits<int>::max());
  m_downsampleRatioZSpin->setValue(1);
  dsLayout->addWidget(m_downsampleRatioZSpin);

  dsLayout->addStretch(1);
  optionsLayout->addWidget(dsRow);

  layout->addWidget(optionsGroup);

  auto* outputGroup = new QGroupBox(tr("Output"), this);
  auto* outputLayout = new QVBoxLayout(outputGroup);
  outputLayout->setContentsMargins(8, 8, 8, 8);
  outputLayout->setSpacing(8);

  m_outputSwcWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                            tr("Output SWC:"),
                                            tr("SWC (*.swc)"),
                                            ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Swc"),
                                            ZSystemInfo::instance().lastOpenedObjPath("Swc"),
                                            QBoxLayout::LeftToRight,
                                            outputGroup);
  outputLayout->addWidget(m_outputSwcWidget);

  m_outputLogWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::SaveFile,
                                            tr("Output Log File:"),
                                            tr("Log (*.txt)"),
                                            ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Swc"),
                                            ZSystemInfo::instance().lastOpenedObjPath("Swc"),
                                            QBoxLayout::LeftToRight,
                                            outputGroup);
  outputLayout->addWidget(m_outputLogWidget);

  m_loadResultCheck = new QCheckBox(tr("Load result SWC into the scene when finished"), outputGroup);
  m_loadResultCheck->setChecked(true);
  outputLayout->addWidget(m_loadResultCheck);

  layout->addWidget(outputGroup);

  layout->addWidget(createButtonBox(tr("Start"), tr("Cancel")));

  rebuildImageCombo();
  rebuildChannelAndTimeCombos();
  rebuildSuggestedOutputs();
  updateBudgetUi();

  auto updateDownsampleUi = [this]() {
    CHECK(m_downsampleCheck != nullptr);
    CHECK(m_downsampleRatioXYSpin != nullptr);
    CHECK(m_downsampleRatioZSpin != nullptr);

    const bool enabled = m_downsampleCheck->isChecked();
    m_downsampleRatioXYSpin->setEnabled(enabled);
    m_downsampleRatioZSpin->setEnabled(enabled);
  };
  updateDownsampleUi();

  connect(m_imageCombo, &QComboBox::currentIndexChanged, this, [this](int) {
    rebuildChannelAndTimeCombos();
    rebuildSuggestedOutputs();
  });

  connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int) {
    rebuildSuggestedOutputs();
  });

  connect(m_timeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
    rebuildSuggestedOutputs();
  });

  connect(m_downsampleCheck, &QCheckBox::toggled, this, [this, updateDownsampleUi](bool) {
    updateDownsampleUi();
    rebuildSuggestedOutputs();
  });

  connect(m_downsampleRatioXYSpin, &QSpinBox::valueChanged, this, [this](int) {
    rebuildSuggestedOutputs();
  });

  connect(m_downsampleRatioZSpin, &QSpinBox::valueChanged, this, [this](int) {
    rebuildSuggestedOutputs();
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

  connect(m_defaultLevelCheck, &QCheckBox::toggled, this, [this](bool) {
    updateBudgetUi();
  });
  connect(m_levelSlider, &QSlider::valueChanged, this, [this](int) {
    updateBudgetUi();
  });
}

ZImgProcessDialog::WorkerSpec ZAutoTraceDialog::createWorkerSpec()
{
  const std::optional<size_t> imgIdOpt = selectedImageId();
  if (!imgIdOpt.has_value()) {
    throw ZException("Please select an image to trace.");
  }
  const size_t imgId = *imgIdOpt;

  if (!m_doc.imgDoc().hasObjWithID(imgId)) {
    throw ZException("The selected image no longer exists.");
  }

  const size_t sc = selectedChannel();
  const size_t t = selectedTime();
  const std::array<size_t, 3> ratio = signalRatio();
  const int traceLevelValue = traceLevel();
  const bool doResample = optimalResamplingEnabled();
  const QString outputSwc = outputSwcPath();
  const QString outputLog = outputLogPath();
  const bool loadResult = loadResultEnabled();

  if (outputSwc.isEmpty()) {
    throw ZException("Please select an output SWC file.");
  }
  if (outputLog.isEmpty()) {
    throw ZException("Please select an output log file.");
  }

  // Keep selection explicit and shared across 2D/3D trace UIs: the dialog is just another view
  // of the shared trace settings state.
  m_doc.traceSettings().setSourceSelection(imgId, sc);

  const bool haveAlgoConfig = m_doc.traceSettings().algoConfigInitialized();
  TraceConfig algoOverrides;
  if (haveAlgoConfig) {
    const ZTraceSettings::AlgoConfig cfg = m_doc.traceSettings().algoConfig();
    algoOverrides.minAutoScore = cfg.minAutoScore;
    algoOverrides.minManualScore = cfg.minManualScore;
    algoOverrides.minSeedScore = cfg.minSeedScore;
    algoOverrides.min2dScore = cfg.min2dScore;
    algoOverrides.refit = cfg.refit;
    algoOverrides.spTest = cfg.spTest;
    algoOverrides.crossoverTest = cfg.crossoverTest;
    algoOverrides.tuneEnd = cfg.tuneEnd;
    algoOverrides.edgePath = cfg.edgePath;
    algoOverrides.enhanceMask = cfg.enhanceMask;
    algoOverrides.seedMethod = cfg.seedMethod;
    algoOverrides.recover = cfg.recover;
    algoOverrides.chainScreenCount = cfg.chainScreenCount;
    algoOverrides.maxEucDist = cfg.maxEucDist;
  }

  const bool docHasAnySwc = !m_doc.swcDoc().objs().empty();

  const std::shared_ptr<ZImgPack> imgPack = m_doc.imgDoc().imgPackShared(imgId);
  CHECK(imgPack != nullptr);

  const ZImgInfo info = imgPack->imgInfo();
  const QString channelLabel =
    (sc < info.channelNames.size()) ? info.displayChannelName(sc) : QStringLiteral("Ch%1").arg(sc + 1);
  const QString timeLabel = QStringLiteral("T%1").arg(t + 1);
  const QString ratioLabel = (ratio == std::array<size_t, 3>{1, 1, 1})
                               ? QString{}
                               : QStringLiteral("ratio=[%1,%2,%3]").arg(ratio[0]).arg(ratio[1]).arg(ratio[2]);
  const QString outputSwcLabel = QFileInfo(outputSwc).fileName();
  const QString outputLabel = outputSwcLabel.isEmpty() ? outputSwc : outputSwcLabel;

  const QString traceCfgPath = QDir(ZSystemInfo::jsonDirPath()).absoluteFilePath("trace_config.json");

  WorkerSpec spec;
  spec.workerName = QStringLiteral("Auto Trace");
  spec.taskTitle = ratioLabel.isEmpty() ? QStringLiteral("Auto Trace: %1, %2, %3 -> %4")
                                            .arg(m_doc.objNameWithModifiedMarkerAndID(imgId))
                                            .arg(channelLabel)
                                            .arg(timeLabel)
                                            .arg(outputLabel)
                                        : QStringLiteral("Auto Trace: %1, %2, %3, %4 -> %5")
                                            .arg(m_doc.objNameWithModifiedMarkerAndID(imgId))
                                            .arg(channelLabel)
                                            .arg(timeLabel)
                                            .arg(ratioLabel)
                                            .arg(outputLabel);
  spec.successMessage = QStringLiteral("wrote %1").arg(outputLabel);
  spec.makeWorker = [imgPack,
                     sc,
                     t,
                     ratio,
                     traceCfgPath,
                     traceLevelValue,
                     haveAlgoConfig,
                     algoOverrides,
                     doResample,
                     docHasAnySwc,
                     outputSwc,
                     outputLog]() -> std::unique_ptr<ZImgProcess> {
    auto worker = std::make_unique<ZNeutubeAutoTraceProcess>();
    worker->setLogFile(outputLog);
    worker->setSelectedChannelTime(sc, t);
    worker->setTraceConfigPath(traceCfgPath);
    worker->setTraceLevel(traceLevelValue);
    if (haveAlgoConfig) {
      worker->setAlgoConfigOverrides(algoOverrides);
    } else {
      worker->clearAlgoConfigOverrides();
    }
    worker->setDoResampleAfterTracing(doResample);
    worker->setDocHasAnySwc(docHasAnySwc);
    worker->setOutputSwcPath(outputSwc);
    worker->setSignalDownsampleRatio(ratio);
    worker->setSignalProvider([imgPack, sc, t, ratio](folly::CancellationToken token) -> ZImg {
      maybeCancel(token);

      const ZImgInfo info = imgPack->imgInfo();
      if (sc >= info.numChannels || t >= info.numTimes) {
        throw ZException(
          QStringLiteral("Auto Trace failed: invalid channel/time selection (c=%1, t=%2).").arg(sc).arg(t));
      }

      ZImg signal = imgPack->assembleChannelTime(ratio, sc, t);
      maybeCancel(token);
      return signal;
    });
    return worker;
  };

  spec.onSuccessUi = [imgId, sc, outputSwc, outputLog, loadResult](ZDoc& doc, ZBackgroundTask& task) {
    ZBackgroundTaskManager& tm = doc.backgroundTaskManager();
    if (!QFile::exists(outputSwc)) {
      tm.setTaskMessage(&task, QStringLiteral("no trace result"));
      return;
    }

    if (!loadResult) {
      return;
    }

    ZSwcDoc& swcDoc = doc.swcDoc();
    QString loadError;
    const size_t newSwcId = swcDoc.loadFile(outputSwc, loadError);
    if (newSwcId == 0) {
      tm.failTask(&task,
                  QStringLiteral("Auto Trace finished but failed to load output SWC.\nSWC: %1\nLog: %2\n\n%3")
                    .arg(outputSwc, outputLog, loadError));
      return;
    }

    tm.setTaskMessage(&task, QStringLiteral("created SWC #%1").arg(newSwcId));
    doc.traceSettings().promoteNewSwcTargetToExistingIfStillNew(imgId, sc, newSwcId);
  };

  return spec;
}

std::optional<size_t> ZAutoTraceDialog::selectedImageId() const
{
  CHECK(m_imageCombo != nullptr);
  if (m_imageCombo->currentIndex() <= 0) {
    return std::nullopt;
  }
  const QVariant data = m_imageCombo->currentData();
  if (!data.isValid()) {
    return std::nullopt;
  }
  return static_cast<size_t>(data.toULongLong());
}

size_t ZAutoTraceDialog::selectedChannel() const
{
  CHECK(m_channelCombo != nullptr);
  const QVariant data = m_channelCombo->currentData();
  if (!data.isValid()) {
    return 0;
  }
  return static_cast<size_t>(data.toULongLong());
}

size_t ZAutoTraceDialog::selectedTime() const
{
  CHECK(m_timeCombo != nullptr);
  const QVariant data = m_timeCombo->currentData();
  if (!data.isValid()) {
    return 0;
  }
  return static_cast<size_t>(data.toULongLong());
}

QString ZAutoTraceDialog::outputSwcPath() const
{
  CHECK(m_outputSwcWidget != nullptr);
  return m_outputSwcWidget->getSelectedSaveFile();
}

QString ZAutoTraceDialog::outputLogPath() const
{
  CHECK(m_outputLogWidget != nullptr);
  return m_outputLogWidget->getSelectedSaveFile();
}

bool ZAutoTraceDialog::loadResultEnabled() const
{
  CHECK(m_loadResultCheck != nullptr);
  return m_loadResultCheck->isChecked();
}

int ZAutoTraceDialog::traceLevel() const
{
  CHECK(m_defaultLevelCheck != nullptr);
  CHECK(m_levelSlider != nullptr);

  if (m_defaultLevelCheck->isChecked()) {
    return 0;
  }
  return m_levelSlider->value();
}

bool ZAutoTraceDialog::optimalResamplingEnabled() const
{
  CHECK(m_resampleCheck != nullptr);
  return m_resampleCheck->isChecked();
}

std::array<size_t, 3> ZAutoTraceDialog::signalRatio() const
{
  CHECK(m_downsampleCheck != nullptr);
  CHECK(m_downsampleRatioXYSpin != nullptr);
  CHECK(m_downsampleRatioZSpin != nullptr);

  if (!m_downsampleCheck->isChecked()) {
    return {1, 1, 1};
  }

  const size_t xy = static_cast<size_t>(m_downsampleRatioXYSpin->value());
  const size_t z = static_cast<size_t>(m_downsampleRatioZSpin->value());
  CHECK(xy > 0);
  CHECK(z > 0);
  return {xy, xy, z};
}

void ZAutoTraceDialog::rebuildImageCombo()
{
  CHECK(m_imageCombo != nullptr);

  const QSignalBlocker blocker(*m_imageCombo);
  m_imageCombo->clear();
  m_imageCombo->addItem(tr("Select an image..."));

  const ZImgDoc& imgDoc = m_doc.imgDoc();
  const std::vector<size_t> imgIds = imgDoc.objs();
  for (size_t id : imgIds) {
    m_imageCombo->addItem(m_doc.objNameWithModifiedMarkerAndID(id), QVariant::fromValue(id));
    m_imageCombo->setItemData(m_imageCombo->count() - 1, imageToolTip(m_doc, id), Qt::ToolTipRole);
  }
  disableFirstComboRow(m_imageCombo);

  if (m_doc.traceSettings().sourceImageId().has_value()) {
    const size_t desiredId = *m_doc.traceSettings().sourceImageId();
    for (int i = 1; i < m_imageCombo->count(); ++i) {
      const QVariant data = m_imageCombo->itemData(i);
      if (data.isValid() && static_cast<size_t>(data.toULongLong()) == desiredId) {
        m_imageCombo->setCurrentIndex(i);
        return;
      }
    }
  }

  if (imgIds.size() == 1) {
    m_imageCombo->setCurrentIndex(1);
  }
}

void ZAutoTraceDialog::rebuildChannelAndTimeCombos()
{
  CHECK(m_channelCombo != nullptr);
  CHECK(m_timeCombo != nullptr);

  const std::optional<size_t> imgId = selectedImageId();
  if (!imgId.has_value()) {
    {
      const QSignalBlocker blocker(*m_channelCombo);
      m_channelCombo->clear();
      m_channelCombo->addItem(tr("Select an image..."));
      disableFirstComboRow(m_channelCombo);
      m_channelCombo->setEnabled(false);
    }
    {
      const QSignalBlocker blocker(*m_timeCombo);
      m_timeCombo->clear();
      m_timeCombo->addItem(tr("Select an image..."));
      disableFirstComboRow(m_timeCombo);
      m_timeCombo->setEnabled(false);
    }
    return;
  }

  const ZImgDoc& imgDoc = m_doc.imgDoc();
  if (!imgDoc.hasObjWithID(*imgId)) {
    return;
  }

  const ZImgInfo info = imgDoc.imgPackShared(*imgId)->imgInfo();

  {
    const QSignalBlocker blocker(*m_channelCombo);
    m_channelCombo->clear();
    for (size_t sc = 0; sc < info.numChannels; ++sc) {
      const QString label =
        (sc < info.channelNames.size()) ? info.displayChannelName(sc) : QStringLiteral("Ch%1").arg(sc + 1);
      m_channelCombo->addItem(label, QVariant::fromValue(sc));

      if (sc < info.channelColors.size()) {
        const col4 col = info.channelColors[sc];
        const QColor channelColor(col.r, col.g, col.b);
        const QColor fg = adjustedChannelTextColor(channelColor, m_channelCombo->palette());
        const int idx = m_channelCombo->count() - 1;
        m_channelCombo->setItemData(idx, QBrush(fg), Qt::ForegroundRole);
      }
    }

    const size_t desiredChannel =
      std::min(m_doc.traceSettings().sourceChannel(), info.numChannels > 0 ? (info.numChannels - 1) : 0);
    if (m_channelCombo->count() > 0) {
      m_channelCombo->setCurrentIndex(static_cast<int>(desiredChannel));
    }
    m_channelCombo->setEnabled(m_channelCombo->count() > 1);
  }

  {
    const QSignalBlocker blocker(*m_timeCombo);
    m_timeCombo->clear();
    for (size_t t = 0; t < info.numTimes; ++t) {
      m_timeCombo->addItem(QStringLiteral("T%1").arg(t + 1), QVariant::fromValue(t));
    }
    if (m_timeCombo->count() > 0) {
      m_timeCombo->setCurrentIndex(0);
    }
    m_timeCombo->setEnabled(m_timeCombo->count() > 1);
  }
}

void ZAutoTraceDialog::rebuildSuggestedOutputs()
{
  CHECK(m_outputSwcWidget != nullptr);
  CHECK(m_outputLogWidget != nullptr);

  const std::optional<size_t> imgIdOpt = selectedImageId();
  if (!imgIdOpt.has_value()) {
    return;
  }

  const size_t imgId = *imgIdOpt;
  const size_t c = selectedChannel();
  const size_t t = selectedTime();

  QString baseDir;
  QString baseStem;
  {
    const ZImgDoc& imgDoc = m_doc.imgDoc();
    const QString imgPath = imgDoc.objPath(imgId);

    const QFileInfo fi(imgPath);
    if (fi.exists() && fi.isFile()) {
      baseDir = fi.absolutePath();
      baseStem = fi.completeBaseName();
    } else {
      baseDir = ZSystemInfo::instance().lastOpenedObjPath("Swc");
      baseStem = m_doc.objName(imgId);
    }
  }

  if (baseDir.isEmpty()) {
    baseDir = QDir::homePath();
  }

  baseStem = makeSafeStem(baseStem);
  QString tag = QStringLiteral("_autotrace_c%1_t%2").arg(c + 1).arg(t + 1);
  const std::array<size_t, 3> ratio = signalRatio();
  if (ratio != std::array<size_t, 3>{1, 1, 1}) {
    tag += QStringLiteral("_dsxy%1_z%2").arg(ratio[0]).arg(ratio[2]);
  }
  const QString suggestedSwc = QDir(baseDir).absoluteFilePath(baseStem + tag + QStringLiteral(".swc"));
  const QString suggestedLog = QDir(baseDir).absoluteFilePath(baseStem + tag + QStringLiteral("_log.txt"));

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

void ZAutoTraceDialog::updateBudgetUi()
{
  CHECK(m_defaultLevelCheck != nullptr);
  CHECK(m_levelSlider != nullptr);
  CHECK(m_levelLabel != nullptr);

  const bool useDefault = m_defaultLevelCheck->isChecked();
  m_levelSlider->setDisabled(useDefault);

  if (useDefault) {
    m_levelLabel->setText(QString{});
  } else {
    m_levelLabel->setText(QStringLiteral("(%1/6)").arg(m_levelSlider->value()));
  }
}

} // namespace nim
