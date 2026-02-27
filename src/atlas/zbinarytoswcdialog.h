#pragma once

#include "zimgprocessdialog.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace nim {

class ZDoc;
class ZSelectFileWidget;

class ZBinaryToSwcDialog : public ZImgProcessDialog
{
  Q_OBJECT

public:
  explicit ZBinaryToSwcDialog(ZDoc& doc, QWidget* parent = nullptr);

protected:
  WorkerSpec createWorkerSpec() override;

private:
  void rebuildSuggestedOutputs();
  void refreshSkeletonizeUiEnabledState();

  [[nodiscard]] QString inputImagePath() const;
  [[nodiscard]] QString skeletonizeConfigPath() const;
  [[nodiscard]] QString outputSwcPath() const;
  [[nodiscard]] QString outputLogPath() const;
  [[nodiscard]] bool loadResultEnabled() const;

private:
  ZDoc& m_doc;

  ZSelectFileWidget* m_inputImageWidget = nullptr;
  ZSelectFileWidget* m_skeletonizeConfigWidget = nullptr;
  ZSelectFileWidget* m_outputSwcWidget = nullptr;
  ZSelectFileWidget* m_outputLogWidget = nullptr;
  QCheckBox* m_loadResultCheck = nullptr;

  QDoubleSpinBox* m_lengthThresholdSpin = nullptr;
  QDoubleSpinBox* m_finalLengthThresholdSpin = nullptr;
  QCheckBox* m_keepShortObjectsCheck = nullptr;

  QCheckBox* m_connectAllCheck = nullptr;
  QDoubleSpinBox* m_distanceThresholdSpin = nullptr;

  QCheckBox* m_excludeSmallObjectsCheck = nullptr;
  QSpinBox* m_minObjSizeSpin = nullptr;

  QCheckBox* m_greyToBinaryCheck = nullptr;
  QComboBox* m_grayOpCombo = nullptr;
  QSpinBox* m_levelSpin = nullptr;

  QCheckBox* m_downsampleCheck = nullptr;
  QSpinBox* m_dsXSpin = nullptr;
  QSpinBox* m_dsYSpin = nullptr;
  QSpinBox* m_dsZSpin = nullptr;

  QCheckBox* m_rebaseCheck = nullptr;
  QCheckBox* m_fillHolesCheck = nullptr;

  bool m_outputSwcCustomized = false;
  bool m_outputLogCustomized = false;
  bool m_applyingSuggestedOutputs = false;
};

} // namespace nim
