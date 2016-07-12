#ifndef ZROIVIEW_H
#define ZROIVIEW_H

#include <map>
#include "zfilterview.h"
#include "zroidoc.h"
#include "zroifilter.h"

namespace nim {

class ZROIView : public ZFilterView<ZROIDoc, ZROIFilter>
{
  Q_OBJECT
public:
  ZROIView(ZROIDoc &doc, ZView &view);

private:
  void docROIsAdded(const QList<size_t> &objs);
  void docROIAdded(size_t id);
};

} // namespace nim

#endif // ZROIVIEW_H
