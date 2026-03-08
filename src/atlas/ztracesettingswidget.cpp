#include "ztracesettingswidget.h"

#include "zcombobox.h"
#include "zdoc.h"
#include "zimgdoc.h"
#include "zimgpack.h"
#include "zobjdoc.h"
#include "zswcdoc.h"
#include "zsysteminfo.h"
#include "ztracesettings.h"

#include "zneutubetraceconfig.h"
#include "zneutubetracezscale.h"

#include <algorithm>

#include <QCheckBox>
#include <QButtonGroup>
#include <QBrush>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QStringList>
#include <QDir>

namespace nim {

namespace {

void disableFirstComboRow(QComboBox* combo)
{
  if (combo == nullptr) {
    return;
  }

  auto* m = qobject_cast<QStandardItemModel*>(combo->model());
  if (m == nullptr) {
    return;
  }

  QStandardItem* item = m->item(0);
  if (item == nullptr) {
    return;
  }

  item->setEnabled(false);
}

[[nodiscard]] QString imageLabel(ZDoc& doc, size_t imgObjId)
{
  const ZImgDoc& imgDoc = doc.imgDoc();
  if (!imgDoc.hasObjWithID(imgObjId)) {
    return doc.objNameWithModifiedMarkerAndID(imgObjId);
  }

  return doc.objNameWithModifiedMarkerAndID(imgObjId);
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

  // Keep the hue (channel identity) but improve contrast for very dark/light colors.
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

[[nodiscard]] QString formatZToXYRatio(double zToXYRatio)
{
  return QString::number(zToXYRatio, 'g', 6);
}

} // namespace

[[nodiscard]] ZTraceSettings::AlgoConfig algoConfigFromTraceConfig(const TraceConfig& cfg)
{
  ZTraceSettings::AlgoConfig out;
  out.minAutoScore = cfg.minAutoScore;
  out.minManualScore = cfg.minManualScore;
  out.minSeedScore = cfg.minSeedScore;
  out.min2dScore = cfg.min2dScore;
  out.refit = cfg.refit;
  out.spTest = cfg.spTest;
  out.crossoverTest = cfg.crossoverTest;
  out.tuneEnd = cfg.tuneEnd;
  out.edgePath = cfg.edgePath;
  out.enhanceMask = cfg.enhanceMask;
  out.seedMethod = cfg.seedMethod;
  out.recover = cfg.recover;
  out.chainScreenCount = cfg.chainScreenCount;
  out.maxEucDist = cfg.maxEucDist;
  return out;
}

ZTraceSettingsWidget::ZTraceSettingsWidget(ZDoc& doc, QWidget* parent)
  : QScrollArea(parent)
  , m_doc(doc)
{
  setWidgetResizable(true);
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  setMinimumWidth(0);

  auto* content = new QWidget(this);
  content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  content->setMinimumWidth(0);
  setWidget(content);

  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);

  auto* sourceGroup = new QGroupBox(tr("Source"), content);
  auto* sourceForm = new QFormLayout(sourceGroup);
  sourceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  sourceForm->setRowWrapPolicy(QFormLayout::WrapLongRows);

  m_sourceImageCombo = new ZComboBox(sourceGroup);
  m_sourceImageCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  sourceForm->addRow(tr("Image:"), m_sourceImageCombo);

  m_channelCombo = new ZComboBox(sourceGroup);
  m_channelCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  sourceForm->addRow(tr("Channel:"), m_channelCombo);

  layout->addWidget(sourceGroup);

  auto* zToXYRatioGroup = new QGroupBox(tr("Z-to-XY Ratio"), content);
  auto* zToXYRatioForm = new QFormLayout(zToXYRatioGroup);
  zToXYRatioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  zToXYRatioForm->setRowWrapPolicy(QFormLayout::WrapLongRows);

  m_derivedZToXYRatioLabel = new QLabel(zToXYRatioGroup);
  m_derivedZToXYRatioLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  zToXYRatioForm->addRow(tr("Derived:"), m_derivedZToXYRatioLabel);

  auto* overrideRow = new QWidget(zToXYRatioGroup);
  auto* overrideLayout = new QHBoxLayout(overrideRow);
  overrideLayout->setContentsMargins(0, 0, 0, 0);
  overrideLayout->setSpacing(8);

  m_overrideZToXYRatioCheck = new QCheckBox(tr("Use override"), overrideRow);
  overrideLayout->addWidget(m_overrideZToXYRatioCheck);

  m_overrideZToXYRatioSpin = new QDoubleSpinBox(overrideRow);
  m_overrideZToXYRatioSpin->setDecimals(6);
  m_overrideZToXYRatioSpin->setRange(0.000001, 1000000.0);
  m_overrideZToXYRatioSpin->setSingleStep(0.1);
  overrideLayout->addWidget(m_overrideZToXYRatioSpin);
  overrideLayout->addStretch(1);

  zToXYRatioForm->addRow(tr("Override:"), overrideRow);

  m_effectiveZToXYRatioLabel = new QLabel(zToXYRatioGroup);
  m_effectiveZToXYRatioLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  zToXYRatioForm->addRow(tr("Effective:"), m_effectiveZToXYRatioLabel);

  auto* zToXYRatioHint = new QLabel(tr("zToXYRatio = voxelSizeZ / voxelSizeXY. Atlas derives voxelSizeXY as "
                                       "(voxelSizeX + voxelSizeY) / 2 unless you override it here."),
                                    zToXYRatioGroup);
  zToXYRatioHint->setWordWrap(true);
  zToXYRatioForm->addRow(QString(), zToXYRatioHint);

  layout->addWidget(zToXYRatioGroup);

  auto* targetGroup = new QGroupBox(tr("SWC Target"), content);
  auto* targetLayout = new QVBoxLayout(targetGroup);
  targetLayout->setContentsMargins(8, 8, 8, 8);

  m_newSwcRadio = new QRadioButton(tr("Create new SWC"), targetGroup);
  targetLayout->addWidget(m_newSwcRadio);

  auto* existingRow = new QWidget(targetGroup);
  auto* existingLayout = new QHBoxLayout(existingRow);
  existingLayout->setContentsMargins(0, 0, 0, 0);
  existingLayout->setSpacing(8);

  m_existingSwcRadio = new QRadioButton(tr("Attach to existing:"), existingRow);
  m_existingSwcCombo = new ZComboBox(existingRow);
  m_existingSwcCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  existingLayout->addWidget(m_existingSwcRadio);
  existingLayout->addWidget(m_existingSwcCombo, 1);
  targetLayout->addWidget(existingRow);

  m_swcTargetButtonGroup = new QButtonGroup(targetGroup);
  m_swcTargetButtonGroup->setExclusive(true);
  m_swcTargetButtonGroup->addButton(m_newSwcRadio);
  m_swcTargetButtonGroup->addButton(m_existingSwcRadio);

  layout->addWidget(targetGroup);

  auto* algoGroup = new QGroupBox(tr("Tracing Config"), content);
  auto* algoForm = new QFormLayout(algoGroup);
  algoForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  algoForm->setRowWrapPolicy(QFormLayout::WrapLongRows);

  m_minAutoScoreSpin = new QDoubleSpinBox(algoGroup);
  m_minAutoScoreSpin->setDecimals(3);
  m_minAutoScoreSpin->setRange(0.0, 1.0);
  m_minAutoScoreSpin->setSingleStep(0.01);
  m_minAutoScoreSpin->setToolTip(tr("Minimal score threshold for auto tracing (trace_config.json: minimalScoreAuto)."));
  algoForm->addRow(tr("Min score (auto):"), m_minAutoScoreSpin);

  m_minManualScoreSpin = new QDoubleSpinBox(algoGroup);
  m_minManualScoreSpin->setDecimals(3);
  m_minManualScoreSpin->setRange(0.0, 1.0);
  m_minManualScoreSpin->setSingleStep(0.01);
  m_minManualScoreSpin->setToolTip(
    tr("Minimal score threshold for interactive/seed tracing (trace_config.json: minimalScoreManual)."));
  algoForm->addRow(tr("Min score (interactive):"), m_minManualScoreSpin);

  m_minSeedScoreSpin = new QDoubleSpinBox(algoGroup);
  m_minSeedScoreSpin->setDecimals(3);
  m_minSeedScoreSpin->setRange(0.0, 1.0);
  m_minSeedScoreSpin->setSingleStep(0.01);
  m_minSeedScoreSpin->setToolTip(
    tr("Minimal score threshold for seed extraction (trace_config.json: minimalScoreSeed)."));
  algoForm->addRow(tr("Min score (seed):"), m_minSeedScoreSpin);

  m_min2dScoreSpin = new QDoubleSpinBox(algoGroup);
  m_min2dScoreSpin->setDecimals(3);
  m_min2dScoreSpin->setRange(0.0, 1.0);
  m_min2dScoreSpin->setSingleStep(0.01);
  m_min2dScoreSpin->setToolTip(tr("Minimal score threshold for 2D tracing (trace_config.json: minimalScore2d)."));
  algoForm->addRow(tr("Min score (2D):"), m_min2dScoreSpin);

  m_refitCheck = new QCheckBox(tr("Hard fitting (refit)"), algoGroup);
  m_refitCheck->setToolTip(tr("Enable harder fitting (trace_config.json: refit)."));
  algoForm->addRow(tr("Fitting:"), m_refitCheck);

  m_tuneEndCheck = new QCheckBox(tr("Tune end"), algoGroup);
  m_tuneEndCheck->setToolTip(tr("Enable end tuning (trace_config.json: tuneEnd)."));
  algoForm->addRow(tr("Trace:"), m_tuneEndCheck);

  m_recoverSpin = new QSpinBox(algoGroup);
  m_recoverSpin->setRange(0, 9);
  m_recoverSpin->setToolTip(tr("Signal recover level (trace_config.json: recover)."));
  algoForm->addRow(tr("Recover level:"), m_recoverSpin);

  m_maxEucDistSpin = new QDoubleSpinBox(algoGroup);
  m_maxEucDistSpin->setDecimals(3);
  m_maxEucDistSpin->setRange(0.0, 9999.0);
  m_maxEucDistSpin->setSingleStep(1.00);
  m_maxEucDistSpin->setToolTip(tr("Max Euclidean distance for assembly (trace_config.json: maxEucDist)."));
  algoForm->addRow(tr("Max gap:"), m_maxEucDistSpin);

  m_spTestCheck = new QCheckBox(tr("Use geodesic (spTest)"), algoGroup);
  m_spTestCheck->setToolTip(tr("Use geodesic-based connection test (trace_config.json: spTest)."));
  algoForm->addRow(tr("Assemble:"), m_spTestCheck);

  m_crossoverCheck = new QCheckBox(tr("Detect crossover"), algoGroup);
  m_crossoverCheck->setToolTip(tr("Enable crossover detection (trace_config.json: crossoverTest)."));
  algoForm->addRow(tr("Assemble:"), m_crossoverCheck);

  m_enhanceMaskCheck = new QCheckBox(tr("Signal enhancement"), algoGroup);
  m_enhanceMaskCheck->setToolTip(tr("Enable mask enhancement (trace_config.json: enhanceMask)."));
  algoForm->addRow(tr("Preprocess:"), m_enhanceMaskCheck);

  m_resetAlgoButton = new QPushButton(tr("Reset to defaults"), algoGroup);
  m_resetAlgoButton->setToolTip(tr("Reload defaults from the bundled trace_config.json."));
  algoForm->addRow(QString(), m_resetAlgoButton);

  layout->addWidget(algoGroup);

  auto* mappingGroup = new QGroupBox(tr("Trace Mapping"), content);
  auto* mappingLayout = new QVBoxLayout(mappingGroup);
  mappingLayout->setContentsMargins(8, 8, 8, 8);
  mappingLayout->setSpacing(6);

  m_mappingLabel = new QLabel(mappingGroup);
  m_mappingLabel->setWordWrap(true);
  m_mappingLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_mappingLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  mappingLayout->addWidget(m_mappingLabel);

  layout->addWidget(mappingGroup);
  layout->addStretch(1);

  connect(m_sourceImageCombo, &QComboBox::currentIndexChanged, this, [this](int) {
    if (m_updating) {
      return;
    }
    updateChannelComboForCurrentSource();
    pushSourceUiToSettings();
  });
  connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int) {
    if (m_updating) {
      return;
    }
    pushSourceUiToSettings();
  });

  connect(m_existingSwcRadio, &QRadioButton::toggled, this, [this](bool on) {
    m_existingSwcCombo->setEnabled(on);
    if (m_updating) {
      return;
    }
    pushTargetUiToSettings();
  });
  connect(m_newSwcRadio, &QRadioButton::toggled, this, [this](bool) {
    if (m_updating) {
      return;
    }
    pushTargetUiToSettings();
  });
  connect(m_existingSwcCombo, &QComboBox::currentIndexChanged, this, [this](int) {
    if (m_updating) {
      return;
    }
    pushTargetUiToSettings();
  });

  connect(m_overrideZToXYRatioCheck, &QCheckBox::toggled, this, [this](bool) {
    if (m_updating) {
      return;
    }
    pushZToXYRatioUiToSettings();
  });
  connect(m_overrideZToXYRatioSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
    if (m_updating) {
      return;
    }
    pushZToXYRatioUiToSettings();
  });

  connect(m_minAutoScoreSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_minManualScoreSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_minSeedScoreSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_min2dScoreSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_recoverSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_maxEucDistSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });

  connect(m_refitCheck, &QCheckBox::toggled, this, [this](bool) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_tuneEndCheck, &QCheckBox::toggled, this, [this](bool) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_spTestCheck, &QCheckBox::toggled, this, [this](bool) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_crossoverCheck, &QCheckBox::toggled, this, [this](bool) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });
  connect(m_enhanceMaskCheck, &QCheckBox::toggled, this, [this](bool) {
    if (m_updating) {
      return;
    }
    pushAlgoUiToSettings();
  });

  connect(m_resetAlgoButton, &QPushButton::clicked, this, [this]() {
    TraceConfig cfg;
    const QString traceCfgPath = QDir(ZSystemInfo::jsonDirPath()).absoluteFilePath("trace_config.json");
    (void)loadTraceConfigLegacyLike(traceCfgPath.toStdString(), cfg);
    m_doc.traceSettings().setAlgoConfig(algoConfigFromTraceConfig(cfg));
  });

  connect(&m_doc, &ZDoc::objAdded, this, [this](size_t, ZObjDoc*) {
    refreshFromDoc();
  });
  connect(&m_doc, &ZDoc::objRemoved, this, [this](size_t, ZObjDoc*) {
    refreshFromDoc();
  });
  connect(&m_doc, &ZDoc::objInfoChanged, this, [this](size_t) {
    refreshFromDoc();
  });

  connect(&m_doc.traceSettings(), &ZTraceSettings::changed, this, [this]() {
    applySettingsToUi();
  });

  initializeAlgoConfigFromLegacyDefaultsIfUnset();
  refreshFromDoc();
}

void ZTraceSettingsWidget::initializeAlgoConfigFromLegacyDefaultsIfUnset()
{
  ZTraceSettings& settings = m_doc.traceSettings();
  if (settings.algoConfigInitialized()) {
    return;
  }

  TraceConfig cfg;
  const QString traceCfgPath = QDir(ZSystemInfo::jsonDirPath()).absoluteFilePath("trace_config.json");
  (void)loadTraceConfigLegacyLike(traceCfgPath.toStdString(), cfg);
  settings.initializeAlgoConfigIfUnset(algoConfigFromTraceConfig(cfg));
}

void ZTraceSettingsWidget::refreshFromDoc()
{
  if (m_updating) {
    return;
  }

  m_updating = true;
  rebuildSourceImageCombo();
  rebuildTargetSwcCombo();
  m_updating = false;

  applySettingsToUi();
}

void ZTraceSettingsWidget::rebuildSourceImageCombo()
{
  CHECK(m_sourceImageCombo != nullptr);

  const QSignalBlocker blocker(*m_sourceImageCombo);
  m_sourceImageCombo->clear();
  m_sourceImageCombo->addItem(tr("Select an image..."));

  const ZImgDoc& imgDoc = m_doc.imgDoc();
  const std::vector<size_t> imgIds = imgDoc.objs();
  for (size_t id : imgIds) {
    m_sourceImageCombo->addItem(imageLabel(m_doc, id), QVariant::fromValue(id));
    m_sourceImageCombo->setItemData(m_sourceImageCombo->count() - 1, imageToolTip(m_doc, id), Qt::ToolTipRole);
  }

  disableFirstComboRow(m_sourceImageCombo);
}

void ZTraceSettingsWidget::rebuildTargetSwcCombo()
{
  CHECK(m_existingSwcRadio != nullptr);
  CHECK(m_existingSwcCombo != nullptr);
  CHECK(m_newSwcRadio != nullptr);

  const QSignalBlocker blocker(*m_existingSwcCombo);
  m_existingSwcCombo->clear();
  m_existingSwcCombo->addItem(tr("Select an SWC..."));

  ZSwcDoc& swcDoc = m_doc.swcDoc();
  const std::vector<size_t> swcIds = swcDoc.objs();
  for (size_t id : swcIds) {
    m_existingSwcCombo->addItem(swcDoc.objNameWithModifiedMarkerAndID(id), QVariant::fromValue(id));
  }
  disableFirstComboRow(m_existingSwcCombo);

  const bool hasSwc = !swcIds.empty();
  m_existingSwcRadio->setEnabled(hasSwc);
  if (!hasSwc) {
    m_newSwcRadio->setChecked(true);
    m_existingSwcCombo->setEnabled(false);
  }
}

std::optional<size_t> ZTraceSettingsWidget::currentSourceImageIdFromUi() const
{
  CHECK(m_sourceImageCombo != nullptr);
  if (m_sourceImageCombo->currentIndex() <= 0) {
    return std::nullopt;
  }
  const QVariant data = m_sourceImageCombo->currentData();
  if (!data.isValid()) {
    return std::nullopt;
  }
  return static_cast<size_t>(data.toULongLong());
}

std::optional<size_t> ZTraceSettingsWidget::currentTargetSwcIdFromUi() const
{
  CHECK(m_existingSwcCombo != nullptr);
  if (m_existingSwcCombo->currentIndex() <= 0) {
    return std::nullopt;
  }
  const QVariant data = m_existingSwcCombo->currentData();
  if (!data.isValid()) {
    return std::nullopt;
  }
  return static_cast<size_t>(data.toULongLong());
}

void ZTraceSettingsWidget::updateChannelComboForCurrentSource()
{
  CHECK(m_channelCombo != nullptr);

  const QSignalBlocker blocker(*m_channelCombo);
  m_channelCombo->clear();
  m_channelCombo->setEnabled(false);

  const std::optional<size_t> imgId = currentSourceImageIdFromUi();
  if (!imgId.has_value()) {
    m_channelCombo->addItem(tr("Select an image..."));
    disableFirstComboRow(m_channelCombo);
    return;
  }

  const ZImgDoc& imgDoc = m_doc.imgDoc();
  if (!imgDoc.hasObjWithID(*imgId)) {
    m_channelCombo->addItem(tr("Image not found"));
    disableFirstComboRow(m_channelCombo);
    return;
  }

  const ZImgInfo info = imgDoc.imgPackShared(*imgId)->imgInfo();
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
  m_channelCombo->setEnabled(m_channelCombo->count() > 0);
}

void ZTraceSettingsWidget::applySettingsToUi()
{
  if (m_updating) {
    return;
  }

  m_updating = true;

  const ZTraceSettings& settings = m_doc.traceSettings();

  CHECK(m_sourceImageCombo != nullptr);
  CHECK(m_channelCombo != nullptr);
  CHECK(m_existingSwcRadio != nullptr);
  CHECK(m_newSwcRadio != nullptr);
  CHECK(m_existingSwcCombo != nullptr);

  if (const std::optional<size_t> storedImgId = settings.sourceImageId(); storedImgId.has_value()) {
    bool found = false;
    for (int i = 1; i < m_sourceImageCombo->count(); ++i) {
      const QVariant data = m_sourceImageCombo->itemData(i);
      if (data.isValid() && static_cast<size_t>(data.toULongLong()) == *storedImgId) {
        m_sourceImageCombo->setCurrentIndex(i);
        found = true;
        break;
      }
    }
    if (!found) {
      m_sourceImageCombo->setCurrentIndex(0);
    }
  } else {
    m_sourceImageCombo->setCurrentIndex(0);
  }

  updateChannelComboForCurrentSource();

  if (settings.swcTargetMode() == ZTraceSettings::SwcTargetMode::ExistingSwc && m_existingSwcRadio->isEnabled()) {
    m_existingSwcRadio->setChecked(true);
    m_existingSwcCombo->setEnabled(true);
  } else {
    m_newSwcRadio->setChecked(true);
    m_existingSwcCombo->setEnabled(false);
  }

  if (const std::optional<size_t> storedSwcId = settings.targetSwcId(); storedSwcId.has_value()) {
    bool found = false;
    for (int i = 1; i < m_existingSwcCombo->count(); ++i) {
      const QVariant data = m_existingSwcCombo->itemData(i);
      if (data.isValid() && static_cast<size_t>(data.toULongLong()) == *storedSwcId) {
        m_existingSwcCombo->setCurrentIndex(i);
        found = true;
        break;
      }
    }
    if (!found) {
      m_existingSwcCombo->setCurrentIndex(0);
    }
  } else {
    m_existingSwcCombo->setCurrentIndex(0);
  }

  updateMappingLabel();
  updateZToXYRatioUi();

  {
    const ZTraceSettings::AlgoConfig cfg = settings.algoConfig();
    const QSignalBlocker b0(*m_minAutoScoreSpin);
    const QSignalBlocker b1(*m_minManualScoreSpin);
    const QSignalBlocker b2(*m_minSeedScoreSpin);
    const QSignalBlocker b3(*m_min2dScoreSpin);
    const QSignalBlocker b4(*m_refitCheck);
    const QSignalBlocker b5(*m_tuneEndCheck);
    const QSignalBlocker b6(*m_recoverSpin);
    const QSignalBlocker b7(*m_maxEucDistSpin);
    const QSignalBlocker b8(*m_spTestCheck);
    const QSignalBlocker b9(*m_crossoverCheck);
    const QSignalBlocker b10(*m_enhanceMaskCheck);

    m_minAutoScoreSpin->setValue(cfg.minAutoScore);
    m_minManualScoreSpin->setValue(cfg.minManualScore);
    m_minSeedScoreSpin->setValue(cfg.minSeedScore);
    m_min2dScoreSpin->setValue(cfg.min2dScore);
    m_refitCheck->setChecked(cfg.refit);
    m_tuneEndCheck->setChecked(cfg.tuneEnd);
    m_recoverSpin->setValue(cfg.recover);
    m_maxEucDistSpin->setValue(cfg.maxEucDist);
    m_spTestCheck->setChecked(cfg.spTest);
    m_crossoverCheck->setChecked(cfg.crossoverTest);
    m_enhanceMaskCheck->setChecked(cfg.enhanceMask);
  }

  m_updating = false;
}

void ZTraceSettingsWidget::updateMappingLabel()
{
  CHECK(m_mappingLabel != nullptr);

  const ZTraceSettings& settings = m_doc.traceSettings();

  const std::optional<size_t> imgIdOpt = settings.sourceImageId();
  if (!imgIdOpt.has_value()) {
    m_mappingLabel->setText(tr("No trace mapping configured.\nSelect a source image and channel."));
    return;
  }

  const size_t imgId = *imgIdOpt;
  QString sourceName;
  if (m_doc.imgDoc().hasObjWithID(imgId)) {
    sourceName = m_doc.objNameWithModifiedMarkerAndID(imgId);
  } else {
    sourceName = tr("Image #%1 (missing)").arg(imgId);
  }

  const size_t sc = settings.sourceChannel();

  QString targetText;
  if (settings.swcTargetMode() == ZTraceSettings::SwcTargetMode::NewSwc) {
    targetText = tr("New SWC (will be created on trace)");
  } else {
    const std::optional<size_t> swcIdOpt = settings.targetSwcId();
    if (!swcIdOpt.has_value()) {
      targetText = tr("Existing SWC (not selected)");
    } else {
      const size_t swcId = *swcIdOpt;
      ZSwcDoc& swcDoc = m_doc.swcDoc();
      if (!swcDoc.hasObjWithID(swcId)) {
        targetText = tr("SWC #%1 (missing)").arg(swcId);
      } else {
        targetText = swcDoc.objNameWithModifiedMarkerAndID(swcId);
      }
    }
  }

  QStringList lines;
  lines << tr("Status: %1").arg(settings.traceInProgress() ? tr("Tracing...") : tr("Idle"));
  lines << tr("Source: %1").arg(sourceName);
  lines << tr("Channel: %1").arg(sc);
  lines << tr("Target SWC: %1").arg(targetText);
  if (settings.swcTargetMode() == ZTraceSettings::SwcTargetMode::NewSwc) {
    lines << tr("Note: after the first successful trace, Atlas auto-selects the created SWC here.");
  }

  m_mappingLabel->setText(lines.join('\n'));
}

std::optional<double> ZTraceSettingsWidget::derivedZToXYRatioForCurrentSource() const
{
  const std::optional<size_t> imgIdOpt = currentSourceImageIdFromUi();
  if (!imgIdOpt.has_value()) {
    return std::nullopt;
  }

  const ZImgDoc& imgDoc = m_doc.imgDoc();
  if (!imgDoc.hasObjWithID(*imgIdOpt)) {
    return std::nullopt;
  }

  const std::shared_ptr<ZImgPack> imgPack = imgDoc.imgPackShared(*imgIdOpt);
  CHECK(imgPack != nullptr);
  return preferredZToXYRatioFromImgInfoLegacyLike(imgPack->imgInfo());
}

void ZTraceSettingsWidget::updateZToXYRatioUi()
{
  CHECK(m_derivedZToXYRatioLabel != nullptr);
  CHECK(m_overrideZToXYRatioCheck != nullptr);
  CHECK(m_overrideZToXYRatioSpin != nullptr);
  CHECK(m_effectiveZToXYRatioLabel != nullptr);

  const std::optional<size_t> imgIdOpt = currentSourceImageIdFromUi();

  size_t sourceChannel = 0;
  if (m_channelCombo != nullptr && m_channelCombo->isEnabled() && m_channelCombo->count() > 0) {
    const QVariant scData = m_channelCombo->currentData();
    if (scData.isValid()) {
      sourceChannel = static_cast<size_t>(scData.toULongLong());
    }
  }

  const std::optional<double> derivedZToXYRatio = derivedZToXYRatioForCurrentSource();
  const std::optional<double> overrideZToXYRatio =
    m_doc.traceSettings().zToXYRatioOverrideForSelection(imgIdOpt, sourceChannel);

  {
    const QSignalBlocker blockerCheck(*m_overrideZToXYRatioCheck);
    const QSignalBlocker blockerSpin(*m_overrideZToXYRatioSpin);

    m_overrideZToXYRatioCheck->setEnabled(imgIdOpt.has_value());
    m_overrideZToXYRatioCheck->setChecked(overrideZToXYRatio.has_value());
    m_overrideZToXYRatioSpin->setValue(overrideZToXYRatio.value_or(derivedZToXYRatio.value_or(1.0)));
    m_overrideZToXYRatioSpin->setEnabled(imgIdOpt.has_value() && overrideZToXYRatio.has_value());
  }

  if (derivedZToXYRatio.has_value()) {
    m_derivedZToXYRatioLabel->setText(formatZToXYRatio(*derivedZToXYRatio));
  } else {
    m_derivedZToXYRatioLabel->setText(tr("N/A"));
  }

  if (overrideZToXYRatio.has_value()) {
    m_effectiveZToXYRatioLabel->setText(tr("%1 (override)").arg(formatZToXYRatio(*overrideZToXYRatio)));
  } else if (derivedZToXYRatio.has_value()) {
    m_effectiveZToXYRatioLabel->setText(tr("%1 (metadata)").arg(formatZToXYRatio(*derivedZToXYRatio)));
  } else {
    m_effectiveZToXYRatioLabel->setText(tr("N/A"));
  }
}

void ZTraceSettingsWidget::pushSourceUiToSettings()
{
  if (m_updating) {
    return;
  }

  ZTraceSettings& settings = m_doc.traceSettings();

  const std::optional<size_t> sourceImageId = currentSourceImageIdFromUi();

  size_t sourceChannel = settings.sourceChannel();
  if (m_channelCombo != nullptr && m_channelCombo->isEnabled() && m_channelCombo->count() > 0) {
    const QVariant scData = m_channelCombo->currentData();
    if (scData.isValid()) {
      sourceChannel = static_cast<size_t>(scData.toULongLong());
    }
  }

  settings.setSourceSelection(sourceImageId, sourceChannel);
}

void ZTraceSettingsWidget::pushTargetUiToSettings()
{
  if (m_updating) {
    return;
  }

  ZTraceSettings& settings = m_doc.traceSettings();

  std::optional<size_t> targetSwcId;
  ZTraceSettings::SwcTargetMode mode = ZTraceSettings::SwcTargetMode::NewSwc;
  if (m_existingSwcRadio != nullptr && m_existingSwcRadio->isChecked()) {
    mode = ZTraceSettings::SwcTargetMode::ExistingSwc;
    targetSwcId = currentTargetSwcIdFromUi();
  }

  settings.setTargetSelection(mode, targetSwcId);
}

void ZTraceSettingsWidget::pushZToXYRatioUiToSettings()
{
  if (m_updating) {
    return;
  }

  const std::optional<size_t> sourceImageId = currentSourceImageIdFromUi();
  if (!sourceImageId.has_value()) {
    return;
  }

  size_t sourceChannel = 0;
  if (m_channelCombo != nullptr && m_channelCombo->isEnabled() && m_channelCombo->count() > 0) {
    const QVariant scData = m_channelCombo->currentData();
    if (scData.isValid()) {
      sourceChannel = static_cast<size_t>(scData.toULongLong());
    }
  }

  std::optional<double> zToXYRatioOverride;
  if (m_overrideZToXYRatioCheck != nullptr && m_overrideZToXYRatioCheck->isChecked()) {
    CHECK(m_overrideZToXYRatioSpin != nullptr);
    zToXYRatioOverride = m_overrideZToXYRatioSpin->value();
  }

  m_doc.traceSettings().setZToXYRatioOverrideForSelection(sourceImageId, sourceChannel, zToXYRatioOverride);
}

void ZTraceSettingsWidget::pushAlgoUiToSettings()
{
  if (m_updating) {
    return;
  }

  ZTraceSettings& settings = m_doc.traceSettings();

  ZTraceSettings::AlgoConfig cfg = settings.algoConfig();
  if (m_minAutoScoreSpin != nullptr) {
    cfg.minAutoScore = m_minAutoScoreSpin->value();
  }
  if (m_minManualScoreSpin != nullptr) {
    cfg.minManualScore = m_minManualScoreSpin->value();
  }
  if (m_minSeedScoreSpin != nullptr) {
    cfg.minSeedScore = m_minSeedScoreSpin->value();
  }
  if (m_min2dScoreSpin != nullptr) {
    cfg.min2dScore = m_min2dScoreSpin->value();
  }
  if (m_refitCheck != nullptr) {
    cfg.refit = m_refitCheck->isChecked();
  }
  if (m_tuneEndCheck != nullptr) {
    cfg.tuneEnd = m_tuneEndCheck->isChecked();
  }
  if (m_recoverSpin != nullptr) {
    cfg.recover = m_recoverSpin->value();
  }
  if (m_maxEucDistSpin != nullptr) {
    cfg.maxEucDist = m_maxEucDistSpin->value();
  }
  if (m_spTestCheck != nullptr) {
    cfg.spTest = m_spTestCheck->isChecked();
  }
  if (m_crossoverCheck != nullptr) {
    cfg.crossoverTest = m_crossoverCheck->isChecked();
  }
  if (m_enhanceMaskCheck != nullptr) {
    cfg.enhanceMask = m_enhanceMaskCheck->isChecked();
  }
  settings.setAlgoConfig(cfg);
}

} // namespace nim
