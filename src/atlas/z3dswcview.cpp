#include "z3dswcview.h"

namespace nim {

Z3DSwcView::Z3DSwcView(ZSwcDoc& doc, Z3DView& view)
  : Z3DFilterView<ZSwcDoc, Z3DSwcFilter>(doc, view)
{
  docSwcsAdded(m_doc.objs());
  connect(&m_doc, &ZSwcDoc::objAdded, this, &Z3DSwcView::docSwcAdded);
}

void Z3DSwcView::docSwcsAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DSwcFilter(globalParas(), this);
      viewControl->setData(m_doc.swcPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
      connect(viewControl, &Z3DSwcFilter::boundBoxChanged, this, &Z3DSwcView::updateBoundBox);
      connect(viewControl, &Z3DSwcFilter::objDeselected, this, &Z3DSwcView::onObjDeselectedFromView);
      connect(viewControl, &Z3DSwcFilter::objSelected, this, &Z3DSwcView::onObjSelectedFromView);
      connect(viewControl, &Z3DSwcFilter::objVisibleChanged, this, &Z3DSwcView::onObjVisibleChangedFromView);
      canvas().addEventListenerToBack(*viewControl);
    }
    if (!objs.empty()) {
      networkEvaluator().updateNetwork();
      m_view.updateBoundBox();

      for (auto id : objs) {
        Q_EMIT objViewReady(id);
      }
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render swc: " << e.what();
    QMessageBox::critical(&m_view.canvas(),
                          QApplication::applicationName(),
                          QString("Failed to render swc:\n%1").arg(e.what()));
  }
}

void Z3DSwcView::docSwcAdded(size_t id)
{
  try {
    auto viewControl = new Z3DSwcFilter(globalParas(), this);
    viewControl->setData(m_doc.swcPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
    connect(viewControl, &Z3DSwcFilter::boundBoxChanged, this, &Z3DSwcView::updateBoundBox);
    connect(viewControl, &Z3DSwcFilter::objDeselected, this, &Z3DSwcView::onObjDeselectedFromView);
    connect(viewControl, &Z3DSwcFilter::objSelected, this, &Z3DSwcView::onObjSelectedFromView);
    connect(viewControl, &Z3DSwcFilter::objVisibleChanged, this, &Z3DSwcView::onObjVisibleChangedFromView);
    canvas().addEventListenerToBack(*viewControl);

    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render swc: " << e.what();
    QMessageBox::critical(&m_view.canvas(),
                          QApplication::applicationName(),
                          QString("Failed to render swc:\n%1").arg(e.what()));
  }
}

} // namespace nim
