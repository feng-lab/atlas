#pragma once

#include <map>
#include "zfilterview.h"
#include "zpunctadoc.h"
#include "zpunctafilter.h"

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

