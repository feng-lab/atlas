#include "z3dobjview.h"

#include "zwidgetsgroup.h"

namespace nim {

Z3DObjView::Z3DObjView(Z3DView &view)
  : QObject(&view)
  , m_view(view)
{
  resetBoundBox();
}

std::shared_ptr<ZWidgetsGroup> Z3DObjView::viewSettingWidgetsGroupOf(size_t id)
{
  Q_UNUSED(id)
  return std::shared_ptr<ZWidgetsGroup>();
}

void Z3DObjView::resetBoundBox()
{
  m_boundBox.resize(6);
  m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = std::numeric_limits<double>::max();
  m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = std::numeric_limits<double>::lowest();
}

void Z3DObjView::expandBoundBox(const std::vector<double> &boundBox)
{
  m_boundBox[0] = std::min(boundBox[0], m_boundBox[0]);
  m_boundBox[1] = std::max(boundBox[1], m_boundBox[1]);
  m_boundBox[2] = std::min(boundBox[2], m_boundBox[2]);
  m_boundBox[3] = std::max(boundBox[3], m_boundBox[3]);
  m_boundBox[4] = std::min(boundBox[4], m_boundBox[4]);
  m_boundBox[5] = std::max(boundBox[5], m_boundBox[5]);
}

} // namespace nim
