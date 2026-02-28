#pragma once

#include "zimgprocessdialog.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;

namespace nim {

class ZDoc;
class ZSelectFileWidget;

class ZEnhanceLineDialog final : public ZImgProcessDialog
{
  Q_OBJECT

public:
  explicit ZEnhanceLineDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  WorkerSpec createWorkerSpec() override;

private:
  void inputImageChanged();

  [[nodiscard]] QString inputImagePath() const;
  [[nodiscard]] QString outputImagePath() const;
  [[nodiscard]] QString outputLogPath() const;
  [[nodiscard]] int channel0() const;
  [[nodiscard]] double sigma() const;
  [[nodiscard]] bool loadResultEnabled() const;

private:
  ZSelectFileWidget* m_inputImageWidget = nullptr;
  ZSelectFileWidget* m_outputImageWidget = nullptr;
  ZSelectFileWidget* m_outputLogWidget = nullptr;
  QComboBox* m_channelCombo = nullptr;
  QDoubleSpinBox* m_sigmaSpin = nullptr;
  QCheckBox* m_openResultCheck = nullptr;
};

} // namespace nim
