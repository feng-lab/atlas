#include "z3dimgview.h"

#include <cassert>

namespace nim {

Z3DImgView::Z3DImgView(ZImgDoc& doc, Z3DView& view)
  : Z3DFilterView<ZImgDoc, Z3DImgFilter>(doc, view)
{
  docImgsAdded(m_doc.objs());
  connect(&m_doc, &ZImgDoc::objAdded, this, &Z3DImgView::docImgAdded);
}

void Z3DImgView::docImgsAdded(const QList<size_t>& objs)
{
  try {
    for (int i = 0; i < objs.size(); ++i) {
      size_t id = objs[i];
      Z3DImgFilter* viewControl = new Z3DImgFilter(globalParas(), this);
      viewControl->setOffset(m_doc.imgPack(id).offsetX(), m_doc.imgPack(id).offsetY(), m_doc.imgPack(id).offsetZ());
      viewControl->setData(m_doc.imgPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      viewControl->outputPort("Image")->connect(compositor().inputPort("Image"));
      viewControl->outputPort("LeftEyeImage")->connect(compositor().inputPort("LeftEyeImage"));
      viewControl->outputPort("RightEyeImage")->connect(compositor().inputPort("RightEyeImage"));
      viewControl->outputPort("VolumeFilter")->connect(compositor().inputPort("VolumeFilters"));
      connect(viewControl, &Z3DImgFilter::boundBoxChanged, this, &Z3DImgView::updateBoundBox);
      connect(viewControl, &Z3DImgFilter::objDeselected, this, &Z3DImgView::onObjDeselectedFromView);
      connect(viewControl, &Z3DImgFilter::objSelected, this, &Z3DImgView::onObjSelectedFromView);
      connect(viewControl, &Z3DImgFilter::objVisibleChanged, this, &Z3DImgView::onObjVisibleChangedFromView);
      canvas().addEventListenerToBack(viewControl);
    }
    if (!objs.empty()) {
      networkEvaluator().updateNetwork();
      m_view.updateBoundBox();

      for (int i = 0; i < objs.size(); ++i) {
        emit objViewReady(objs[i]);
      }
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render image: " << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render image"), e.what());
  }
}

void Z3DImgView::docImgAdded(size_t id)
{
  try {
    Z3DImgFilter* viewControl = new Z3DImgFilter(globalParas(), this);
    viewControl->setOffset(m_doc.imgPack(id).offsetX(), m_doc.imgPack(id).offsetY(), m_doc.imgPack(id).offsetZ());
    viewControl->setData(m_doc.imgPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    viewControl->outputPort("Image")->connect(compositor().inputPort("Image"));
    viewControl->outputPort("LeftEyeImage")->connect(compositor().inputPort("LeftEyeImage"));
    viewControl->outputPort("RightEyeImage")->connect(compositor().inputPort("RightEyeImage"));
    viewControl->outputPort("VolumeFilter")->connect(compositor().inputPort("VolumeFilters"));
    connect(viewControl, &Z3DImgFilter::boundBoxChanged, this, &Z3DImgView::updateBoundBox);
    connect(viewControl, &Z3DImgFilter::objDeselected, this, &Z3DImgView::onObjDeselectedFromView);
    connect(viewControl, &Z3DImgFilter::objSelected, this, &Z3DImgView::onObjSelectedFromView);
    connect(viewControl, &Z3DImgFilter::objVisibleChanged, this, &Z3DImgView::onObjVisibleChangedFromView);
    canvas().addEventListenerToBack(viewControl);

    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    emit objViewReady(id);
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render image: " << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render image"), e.what());
  }
}

} // namespace nim
