#include "z3dobjview.h"

#include "zwidgetsgroup.h"

namespace nim {

Z3DObjView::Z3DObjView(Z3DRenderingEngine& engine)
  : m_engine(engine)
{}

std::shared_ptr<ZWidgetsGroup> Z3DObjView::viewSettingWidgetsGroupOf(size_t /*id*/)
{
  return {};
}

} // namespace nim
