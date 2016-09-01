#pragma once

#include "zfilterview.h"
#include "zroidoc.h"
#include "zroifilter.h"
#include <map>

namespace nim {

class ZROIView : public ZFilterView<ZROIDoc, ZROIFilter>
{
Q_OBJECT
public:
  ZROIView(ZROIDoc& doc, ZView& view);

private:
  void docROIsAdded(const QList<size_t>& objs);

  void docROIAdded(size_t id);
};

} // namespace nim

