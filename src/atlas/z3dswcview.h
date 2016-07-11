#ifndef Z3DSWCVIEW_H
#define Z3DSWCVIEW_H

#include "z3dfilterview.h"
#include "zswcdoc.h"
#include "z3dswcfilter.h"

namespace nim {

class Z3DSwcView : public Z3DFilterView<ZSwcDoc, Z3DSwcFilter>
{
  Q_OBJECT
public:
  Z3DSwcView(ZSwcDoc &doc, Z3DView &view);

private slots:
  void docSwcsAdded(const QList<size_t> &objs);
  void docSwcAdded(size_t id);

private:
};

} // namespace nim

#endif // Z3DSWCVIEW_H
