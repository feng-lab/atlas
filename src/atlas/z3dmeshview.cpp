#include "z3dmeshview.h"

#include <cassert>

namespace nim {

Z3DMeshView::Z3DMeshView(ZMeshDoc &doc, Z3DView &view)
  : Z3DFilterView<ZMeshDoc, Z3DMeshFilter>(doc, view)
{
  docMeshAdded(m_doc.objs());
  connect(&m_doc, SIGNAL(objAdded(size_t,ZObjDoc*)), this, SLOT(docMeshAdded(size_t)));
}

void Z3DMeshView::docMeshAdded(const QList<size_t> &objs)
{
  try {
    for (int i=0; i<objs.size(); ++i) {
      size_t id = objs[i];
      Z3DMeshFilter *viewControl = new Z3DMeshFilter(globalParas(), this);
      viewControl->setData(m_doc.meshList(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id] = viewControl;

      viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
      connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
      connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
      connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
      connect(viewControl, SIGNAL(objVisibleChanged(bool)), this, SLOT(onObjVisibleChangedFromView(bool)));
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
    LERROR() << "Failed to render mesh:" << e.what();
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
    m_idToFilter[id] = viewControl;

    viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
    connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
    connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
    connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
    connect(viewControl, SIGNAL(objVisibleChanged(bool)), this, SLOT(onObjVisibleChangedFromView(bool)));
    canvas().addEventListenerToBack(viewControl);

    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    emit objViewReady(id);
  }
  catch (const ZException & e) {
    LERROR() << "Failed to render mesh:" << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render mesh"), e.what());
  }
}

} // namespace nim
