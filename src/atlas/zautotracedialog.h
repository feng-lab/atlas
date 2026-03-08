#pragma once

#include "zimgprocessdialog.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QSpinBox;

namespace nim {

class ZDoc;
class ZSelectFileWidget;

class ZAutoTraceDialog final : public ZImgProcessDialog
{
  Q_OBJECT

public:
  explicit ZAutoTraceDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  WorkerSpec createWorkerSpec() override;

private:
  [[nodiscard]] std::optional<size_t> selectedImageId() const;
  [[nodiscard]] size_t selectedChannel() const;
  [[nodiscard]] size_t selectedTime() const;
  [[nodiscard]] QString outputBlockedDirPath() const;
  [[nodiscard]] QString outputSwcPath() const;
  [[nodiscard]] QString outputLogPath() const;
  [[nodiscard]] bool loadResultEnabled() const;
  [[nodiscard]] int traceLevel() const;
  [[nodiscard]] bool optimalResamplingEnabled() const;
  [[nodiscard]] std::array<size_t, 3> signalRatio() const;
  [[nodiscard]] bool blockedTraceEnabled() const;
  [[nodiscard]] int blockedTraceBlockSize() const;
  [[nodiscard]] int blockedTracePaddingSize() const;
  [[nodiscard]] std::optional<double> derivedZScale() const;
  [[nodiscard]] std::optional<double> selectedZScaleOverride() const;

  void rebuildImageCombo();
  void rebuildChannelAndTimeCombos();
  void rebuildSuggestedOutputs();
  void syncSourceSelectionToTraceSettings();
  void setSelectedZScaleOverride(std::optional<double> zScale);
  void updateBudgetUi();
  void updateZScaleUi();

  ZDoc& m_doc;

  QComboBox* m_imageCombo = nullptr;
  QComboBox* m_channelCombo = nullptr;
  QComboBox* m_timeCombo = nullptr;

  ZSelectFileWidget* m_outputSwcWidget = nullptr;
  ZSelectFileWidget* m_outputLogWidget = nullptr;
  ZSelectFileWidget* m_outputBlockedDirWidget = nullptr;
  QCheckBox* m_loadResultCheck = nullptr;
  QLabel* m_blockedSessionInfoLabel = nullptr;

  QLabel* m_levelLabel = nullptr;
  QSlider* m_levelSlider = nullptr;
  QCheckBox* m_defaultLevelCheck = nullptr;
  QCheckBox* m_resampleCheck = nullptr;

  QCheckBox* m_downsampleCheck = nullptr;
  QSpinBox* m_downsampleRatioXYSpin = nullptr;
  QSpinBox* m_downsampleRatioZSpin = nullptr;
  QLabel* m_derivedZScaleLabel = nullptr;
  QCheckBox* m_overrideZScaleCheck = nullptr;
  QDoubleSpinBox* m_overrideZScaleSpin = nullptr;
  QLabel* m_effectiveZScaleLabel = nullptr;

  QCheckBox* m_blockedTraceCheck = nullptr;
  QSpinBox* m_blockedTraceBlockSizeSpin = nullptr;
  QSpinBox* m_blockedTracePaddingSpin = nullptr;

  bool m_outputSwcCustomized = false;
  bool m_outputLogCustomized = false;
  bool m_outputBlockedDirCustomized = false;
  bool m_applyingSuggestedOutputs = false;
  bool m_applyingBlockedSessionAutofill = false;

  // If set, the selected blocked output folder contains a manifest.json. We parse minimal fields so the GUI
  // can lock tracing parameters to the session and avoid mismatch errors at run time.
  bool m_blockedSessionManifestPresent = false;
  QString m_blockedSessionManifestError;
  std::optional<std::array<size_t, 3>> m_blockedSessionSignalRatio;
  std::optional<double> m_blockedSessionZScale;
  // {coreX, coreY, coreZ, halo} in tracing voxels.
  std::optional<std::array<int64_t, 4>> m_blockedSessionBlock;
  bool m_blockedTracePreferred = true;
};

} // namespace nim
