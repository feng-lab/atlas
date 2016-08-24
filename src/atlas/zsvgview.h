//
// Created by Linqing Feng on 8/24/16.
//

#pragma once

#include <map>
#include "zfilterview.h"
#include "zsvgdoc.h"
#include "zsvgfilter.h"

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


