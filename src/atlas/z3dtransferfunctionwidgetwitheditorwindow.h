#pragma once

#include "zclickablelabel.h"

namespace nim {

class Z3DTransferFunctionEditor;

class Z3DTransferFunctionWidgetWithEditorWindow : public ZClickableTransferFunctionLabel
{
Q_OBJECT
public:
  explicit Z3DTransferFunctionWidgetWithEditorWindow(Z3DTransferFunctionParameter* tf, QWidget* parent = nullptr);

protected:
  void createEditorWindow();

  virtual void labelClicked() override;

private:
  Z3DTransferFunctionParameter* m_transferFunction;
  Z3DTransferFunctionEditor* m_transferFunctionEditor;

  std::unique_ptr<QWidget> m_editorWindow;
};

} // namespace nim

