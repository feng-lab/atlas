#include "z3dimgview.h"

#include <cassert>

namespace nim {

Z3DImgView::Z3DImgView(ZImgDoc &doc, Z3DView &view)
  : Z3DFilterView<ZImgDoc, Z3DImgFilter>(doc, view)
{
  docImgAdded(m_doc.objs());
  connect(&m_doc, SIGNAL(objAdded(size_t,ZObjDoc*)), this, SLOT(docImgAdded(size_t)));
}

void Z3DImgView::docImgAdded(const QList<size_t> &objs)
{
  try {
    for (int i=0; i<objs.size(); ++i) {
      size_t id = objs[i];
      Z3DImgFilter *viewControl = new Z3DImgFilter(globalParas(), this);
      viewControl->setOffset(m_doc.imgPack(id).offsetX(), m_doc.imgPack(id).offsetY(), m_doc.imgPack(id).offsetZ());
      viewControl->setData(m_doc.imgPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id] = viewControl;

      viewControl->outputPort("Image")->connect(compositor().inputPort("Image"));
      viewControl->outputPort("LeftEyeImage")->connect(compositor().inputPort("LeftEyeImage"));
      viewControl->outputPort("RightEyeImage")->connect(compositor().inputPort("RightEyeImage"));
      viewControl->outputPort("VolumeFilter")->connect(compositor().inputPort("VolumeFilters"));
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
    LERROR() << "Failed to render image:" << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render image"), e.what());
  }
}

void Z3DImgView::docImgAdded(size_t id)
{
  try {
    Z3DImgFilter *viewControl = new Z3DImgFilter(globalParas(), this);
    viewControl->setOffset(m_doc.imgPack(id).offsetX(), m_doc.imgPack(id).offsetY(), m_doc.imgPack(id).offsetZ());
    viewControl->setData(m_doc.imgPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id] = viewControl;

    viewControl->outputPort("Image")->connect(compositor().inputPort("Image"));
    viewControl->outputPort("LeftEyeImage")->connect(compositor().inputPort("LeftEyeImage"));
    viewControl->outputPort("RightEyeImage")->connect(compositor().inputPort("RightEyeImage"));
    viewControl->outputPort("VolumeFilter")->connect(compositor().inputPort("VolumeFilters"));
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
    LERROR() << "Failed to render image:" << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render image"), e.what());
  }
}

} // namespace nim
