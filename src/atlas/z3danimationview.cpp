#include "z3danimationview.h"

namespace nim {

Z3DAnimationView::Z3DAnimationView(Z3DAnimationDoc& doc, Z3DView& view)
  : Z3DFilterView<Z3DAnimationDoc, Z3DAnimationFilter>(doc, view)
{
  docAnimationsAdded(m_doc.objs());
  connect(&m_doc, &Z3DAnimationDoc::objAdded, this, &Z3DAnimationView::docAnimationAdded);
}

void Z3DAnimationView::docAnimationsAdded(const QList<size_t>& objs)
{
  try {
    for (auto id : objs) {
      Z3DAnimationFilter* viewControl = new Z3DAnimationFilter(globalParas(), this);
      viewControl->setData(&m_doc.animation(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
      connect(viewControl, &Z3DAnimationFilter::boundBoxChanged, this, &Z3DAnimationView::updateBoundBox);
      connect(viewControl, &Z3DAnimationFilter::objVisibleChanged, this,
              &Z3DAnimationView::onObjVisibleChangedFromView);
      emit objViewReady(id);
    }
    if (!objs.empty()) {
      networkEvaluator().updateNetwork();
      m_view.updateBoundBox();
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render 3d animation: " << e.what();
    QMessageBox::critical(&m_view.canvas(), qApp->applicationName(), "Failed to render 3d animation.\n" + e.what());
  }
}

void Z3DAnimationView::docAnimationAdded(size_t id)
{
  try {
    Z3DAnimationFilter* viewControl = new Z3DAnimationFilter(globalParas(), this);
    viewControl->setData(&m_doc.animation(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
    connect(viewControl, &Z3DAnimationFilter::boundBoxChanged, this, &Z3DAnimationView::updateBoundBox);
    connect(viewControl, &Z3DAnimationFilter::objVisibleChanged, this, &Z3DAnimationView::onObjVisibleChangedFromView);
    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    emit objViewReady(id);
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render 3d animation: " << e.what();
    QMessageBox::critical(&m_view.canvas(), qApp->applicationName(), "Failed to render 3d animation.\n" + e.what());
  }
}

} // namespace nim
