#include "zcolormapwidgetwitheditorwindow.h"
#include <QtGui>
#include <QtWidgets>
#include "zcolormap.h"
#include "zcolormapeditor.h"
#include "zlog.h"

namespace nim {

ZColorMapWidgetWithEditorWindow::ZColorMapWidgetWithEditorWindow(ZColorMapParameter* cm, QWidget* parent)
  : ZClickableColorMapLabel(cm, parent)
  , m_colorMap(cm)
  , m_editorWindow(nullptr)
{
}

void ZColorMapWidgetWithEditorWindow::createEditorWindow()
{
  if (m_editorWindow) {
    m_editorWindow->showNormal();
    m_editorWindow->raise();
    m_editorWindow->activateWindow();
  } else {
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

    m_editorWindow = new ZColorMapEditor(m_colorMap);
    m_editorWindow->showNormal();
    m_editorWindow->raise();
    m_editorWindow->activateWindow();

    QApplication::restoreOverrideCursor();
  }
}

void ZColorMapWidgetWithEditorWindow::labelClicked()
{
  createEditorWindow();
}

} // namespace nim
