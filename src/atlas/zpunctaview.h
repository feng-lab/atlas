#pragma once

#include "zfilterview.h"
#include "zpunctadoc.h"
#include "zpunctafilter.h"
#include <map>

namespace nim {

class ZPunctaView : public ZFilterView<ZPunctaDoc, ZPunctaFilter>
{
Q_OBJECT
public:
  ZPunctaView(ZPunctaDoc& doc, ZView& view);

private:
  void docPunctasAdded(const QList<size_t>& objs);

  void docPunctaAdded(size_t id);
};

} // namespace nim

