#include "zimgview.h"

#include <cassert>
#include <QMessageBox>
#include <QApplication>

namespace nim {

ZImgView::ZImgView(ZImgDoc &doc, ZView &view)
  : ZFilterView<ZImgDoc, ZImgFilter>(doc, view)
{
  docImgAdded(m_doc.objs());
  connect(&m_doc, SIGNAL(objAdded(size_t,ZObjDoc*)), this, SLOT(docImgAdded(size_t)));
  connect(&m_doc, SIGNAL(imgChanged(size_t)), this, SLOT(docImgChanged(size_t)));
}

QString ZImgView::infoOfPos(double x, double y)
{
  QString info;
  try {
  for (std::map<size_t, ZImgFilter*>::iterator it = m_idToFilter.begin();
       it != m_idToFilter.end(); ++it) {
    ZImgFilter *viewControl = it->second;
    if (!viewControl->isVisible())
      continue;
    size_t id = it->first;
    const ZImgPack& imgPack = m_doc.imgPack(id);
    int lx = x - imgPack.offsetX();
    int ly = y - imgPack.offsetY();
    if (lx >= 0 && size_t(lx) < imgPack.imgInfo().width && ly >= 0 && size_t(ly) < imgPack.imgInfo().height) {
      int lz = m_view.isNormalView() ? viewControl->imgSlice() : 0;
      int lt = viewControl->imgTime();
      info += imgPack.sizeInfo();
      if (imgPack.imgInfo().numTimes == 1) {
        info += QString(" Coord: (%1,%2,%3)").arg(lx).arg(ly).arg(lz);
      } else {
        info += QString(" Coord: (%1,%2,%3,%4)").arg(lx).arg(ly).arg(lz).arg(lt);
      }
      info += QString(" Intensity: (%1").arg(imgPack.value(lx,ly,lz,0,lt,m_view.isMaxZProjView()));
      for (size_t c = 1; c < imgPack.imgInfo().numChannels; ++c)
        info += QString(",%1").arg(imgPack.value(lx,ly,lz,c,lt,m_view.isMaxZProjView()));
      info += ")      ";
    }
  }
  } catch (const ZException &e) {
    QMessageBox::critical(QApplication::activeWindow(), "Error", e.what());
  }
  return info;
}

void ZImgView::docImgAdded(const QList<size_t> &objs)
{
  for (int i=0; i<objs.size(); ++i) {
    ZImgFilter *viewControl = new ZImgFilter(m_view);
    viewControl->setData(m_doc.imgPack(objs[i]));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[objs[i]] = viewControl;
    connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
    connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
    connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
    emit objViewReady(objs[i]);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZImgView::docImgAdded(size_t id)
{
  ZImgFilter *viewControl = new ZImgFilter(m_view);
  viewControl->setData(m_doc.imgPack(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id] = viewControl;
  m_view.updateBoundBox();
  connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
  connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
  connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
  emit objViewReady(id);
}

void ZImgView::docImgChanged(size_t id)
{
  m_idToFilter.at(id)->setData(m_doc.imgPack(id));
}

} // namespace nim
