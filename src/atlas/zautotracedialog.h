#pragma once

#include <QDialog>

#include <cstddef>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;

namespace nim {

class ZDoc;
class ZSelectFileWidget;

class ZAutoTraceDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit ZAutoTraceDialog(ZDoc& doc, QWidget* parent = nullptr);

  [[nodiscard]] std::optional<size_t> selectedImageId() const;
  [[nodiscard]] size_t selectedChannel() const;
  [[nodiscard]] size_t selectedTime() const;

  [[nodiscard]] QString outputSwcPath() const;
  [[nodiscard]] QString outputLogPath() const;
  [[nodiscard]] bool loadResultEnabled() const;

  // 0 means "default" (no per-level override), matching NeuTu semantics.
  [[nodiscard]] int traceLevel() const;

  [[nodiscard]] bool optimalResamplingEnabled() const;

private:
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

  bool m_outputSwcCustomized = false;
  bool m_outputLogCustomized = false;
  bool m_applyingSuggestedOutputs = false;
};

} // namespace nim
