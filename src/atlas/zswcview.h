#pragma once

#include "zfilterview.h"
#include "zswcdoc.h"
#include "zswcfilter.h"
#include <map>

namespace nim {

class ZSwcView : public ZFilterView<ZSwcDoc, ZSwcFilter>
{
Q_OBJECT
public:
  ZSwcView(ZSwcDoc& doc, ZView& view);

private:
  void docSwcsAdded(const QList<size_t>& objs);

  void docSwcAdded(size_t id);
};

} // namespace nim

