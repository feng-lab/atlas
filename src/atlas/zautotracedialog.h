#pragma once

#include "zimgprocessdialog.h"

#include <cstddef>
#include <optional>
#include <array>

class QCheckBox;
class QComboBox;
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
  [[nodiscard]] QString outputSwcPath() const;
  [[nodiscard]] QString outputLogPath() const;
  [[nodiscard]] bool loadResultEnabled() const;
  [[nodiscard]] int traceLevel() const;
  [[nodiscard]] bool optimalResamplingEnabled() const;
  [[nodiscard]] std::array<size_t, 3> signalRatio() const;

  void rebuildImageCombo();
  void rebuildChannelAndTimeCombos();
  void rebuildSuggestedOutputs();
  void updateBudgetUi();

  ZDoc& m_doc;

  QComboBox* m_imageCombo = nullptr;
  QComboBox* m_channelCombo = nullptr;
  QComboBox* m_timeCombo = nullptr;

  ZSelectFileWidget* m_outputSwcWidget = nullptr;
  ZSelectFileWidget* m_outputLogWidget = nullptr;
  QCheckBox* m_loadResultCheck = nullptr;

  QLabel* m_levelLabel = nullptr;
  QSlider* m_levelSlider = nullptr;
  QCheckBox* m_defaultLevelCheck = nullptr;
  QCheckBox* m_resampleCheck = nullptr;

  QCheckBox* m_downsampleCheck = nullptr;
  QSpinBox* m_downsampleRatioXYSpin = nullptr;
  QSpinBox* m_downsampleRatioZSpin = nullptr;

  bool m_outputSwcCustomized = false;
  bool m_outputLogCustomized = false;
  bool m_applyingSuggestedOutputs = false;
};

} // namespace nim
