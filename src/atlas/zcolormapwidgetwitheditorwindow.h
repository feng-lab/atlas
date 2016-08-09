#pragma once

#include "zclickablelabel.h"

namespace nim {

class ZColorMapParameter;

class ZColorMapEditor;

class ZColorMapWidgetWithEditorWindow : public ZClickableColorMapLabel
{
Q_OBJECT
public:
  explicit ZColorMapWidgetWithEditorWindow(ZColorMapParameter* cm, QWidget* parent = 0);

protected:
  void createEditorWindow();

  virtual void labelClicked() override;

private:
  ZColorMapParameter* m_colorMap;
  ZColorMapEditor* m_colorMapEditor;

  QWidget* m_editorWindow;
};

} // namespace nim

