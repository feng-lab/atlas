#include "z3danimationview.h"

#include <cassert>

namespace nim {

Z3DAnimationView::Z3DAnimationView(Z3DAnimationDoc &doc, Z3DView &view)
  : Z3DFilterView<Z3DAnimationDoc, Z3DAnimationFilter>(doc, view)
{
  docAnimationAdded(m_doc.objs());
  connect(&m_doc, SIGNAL(objAdded(size_t,ZObjDoc*)), this, SLOT(docAnimationAdded(size_t)));
}

void Z3DAnimationView::docAnimationAdded(const QList<size_t> &objs)
{
  try {
    for (int i=0; i<objs.size(); ++i) {
      size_t id = objs[i];
      Z3DAnimationFilter *viewControl = new Z3DAnimationFilter(globalParas(), this);
      viewControl->setData(m_doc.animation(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id] = viewControl;

      viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
      connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
      connect(viewControl, SIGNAL(objVisibleChanged(bool)), this, SLOT(onObjVisibleChangedFromView(bool)));
      emit objViewReady(id);
    }
    if (!objs.empty()) {
      networkEvaluator().updateNetwork();
      m_view.updateBoundBox();
    }
  }
  catch (const ZException & e) {
    LERROR() << "Failed to render 3d animation:" << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render 3d animation"), e.what());
  }
}

void Z3DAnimationView::docAnimationAdded(size_t id)
{
  try {
    Z3DAnimationFilter *viewControl = new Z3DAnimationFilter(globalParas(), this);
    viewControl->setData(m_doc.animation(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id] = viewControl;

    viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
    connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
    connect(viewControl, SIGNAL(objVisibleChanged(bool)), this, SLOT(onObjVisibleChangedFromView(bool)));
    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    emit objViewReady(id);
  }
  catch (const ZException & e) {
    LERROR() << "Failed to render 3d animation:" << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render 3d animation"), e.what());
  }
}

} // namespace nim
