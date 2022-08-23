#include "zobjview.h"

#include "zview.h"
#include <QAction>

namespace nim {

ZObjView::ZObjView(ZView& view)
  : QObject(&view)
  , m_view(view)
{}

QString ZObjView::infoOfPos(double /*x*/, double /*y*/)
{
  return QString();
}

std::shared_ptr<ZWidgetsGroup> ZObjView::viewSettingWidgetsGroupOf(size_t /*id*/)
{
  return std::shared_ptr<ZWidgetsGroup>();
}

} // namespace nim
