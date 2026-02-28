#pragma once

#include "zimgprocessdialog.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace nim {

class ZDoc;
class ZSelectFileWidget;

class ZSubtractBackgroundDialog final : public ZImgProcessDialog
{
  Q_OBJECT

public:
  explicit ZSubtractBackgroundDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  WorkerSpec createWorkerSpec() override;

private:
  void inputImageChanged();

  [[nodiscard]] QString inputImagePath() const;
  [[nodiscard]] QString outputImagePath() const;
  [[nodiscard]] QString outputLogPath() const;
  [[nodiscard]] int channel0() const;
  [[nodiscard]] double minForegroundRatio() const;
  [[nodiscard]] int maxIterations() const;
  [[nodiscard]] bool loadResultEnabled() const;

private:
  ZSelectFileWidget* m_inputImageWidget = nullptr;
  ZSelectFileWidget* m_outputImageWidget = nullptr;
  ZSelectFileWidget* m_outputLogWidget = nullptr;
  QComboBox* m_channelCombo = nullptr;
  QDoubleSpinBox* m_minFrSpin = nullptr;
  QSpinBox* m_maxIterSpin = nullptr;
  QCheckBox* m_openResultCheck = nullptr;
};

} // namespace nim
