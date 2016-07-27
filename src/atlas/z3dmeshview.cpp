#include "z3dmeshview.h"

#include <cassert>

namespace nim {

Z3DMeshView::Z3DMeshView(ZMeshDoc &doc, Z3DView &view)
  : Z3DFilterView<ZMeshDoc, Z3DMeshFilter>(doc, view)
{
  docMeshesAdded(m_doc.objs());
  connect(&m_doc, &ZMeshDoc::objAdded, this, &Z3DMeshView::docMeshAdded);
}

void Z3DMeshView::docMeshesAdded(const QList<size_t> &objs)
{
  try {
    for (int i=0; i<objs.size(); ++i) {
      size_t id = objs[i];
      Z3DMeshFilter *viewControl = new Z3DMeshFilter(globalParas(), this);
      viewControl->setData(m_doc.meshList(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
      connect(viewControl, &Z3DMeshFilter::boundBoxChanged, this, &Z3DMeshView::updateBoundBox);
      connect(viewControl, &Z3DMeshFilter::objDeselected, this, &Z3DMeshView::onObjDeselectedFromView);
      connect(viewControl, &Z3DMeshFilter::objSelected, this, &Z3DMeshView::onObjSelectedFromView);
      connect(viewControl, &Z3DMeshFilter::objVisibleChanged, this, &Z3DMeshView::onObjVisibleChangedFromView);
      canvas().addEventListenerToBack(viewControl);
    }
    if (!objs.empty()) {
      networkEvaluator().updateNetwork();
      m_view.updateBoundBox();

      for (int i=0; i<objs.size(); ++i) {
        emit objViewReady(objs[i]);
      }
    }
  }
  catch (const ZException & e) {
    LOG(ERROR) << "Failed to render mesh: " << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render mesh"), e.what());
  }
}

void Z3DMeshView::docMeshAdded(size_t id)
{
  try {
    Z3DMeshFilter *viewControl = new Z3DMeshFilter(globalParas(), this);
    viewControl->setData(m_doc.meshList(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
    connect(viewControl, &Z3DMeshFilter::boundBoxChanged, this, &Z3DMeshView::updateBoundBox);
    connect(viewControl, &Z3DMeshFilter::objDeselected, this, &Z3DMeshView::onObjDeselectedFromView);
    connect(viewControl, &Z3DMeshFilter::objSelected, this, &Z3DMeshView::onObjSelectedFromView);
    connect(viewControl, &Z3DMeshFilter::objVisibleChanged, this, &Z3DMeshView::onObjVisibleChangedFromView);
    canvas().addEventListenerToBack(viewControl);

    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    emit objViewReady(id);
  }
  catch (const ZException & e) {
    LOG(ERROR) << "Failed to render mesh: " << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render mesh"), e.what());
  }
}

} // namespace nim
