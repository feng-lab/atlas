#pragma once

#include "zclickablelabel.h"

namespace nim {

class ZColorMapParameter;

class ZColorMapEditor;

class ZColorMapWidgetWithEditorWindow : public ZClickableColorMapLabel
{
Q_OBJECT
public:
  explicit ZColorMapWidgetWithEditorWindow(ZColorMapParameter* cm, QWidget* parent = nullptr);

protected:
  void createEditorWindow();

  void labelClicked() override;

private:
  ZColorMapParameter* m_colorMap;
  ZColorMapEditor* m_editorWindow = nullptr;
};

} // namespace nim

