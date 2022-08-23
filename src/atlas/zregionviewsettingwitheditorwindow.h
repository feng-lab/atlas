#pragma once

#include "zclickablelabel.h"

namespace nim {

class ZROIFilter;

class ZRegionViewSettingWithEditorWindow : public ZRegionViewSettingLabel
{
  Q_OBJECT

public:
  explicit ZRegionViewSettingWithEditorWindow(ZROIFilter* rf, QWidget* parent = nullptr);

protected:
  void createEditorWindow();

  void labelClicked() override;

private:
  ZROIFilter* m_roiFilter;
  QWidget* m_editorWindow = nullptr;
};

} // namespace nim
