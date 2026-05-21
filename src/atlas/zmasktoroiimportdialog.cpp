#include "zmasktoroiimportdialog.h"

#include "zlog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace nim {
namespace {

constexpr int kOutputSplineIndex = 0;
constexpr int kOutputSampledSplineIndex = 1;
constexpr int kOutputPolygonIndex = 2;

constexpr int kFallbackKeepSplineIndex = 0;
constexpr int kFallbackPolygonIndex = 1;

constexpr double kDefaultImportTolerancePx = 5.0;
constexpr double kMinimumImportTolerancePx = 0.01;
constexpr double kMaximumImportTolerancePx = 1000000.0;
constexpr double kMaximumMinKnotSpacingPx = 1000000.0;
constexpr int kDefaultSampledSplineTargetPoints = 20;
constexpr int kDefaultSampledSplineMaxPointSpacing = 30;
constexpr int kMaximumSampledSplineParameter = 1000000;

constexpr auto kSettingsGroup = "MaskToROIImport";
constexpr auto kSettingsOutputType = "output_type";
constexpr auto kSettingsTolerance = "tolerance_px";
constexpr auto kSettingsMinKnotSpacing = "min_knot_spacing_px";
constexpr auto kSettingsSampledTargetPoints = "sampled_target_points";
constexpr auto kSettingsSampledMaxPointSpacing = "sampled_max_point_spacing";
constexpr auto kSettingsFallback = "spline_fallback";
constexpr auto kSettingsPreserveHoles = "preserve_holes";
constexpr auto kSettingsFormatIndex = "format_index";

[[nodiscard]] QString compactNameFilterLabel(const QString& filter)
{
  const int extensionListStart = filter.indexOf(" (");
  if (extensionListStart > 0) {
    return filter.left(extensionListStart);
  }
  return filter;
}

} // namespace

ZMaskToROIImportDialog::ZMaskToROIImportDialog(QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Import Mask Image"));
  setModal(true);

  ZImg::getQtReadNameFilter(m_filters, m_formats);
  CHECK(m_filters.size() == static_cast<int>(m_formats.size()));

  auto* rootLayout = new QVBoxLayout(this);

  auto* fileGroup = new QGroupBox(tr("Input"), this);
  auto* fileLayout = new QFormLayout(fileGroup);
  fileLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  {
    auto* row = new QHBoxLayout();
    m_fileEdit = new QLineEdit(fileGroup);
    auto* browseButton = new QPushButton(tr("Browse..."), fileGroup);
    row->addWidget(m_fileEdit, 1);
    row->addWidget(browseButton);
    fileLayout->addRow(tr("Mask image:"), row);
    connect(browseButton, &QPushButton::clicked, this, &ZMaskToROIImportDialog::browseFile);
  }
  {
    m_formatCombo = new QComboBox(fileGroup);
    m_formatCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_formatCombo->setMinimumContentsLength(24);
    m_formatCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    for (int i = 0; i < m_filters.size(); ++i) {
      m_formatCombo->addItem(compactNameFilterLabel(m_filters[i]));
      m_formatCombo->setItemData(i, m_filters[i], Qt::ToolTipRole);
    }
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
      m_formatCombo->setToolTip(index >= 0 && index < m_filters.size() ? m_filters[index] : QString());
    });
    fileLayout->addRow(tr("File format:"), m_formatCombo);
  }
  rootLayout->addWidget(fileGroup);

  auto* optionsGroup = new QGroupBox(tr("Conversion"), this);
  auto* optionsLayout = new QFormLayout(optionsGroup);
  optionsLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  {
    m_outputTypeCombo = new QComboBox(optionsGroup);
    m_outputTypeCombo->addItem(tr("Adaptive spline"));
    m_outputTypeCombo->addItem(tr("Sampled spline"));
    m_outputTypeCombo->addItem(tr("Polygon"));
    optionsLayout->addRow(tr("Output type:"), m_outputTypeCombo);
    connect(m_outputTypeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
      updateEnabledControls();
    });
  }
  {
    m_epsilonSpin = new QDoubleSpinBox(optionsGroup);
    m_epsilonSpin->setRange(kMinimumImportTolerancePx, kMaximumImportTolerancePx);
    m_epsilonSpin->setDecimals(2);
    m_epsilonSpin->setSingleStep(0.25);
    m_epsilonSpin->setSuffix(tr(" px"));
    m_epsilonSpin->setValue(kDefaultImportTolerancePx);
    optionsLayout->addRow(tr("Boundary tolerance:"), m_epsilonSpin);
  }
  {
    m_minKnotSpacingSpin = new QDoubleSpinBox(optionsGroup);
    m_minKnotSpacingSpin->setRange(0.0, kMaximumMinKnotSpacingPx);
    m_minKnotSpacingSpin->setDecimals(2);
    m_minKnotSpacingSpin->setSingleStep(0.25);
    m_minKnotSpacingSpin->setSuffix(tr(" px"));
    m_minKnotSpacingSpin->setSpecialValueText(tr("Disabled"));
    optionsLayout->addRow(tr("Min knot spacing:"), m_minKnotSpacingSpin);
  }
  {
    m_sampledTargetPointsSpin = new QSpinBox(optionsGroup);
    m_sampledTargetPointsSpin->setRange(1, kMaximumSampledSplineParameter);
    m_sampledTargetPointsSpin->setValue(kDefaultSampledSplineTargetPoints);
    optionsLayout->addRow(tr("Sampled target points:"), m_sampledTargetPointsSpin);
  }
  {
    m_sampledMaxPointSpacingSpin = new QSpinBox(optionsGroup);
    m_sampledMaxPointSpacingSpin->setRange(1, kMaximumSampledSplineParameter);
    m_sampledMaxPointSpacingSpin->setValue(kDefaultSampledSplineMaxPointSpacing);
    optionsLayout->addRow(tr("Sampled max spacing:"), m_sampledMaxPointSpacingSpin);
  }
  {
    m_fallbackCombo = new QComboBox(optionsGroup);
    m_fallbackCombo->addItem(tr("Keep best spline and warn"));
    m_fallbackCombo->addItem(tr("Use polygon if tolerance cannot be met"));
    optionsLayout->addRow(tr("Spline fallback:"), m_fallbackCombo);
  }
  {
    m_preserveHolesCheck = new QCheckBox(tr("Preserve holes"), optionsGroup);
    m_preserveHolesCheck->setChecked(true);
    optionsLayout->addRow(QString(), m_preserveHolesCheck);
  }
  rootLayout->addWidget(optionsGroup);

  auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
  connect(buttonBox, &QDialogButtonBox::accepted, this, &ZMaskToROIImportDialog::accept);
  connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
  rootLayout->addWidget(buttonBox);

  loadSettings();
  m_formatCombo->setToolTip(m_formatCombo->currentIndex() >= 0 && m_formatCombo->currentIndex() < m_filters.size()
                              ? m_filters[m_formatCombo->currentIndex()]
                              : QString());
  updateEnabledControls();
}

void ZMaskToROIImportDialog::setInitialDirectory(const QString& path)
{
  m_initialDirectory = path;
}

QString ZMaskToROIImportDialog::selectedFile() const
{
  return m_fileEdit->text().trimmed();
}

FileFormat ZMaskToROIImportDialog::selectedFormat() const
{
  const int index = m_formatCombo->currentIndex();
  CHECK(index >= 0 && index < static_cast<int>(m_formats.size()));
  return m_formats[static_cast<size_t>(index)];
}

ZMaskToROIOptions ZMaskToROIImportDialog::selectedOptions() const
{
  ZMaskToROIOptions options;
  switch (m_outputTypeCombo->currentIndex()) {
    case kOutputSampledSplineIndex:
      options.outputType = ZMaskToROIOutputType::SampledSpline;
      break;
    case kOutputPolygonIndex:
      options.outputType = ZMaskToROIOutputType::Polygon;
      break;
    case kOutputSplineIndex:
    default:
      options.outputType = ZMaskToROIOutputType::Spline;
      break;
  }
  options.epsilonPx = m_epsilonSpin->value();
  options.minKnotSpacingPx = m_minKnotSpacingSpin->value();
  options.sampledSplineTargetPoints = m_sampledTargetPointsSpin->value();
  options.sampledSplineMaxPointSpacing = m_sampledMaxPointSpacingSpin->value();
  options.preserveHoles = m_preserveHolesCheck->isChecked();

  switch (m_fallbackCombo->currentIndex()) {
    case kFallbackPolygonIndex:
      options.splineFallback = ZMaskToROISplineFallback::UsePolygon;
      break;
    case kFallbackKeepSplineIndex:
    default:
      options.splineFallback = ZMaskToROISplineFallback::KeepBestSpline;
      break;
  }
  return options;
}

void ZMaskToROIImportDialog::accept()
{
  const QString file = selectedFile();
  if (file.isEmpty()) {
    QMessageBox::warning(this, windowTitle(), tr("Choose a mask image file to import."));
    return;
  }
  if (!QFileInfo::exists(file)) {
    QMessageBox::warning(this, windowTitle(), tr("The selected mask image does not exist."));
    return;
  }
  if (m_formatCombo->currentIndex() < 0 || m_formatCombo->currentIndex() >= static_cast<int>(m_formats.size())) {
    QMessageBox::warning(this, windowTitle(), tr("Choose a valid file format."));
    return;
  }

  saveSettings();
  QDialog::accept();
}

void ZMaskToROIImportDialog::browseFile()
{
  QFileDialog dialog(this);
  dialog.setFileMode(QFileDialog::ExistingFile);
  dialog.setNameFilters(m_filters);
  if (!m_initialDirectory.isEmpty()) {
    dialog.setDirectory(m_initialDirectory);
  }
  if (m_formatCombo->currentIndex() >= 0 && m_formatCombo->currentIndex() < m_filters.size()) {
    dialog.selectNameFilter(m_filters[m_formatCombo->currentIndex()]);
  }
  dialog.setWindowTitle(tr("Import Mask Image File"));

  if (dialog.exec()) {
    m_fileEdit->setText(dialog.selectedFiles().at(0));
    const int formatIndex = m_filters.indexOf(dialog.selectedNameFilter());
    if (formatIndex >= 0) {
      m_formatCombo->setCurrentIndex(formatIndex);
    }
  }
}

void ZMaskToROIImportDialog::updateEnabledControls()
{
  const int outputType = m_outputTypeCombo->currentIndex();
  const bool adaptiveSplineOutput = (outputType == kOutputSplineIndex);
  const bool sampledSplineOutput = (outputType == kOutputSampledSplineIndex);
  m_epsilonSpin->setEnabled(!sampledSplineOutput);
  m_minKnotSpacingSpin->setEnabled(adaptiveSplineOutput);
  m_fallbackCombo->setEnabled(adaptiveSplineOutput);
  m_sampledTargetPointsSpin->setEnabled(sampledSplineOutput);
  m_sampledMaxPointSpacingSpin->setEnabled(sampledSplineOutput);
}

void ZMaskToROIImportDialog::loadSettings()
{
  QSettings settings;
  settings.beginGroup(kSettingsGroup);

  m_outputTypeCombo->setCurrentIndex(std::clamp(settings.value(kSettingsOutputType, kOutputSplineIndex).toInt(),
                                                kOutputSplineIndex,
                                                kOutputPolygonIndex));
  m_epsilonSpin->setValue(settings.value(kSettingsTolerance, kDefaultImportTolerancePx).toDouble());
  m_minKnotSpacingSpin->setValue(settings.value(kSettingsMinKnotSpacing, 0.0).toDouble());
  m_sampledTargetPointsSpin->setValue(
    settings.value(kSettingsSampledTargetPoints, kDefaultSampledSplineTargetPoints).toInt());
  m_sampledMaxPointSpacingSpin->setValue(
    settings.value(kSettingsSampledMaxPointSpacing, kDefaultSampledSplineMaxPointSpacing).toInt());
  m_fallbackCombo->setCurrentIndex(std::clamp(settings.value(kSettingsFallback, kFallbackKeepSplineIndex).toInt(),
                                              kFallbackKeepSplineIndex,
                                              kFallbackPolygonIndex));
  m_preserveHolesCheck->setChecked(settings.value(kSettingsPreserveHoles, true).toBool());

  const int formatIndex =
    std::clamp(settings.value(kSettingsFormatIndex, 0).toInt(), 0, std::max(0, m_formatCombo->count() - 1));
  m_formatCombo->setCurrentIndex(formatIndex);

  settings.endGroup();
}

void ZMaskToROIImportDialog::saveSettings() const
{
  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  settings.setValue(kSettingsOutputType, m_outputTypeCombo->currentIndex());
  settings.setValue(kSettingsTolerance, m_epsilonSpin->value());
  settings.setValue(kSettingsMinKnotSpacing, m_minKnotSpacingSpin->value());
  settings.setValue(kSettingsSampledTargetPoints, m_sampledTargetPointsSpin->value());
  settings.setValue(kSettingsSampledMaxPointSpacing, m_sampledMaxPointSpacingSpin->value());
  settings.setValue(kSettingsFallback, m_fallbackCombo->currentIndex());
  settings.setValue(kSettingsPreserveHoles, m_preserveHolesCheck->isChecked());
  settings.setValue(kSettingsFormatIndex, m_formatCombo->currentIndex());
  settings.endGroup();
}

} // namespace nim
