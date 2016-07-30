#include "z3dtransferfunctionwidgetwitheditorwindow.h"
#include "z3dtransferfunction.h"
#include "z3dtransferfunctioneditor.h"
#include <QApplication>

namespace nim {

Z3DTransferFunctionWidgetWithEditorWindow::Z3DTransferFunctionWidgetWithEditorWindow(Z3DTransferFunctionParameter* tf,
                                                                                     QWidget* parent)
  : ZClickableTransferFunctionLabel(tf, parent)
  , m_transferFunction(tf)
{
}

void Z3DTransferFunctionWidgetWithEditorWindow::createEditorWindow()
{
  if (m_editorWindow) {
    if (!m_editorWindow->isVisible()) {
      m_editorWindow->show();
    }
    m_editorWindow->raise();
  } else {
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

    m_editorWindow.reset(new Z3DTransferFunctionEditor(m_transferFunction));
    m_editorWindow->show();
    m_editorWindow->raise();

    QApplication::restoreOverrideCursor();
  }
}

void Z3DTransferFunctionWidgetWithEditorWindow::labelClicked()
{
  createEditorWindow();
}

} // namespace nim
