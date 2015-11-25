#ifndef Z3DIMGVIEW_H
#define Z3DIMGVIEW_H

#include "z3dfilterview.h"
#include "zimgdoc.h"
#include "z3dimgfilter.h"

namespace nim {

class Z3DImgView : public Z3DFilterView<ZImgDoc, Z3DImgFilter>
{
  Q_OBJECT
public:
  Z3DImgView(ZImgDoc &doc, Z3DView &view);

private slots:
  void docImgAdded(const QList<size_t> &objs);
  void docImgAdded(size_t id);

private:
};

} // namespace nim

#endif // Z3DIMGVIEW_H
