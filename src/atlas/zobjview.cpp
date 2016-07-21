#include "zobjview.h"

#include "zview.h"
#include <QAction>

namespace nim {

ZObjView::ZObjView(ZView &view)
  : QObject(&view)
  , m_view(view)
{
  resetBoundBox();
}

QString ZObjView::infoOfPos(double x, double y)
{
  Q_UNUSED(x)
  Q_UNUSED(y)
  return QString();
}

std::shared_ptr<ZWidgetsGroup> ZObjView::viewSettingWidgetsGroupOf(size_t id)
{
  Q_UNUSED(id)
  return std::shared_ptr<ZWidgetsGroup>();
}

void ZObjView::resetBoundBox()
{
  m_boundBox.resize(8);
  m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = m_boundBox[6] = std::numeric_limits<int>::max();
  m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = m_boundBox[7] = std::numeric_limits<int>::min();
}

void ZObjView::expandBoundBox(const std::vector<int> &boundBox)
{
  m_boundBox[0] = std::min(boundBox[0], m_boundBox[0]);
  m_boundBox[1] = std::max(boundBox[1], m_boundBox[1]);
  m_boundBox[2] = std::min(boundBox[2], m_boundBox[2]);
  m_boundBox[3] = std::max(boundBox[3], m_boundBox[3]);
  m_boundBox[4] = std::min(boundBox[4], m_boundBox[4]);
  m_boundBox[5] = std::max(boundBox[5], m_boundBox[5]);
  m_boundBox[6] = std::min(boundBox[6], m_boundBox[6]);
  m_boundBox[7] = std::max(boundBox[7], m_boundBox[7]);
}

} // namespace nim
