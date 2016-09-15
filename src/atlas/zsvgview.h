#pragma once

#include "zfilterview.h"
#include "zsvgdoc.h"
#include "zsvgfilter.h"
#include <map>

namespace nim {

class ZSvgView : public ZFilterView<ZSvgDoc, ZSvgFilter>
{
Q_OBJECT
public:
  ZSvgView(ZSvgDoc& doc, ZView& view);

private:
  void docSvgsAdded(const QList<size_t>& objs);

  void docSvgAdded(size_t id);
};

} // namespace nim


