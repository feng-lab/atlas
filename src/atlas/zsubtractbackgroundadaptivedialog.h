#pragma once

#include "zimgprocessdialog.h"

class QCheckBox;
class QComboBox;
class QSpinBox;

namespace nim {

class ZDoc;
class ZSelectFileWidget;

class ZSubtractBackgroundAdaptiveDialog final : public ZImgProcessDialog
{
  Q_OBJECT

public:
  explicit ZSubtractBackgroundAdaptiveDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  WorkerSpec createWorkerSpec() override;

private:
  void inputImageChanged();

  [[nodiscard]] QString inputImagePath() const;
  [[nodiscard]] QString outputImagePath() const;
  [[nodiscard]] QString outputLogPath() const;
  [[nodiscard]] int channel0() const;
  [[nodiscard]] int numSamples() const;
  [[nodiscard]] int stride() const;
  [[nodiscard]] bool loadResultEnabled() const;

private:
  ZSelectFileWidget* m_inputImageWidget = nullptr;
  ZSelectFileWidget* m_outputImageWidget = nullptr;
  ZSelectFileWidget* m_outputLogWidget = nullptr;
  QComboBox* m_channelCombo = nullptr;
  QSpinBox* m_numSamplesSpin = nullptr;
  QSpinBox* m_strideSpin = nullptr;
  QCheckBox* m_openResultCheck = nullptr;
};

} // namespace nim
