#pragma once

#include <QWidget>

#include <cstddef>
#include <optional>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QButtonGroup;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace nim {

class ZDoc;

// Persistent trace selection UI shared between the 2D and 3D main windows.
//
// This widget edits `ZDoc::traceSettings()` and intentionally avoids heuristics:
// it does not infer defaults from active/selected objects.
class ZTraceSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ZTraceSettingsWidget(ZDoc& doc, QWidget* parent = nullptr);

private:
  void refreshFromDoc();

  void rebuildSourceImageCombo();
  void rebuildTargetSwcCombo();
  void updateChannelComboForCurrentSource();
  void updateMappingLabel();
  void initializeAlgoConfigFromLegacyDefaultsIfUnset();

  void applySettingsToUi();
  void pushSourceUiToSettings();
  void pushTargetUiToSettings();
  void pushAlgoUiToSettings();

  [[nodiscard]] std::optional<size_t> currentSourceImageIdFromUi() const;
  [[nodiscard]] std::optional<size_t> currentTargetSwcIdFromUi() const;

private:
  ZDoc& m_doc;

  QComboBox* m_sourceImageCombo = nullptr;
  QComboBox* m_channelCombo = nullptr;

  QRadioButton* m_newSwcRadio = nullptr;
  QRadioButton* m_existingSwcRadio = nullptr;
  QComboBox* m_existingSwcCombo = nullptr;
  QButtonGroup* m_swcTargetButtonGroup = nullptr;

  QLabel* m_mappingLabel = nullptr;

  QDoubleSpinBox* m_minAutoScoreSpin = nullptr;
  QDoubleSpinBox* m_minManualScoreSpin = nullptr;
  QDoubleSpinBox* m_minSeedScoreSpin = nullptr;
  QDoubleSpinBox* m_min2dScoreSpin = nullptr;
  QCheckBox* m_refitCheck = nullptr;
  QCheckBox* m_tuneEndCheck = nullptr;
  QSpinBox* m_recoverSpin = nullptr;
  QDoubleSpinBox* m_maxEucDistSpin = nullptr;
  QCheckBox* m_spTestCheck = nullptr;
  QCheckBox* m_crossoverCheck = nullptr;
  QCheckBox* m_enhanceMaskCheck = nullptr;
  QPushButton* m_resetAlgoButton = nullptr;

  bool m_updating = false;
};

} // namespace nim
