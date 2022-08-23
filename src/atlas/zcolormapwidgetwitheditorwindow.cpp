#include "zcolormapwidgetwitheditorwindow.h"

#include "zcolormap.h"
#include "zcolormapeditor.h"
#include "zlog.h"
#include <QtWidgets>

namespace nim {

ZColorMapWidgetWithEditorWindow::ZColorMapWidgetWithEditorWindow(ZColorMapParameter* cm, QWidget* parent)
  : ZClickableColorMapLabel(cm, parent)
  , m_colorMap(cm)
{}

void ZColorMapWidgetWithEditorWindow::createEditorWindow()
{
  if (m_editorWindow) {
    m_editorWindow->showNormal();
    m_editorWindow->raise();
    m_editorWindow->activateWindow();
  } else {
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

    m_editorWindow = new ZColorMapEditor(m_colorMap, QApplication::activeWindow());
    m_editorWindow->showNormal();
    m_editorWindow->raise();
    m_editorWindow->activateWindow();
    //    connect(this, &ZColorMapWidgetWithEditorWindow::destroyed,
    //            this, &ZColorMapWidgetWithEditorWindow::aboutToBeDestroyed);
    connect(this, &ZColorMapWidgetWithEditorWindow::destroyed, m_editorWindow, &QWidget::deleteLater);

    QApplication::restoreOverrideCursor();
  }
}

void ZColorMapWidgetWithEditorWindow::labelClicked()
{
  createEditorWindow();
}

// void ZColorMapWidgetWithEditorWindow::aboutToBeDestroyed()
//{
//   LOG(INFO) << "here";
// }
//
// void ZColorMapWidgetWithEditorWindow::closeEvent(QCloseEvent *event)
//{
//   LOG(INFO) << "here";
//   if (m_editorWindow)
//     delete m_editorWindow;
// }

} // namespace nim
