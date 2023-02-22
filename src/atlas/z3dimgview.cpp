#include "z3dimgview.h"

#include <QApplication>

namespace nim {

Z3DImgView::Z3DImgView(ZImgDoc& doc, Z3DView& view)
  : Z3DFilterView<ZImgDoc, Z3DImgFilter>(doc, view)
{
  docImgsAdded(m_doc.objs());
  connect(&m_doc, &ZImgDoc::objAdded, this, &Z3DImgView::docImgAdded);
}

void Z3DImgView::docImgsAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DImgFilter(globalParas(), this);
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
    LOG(ERROR) << "Failed to render image: " << e.what();
    QMessageBox::critical(&m_view.canvas(),
                          QApplication::applicationName(),
                          QString("Failed to render image:\n%1").arg(e.what()));
  }
}

void Z3DImgView::docImgAdded(size_t id)
{
  try {
    auto viewControl = new Z3DImgFilter(globalParas(), this);
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
    canvas().addEventListenerToBack(*viewControl);

    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render image: " << e.what();
    QMessageBox::critical(&m_view.canvas(),
                          QApplication::applicationName(),
                          QString("Failed to render image:\n%1").arg(e.what()));
  }
}

} // namespace nim
