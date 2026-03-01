#pragma once

#include "zimgprocessdialog.h"

class QCheckBox;
class QDoubleSpinBox;
class QRadioButton;

namespace nim {

class ZDoc;
class ZSelectFileWidget;

class ZRescaleSwcDialog : public ZImgProcessDialog
{
  Q_OBJECT

public:
  explicit ZRescaleSwcDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  WorkerSpec createWorkerSpec() override;

private:
  void updateScaleUi();

  [[nodiscard]] QString inputSwcPath() const;
  [[nodiscard]] QString outputSwcPath() const;
  [[nodiscard]] bool loadResultEnabled() const;

private:
  ZDoc& m_doc;

  ZSelectFileWidget* m_inputSwcWidget = nullptr;
  ZSelectFileWidget* m_outputSwcWidget = nullptr;
  QCheckBox* m_loadResultCheck = nullptr;

  QDoubleSpinBox* m_preTranslateX = nullptr;
  QDoubleSpinBox* m_preTranslateY = nullptr;
  QDoubleSpinBox* m_preTranslateZ = nullptr;

  QRadioButton* m_scaleUseResolution = nullptr;
  QRadioButton* m_scaleManually = nullptr;
  QDoubleSpinBox* m_currentResXY = nullptr;
  QDoubleSpinBox* m_currentResZ = nullptr;
  QDoubleSpinBox* m_targetResXY = nullptr;
  QDoubleSpinBox* m_targetResZ = nullptr;
  QDoubleSpinBox* m_scaleX = nullptr;
  QDoubleSpinBox* m_scaleY = nullptr;
  QDoubleSpinBox* m_scaleZ = nullptr;

  QDoubleSpinBox* m_postTranslateX = nullptr;
  QDoubleSpinBox* m_postTranslateY = nullptr;
  QDoubleSpinBox* m_postTranslateZ = nullptr;

  QCheckBox* m_scaleRadiusCheck = nullptr;
};

} // namespace nim
