#include "zregionviewsettingwitheditorwindow.h"

#include "zroifilter.h"
#include "zlog.h"
#include <QtWidgets>

namespace nim {

ZRegionViewSettingWithEditorWindow::ZRegionViewSettingWithEditorWindow(ZROIFilter* rf, QWidget* parent)
  : ZRegionViewSettingLabel(rf, parent)
  , m_roiFilter(rf)
{
}

void ZRegionViewSettingWithEditorWindow::createEditorWindow()
{
  if (m_editorWindow) {
    m_editorWindow->showNormal();
    m_editorWindow->raise();
    m_editorWindow->activateWindow();
  } else {
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

    auto wg = m_roiFilter->viewSettingWidgetsGroupForAnnotationFilter();
    auto label = new QLabel(QString("Region: %1").arg(m_roiFilter->regionName()));
    m_editorWindow = wg->createWidget(true, false, label);
    m_editorWindow->setParent(QApplication::activeWindow());
    m_editorWindow->setWindowFlag(Qt::Window, true);
    m_editorWindow->showNormal();
    m_editorWindow->raise();
    m_editorWindow->activateWindow();

    QApplication::restoreOverrideCursor();
  }
}

void ZRegionViewSettingWithEditorWindow::labelClicked()
{
  createEditorWindow();
}

} // namespace nim
