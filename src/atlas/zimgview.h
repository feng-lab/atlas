#ifndef ZIMGVIEW_H
#define ZIMGVIEW_H

#include <map>
#include "zfilterview.h"
#include "zimgdoc.h"
#include "zimgfilter.h"

namespace nim {

class ZImgView : public ZFilterView<ZImgDoc, ZImgFilter>
{
Q_OBJECT
public:
  ZImgView(ZImgDoc& doc, ZView& view);

  // ZObjView interface
public:
  virtual QString infoOfPos(double x, double y) override;

private:
  void docImgsAdded(const QList<size_t>& objs);

  void docImgAdded(size_t id);

  void docImgChanged(size_t id);
};

} // namespace nim

#endif // ZIMGVIEW_H
