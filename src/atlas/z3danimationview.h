#ifndef Z3DANIMATIONVIEW_H
#define Z3DANIMATIONVIEW_H

#include "z3dfilterview.h"
#include "z3danimationdoc.h"
#include "z3danimationfilter.h"

namespace nim {

class Z3DAnimationView : public Z3DFilterView<Z3DAnimationDoc, Z3DAnimationFilter>
{
  Q_OBJECT
public:
  Z3DAnimationView(Z3DAnimationDoc &doc, Z3DView &view);

private slots:
  void docAnimationAdded(const QList<size_t> &objs);
  void docAnimationAdded(size_t id);

private:
};

} // namespace nim

#endif // Z3DANIMATIONVIEW_H
