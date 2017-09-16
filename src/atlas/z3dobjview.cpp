#include "z3dobjview.h"

#include "zwidgetsgroup.h"

namespace nim {

Z3DObjView::Z3DObjView(Z3DView& view)
  : QObject(&view)
  , m_view(view)
{
}

std::shared_ptr<ZWidgetsGroup> Z3DObjView::viewSettingWidgetsGroupOf(size_t /*id*/)
{
  return std::shared_ptr<ZWidgetsGroup>();
}

} // namespace nim
