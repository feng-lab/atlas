#ifndef ZSWCVIEW_H
#define ZSWCVIEW_H

#include <map>
#include "zfilterview.h"
#include "zswcdoc.h"
#include "zswcfilter.h"

namespace nim {

class ZSwcView : public ZFilterView<ZSwcDoc, ZSwcFilter>
{
  Q_OBJECT
public:
  ZSwcView(ZSwcDoc &doc, ZView &view);

private:
  void docSwcsAdded(const QList<size_t> &objs);
  void docSwcAdded(size_t id);
};

} // namespace nim

#endif // ZSWCVIEW_H
