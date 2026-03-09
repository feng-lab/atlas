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
#include "zneutubeblockedautotraceprocess.h"
#include "zneutubetracezscale.h"
#include "zcancellation.h"

#include <folly/OperationCancelled.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/WithCancellation.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QBrush>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
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
#include <cmath>
#include <limits>

namespace nim {

namespace {

constexpr int kBlockedAutoTraceMinBlockSizeVoxels = 1024;
constexpr int kBlockedAutoTraceMinPaddingVoxels = 128;

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

[[nodiscard]] QString formatZToXYRatio(double zToXYRatio)
{
  return QString::number(zToXYRatio, 'g', 6);
}

struct BlockedSessionManifestLoadResult
{
  bool hasManifest = false;
  bool ok = false;
  QString manifestPath;
  QString error;
  std::array<size_t, 3> ratio = {1, 1, 1};
  double zToXYRatio = 1.0;
  // {coreX, coreY, coreZ, halo}
  std::array<int64_t, 4> block = {0, 0, 0, 0};
};

[[nodiscard]] BlockedSessionManifestLoadResult tryLoadBlockedSessionManifestFromDir(const QString& sessionDir)
{
  BlockedSessionManifestLoadResult res;

  const QString cleanDir = QDir::cleanPath(sessionDir);
  if (cleanDir.trimmed().isEmpty()) {
    return res;
  }

  res.manifestPath = QDir(cleanDir).absoluteFilePath(QStringLiteral("manifest.json"));
  if (!QFileInfo::exists(res.manifestPath)) {
    return res;
  }

  res.hasManifest = true;

  auto requireNumber = [](const json::object& o, const char* key) -> const json::value& {
    const auto it = o.find(key);
    if (it == o.end()) {
      throw ZException(QStringLiteral("Missing key '%1'").arg(QString::fromUtf8(key)));
    }
    if (!it->value().is_number()) {
      throw ZException(QStringLiteral("Invalid '%1' type: expected number, got %2")
                         .arg(QString::fromUtf8(key), QString::fromStdString(jsonTypeName(it->value()))));
    }
    return it->value();
  };

  auto requireObject = [](const json::object& o, const char* key) -> const json::object& {
    const auto it = o.find(key);
    if (it == o.end()) {
      throw ZException(QStringLiteral("Missing key '%1'").arg(QString::fromUtf8(key)));
    }
    if (!it->value().is_object()) {
      throw ZException(QStringLiteral("Invalid '%1' type: expected object, got %2")
                         .arg(QString::fromUtf8(key), QString::fromStdString(jsonTypeName(it->value()))));
    }
    return it->value().as_object();
  };

  auto requireArray = [](const json::object& o, const char* key) -> const json::array& {
    const auto it = o.find(key);
    if (it == o.end()) {
      throw ZException(QStringLiteral("Missing key '%1'").arg(QString::fromUtf8(key)));
    }
    if (!it->value().is_array()) {
      throw ZException(QStringLiteral("Invalid '%1' type: expected array, got %2")
                         .arg(QString::fromUtf8(key), QString::fromStdString(jsonTypeName(it->value()))));
    }
    return it->value().as_array();
  };

  try {
    const json::object jo = loadJsonObject(res.manifestPath);

    const int fmt = json::value_to<int>(requireNumber(jo, "format_version"));
    if (fmt != kBlockedAutoTraceManifestFormatVersion) {
      throw ZException(QStringLiteral("Unsupported manifest format_version=%1").arg(fmt));
    }

    const json::array& ratio = requireArray(jo, "signal_downsample_ratio");
    if (ratio.size() != 3) {
      throw ZException(QStringLiteral("Invalid signal_downsample_ratio: expected array[3], got array[%1].")
                         .arg(static_cast<int>(ratio.size())));
    }
    const size_t rx = json::value_to<size_t>(ratio.at(0));
    const size_t ry = json::value_to<size_t>(ratio.at(1));
    const size_t rz = json::value_to<size_t>(ratio.at(2));
    if (rx == 0 || ry == 0 || rz == 0) {
      throw ZException("Invalid signal_downsample_ratio: values must be > 0.");
    }
    if (rx != ry) {
      throw ZException(QStringLiteral("Invalid signal_downsample_ratio: XY ratio must be uniform, got [%1,%2,%3].")
                         .arg(static_cast<qulonglong>(rx))
                         .arg(static_cast<qulonglong>(ry))
                         .arg(static_cast<qulonglong>(rz)));
    }
    res.ratio = {rx, ry, rz};

    const double zToXYRatio = json::value_to<double>(requireNumber(jo, "z_scale"));
    if (!std::isfinite(zToXYRatio) || !(zToXYRatio > 0.0)) {
      throw ZException(QStringLiteral("Invalid z_scale: expected a finite number > 0, got %1.").arg(zToXYRatio));
    }
    res.zToXYRatio = zToXYRatio;

    const json::object& block = requireObject(jo, "block");
    const int64_t coreX = json::value_to<int64_t>(requireNumber(block, "core_x"));
    const int64_t coreY = json::value_to<int64_t>(requireNumber(block, "core_y"));
    const int64_t coreZ = json::value_to<int64_t>(requireNumber(block, "core_z"));
    const int64_t halo = json::value_to<int64_t>(requireNumber(block, "halo"));

    if (coreX <= 0 || coreY <= 0 || coreZ <= 0) {
      throw ZException(QStringLiteral("Invalid block core size: (%1,%2,%3) must all be > 0.")
                         .arg(static_cast<long long>(coreX))
                         .arg(static_cast<long long>(coreY))
                         .arg(static_cast<long long>(coreZ)));
    }
    if (halo < 0) {
      throw ZException(QStringLiteral("Invalid block halo: %1 must be >= 0.").arg(static_cast<long long>(halo)));
    }
    if (coreX < kBlockedAutoTraceMinBlockSizeVoxels || coreY < kBlockedAutoTraceMinBlockSizeVoxels ||
        coreZ < kBlockedAutoTraceMinBlockSizeVoxels) {
      throw ZException(QStringLiteral("Blocked auto trace: block core size too small for resume.\n"
                                      "Manifest core=(%1,%2,%3), but the hard minimum is %4 voxels.")
                         .arg(static_cast<long long>(coreX))
                         .arg(static_cast<long long>(coreY))
                         .arg(static_cast<long long>(coreZ))
                         .arg(kBlockedAutoTraceMinBlockSizeVoxels));
    }
    if (halo < kBlockedAutoTraceMinPaddingVoxels) {
      throw ZException(QStringLiteral("Blocked auto trace: halo/padding too small for resume.\n"
                                      "Manifest halo=%1, but the hard minimum is %2 voxels.")
                         .arg(static_cast<long long>(halo))
                         .arg(kBlockedAutoTraceMinPaddingVoxels));
    }
    res.block = {coreX, coreY, coreZ, halo};
    res.ok = true;
  }
  catch (const ZException& e) {
    res.error = e.what();
  }
  catch (const std::exception& e) {
    res.error = QString::fromUtf8(e.what());
  }

  return res;
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

  auto* zToXYRatioGroup = new QGroupBox(tr("Z-to-XY Ratio"), this);
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

  auto* zToXYRatioHint =
    new QLabel(tr("zToXYRatio = voxelSizeZ / voxelSizeXY. Atlas derives voxelSizeXY as "
                  "(voxelSizeX + voxelSizeY) / 2 after the current downsample ratio unless you override it here."),
               zToXYRatioGroup);
  zToXYRatioHint->setWordWrap(true);
  zToXYRatioForm->addRow(QString(), zToXYRatioHint);

  layout->addWidget(zToXYRatioGroup);

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

  m_blockedTraceCheck = new QCheckBox(tr("Blocked tracing (large image / resumable)"), optionsGroup);
  m_blockedTraceCheck->setToolTip(
    tr("Runs auto tracing block-by-block (core + halo) to support very large disk-cached / Neuroglancer datasets.\n"
       "Writes an append-only session directory into the selected output folder to support crash-safe resume.\n"
       "For small in-memory images, the legacy whole-volume tracer is usually faster."));
  m_blockedTraceCheck->setChecked(true);
  optionsLayout->addWidget(m_blockedTraceCheck);

  auto* blockedParamsRow = new QWidget(optionsGroup);
  auto* blockedParamsLayout = new QHBoxLayout(blockedParamsRow);
  blockedParamsLayout->setContentsMargins(20, 0, 0, 0);
  blockedParamsLayout->setSpacing(8);

  blockedParamsLayout->addWidget(new QLabel(tr("Block size:"), blockedParamsRow));
  m_blockedTraceBlockSizeSpin = new QSpinBox(blockedParamsRow);
  m_blockedTraceBlockSizeSpin->setMinimum(kBlockedAutoTraceMinBlockSizeVoxels);
  m_blockedTraceBlockSizeSpin->setMaximum(std::numeric_limits<int>::max());
  m_blockedTraceBlockSizeSpin->setValue(kBlockedAutoTraceMinBlockSizeVoxels);
  m_blockedTraceBlockSizeSpin->setToolTip(tr("Core block size in tracing voxels (after downsample).\n"
                                             "This UI uses cubic blocks (same size in X/Y/Z).\n"
                                             "Hard minimum: %1 voxels.")
                                            .arg(kBlockedAutoTraceMinBlockSizeVoxels));
  blockedParamsLayout->addWidget(m_blockedTraceBlockSizeSpin);

  blockedParamsLayout->addWidget(new QLabel(tr("Padding:"), blockedParamsRow));
  m_blockedTracePaddingSpin = new QSpinBox(blockedParamsRow);
  m_blockedTracePaddingSpin->setMinimum(kBlockedAutoTraceMinPaddingVoxels);
  m_blockedTracePaddingSpin->setMaximum(std::numeric_limits<int>::max());
  m_blockedTracePaddingSpin->setValue(kBlockedAutoTraceMinPaddingVoxels);
  m_blockedTracePaddingSpin->setToolTip(
    tr("Halo/padding size added on each side in tracing voxels (after downsample).\n"
       "Hard minimum: %1 voxels.")
      .arg(kBlockedAutoTraceMinPaddingVoxels));
  blockedParamsLayout->addWidget(m_blockedTracePaddingSpin);

  blockedParamsLayout->addStretch(1);
  optionsLayout->addWidget(blockedParamsRow);

  layout->addWidget(optionsGroup);

  auto* outputGroup = new QGroupBox(tr("Output"), this);
  auto* outputLayout = new QVBoxLayout(outputGroup);
  outputLayout->setContentsMargins(8, 8, 8, 8);
  outputLayout->setSpacing(8);

  m_outputBlockedDirWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::Directory,
                                                   tr("Output Folder:"),
                                                   QString{},
                                                   ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Swc"),
                                                   ZSystemInfo::instance().lastOpenedObjPath("Swc"),
                                                   QBoxLayout::LeftToRight,
                                                   outputGroup);
  m_outputBlockedDirWidget->setToolTip(tr("Blocked tracing writes all results into this folder:\n"
                                          "- manifest.json + blocks/ (resume checkpoints)\n"
                                          "- result_tracing.swc (rolling progress; tracing coords)\n"
                                          "- result.swc (final output)\n"
                                          "- log.txt"));
  outputLayout->addWidget(m_outputBlockedDirWidget);

  m_blockedSessionInfoLabel = new QLabel(outputGroup);
  m_blockedSessionInfoLabel->setWordWrap(true);
  m_blockedSessionInfoLabel->setVisible(false);
  outputLayout->addWidget(m_blockedSessionInfoLabel);

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

  auto updateBlockedTraceUi = [this]() {
    CHECK(m_blockedTraceCheck != nullptr);
    CHECK(m_blockedTraceBlockSizeSpin != nullptr);
    CHECK(m_blockedTracePaddingSpin != nullptr);

    const std::optional<size_t> imgIdOpt = selectedImageId();
    if (!imgIdOpt.has_value()) {
      const QSignalBlocker blocker(*m_blockedTraceCheck);
      m_blockedTraceCheck->setChecked(false);
      m_blockedTraceCheck->setEnabled(false);
      m_blockedTraceCheck->setToolTip(tr("Select an image to enable blocked tracing."));
      return;
    }

    const ZImgDoc& imgDoc = m_doc.imgDoc();
    if (!imgDoc.hasObjWithID(*imgIdOpt)) {
      const QSignalBlocker blocker(*m_blockedTraceCheck);
      m_blockedTraceCheck->setChecked(false);
      m_blockedTraceCheck->setEnabled(false);
      m_blockedTraceCheck->setToolTip(tr("The selected image no longer exists."));
      return;
    }

    const std::shared_ptr<ZImgPack> imgPack = imgDoc.imgPackShared(*imgIdOpt);
    CHECK(imgPack != nullptr);

    const bool supported = imgPack->isDiskCached() || imgPack->isNeuroglancerPrecomputed();
    if (!supported) {
      const QSignalBlocker blocker(*m_blockedTraceCheck);
      m_blockedTraceCheck->setChecked(false);
      m_blockedTraceCheck->setEnabled(false);
      m_blockedTraceCheck->setToolTip(
        tr("Blocked tracing is currently supported only for disk-cached and Neuroglancer datasets."));
      return;
    }

    m_blockedTraceCheck->setEnabled(true);
    {
      const QSignalBlocker blocker(*m_blockedTraceCheck);
      // m_blockedTraceCheck->setChecked(m_blockedTracePreferred);
      m_blockedTraceCheck->setChecked(false);
    }
    m_blockedTraceCheck->setToolTip(tr("Blocked tracing is recommended for large disk-cached / Neuroglancer datasets.\n"
                                       "It supports crash-safe resume via the output folder contents."));
  };

  auto updateDownsampleUi = [this]() {
    CHECK(m_downsampleCheck != nullptr);
    CHECK(m_downsampleRatioXYSpin != nullptr);
    CHECK(m_downsampleRatioZSpin != nullptr);

    const bool locked = blockedTraceEnabled() && m_blockedSessionSignalRatio.has_value();
    m_downsampleCheck->setEnabled(!locked);

    const bool enabled = m_downsampleCheck->isChecked();
    m_downsampleRatioXYSpin->setEnabled(enabled && !locked);
    m_downsampleRatioZSpin->setEnabled(enabled && !locked);
  };
  updateDownsampleUi();
  updateBlockedTraceUi();

  auto updateOutputUi = [this]() {
    CHECK(m_outputBlockedDirWidget != nullptr);
    CHECK(m_outputSwcWidget != nullptr);
    CHECK(m_outputLogWidget != nullptr);
    CHECK(m_blockedTraceBlockSizeSpin != nullptr);
    CHECK(m_blockedTracePaddingSpin != nullptr);

    const bool useBlocked = blockedTraceEnabled();
    const bool locked = useBlocked && m_blockedSessionSignalRatio.has_value();

    m_outputBlockedDirWidget->setVisible(useBlocked);
    m_outputBlockedDirWidget->setEnabled(useBlocked);

    m_outputSwcWidget->setVisible(!useBlocked);
    m_outputSwcWidget->setEnabled(!useBlocked);

    m_outputLogWidget->setVisible(!useBlocked);
    m_outputLogWidget->setEnabled(!useBlocked);

    m_blockedTraceBlockSizeSpin->setEnabled(useBlocked && !locked);
    m_blockedTracePaddingSpin->setEnabled(useBlocked && !locked);
  };

  updateOutputUi();

  auto updateBlockedSessionUi = [this]() {
    CHECK(m_outputBlockedDirWidget != nullptr);
    CHECK(m_downsampleCheck != nullptr);
    CHECK(m_downsampleRatioXYSpin != nullptr);
    CHECK(m_downsampleRatioZSpin != nullptr);
    CHECK(m_blockedTraceBlockSizeSpin != nullptr);
    CHECK(m_blockedTracePaddingSpin != nullptr);
    CHECK(m_blockedSessionInfoLabel != nullptr);

    m_blockedSessionManifestPresent = false;
    m_blockedSessionManifestError.clear();
    m_blockedSessionSignalRatio.reset();
    m_blockedSessionZToXYRatio.reset();
    m_blockedSessionBlock.reset();

    m_blockedSessionInfoLabel->setVisible(false);
    m_blockedSessionInfoLabel->setToolTip(QString{});
    m_blockedSessionInfoLabel->setStyleSheet(QString{});

    if (!blockedTraceEnabled()) {
      return;
    }

    const QString outDir = outputBlockedDirPath();
    const BlockedSessionManifestLoadResult manifest = tryLoadBlockedSessionManifestFromDir(outDir);
    if (!manifest.hasManifest) {
      return;
    }

    m_blockedSessionManifestPresent = true;
    if (!manifest.ok) {
      m_blockedSessionManifestError = manifest.error;
      m_blockedSessionInfoLabel->setText(
        tr(
          "Blocked tracing session detected in the selected output folder, but the session manifest is invalid:\n%1\n\n"
          "Choose a different (empty) output folder to start a new session, or fix/remove manifest.json to resume.")
          .arg(m_blockedSessionManifestError));
      m_blockedSessionInfoLabel->setStyleSheet(QStringLiteral("QLabel { color: #b00020; }"));
      m_blockedSessionInfoLabel->setVisible(true);
      return;
    }

    const int64_t coreX = manifest.block[0];
    const int64_t coreY = manifest.block[1];
    const int64_t coreZ = manifest.block[2];
    const int64_t halo = manifest.block[3];

    if (coreX > std::numeric_limits<int>::max() || coreY > std::numeric_limits<int>::max() ||
        coreZ > std::numeric_limits<int>::max() || halo > std::numeric_limits<int>::max()) {
      m_blockedSessionManifestError = tr("Manifest block sizes exceed UI limits (int32).");
      m_blockedSessionInfoLabel->setText(
        tr("Blocked tracing session detected, but the manifest block sizes exceed the GUI limits.\n"
           "Core=(%1,%2,%3) halo=%4\n\n"
           "Choose a different output folder, or resume via a CLI/config path that can represent these values.")
          .arg(static_cast<long long>(coreX))
          .arg(static_cast<long long>(coreY))
          .arg(static_cast<long long>(coreZ))
          .arg(static_cast<long long>(halo)));
      m_blockedSessionInfoLabel->setStyleSheet(QStringLiteral("QLabel { color: #b00020; }"));
      m_blockedSessionInfoLabel->setVisible(true);
      return;
    }

    m_blockedSessionSignalRatio = manifest.ratio;
    m_blockedSessionZToXYRatio = manifest.zToXYRatio;
    m_blockedSessionBlock = manifest.block;

    const bool cubic = (coreX == coreY && coreX == coreZ);

    // Apply and lock tracing parameters to the existing session manifest to avoid mismatch errors.
    m_applyingBlockedSessionAutofill = true;
    {
      const QSignalBlocker blocker(*m_downsampleCheck);
      const bool ds = (manifest.ratio != std::array<size_t, 3>{1, 1, 1});
      m_downsampleCheck->setChecked(ds);
    }
    {
      const QSignalBlocker blockerXY(*m_downsampleRatioXYSpin);
      const QSignalBlocker blockerZ(*m_downsampleRatioZSpin);
      m_downsampleRatioXYSpin->setValue(static_cast<int>(manifest.ratio[0]));
      m_downsampleRatioZSpin->setValue(static_cast<int>(manifest.ratio[2]));
    }
    {
      const QSignalBlocker blockerCore(*m_blockedTraceBlockSizeSpin);
      m_blockedTraceBlockSizeSpin->setValue(static_cast<int>(coreX));
      if (cubic) {
        m_blockedTraceBlockSizeSpin->setToolTip(tr("Resuming an existing session.\n"
                                                   "Block size is locked to manifest.json."));
      } else {
        m_blockedTraceBlockSizeSpin->setToolTip(
          tr("Resuming an existing session.\n"
             "This session uses a non-cubic core size (core_x/core_y/core_z differ).\n"
             "The GUI shows core_x, but tracing will resume with the full core=(%1,%2,%3) from manifest.json.")
            .arg(static_cast<long long>(coreX))
            .arg(static_cast<long long>(coreY))
            .arg(static_cast<long long>(coreZ)));
      }
    }
    {
      const QSignalBlocker blockerHalo(*m_blockedTracePaddingSpin);
      m_blockedTracePaddingSpin->setValue(static_cast<int>(halo));
      m_blockedTracePaddingSpin->setToolTip(tr("Resuming an existing session.\n"
                                               "Padding/halo is locked to manifest.json."));
    }
    m_applyingBlockedSessionAutofill = false;

    const QString coreLabel = cubic ? QStringLiteral("%1").arg(static_cast<long long>(coreX))
                                    : QStringLiteral("(%1,%2,%3)")
                                        .arg(static_cast<long long>(coreX))
                                        .arg(static_cast<long long>(coreY))
                                        .arg(static_cast<long long>(coreZ));

    m_blockedSessionInfoLabel->setText(
      tr("Existing blocked tracing session detected in the selected output folder.\n"
         "Resuming with: ratio=[%1,%2,%3], zToXYRatio=%4, core=%5, halo=%6.\n"
         "These parameters are locked to the session manifest to keep resume deterministic.\n"
         "Choose a different (empty) output folder to start a new session.")
        .arg(static_cast<qulonglong>(manifest.ratio[0]))
        .arg(static_cast<qulonglong>(manifest.ratio[1]))
        .arg(static_cast<qulonglong>(manifest.ratio[2]))
        .arg(formatZToXYRatio(manifest.zToXYRatio))
        .arg(coreLabel)
        .arg(static_cast<long long>(halo)));
    m_blockedSessionInfoLabel->setVisible(true);
  };

  updateBlockedSessionUi();
  updateDownsampleUi();
  updateOutputUi();
  updateZToXYRatioUi();

  connect(m_imageCombo,
          &QComboBox::currentIndexChanged,
          this,
          [this, updateBlockedTraceUi, updateBlockedSessionUi, updateDownsampleUi, updateOutputUi](int) {
            rebuildChannelAndTimeCombos();
            syncSourceSelectionToTraceSettings();
            rebuildSuggestedOutputs();
            updateBlockedTraceUi();
            updateBlockedSessionUi();
            updateDownsampleUi();
            updateOutputUi();
            updateZToXYRatioUi();
          });

  connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int) {
    syncSourceSelectionToTraceSettings();
    rebuildSuggestedOutputs();
    updateZToXYRatioUi();
  });

  connect(m_timeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
    rebuildSuggestedOutputs();
  });

  connect(m_downsampleCheck, &QCheckBox::toggled, this, [this, updateDownsampleUi](bool) {
    updateDownsampleUi();
    rebuildSuggestedOutputs();
    updateZToXYRatioUi();
  });

  connect(m_downsampleRatioXYSpin, &QSpinBox::valueChanged, this, [this](int) {
    rebuildSuggestedOutputs();
    updateZToXYRatioUi();
  });

  connect(m_downsampleRatioZSpin, &QSpinBox::valueChanged, this, [this](int) {
    rebuildSuggestedOutputs();
    updateZToXYRatioUi();
  });

  connect(m_blockedTraceCheck,
          &QCheckBox::toggled,
          this,
          [this, updateBlockedSessionUi, updateDownsampleUi, updateOutputUi](bool checked) {
            m_blockedTracePreferred = checked;
            updateBlockedSessionUi();
            updateDownsampleUi();
            updateOutputUi();
            rebuildSuggestedOutputs();
            updateZToXYRatioUi();
          });

  connect(m_blockedTraceBlockSizeSpin, &QSpinBox::valueChanged, this, [this](int) {
    rebuildSuggestedOutputs();
  });

  connect(m_blockedTracePaddingSpin, &QSpinBox::valueChanged, this, [this](int) {
    rebuildSuggestedOutputs();
  });

  connect(m_outputBlockedDirWidget,
          &ZSelectFileWidget::changed,
          this,
          [this, updateBlockedSessionUi, updateDownsampleUi, updateOutputUi]() {
            if (!m_applyingSuggestedOutputs) {
              m_outputBlockedDirCustomized = true;
            }
            if (m_applyingBlockedSessionAutofill) {
              return;
            }
            // Output folder changes may switch between "new session" and "resume existing session" mode.
            // Refresh manifest-derived locks first, then update enable/disable state.
            updateBlockedSessionUi();
            updateDownsampleUi();
            updateOutputUi();
            updateZToXYRatioUi();
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

  connect(m_overrideZToXYRatioCheck, &QCheckBox::toggled, this, [this](bool checked) {
    if (!checked) {
      setSelectedZToXYRatioOverride(std::nullopt);
    } else {
      CHECK(m_overrideZToXYRatioSpin != nullptr);
      setSelectedZToXYRatioOverride(m_overrideZToXYRatioSpin->value());
    }
    updateZToXYRatioUi();
  });
  connect(m_overrideZToXYRatioSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    CHECK(m_overrideZToXYRatioCheck != nullptr);
    if (!m_overrideZToXYRatioCheck->isChecked()) {
      return;
    }
    setSelectedZToXYRatioOverride(value);
    updateZToXYRatioUi();
  });

  connect(&m_doc.traceSettings(), &ZTraceSettings::changed, this, [this]() {
    updateZToXYRatioUi();
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
  std::array<size_t, 3> ratio = signalRatio();
  const int traceLevelValue = traceLevel();
  const bool doResample = optimalResamplingEnabled();
  const bool loadResult = loadResultEnabled();

  const bool useBlockedRequested = blockedTraceEnabled();

  QString outputDir;
  QString outputSwc;
  QString outputLog;
  QString sessionDir;
  std::optional<double> resumeZToXYRatio;
  std::array<int64_t, 4> blockedBlock = {0, 0, 0, 0}; // {coreX, coreY, coreZ, halo}

  if (useBlockedRequested) {
    outputDir = outputBlockedDirPath();
    if (outputDir.isEmpty()) {
      throw ZException("Please select an output folder.");
    }

    const BlockedSessionManifestLoadResult manifest = tryLoadBlockedSessionManifestFromDir(outputDir);
    if (manifest.hasManifest) {
      if (!manifest.ok) {
        throw ZException(
          QStringLiteral("The selected output folder contains an invalid manifest.json:\n%1").arg(manifest.error));
      }
      ratio = manifest.ratio;
      resumeZToXYRatio = manifest.zToXYRatio;
      blockedBlock = manifest.block;
    } else {
      const int64_t core = static_cast<int64_t>(blockedTraceBlockSize());
      const int64_t halo = static_cast<int64_t>(blockedTracePaddingSize());
      blockedBlock = {core, core, core, halo};
    }

    // Blocked tracing output is a directory. Keep output artifacts stable and resumable:
    // - session state (manifest + blocks/) lives in the output folder
    // - final SWC and log are placed at well-known file names
    sessionDir = outputDir;
    outputSwc = QDir(outputDir).absoluteFilePath(QStringLiteral("result.swc"));
    outputLog = QDir(outputDir).absoluteFilePath(QStringLiteral("log.txt"));
  } else {
    outputSwc = outputSwcPath();
    outputLog = outputLogPath();

    if (outputSwc.isEmpty()) {
      throw ZException("Please select an output SWC file.");
    }
    if (outputLog.isEmpty()) {
      throw ZException("Please select an output log file.");
    }
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
  const std::string datasetId = imgPack->imgSource().toString();

  const ZImgInfo info = imgPack->imgInfo();
  const double derivedZToXYRatio = preferredZToXYRatioFromImgInfoLegacyLike(info, ratio);
  const double zToXYRatio = resumeZToXYRatio.value_or(
    m_doc.traceSettings().zToXYRatioOverrideForSelection(imgId, sc).value_or(derivedZToXYRatio));
  const QString channelLabel =
    (sc < info.channelNames.size()) ? info.displayChannelName(sc) : QStringLiteral("Ch%1").arg(sc + 1);
  const QString timeLabel = QStringLiteral("T%1").arg(t + 1);
  const QString ratioLabel = (ratio == std::array<size_t, 3>{1, 1, 1})
                               ? QString{}
                               : QStringLiteral("ratio=[%1,%2,%3]").arg(ratio[0]).arg(ratio[1]).arg(ratio[2]);
  const QString outputLabel = [&]() -> QString {
    if (useBlockedRequested) {
      const QString dirLabel = QFileInfo(outputDir).fileName();
      return dirLabel.isEmpty() ? outputDir : dirLabel;
    }
    const QString swcLabel = QFileInfo(outputSwc).fileName();
    return swcLabel.isEmpty() ? outputSwc : swcLabel;
  }();

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
  spec.successMessage =
    useBlockedRequested ? QStringLiteral("wrote result.swc") : QStringLiteral("wrote %1").arg(outputLabel);
  spec.makeWorker = [imgPack,
                     sc,
                     t,
                     ratio,
                     traceCfgPath,
                     traceLevelValue,
                     zToXYRatio,
                     haveAlgoConfig,
                     algoOverrides,
                     doResample,
                     docHasAnySwc,
                     outputSwc,
                     outputLog,
                     sessionDir,
                     blockedBlock,
                     datasetId,
                     useBlockedRequested]() -> std::unique_ptr<ZImgProcess> {
    const bool useBlocked = useBlockedRequested;
    if (useBlocked && !(imgPack->isDiskCached() || imgPack->isNeuroglancerPrecomputed())) {
      throw ZException("Blocked Auto Trace is only supported for disk-cached and Neuroglancer datasets.");
    }

    if (useBlocked) {
      auto worker = std::make_unique<ZNeutubeBlockedAutoTraceProcess>();
      worker->setLogFile(outputLog);
      worker->setSelectedChannelTime(sc, t);
      worker->setTraceConfigPath(traceCfgPath);
      worker->setTraceLevel(traceLevelValue);
      worker->setZToXYRatio(zToXYRatio);
      if (haveAlgoConfig) {
        worker->setAlgoConfigOverrides(algoOverrides);
      } else {
        worker->clearAlgoConfigOverrides();
      }
      worker->setDoResampleAfterTracing(doResample);
      worker->setDocHasAnySwc(docHasAnySwc);
      worker->setOutputSwcPath(outputSwc);
      worker->setOutputSessionDir(sessionDir);
      worker->setDatasetId(datasetId);
      worker->setSignalDownsampleRatio(ratio);
      worker->setBlockCoreSizeXYZ(blockedBlock[0], blockedBlock[1], blockedBlock[2]);
      worker->setBlockHalo(blockedBlock[3]);

      worker->setSignalInfo(imgPack->imgInfo());
      worker->setRoiSignalProvider(
        [imgPack,
         sc,
         t,
         ratio](int64_t sx, int64_t sy, int64_t sz, int64_t w, int64_t h, int64_t d, folly::CancellationToken token)
          -> ZNeutubeBlockedAutoTraceProcess::RoiSignalResult {
          maybeCancel(token);

          const ZImgInfo info = imgPack->imgInfo();
          if (sc >= info.numChannels || t >= info.numTimes) {
            throw ZException(
              QStringLiteral("Auto Trace failed: invalid channel/time selection (c=%1, t=%2).").arg(sc).arg(t));
          }

          CHECK(sx >= 0 && sy >= 0 && sz >= 0);
          CHECK(w >= 0 && h >= 0 && d >= 0);

          ZImgInfo resInfo = info;
          resInfo.width = static_cast<size_t>(w);
          resInfo.height = static_cast<size_t>(h);
          resInfo.depth = static_cast<size_t>(d);
          resInfo.numChannels = 1;
          resInfo.numTimes = 1;
          resInfo.createDefaultDescriptions();

          try {
            const index_t xyRatio = static_cast<index_t>(ratio[0]);
            const index_t zRatio = static_cast<index_t>(ratio[2]);
            CHECK(xyRatio > 0);
            CHECK(zRatio > 0);

            return ZNeutubeBlockedAutoTraceProcess::RoiSignalResult::ok(folly::coro::blockingWait(
              folly::coro::co_withCancellation(token,
                                               imgPack->readRegionToImgAsync(/*xyRatio*/ xyRatio,
                                                                             /*zRatio*/ zRatio,
                                                                             static_cast<index_t>(sx),
                                                                             static_cast<index_t>(sy),
                                                                             static_cast<index_t>(sz),
                                                                             sc,
                                                                             t,
                                                                             resInfo,
                                                                             info.dataRangeMin(),
                                                                             info.dataRangeMax()))));
          }
          catch (const folly::OperationCancelled&) {
            maybeCancel(token);
            throw;
          }
        });

      return worker;
    }

    // Legacy dense-volume auto trace (supports downsampled tracing).
    auto worker = std::make_unique<ZNeutubeAutoTraceProcess>();
    worker->setLogFile(outputLog);
    worker->setSelectedChannelTime(sc, t);
    worker->setTraceConfigPath(traceCfgPath);
    worker->setTraceLevel(traceLevelValue);
    worker->setZToXYRatio(zToXYRatio);
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

QString ZAutoTraceDialog::outputBlockedDirPath() const
{
  CHECK(m_outputBlockedDirWidget != nullptr);
  return m_outputBlockedDirWidget->getSelectedDirectory();
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

bool ZAutoTraceDialog::blockedTraceEnabled() const
{
  CHECK(m_blockedTraceCheck != nullptr);
  return m_blockedTraceCheck->isEnabled() && m_blockedTraceCheck->isChecked();
}

int ZAutoTraceDialog::blockedTraceBlockSize() const
{
  CHECK(m_blockedTraceBlockSizeSpin != nullptr);
  CHECK(m_blockedTraceBlockSizeSpin->value() > 0);
  return m_blockedTraceBlockSizeSpin->value();
}

int ZAutoTraceDialog::blockedTracePaddingSize() const
{
  CHECK(m_blockedTracePaddingSpin != nullptr);
  CHECK(m_blockedTracePaddingSpin->value() >= 0);
  return m_blockedTracePaddingSpin->value();
}

std::optional<double> ZAutoTraceDialog::derivedZToXYRatio() const
{
  const std::optional<size_t> imgIdOpt = selectedImageId();
  if (!imgIdOpt.has_value()) {
    return std::nullopt;
  }

  const ZImgDoc& imgDoc = m_doc.imgDoc();
  if (!imgDoc.hasObjWithID(*imgIdOpt)) {
    return std::nullopt;
  }

  const std::shared_ptr<ZImgPack> imgPack = imgDoc.imgPackShared(*imgIdOpt);
  CHECK(imgPack != nullptr);
  return preferredZToXYRatioFromImgInfoLegacyLike(imgPack->imgInfo(), signalRatio());
}

std::optional<double> ZAutoTraceDialog::selectedZToXYRatioOverride() const
{
  return m_doc.traceSettings().zToXYRatioOverrideForSelection(selectedImageId(), selectedChannel());
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

void ZAutoTraceDialog::syncSourceSelectionToTraceSettings()
{
  m_doc.traceSettings().setSourceSelection(selectedImageId(), selectedChannel());
}

void ZAutoTraceDialog::setSelectedZToXYRatioOverride(std::optional<double> zToXYRatio)
{
  if (blockedTraceEnabled() && m_blockedSessionZToXYRatio.has_value()) {
    return;
  }

  const std::optional<size_t> imgIdOpt = selectedImageId();
  if (!imgIdOpt.has_value()) {
    return;
  }

  m_doc.traceSettings().setZToXYRatioOverrideForSelection(imgIdOpt, selectedChannel(), zToXYRatio);
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
  CHECK(m_outputBlockedDirWidget != nullptr);
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
  const bool useBlocked = blockedTraceEnabled();

  if (useBlocked) {
    const QString blockedTag =
      tag + QStringLiteral("_blk%1_pad%2_blocked").arg(blockedTraceBlockSize()).arg(blockedTracePaddingSize());
    const QString suggestedDir = QDir(baseDir).absoluteFilePath(baseStem + blockedTag);
    const QString uniqueDir = makeUniqueOutputPath(suggestedDir);

    m_applyingSuggestedOutputs = true;
    if (!m_outputBlockedDirCustomized) {
      const QSignalBlocker blocker(*m_outputBlockedDirWidget);
      m_outputBlockedDirWidget->setFile(uniqueDir);
    }
    m_applyingSuggestedOutputs = false;
    return;
  }

  const QString suggestedSwc = QDir(baseDir).absoluteFilePath(baseStem + tag + QStringLiteral(".swc"));
  const QString suggestedLog = QDir(baseDir).absoluteFilePath(baseStem + tag + QStringLiteral("_log.txt"));
  const QStringList outputPaths = makeUniqueOutputPaths({suggestedSwc, suggestedLog});
  CHECK(outputPaths.size() == 2);

  m_applyingSuggestedOutputs = true;
  if (!m_outputSwcCustomized) {
    const QSignalBlocker blocker(*m_outputSwcWidget);
    m_outputSwcWidget->setFile(outputPaths[0]);
  }
  if (!m_outputLogCustomized) {
    const QSignalBlocker blocker(*m_outputLogWidget);
    m_outputLogWidget->setFile(outputPaths[1]);
  }
  m_applyingSuggestedOutputs = false;
}

void ZAutoTraceDialog::updateZToXYRatioUi()
{
  CHECK(m_derivedZToXYRatioLabel != nullptr);
  CHECK(m_overrideZToXYRatioCheck != nullptr);
  CHECK(m_overrideZToXYRatioSpin != nullptr);
  CHECK(m_effectiveZToXYRatioLabel != nullptr);

  const std::optional<size_t> imgIdOpt = selectedImageId();
  const std::optional<double> derivedValue = derivedZToXYRatio();
  const std::optional<double> overrideValue = selectedZToXYRatioOverride();
  const bool lockedToSession = blockedTraceEnabled() && m_blockedSessionZToXYRatio.has_value();

  {
    const QSignalBlocker blockerCheck(*m_overrideZToXYRatioCheck);
    const QSignalBlocker blockerSpin(*m_overrideZToXYRatioSpin);

    if (lockedToSession) {
      CHECK(m_blockedSessionZToXYRatio.has_value());
      m_overrideZToXYRatioCheck->setEnabled(false);
      m_overrideZToXYRatioCheck->setChecked(true);
      m_overrideZToXYRatioSpin->setValue(*m_blockedSessionZToXYRatio);
      m_overrideZToXYRatioSpin->setEnabled(false);
    } else {
      m_overrideZToXYRatioCheck->setEnabled(imgIdOpt.has_value());
      m_overrideZToXYRatioCheck->setChecked(overrideValue.has_value());
      m_overrideZToXYRatioSpin->setValue(overrideValue.value_or(derivedValue.value_or(1.0)));
      m_overrideZToXYRatioSpin->setEnabled(imgIdOpt.has_value() && overrideValue.has_value());
    }
  }

  if (derivedValue.has_value()) {
    m_derivedZToXYRatioLabel->setText(formatZToXYRatio(*derivedValue));
  } else {
    m_derivedZToXYRatioLabel->setText(tr("N/A"));
  }

  if (lockedToSession) {
    CHECK(m_blockedSessionZToXYRatio.has_value());
    m_effectiveZToXYRatioLabel->setText(tr("%1 (resume manifest)").arg(formatZToXYRatio(*m_blockedSessionZToXYRatio)));
    m_overrideZToXYRatioCheck->setToolTip(tr("Blocked session resume locks zToXYRatio to manifest.json."));
    m_overrideZToXYRatioSpin->setToolTip(tr("Blocked session resume locks zToXYRatio to manifest.json."));
    return;
  }

  m_overrideZToXYRatioCheck->setToolTip(
    tr("Use an explicit tracing zToXYRatio instead of the metadata-derived value."));
  m_overrideZToXYRatioSpin->setToolTip(tr("Explicit tracing zToXYRatio override."));

  if (overrideValue.has_value()) {
    m_effectiveZToXYRatioLabel->setText(tr("%1 (override)").arg(formatZToXYRatio(*overrideValue)));
  } else if (derivedValue.has_value()) {
    m_effectiveZToXYRatioLabel->setText(tr("%1 (metadata)").arg(formatZToXYRatio(*derivedValue)));
  } else {
    m_effectiveZToXYRatioLabel->setText(tr("N/A"));
  }
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
