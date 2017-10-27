#pragma once

#include "zclickablelabel.h"
#include <memory>

namespace nim {

class Z3DTransferFunctionEditor;

class Z3DTransferFunctionWidgetWithEditorWindow : public ZClickableTransferFunctionLabel
{
Q_OBJECT
public:
  explicit Z3DTransferFunctionWidgetWithEditorWindow(Z3DTransferFunctionParameter* tf, QWidget* parent = nullptr);

protected:
  void createEditorWindow();

  void labelClicked() override;

private:
  Z3DTransferFunctionParameter* m_transferFunction;
  Z3DTransferFunctionEditor* m_editorWindow = nullptr;
};

} // namespace nim

