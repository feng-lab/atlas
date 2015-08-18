#include "zcolormapwidgetwitheditorwindow.h"
#include <QtGui>
#ifndef _QT4_
#include <QtWidgets>
#endif
#include "zcolormap.h"
#include "zcolormapeditor.h"
#include "QsLog.h"

namespace nim {

ZColorMapWidgetWithEditorWindow::ZColorMapWidgetWithEditorWindow(ZColorMapParameter *cm, QWidget *parent)
  : ZClickableColorMapLabel(cm, parent)
  , m_colorMap(cm)
  , m_editorWindow(nullptr)
{
}

void ZColorMapWidgetWithEditorWindow::createEditorWindow()
{
  if (m_editorWindow) {
    if (!m_editorWindow->isVisible()) {
      m_editorWindow->show();
    }
    m_editorWindow->raise();
  } else {
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

    m_editorWindow = new ZColorMapEditor(m_colorMap);
    m_editorWindow->show();
    m_editorWindow->raise();

    QApplication::restoreOverrideCursor();
  }
}

void ZColorMapWidgetWithEditorWindow::labelClicked()
{
  createEditorWindow();
}

} // namespace nim
